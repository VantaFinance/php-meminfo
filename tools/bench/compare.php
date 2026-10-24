<?php
/**
 * Compare two bench-dump.php results and fail when the head is slower.
 *
 *   php compare.php <base.json>... -- <head.json>... [--threshold=15]
 *
 * Several results per side are allowed (runs interleaved base, head, base,
 * head to spread the noise of a shared CI runner); each side takes the best
 * median per target. Exit status 1 when a target of the head is more than
 * --threshold percent slower than the base, 2 on bad input.
 *
 * Kept parseable by PHP 7.0, the oldest version the extension supports.
 */

$threshold = 15.0;
$base = array();
$head = array();
$side = 'base';

foreach (array_slice($argv, 1) as $arg) {
    if (strpos($arg, '--threshold=') === 0) {
        $threshold = (float) substr($arg, strlen('--threshold='));
        continue;
    }

    if ($arg === '--') {
        $side = 'head';
        continue;
    }

    $result = json_decode((string) @file_get_contents($arg), true);
    if (!is_array($result) || !isset($result['targets'])) {
        fwrite(STDERR, "error: $arg is not a bench-dump.php result\n");
        exit(2);
    }

    if ($side === 'base') {
        $base[] = $result;
        continue;
    }

    $head[] = $result;
}

if (!$base || !$head) {
    fwrite(STDERR, "error: usage: compare.php <base.json>... -- <head.json>... [--threshold=15]\n");
    exit(2);
}

/**
 * @return array target => best (lowest) median over the given results
 */
function compare_best(array $results)
{
    $best = array();
    foreach ($results as $result) {
        foreach ($result['targets'] as $target => $numbers) {
            $median = $numbers['median_ms'];
            $best[$target] = isset($best[$target]) ? min($best[$target], $median) : $median;
        }
    }

    return $best;
}

$baseBest = compare_best($base);
$headBest = compare_best($head);
$failed = false;

printf("%-6s %12s %12s %9s   items: base %d, head %d\n", 'target', 'base ms', 'head ms', 'change', $base[0]['items'], $head[0]['items']);

foreach ($baseBest as $target => $baseMs) {
    if (!isset($headBest[$target])) {
        continue;
    }

    $change = ($headBest[$target] - $baseMs) / $baseMs * 100;
    $slower = $change > $threshold;
    $failed = $failed || $slower;
    printf("%-6s %12.1f %12.1f %+8.1f%%%s\n", $target, $baseMs, $headBest[$target], $change, $slower ? '   <- slower than ' . $threshold . '%' : '');
}

if ($failed) {
    fwrite(STDERR, "error: the dump got more than {$threshold}% slower than the base\n");
    exit(1);
}
