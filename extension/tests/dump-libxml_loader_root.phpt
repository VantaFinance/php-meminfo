--TEST--
The libxml external entity loader is a root (8.2+)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!function_exists('libxml_get_external_entity_loader')) die('skip libxml_get_external_entity_loader() needs PHP 8.2 and ext/libxml');
?>
--FILE--
<?php
    class EntityLoaderMarker {}

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
        $marker = new EntityLoaderMarker();
        libxml_set_external_entity_loader(function () use ($marker) {
            return null;
        });
    }

    register();
    $items = dump_items();

    echo "root: ", in_array('libxml_set_external_entity_loader', root_symbols($items, '<CALLBACK>'), true) ? 'yes' : 'no', "\n";
    echo "EntityLoaderMarker: ", reached($items, 'EntityLoaderMarker'), "\n";
?>
--EXPECT--
root: yes
EntityLoaderMarker: yes
