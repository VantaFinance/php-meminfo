--TEST--
meminfo_dump() releases its state when a fatal error bails out of the walk, and a later dump works
--DESCRIPTION--
The object is on the declared-slot path (no property table yet, declared properties) and its
get_gc() handler grows a Zend-allocated buffer past memory_limit, so the fatal error is raised
after meminfo_item_open() has already collected the slots. This test checks that the bailout
reaches the script's shutdown and that the extension is still usable there. A leak would only
show when running the script itself under valgrind with the Zend allocator on: run-tests.php -m
sets USE_ZEND_ALLOC=0, which leaves memory_limit unenforced, so the test skips there.
--SKIPIF--
<?php
if (getenv('USE_ZEND_ALLOC') === '0') {
    die('skip memory_limit is not enforced without the Zend allocator');
}
?>
--FILE--
<?php
class MeminfoBailoutList extends SplDoublyLinkedList
{
    public $label = 'list';
    public $extra;
}

$list = new MeminfoBailoutList();
for ($i = 0; $i < 400000; $i++) {
    $list->push(new stdClass());
}

register_shutdown_function(function () {
    ini_set('memory_limit', '-1');
    // PHP 7.x SPL containers keep a corrupt get_gc() buffer after their erealloc failed: drop it.
    $GLOBALS['list'] = null;

    $stream = fopen('php://memory', 'w+');
    meminfo_dump($stream);
    rewind($stream);
    echo "after the fatal error, a second dump is valid JSON: ",
        json_decode(stream_get_contents($stream)) !== null ? "yes" : "no", "\n";
});

echo "built\n";
ini_set('memory_limit', (string) (memory_get_usage(true) + 2 * 1048576));
meminfo_dump(fopen('php://memory', 'w'));
echo "not reached\n";
?>
--EXPECTF--
built
%AFatal error: Allowed memory size of %d bytes exhausted%A
after the fatal error, a second dump is valid JSON: yes
