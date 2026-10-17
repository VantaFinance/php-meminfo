<?php
/**
 * Regenerate the real dump fixtures used by the UI tests.
 *
 *   php ui/tests/fixtures/make-fixtures.php
 *   MEMINFO_EXT=/path/to/meminfo.so php -n ui/tests/fixtures/make-fixtures.php
 *   php ui/tests/fixtures/make-fixtures.php --parity   # see below
 *
 * Needs PHP 8.1+ with the meminfo extension (enums, Fibers). Each scenario runs
 * in its own process so that statics and classes of one never show up in the
 * dump of another. Addresses differ on every run; tests must never hard-code
 * them.
 *
 * Two groups:
 *  - parity/  frozen. Their *.expected.json baselines were recorded by the PHP
 *             CLI analyzer, which has since been removed, and they hold the
 *             addresses of these exact dumps. Regenerating a parity dump would
 *             leave its baseline unmatched with nothing left to rebuild it, so
 *             they are skipped unless --parity is passed, and even then only
 *             once the stale baselines have been deleted by hand;
 *  - ui-only/ quirks that json_decode loses or rejects (invalid UTF-8,
 *             NUL-truncated keys, duplicate keys, lost roots).
 */

$scenarios = array(
    'parity/basic' => 'scenario_basic',
    'parity/leak-before' => 'scenario_leak_before',
    'parity/leak-after' => 'scenario_leak_after',
    'ui-only/invalid-utf8' => 'scenario_invalid_utf8',
    'ui-only/nul-keys' => 'scenario_nul_keys',
    'ui-only/dup-private' => 'scenario_dup_private',
    'ui-only/lost-root' => 'scenario_lost_root',
);

if ($argc < 3) {
    $withParity = in_array('--parity', $argv, true);

    if ($withParity) {
        $baselines = glob(__DIR__ . '/parity/*.expected.json');
        if ($baselines) {
            fwrite(STDERR, "refusing --parity: these baselines would no longer match and cannot be rebuilt:\n");
            foreach ($baselines as $baseline) {
                fwrite(STDERR, '  ' . $baseline . "\n");
            }
            fwrite(STDERR, "delete them (and replace the parity tests in basic.test.ts) first\n");
            exit(1);
        }
    }

    if (!function_exists('meminfo_dump')) {
        fwrite(STDERR, "meminfo is not loaded; set MEMINFO_EXT or enable it in php.ini\n");
        exit(1);
    }

    $command = escapeshellarg(PHP_BINARY);
    $extension = getenv('MEMINFO_EXT');
    if ($extension !== false && $extension !== '') {
        $command .= ' -n -d ' . escapeshellarg('extension=' . $extension);
    }
    $command .= ' -d memory_limit=-1 ' . escapeshellarg(__FILE__);

    foreach ($scenarios as $name => $function) {
        if (strpos($name, 'parity/') === 0 && !$withParity) {
            printf("%-26s skipped (frozen, see --parity)\n", $name);
            continue;
        }

        $out = __DIR__ . '/' . $name . '.json';
        if (!is_dir(dirname($out))) {
            mkdir(dirname($out), 0777, true);
        }

        passthru($command . ' ' . escapeshellarg($function) . ' ' . escapeshellarg($out), $code);
        if ($code !== 0) {
            fwrite(STDERR, "scenario $name failed with exit code $code\n");
            exit(1);
        }

        printf("%-26s %7d bytes\n", $name, filesize($out));
    }

    exit(0);
}

$function = $argv[1];
$out = $argv[2];

if (!in_array($function, $scenarios, true)) {
    fwrite(STDERR, "unknown scenario $function\n");
    exit(1);
}

/* --- parity -------------------------------------------------------------- */

enum Suit: string
{
    case Hearts = 'H';
    case Spades = 'S';
}

class Base
{
    protected $prot = 'protected-value';
    private $priv = 1;
    public $pub = array(1, 2);
}

class Child extends Base
{
    public $extra;
}

class Registry
{
    public static $items = array();
    public static $counter = 0;
}

function scenario_basic($out)
{
    $scalar = 42;
    $float = 1.5;
    $flag = true;
    $nothing = null;

    $list = array(10, 20, 30);
    $map = array('a' => 1, 'b' => array('nested' => 'x'));
    $mixedKeys = array(5 => 'five', 'k' => 'v', -3 => 'negative', PHP_INT_MAX => 'max');

    $object = new Child();
    $object->extra = new stdClass();
    $object->extra->back = $object;   // cycle

    Registry::$items['first'] = new Base();
    Registry::$counter = 7;

    $suit = Suit::Hearts;

    $shared = str_repeat('s', 100);   // one string, several zvals
    $copyA = $shared;
    $copyB = $shared;

    $cow = array('x' => 1, 'y' => 2); // copy-on-write shared array
    $cowCopy = $cow;

    $target = 'referenced';
    $reference = &$target;            // collapses into one item

    $fiber = new Fiber(function () {
        $insideFiber = 'suspended local';
        Fiber::suspend(1);
    });
    $fiber->start();

    meminfo_dump(fopen($out, 'w'));
}

class Request
{
    public $payload;
    public $tags;

    public function __construct(public int $id)
    {
        $this->payload = str_repeat('x', 64);
        $this->tags = array('id-' . $id, 'web');
    }
}

class LeakyCache
{
    public static $requests = array();
}

function handle_request($id)
{
    // The leak: every handled request is kept forever.
    LeakyCache::$requests[] = new Request($id);
}

function scenario_leak($out, $count)
{
    for ($i = 0; $i < $count; $i++) {
        handle_request($i);
    }

    $notLeaking = array_fill(0, 10, 'steady');

    meminfo_dump(fopen($out, 'w'));
}

function scenario_leak_before($out)
{
    scenario_leak($out, 50);
}

function scenario_leak_after($out)
{
    scenario_leak($out, 200);
}

/* --- ui-only ------------------------------------------------------------- */

function scenario_invalid_utf8($out)
{
    // Keys are written raw: invalid UTF-8 reaches the dump as is, control
    // characters as \u00XX, quotes and backslashes escaped.
    $keys = array(
        'plain' => 1,
        "bad\xff\xfe" => 2,
        "ctl\x01\x1f" => 3,
        "quote\"back\\slash" => 4,
        "caf\xc3\xa9" => 5,
    );

    meminfo_dump(fopen($out, 'w'));
}

function scenario_nul_keys($out)
{
    // Keys are cut at the first NUL byte: both become "a".
    $keys = array("a\0x" => 1, "a\0y" => 2, 'b' => 3);

    meminfo_dump(fopen($out, 'w'));
}

class DupParent
{
    private $secret = 'parent';
}

class DupChild extends DupParent
{
    private $secret = 'child';
}

function scenario_dup_private($out)
{
    // Two private properties named "secret": emitted as duplicate keys.
    $object = new DupChild();

    meminfo_dump(fopen($out, 'w'));
}

class Holder
{
    public static $kept;
}

function scenario_lost_root($out)
{
    // Reached first as a local of this frame: the global and the static that
    // also hold it emit no item at all.
    $local = new stdClass();
    $local->tag = 'shared';
    Holder::$kept = $local;
    $GLOBALS['alsoHeld'] = $local;

    meminfo_dump(fopen($out, 'w'));
}

// Last: enums and classes that extend others are declared at runtime, in order.
$function($out);
