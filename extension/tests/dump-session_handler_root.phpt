--TEST--
What the session save handler callables capture is reachable from its <CALLBACK> roots
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!extension_loaded('session')) die('skip session ext not loaded');
    if (PHP_ZTS) die('skip session globals are read only in NTS builds');
?>
--FILE--
<?php
    class SessionHandlerMarker {}

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
        $marker = new SessionHandlerMarker();
        $handler = function () use ($marker) {
            return true;
        };
        // The callback form works on every version; 8.4 deprecates it, and any
        // output here would send headers and make the call fail.
        @session_set_save_handler($handler, $handler, $handler, $handler, $handler, $handler);
    }

    register();
    $items = dump_items();

    $symbols = root_symbols($items, '<CALLBACK>');
    echo "has session roots: ", count(preg_grep('/^session_set_save_handler\[\d+\]$/', $symbols)) > 0 ? 'yes' : 'no', "\n";
    echo "SessionHandlerMarker: ", reached($items, 'SessionHandlerMarker'), "\n";
?>
--EXPECT--
has session roots: yes
SessionHandlerMarker: yes
