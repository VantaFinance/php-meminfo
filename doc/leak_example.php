<?php
/**
 * A small worker that leaks, for doc/hunting_down_memory_leaks.md.
 *
 * It handles "orders" in a loop, the way a queue consumer does. Each order is
 * meant to be forgotten once handled, but a listener is registered for every
 * order and never removed, and each listener keeps its order alive.
 *
 * Usage: php -d extension=meminfo.so doc/leak_example.php [dir]
 * Writes <dir>/leak-before.json after 100 orders and <dir>/leak-after.json
 * after 1000 (dir defaults to the system temp directory).
 */

class Order
{
    public $id;
    public $lines = array();

    public function __construct($id)
    {
        $this->id = $id;
        for ($i = 0; $i < 5; $i++) {
            $this->lines[] = array('sku' => 'SKU-' . $id . '-' . $i, 'qty' => $i + 1);
        }
    }
}

class AuditListener
{
    private $order;

    public function __construct(Order $order)
    {
        $this->order = $order;
    }

    public function onShipped()
    {
        // Would write an audit line for $this->order.
    }
}

class Dispatcher
{
    private $listeners = array();

    public function listen($event, $listener)
    {
        $this->listeners[$event][] = $listener;
    }

    public function forget($event, $listener)
    {
        $key = array_search($listener, isset($this->listeners[$event]) ? $this->listeners[$event] : array(), true);
        if ($key !== false) {
            unset($this->listeners[$event][$key]);
        }
    }
}

function handle_order(Dispatcher $dispatcher, $id)
{
    $order = new Order($id);

    // The leak: a listener per order, registered on a long-lived dispatcher
    // and never removed. It holds the order, so no order is ever freed.
    $dispatcher->listen('order.shipped', new AuditListener($order));
}

$dir = isset($argv[1]) ? rtrim($argv[1], '/') : sys_get_temp_dir();
$dispatcher = new Dispatcher();

for ($id = 1; $id <= 1000; $id++) {
    handle_order($dispatcher, $id);

    if ($id === 100) {
        meminfo_dump(fopen($dir . '/leak-before.json', 'w'));
    }
}

meminfo_dump(fopen($dir . '/leak-after.json', 'w'));
echo $dir . "/leak-before.json\n" . $dir . "/leak-after.json\n";
