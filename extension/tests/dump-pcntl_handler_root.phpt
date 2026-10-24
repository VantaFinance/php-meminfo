--TEST--
pcntl_signal() handlers are roots
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!function_exists('pcntl_signal_get_handler')) die('skip pcntl_signal_get_handler() not available');
?>
--FILE--
<?php
    class SignalHandlerMarker {}

    function reached(array $items, $class)
    {
        foreach ($items as $item) {
            if (isset($item["class"]) && $item["class"] === $class) {
                return "yes";
            }
        }

        return "no";
    }

    function root_symbols(array $items, $frame)
    {
        $symbols = array();
        foreach ($items as $item) {
            if ($item["is_root"] && $item["frame"] === $frame) {
                $symbols[] = $item["symbol_name"];
            }
        }
        sort($symbols);

        return $symbols;
    }

    function dump_items()
    {
        $dump = fopen("php://memory", "rw");
        meminfo_dump($dump);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        return $data["items"];
    }

    function register()
    {
        $marker = new SignalHandlerMarker();
        pcntl_signal(SIGUSR2, function () use ($marker) {
        });
    }

    register();
    $items = dump_items();

    echo "root: ", in_array('pcntl_signal[' . SIGUSR2 . ']', root_symbols($items, '<CALLBACK>'), true) ? 'yes' : 'no', "\n";
    echo "SignalHandlerMarker: ", reached($items, 'SignalHandlerMarker'), "\n";
?>
--EXPECT--
root: yes
SignalHandlerMarker: yes
