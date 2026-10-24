--TEST--
One item per allocation: shared strings and copy-on-write arrays are dumped once
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    // A string built at runtime is refcounted; held by 100 array elements.
    $big = str_repeat('x', 1000);
    $holders = [];
    for ($i = 0; $i < 100; $i++) {
        $holders[] = $big;
    }

    // One copy-on-write array held by five containers.
    $shared = [1, 2, 3];
    $owners = [];
    for ($i = 0; $i < 5; $i++) {
        $owners['o' . $i] = $shared;
    }

    // Written to after the copy: separated, a second allocation.
    $separated = $shared;
    $separated[] = 4;

    // Interned literals are not refcounted and keep one item per zval.
    $literalA = 'interned literal';
    $literalB = 'interned literal';

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    echo "identity_model: ", $meminfoData['header']['identity_model'], "\n";

    $items = $meminfoData['items'];
    $roots = [];
    foreach ($items as $id => $item) {
        if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>') {
            $roots[$item['symbol_name']] = $id;
        }
    }

    // Every holder element points at the item of $big.
    $bigId = $roots['big'];
    $holderTargets = array_unique(array_values($items[$roots['holders']]['children']));
    echo "holders point at one item: ", var_export($holderTargets === [$bigId], true), "\n";

    // Counted by size, not by content: the environment run-tests passes can
    // hold other long strings.
    $bigStrings = 0;
    foreach ($items as $item) {
        if ($item['type'] === 'string' && $item['size'] === $items[$bigId]['size']) {
            $bigStrings++;
        }
    }
    echo "items of the shared string: ", $bigStrings, "\n";

    // The five owners and $shared share one array item.
    $sharedId = $roots['shared'];
    $ownerTargets = array_unique(array_values($items[$roots['owners']]['children']));
    echo "owners point at one item: ", var_export($ownerTargets === [$sharedId], true), "\n";
    echo "shared array has 3 children: ", count($items[$sharedId]['children']), "\n";

    echo "separated is another item: ", var_export(isset($roots['separated']) && $roots['separated'] !== $sharedId, true), "\n";
    echo "separated has 4 children: ", count($items[$roots['separated']]['children']), "\n";

    echo "interned literals stay apart: ", var_export(isset($roots['literalA'], $roots['literalB']) && $roots['literalA'] !== $roots['literalB'], true), "\n";
?>
--EXPECT--
identity_model: 2
holders point at one item: true
items of the shared string: 1
owners point at one item: true
shared array has 3 children: 3
separated is another item: true
separated has 4 children: 4
interned literals stay apart: true
