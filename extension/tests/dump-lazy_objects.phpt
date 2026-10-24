--TEST--
Lazy objects (8.4+): the initializer and the instance of an initialized proxy are reachable, nothing is initialized
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80400) die('skip lazy objects need PHP 8.4');
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class LazyService
    {
        public $config;

        public function __construct($config)
        {
            $this->config = $config;
        }
    }

    class GhostContainer {}
    class ProxyContainer {}

    $reflection = new ReflectionClass(LazyService::class);

    // Held only by the initializer closure of an uninitialized ghost.
    $ghost = (function () use ($reflection) {
        $container = new GhostContainer();

        return $reflection->newLazyGhost(function (LazyService $object) use ($container) {
            $object->__construct($container);
        });
    })();

    // An initialized proxy: the real instance lives outside the proxy object.
    $proxy = (function () use ($reflection) {
        $container = new ProxyContainer();

        return $reflection->newLazyProxy(function () use ($container) {
            return new LazyService($container);
        });
    })();
    $proxy->config;

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $items = $meminfoData['items'];

    /** Whether an item of class $class can be reached from $from. */
    function reaches(array $items, $from, $class)
    {
        $seen = [];
        $todo = [$from];
        while ($todo) {
            $id = array_pop($todo);
            if (isset($seen[$id]) || !isset($items[$id])) {
                continue;
            }
            $seen[$id] = true;
            if (isset($items[$id]['class']) && $items[$id]['class'] === $class) {
                return true;
            }
            foreach (isset($items[$id]['children']) ? $items[$id]['children'] : [] as $child) {
                $todo[] = $child;
            }
        }

        return false;
    }

    $roots = [];
    foreach ($items as $id => $item) {
        if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>') {
            $roots[$item['symbol_name']] = $id;
        }
    }

    echo "ghost reaches its initializer's container: ", var_export(reaches($items, $roots['ghost'], 'GhostContainer'), true), "\n";
    echo "proxy reaches its instance's container: ", var_export(reaches($items, $roots['proxy'], 'ProxyContainer'), true), "\n";
    echo "ghost still uninitialized: ", var_export($reflection->isUninitializedLazyObject($ghost), true), "\n";
?>
--EXPECT--
ghost reaches its initializer's container: true
proxy reaches its instance's container: true
ghost still uninitialized: true
