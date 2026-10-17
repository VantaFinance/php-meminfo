<?php
/**
 * Record what the PHP analyzer computes on the parity fixtures, so the UI's
 * engine can be checked against it without PHP in the UI's CI job.
 *
 *   (cd analyzer && composer install)
 *   php ui/tests/fixtures/analyzer-baseline.php
 *
 * The analyzer's CLI only prints tables, so its Analyzer classes are called
 * directly. Writes parity/<name>.expected.json next to each fixture.
 */

use BitOne\PhpMemInfo\Analyzer\GraphBrowser;
use BitOne\PhpMemInfo\Analyzer\QueryExecutor;
use BitOne\PhpMemInfo\Analyzer\SummaryCreator;
use BitOne\PhpMemInfo\Analyzer\TopSearcher;
use BitOne\PhpMemInfo\Loader;

require __DIR__ . '/../../../analyzer/vendor/autoload.php';

const TOP_LIMIT = 20;

/** Filter sets run through QueryExecutor, as the `query` command builds them. */
$queries = array(
    'roots' => array('is_root' => array('operator' => '=', 'value' => 1)),
    'strings' => array('type' => array('operator' => '=', 'value' => 'string')),
    'request objects' => array(
        'class' => array('operator' => '=', 'value' => 'Request'),
        'is_root' => array('operator' => '=', 'value' => 0),
    ),
    'class regex' => array('class' => array('operator' => '~', 'value' => '^(Child|Base|Registry)$')),
);

foreach (glob(__DIR__ . '/parity/*.json') as $file) {
    if (substr($file, -14) === '.expected.json') {
        continue;
    }

    $loader = new Loader();
    $items = $loader->load($file);

    $summary = array();
    $summaryCreator = new SummaryCreator($items);
    foreach ($summaryCreator->createSummary() as $type => $row) {
        $summary[] = array('type' => (string) $type, 'count' => $row['count'], 'self_size' => $row['self_size']);
    }

    $top = array();
    $topSearcher = new TopSearcher($items);
    foreach ($topSearcher->createTopChildren(TOP_LIMIT) as $id => $count) {
        $top[] = array('id' => $id, 'children' => $count);
    }

    $queryResults = array();
    $executor = new QueryExecutor($items);
    foreach ($queries as $name => $filters) {
        $queryResults[] = array(
            'name' => $name,
            'filters' => $filters,
            'limit' => 10,
            'ids' => array_keys($executor->executeQuery($filters, 10)),
        );
    }

    // Reference paths for a few targets: the first object of each class, and
    // the first root.
    $targets = array();
    foreach ($items as $id => $item) {
        if ($item['type'] === 'object' && !isset($targets[$item['class']])) {
            $targets[$item['class']] = $id;
        }
    }
    foreach ($items as $id => $item) {
        if ($item['is_root']) {
            $targets['<first root>'] = $id;
            break;
        }
    }

    $paths = array();
    $browser = new GraphBrowser($items);
    foreach ($targets as $label => $target) {
        $found = array();
        foreach ($browser->findReferencePaths($target) as $rootId => $edges) {
            $names = array();
            foreach ($edges as $edge) {
                $names[] = (string) $edge->getAttribute('name');
            }
            $found[] = array('root' => (string) $rootId, 'length' => count($edges), 'names' => $names);
        }
        usort($found, function ($a, $b) {
            return strcmp($a['root'], $b['root']);
        });
        $paths[] = array('label' => $label, 'target' => $target, 'paths' => $found);
    }

    $expected = array(
        'fixture' => basename($file),
        'items' => count($items),
        'summary' => $summary,
        'topChildren' => $top,
        'queries' => $queryResults,
        'referencePaths' => $paths,
    );

    $out = substr($file, 0, -5) . '.expected.json';
    file_put_contents($out, json_encode($expected, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n");
    printf("%-22s %d items, %d summary rows, %d targets\n", basename($out), count($items), count($summary), count($paths));
}
