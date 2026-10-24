--TEST--
Autoloaders, tick functions, header_register_callback() and the default stream context are roots
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class AutoloaderMarker {}
    class TickMarker {}
    class TickArgumentMarker {}
    class HeaderCallbackMarker {}

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
            // Extensions may add callbacks of their own (Swoole registers a shutdown function).
            if ($item["is_root"] && $item["frame"] === $frame
                && strpos($item["symbol_name"], "register_shutdown_function") !== 0) {
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
        $autoload = new AutoloaderMarker();
        spl_autoload_register(function ($class) use ($autoload) {
        });

        $tick = new TickMarker();
        register_tick_function(function ($argument) use ($tick) {
        }, new TickArgumentMarker());

        $header = new HeaderCallbackMarker();
        header_register_callback(function () use ($header) {
        });

        stream_context_set_default(array('http' => array('user_agent' => str_repeat('a', 20))));
    }

    register();
    $items = dump_items();

    echo implode("\n", root_symbols($items, '<CALLBACK>')), "\n";
    foreach (array('AutoloaderMarker', 'TickMarker', 'TickArgumentMarker', 'HeaderCallbackMarker') as $class) {
        echo $class, ': ', reached($items, $class), "\n";
    }
?>
--EXPECT--
header_register_callback
register_tick_function[0]
register_tick_function[0]($0)
spl_autoload_register[0]
stream_context_set_default
AutoloaderMarker: yes
TickMarker: yes
TickArgumentMarker: yes
HeaderCallbackMarker: yes
