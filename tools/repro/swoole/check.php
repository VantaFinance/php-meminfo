<?php
/**
 * Check one dump: valid JSON, and which marker classes it holds.
 *
 *   php -n check.php <label> <dump.json> [Expected ...] [--gap Known ...]
 *
 * Prints one PASS/FAIL line per expected marker and an INFO line per known gap
 * (a marker the dump is known not to reach). Exits 1 on any FAIL.
 */

ini_set('memory_limit', '-1');

$label = $argv[1];
$file = $argv[2];
$expected = array();
$gaps = array();
$target = 'expected';
foreach (array_slice($argv, 3) as $argument) {
    if ($argument === '--gap') {
        $target = 'gaps';
        continue;
    }
    ${$target}[] = $argument;
}

$data = is_file($file) ? json_decode(file_get_contents($file), true) : null;
if (!is_array($data)) {
    printf("FAIL %s: %s is not a valid dump (%s)\n", $label, $file, json_last_error_msg());
    exit(1);
}

$counts = array();
foreach ($data['items'] as $item) {
    if (isset($item['class'])) {
        $counts[$item['class']] = (isset($counts[$item['class']]) ? $counts[$item['class']] : 0) + 1;
    }
}

printf("     %s: valid JSON, %d items, %d bytes\n", $label, count($data['items']), filesize($file));

$failed = false;
foreach ($expected as $marker) {
    $count = isset($counts[$marker]) ? $counts[$marker] : 0;
    printf("%s %s: %s x%d\n", $count > 0 ? 'PASS' : 'FAIL', $label, $marker, $count);
    $failed = $failed || $count === 0;
}
foreach ($gaps as $marker) {
    $count = isset($counts[$marker]) ? $counts[$marker] : 0;
    printf("INFO %s: %s x%d (known gap)\n", $label, $marker, $count);
}

exit($failed ? 1 : 0);
