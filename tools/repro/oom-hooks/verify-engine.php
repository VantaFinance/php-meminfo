<?php
/** Record binary provenance and independently verify the selected OOM class. */
require __DIR__ . '/../../../extension/tests/fixtures/signal_helpers.php';

$expected = isset($argv[1]) ? $argv[1] : '';
if (!in_array($expected, array('legacy-fatal', 'immediate-exit'), true)) {
    throw new InvalidArgumentException('expected legacy-fatal or immediate-exit');
}
$native = meminfo_test_native_oom_behavior();
$observed = $native['immediate_exit'] ? 'immediate-exit' : 'legacy-fatal';
$facts = array('expected' => $expected, 'observed' => $observed,
    'php' => PHP_VERSION, 'zend' => zend_version(), 'sapi' => PHP_SAPI,
    'arch' => php_uname('m'), 'binary_sha256' => hash_file('sha256', PHP_BINARY),
    'module_sha256' => hash_file('sha256', meminfo_test_extension_path()),
    'signal_source_sha256' => hash_file('sha256', __DIR__ . '/../../../extension/signal.c'),
    'native' => $native);
echo json_encode($facts, JSON_PRETTY_PRINT), "\n";
if ($observed !== $expected) {
    throw new RuntimeException('error: pinned OOM engine changed behavior');
}
