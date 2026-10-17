--TEST--
Check that the content of SPL containers and WeakMap is dumped (GitHub issue #133)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class InArrayObject {}
    class InArrayIterator {}
    class InObjectStorage {}
    class StorageInfo {}
    class InLinkedList {}
    class InHeap {}
    class InPriorityQueue {}
    class InFixedArray {}
    class InWeakMap {}

    $arrayObject = new ArrayObject(array(new InArrayObject()));
    $arrayIterator = new ArrayIterator(array('k' => new InArrayIterator()));

    $storage = new SplObjectStorage();
    $storage[new InObjectStorage()] = new StorageInfo();

    $list = new SplDoublyLinkedList();
    $list->push(new InLinkedList());

    $heap = new SplMinHeap();
    $heap->insert(array(new InHeap()));

    $queue = new SplPriorityQueue();
    $queue->insert(new InPriorityQueue(), 1);

    $fixed = new SplFixedArray(1);
    $fixed[0] = new InFixedArray();

    $expected = array('InArrayObject', 'InArrayIterator', 'InObjectStorage', 'StorageInfo',
        'InLinkedList', 'InHeap', 'InPriorityQueue', 'InFixedArray');

    if (PHP_VERSION_ID >= 80000) {
        $key = new stdClass();
        $weakMap = new WeakMap();
        $weakMap[$key] = new InWeakMap();
        $expected[] = 'InWeakMap';
    }

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $found = array();
    foreach ($meminfoData['items'] as $item) {
        if (isset($item['class'])) {
            $found[$item['class']] = true;
        }
    }

    $missing = array();
    foreach ($expected as $class) {
        if (!isset($found[$class])) {
            $missing[] = $class;
        }
    }
    echo $missing ? 'missing: ' . implode(', ', $missing) : 'all found', "\n";
?>
--EXPECT--
all found
