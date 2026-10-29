--TEST--
mid-walk OOM preserves native lifecycle and permits a later dump when the engine supports bailout
--DESCRIPTION--
The object is on the declared-slot path (no property table yet, declared properties) and its
get_gc() handler grows a Zend-allocated buffer past memory_limit, so the fatal error is raised
after meminfo_item_open() has already collected the slots. This test checks that the bailout
reaches the script's shutdown and that the extension is still usable there. A leak would only
show when running the script itself under valgrind with the Zend allocator on: run-tests.php -m
sets USE_ZEND_ALLOC=0, which leaves memory_limit unenforced, so the test skips there.
An independent control without meminfo identifies immediate-exit security backports, where the
engine does not bail out or run shutdown. Those must terminate without claiming recoverable state.
--SKIPIF--
<?php
if (getenv('USE_ZEND_ALLOC') === '0') {
    die('skip memory_limit is not enforced without the Zend allocator');
}
if (!function_exists('proc_open')) die('skip proc_open required for native lifecycle control');
if (DIRECTORY_SEPARATOR === '\\') die('skip POSIX child fixture required');
if (!extension_loaded('json')) die('skip json required');
?>
--FILE--
<?php
require __DIR__ . '/fixtures/signal_helpers.php';
$native = meminfo_test_native_oom_behavior();
$code = <<<'TARGET'
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
    fwrite(STDERR, "SPL_SHUTDOWN_ENTERED\n");
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
TARGET;
$ini = array('memory_limit' => '128M');
if (PHP_VERSION_ID < 80000 && is_file(ini_get('extension_dir') . '/json.so')) {
    $ini['extension'] = ini_get('extension_dir') . '/json.so';
}
$child = meminfo_test_spawn($code, $ini);
$status = meminfo_test_finish_child($child, 30);
$output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
echo 'built: ', strpos($output, "built\n") !== false ? 'yes' : 'no', "\n";
echo 'memory error: ', strpos($output, 'Allowed memory size') !== false ? 'yes' : 'no', "\n";
echo 'native lifecycle preserved: ', $status === $native['status']
    && (strpos($output, 'SPL_SHUTDOWN_ENTERED') !== false) === $native['shutdown'] ? 'yes' : 'no', "\n";
echo 'walk did not return: ', strpos($output, 'not reached') === false ? 'yes' : 'no', "\n";
if ($native['immediate_exit']) {
    echo "native immediate exit: no shutdown dump\n";
} else {
    echo 'after the fatal error, a second dump is valid JSON: ',
        strpos($output, 'after the fatal error, a second dump is valid JSON: yes') !== false ? 'yes' : 'no', "\n";
}
meminfo_test_cleanup($child);
?>
--EXPECTREGEX--
built: yes
memory error: yes
native lifecycle preserved: yes
walk did not return: yes
(?:native immediate exit: no shutdown dump|after the fatal error, a second dump is valid JSON: yes)
