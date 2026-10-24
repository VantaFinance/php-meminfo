--TEST--
meminfo_histogram() counts live objects and their bytes per class, without a dump
--FILE--
<?php
    class HistogramEntry
    {
        public $a;
        public $b;
    }

    $kept = array();
    for ($i = 0; $i < 10; $i++) {
        $kept[] = new HistogramEntry();
    }

    $histogram = meminfo_histogram();
    echo "HistogramEntry: ", $histogram['HistogramEntry']['count'], "\n";
    echo "bytes: ", var_export($histogram['HistogramEntry']['size'] === 10 * (56 + 16), true), "\n";

    $kept = array_slice($kept, 0, 4);
    $after = meminfo_histogram();
    echo "after freeing six: ", $after['HistogramEntry']['count'], "\n";
    echo "rows have count and size: ", implode(',', array_keys($after['HistogramEntry'])), "\n";
?>
--EXPECT--
HistogramEntry: 10
bytes: true
after freeing six: 4
rows have count and size: count,size
