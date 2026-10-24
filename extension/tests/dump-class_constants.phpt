--TEST--
Enum cases in use are <CLASS_CONSTANT> roots; unused cases are not evaluated by the dump (8.1+)
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80100) die('skip enums need PHP 8.1');
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    enum Suit: string
    {
        case Hearts = 'H';
        case Spades = 'S';
    }

    enum NeverUsed
    {
        case Nobody;
    }

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

    // Not kept in a variable: the case objects are held only by the class.
    // Both are used: 8.1 creates a case object on its first use, later
    // versions may create all cases of the enum at once.
    $value = Suit::Hearts->value . Suit::Spades->value;
    $items = dump_items();

    $symbols = root_symbols($items, '<CLASS_CONSTANT>');
    echo implode("\n", array_values(preg_grep('/^(Suit|NeverUsed)::/', $symbols))), "\n";
?>
--EXPECT--
Suit::Hearts
Suit::Spades
