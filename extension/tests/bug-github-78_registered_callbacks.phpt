--TEST--
Check that registered callbacks and what they capture are dumped (GitHub issue #78)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class ErrorHandlerMarker {}
    class OlderErrorHandlerMarker {}
    class ExceptionHandlerMarker {}
    class ShutdownMarker {}
    class ShutdownArgumentMarker {}
    class OutputHandlerMarker {}

    function register()
    {
        $older = new OlderErrorHandlerMarker();
        set_error_handler(function () use ($older) {
            return false;
        });

        $marker = new ErrorHandlerMarker();
        set_error_handler(function () use ($marker) {
            return false;
        });

        $exception = new ExceptionHandlerMarker();
        set_exception_handler(function () use ($exception) {
        });

        $shutdown = new ShutdownMarker();
        register_shutdown_function(function ($argument) use ($shutdown) {
        }, new ShutdownArgumentMarker());

        $output = new OutputHandlerMarker();
        ob_start(function ($buffer) use ($output) {
            return $buffer;
        });
    }

    register();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    ob_end_flush();
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $classes = array();
    $roots = array();
    foreach ($meminfoData['items'] as $item) {
        if (isset($item['class'])) {
            $classes[$item['class']] = true;
        }
        if ($item['is_root'] && $item['frame'] === '<CALLBACK>') {
            // Where the older handler sits in the stack differs between PHP
            // versions, and extensions (Swoole) register shutdown functions too.
            $roots[] = preg_replace('/^(set_error_handler|register_shutdown_function)\[\d+\]/', '$1[n]', $item['symbol_name']);
        }
    }

    $roots = array_unique($roots);
    sort($roots);
    echo implode("\n", $roots), "\n";

    foreach (array('ErrorHandlerMarker', 'OlderErrorHandlerMarker', 'ExceptionHandlerMarker',
        'ShutdownMarker', 'ShutdownArgumentMarker', 'OutputHandlerMarker') as $class) {
        echo $class, ': ', isset($classes[$class]) ? 'yes' : 'no', "\n";
    }
?>
--EXPECT--
ob_start[0]
register_shutdown_function[n]
register_shutdown_function[n]($0)
set_error_handler
set_error_handler[n]
set_exception_handler
ErrorHandlerMarker: yes
OlderErrorHandlerMarker: yes
ExceptionHandlerMarker: yes
ShutdownMarker: yes
ShutdownArgumentMarker: yes
OutputHandlerMarker: yes
