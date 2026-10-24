/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Public header of the meminfo extension.
 *
 * What the phpize/PECL convention expects from php_<ext>.h: the module entry,
 * the module globals, the version and copyright macros and the few functions
 * exported from meminfo.so. Everything shared only between the extension's own
 * source files is in internal.h instead.
 */

#ifndef PHP_MEMINFO_H
#define PHP_MEMINFO_H 1

extern zend_module_entry meminfo_module_entry;
#define phpext_meminfo_ptr &meminfo_module_entry

/**
 * Whether this build can dump on a signal.
 *
 * The target side needs zend_interrupt_function and EG(vm_interrupt), which
 * arrived in 7.1, and a POSIX signal API. Under ZTS a signal is delivered to an
 * arbitrary thread while the globals and the interrupt flag are per-thread, so
 * the handler is not installed there either.
 *
 * meminfo_dump() and meminfo_dump_process() work regardless — a process can
 * always be asked for a dump from a build that cannot produce one itself.
 */
#if PHP_VERSION_ID >= 70100 && !defined(ZTS) && !defined(PHP_WIN32)
# define MEMINFO_HAVE_SIGNAL_DUMP 1
#else
# define MEMINFO_HAVE_SIGNAL_DUMP 0
#endif

#ifndef PHP_WIN32
# include <signal.h>
#endif

#define MEMINFO_NAME "PHP Meminfo"
#define MEMINFO_AUTHOR "Benoit Jacquemont"

// Both come from configure (--with-meminfo-version, --with-meminfo-copyright-year)
// through config.h; the fallbacks only serve IDE parsing and -D overrides.
#ifndef MEMINFO_VERSION
# define MEMINFO_VERSION "dev"
#endif
#ifndef MEMINFO_COPYRIGHT_YEAR
# define MEMINFO_COPYRIGHT_YEAR "unknown"
#endif

#define MEMINFO_COPYRIGHT_SHORT "Copyright (c) 2010-" MEMINFO_COPYRIGHT_YEAR
#define MEMINFO_COPYRIGHT MEMINFO_COPYRIGHT_SHORT " by Benoit Jacquemont, Vlad Shashkov, Vanta Team & contributors"

// Size of the writer's staging buffer. Big enough that a dump of a large object
// graph costs hundreds of writes rather than millions; 1 MiB measured 10-15%
// faster than 64 KiB on every supported version. The writer is always malloc'd
// (see below) — never put one on the stack.
#define MEMINFO_WRITER_BUFFER_SIZE (1024 * 1024)

/**
 * Output sink of a dump.
 *
 * Output is staged in buf and handed over in large chunks, either through the
 * stream layer (so that a user stream wrapper passed to meminfo_dump() keeps
 * working) or straight to a file descriptor — a dump triggered from an
 * interrupt has no php_stream and must not allocate one.
 *
 * Once a write fails, err holds its errno and every later append is dropped:
 * the traversal notices and stops instead of spending minutes producing output
 * that cannot be stored.
 *
 * Allocate it with malloc, never emalloc: the buffer must not be charged to
 * the memory_limit of the process being inspected, which may already be at its
 * limit — that is often why it is being dumped.
 */
typedef struct {
    php_stream *stream;     /* flush through the stream layer, or NULL */
    int fd;                 /* flush with write(2), or -1 */
    size_t len;             /* bytes staged in buf */
    int err;                /* errno of the write that failed (never 0 then); output is being discarded */
    char buf[MEMINFO_WRITER_BUFFER_SIZE];
} meminfo_writer;

#if PHP_VERSION_ID >= 80100
/**
 * Where one suspended fiber context stopped: its top frame at the moment it
 * was switched away from.
 */
typedef struct {
    void *context;                  /* zend_fiber_context *; NULL marks a free slot */
    void *frame;                    /* zend_execute_data * */
} meminfo_suspended_context;

/**
 * Open-addressing table of every suspended fiber context, keyed by context.
 * malloc'd, like everything else the extension keeps, so that it never counts
 * towards memory_limit.
 */
typedef struct {
    meminfo_suspended_context *slots;
    size_t capacity;                /* power of two, 0 before the first insert */
    size_t count;
} meminfo_suspended_map;
#endif

/**
 * Per-process state of the extension.
 *
 * dump_requested and dump_id are written by the signal handler and read at the
 * next safe point in the VM, hence volatile sig_atomic_t: nothing else in a
 * handler is guaranteed to be observed correctly.
 */
/* Options of a dump (meminfo_dump()'s array, or the ini defaults on a signal) */
#define MEMINFO_DUMP_UNREACHABLE    (1 << 0)    /* objects alive but reached from no root */
#define MEMINFO_DUMP_STRING_HASHES  (1 << 1)    /* a hash of strings of 32 bytes or more */

/* One object store slot as meminfo_mark() last saw it (generations.c) */
typedef struct {
    zend_object *object;
    zend_class_entry *ce;
    uint32_t generation;
} meminfo_generation_slot;

/* Where an object was created, by handle (generations.c, meminfo.track_allocations) */
typedef struct {
    zend_object *object;
    zend_class_entry *ce;
    zend_string *file;
    uint32_t line;
} meminfo_site_slot;

ZEND_BEGIN_MODULE_GLOBALS(meminfo)
    /* ini: signal name or number to dump on, "0" or empty to disable */
    char *dump_signal_setting;
    /* ini: directory for dump files, empty means the system temp directory */
    char *dump_dir;
    /* ini: defaults of the dump options, for dumps taken on a signal */
    zend_bool dump_unreachable;
    zend_bool dump_string_hashes;

    /* MEMINFO_DUMP_* flags of the dump being written */
    int dump_flags;
    /* what the dump being written was taken on, for the header: "call", "signal", … */
    const char *dump_trigger;
    /* the pending request comes from the threshold, not from a signal */
    zend_bool request_from_threshold;
    /* ini: every dump marks a new generation first */
    zend_bool auto_mark;
    /* ini: a dump signal waits for meminfo_dump_if_requested() */
    zend_bool dump_defer;
    /* ini: dump when memory_limit is reached, and once usage passes a threshold */
    zend_bool dump_on_limit;
    char *dump_threshold;
    /* this request already dumped on the limit / on the threshold */
    zend_bool limit_dumped;
    zend_bool threshold_crossed;
    /* dump_threshold in bytes, worked out for this memory_limit (it can change at runtime) */
    size_t threshold_bytes;
    zend_long threshold_limit_seen;
    /* ini: record where objects are created ("new") */
    zend_bool track_allocations;
    /* the sites by handle; the last "new" not yet matched to its object */
    meminfo_site_slot *sites;
    uint32_t sites_capacity;
    int pending_site_handle;
    zend_string *pending_site_file;
    uint32_t pending_site_line;
    zend_class_entry *pending_site_ce;
    /* the site table could not grow: logged once */
    zend_bool sites_failed;

    /* generations.c: the last mark (0: none this request) and the table by handle */
    uint32_t epoch;
    meminfo_generation_slot *generations;
    uint32_t generations_capacity;

    /* dump_signal_setting resolved to a signal number, 0 when disabled */
    int dump_signal;

    /* set by the signal handler, consumed by the interrupt hook */
    volatile sig_atomic_t dump_requested;
    volatile sig_atomic_t dump_id;

    /* the handler is installed for the duration of a request only */
    zend_bool handler_installed;
    /* complained once already about a signal that cannot be installed */
    zend_bool install_failed;
    /* guards against a dump started while one is already running */
    zend_bool dumping;

#if MEMINFO_HAVE_SIGNAL_DUMP
    struct sigaction previous_action;
#endif

#if PHP_VERSION_ID >= 80100
    /* fiber contexts that are suspended right now, filled by the fiber observers */
    meminfo_suspended_map suspended;
#endif
ZEND_END_MODULE_GLOBALS(meminfo)

ZEND_EXTERN_MODULE_GLOBALS(meminfo)

#define MEMINFO_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(meminfo, v)

PHP_FUNCTION(meminfo_dump);
PHP_FUNCTION(meminfo_mark);
PHP_FUNCTION(meminfo_histogram);
PHP_FUNCTION(meminfo_dump_if_requested);

#ifndef PHP_WIN32
/**
 * meminfo_dump_process(int $pid, array $options = []): string
 *
 * Signals another process to dump its memory and waits for the file. Works
 * from any build, including ones that cannot serve a dump themselves.
 *
 * Returns the path of the finished dump. Every failure throws instead of
 * returning a value: InvalidArgumentException when the call itself is wrong,
 * RuntimeException for anything about the target or the environment.
 */
PHP_FUNCTION(meminfo_dump_process);
#endif

zend_ulong   meminfo_get_element_size(zval* z);

/**
 * Dump every item reachable from start_frame plus all class static members.
 *
 * start_frame is the innermost execution frame to walk; passing the frame of
 * the caller rather than reading EG(current_execute_data) is what makes the
 * traversal usable from an interrupt handler.
 *
 * include_global_symbol_table additionally walks EG(symbol_table) as
 * <GLOBAL>. The global scope is normally reached through the main script's
 * frame, but a dump taken during shutdown — from a shutdown function or a
 * destructor — starts from a chain that no longer contains it.
 *
 * Returns MEMINFO_DUMP_COMPLETE, or why the dump is not: MEMINFO_DUMP_INCOMPLETE
 * when the walk ran out of memory for its own bookkeeping (the output is still
 * closed as valid JSON, with items missing), MEMINFO_DUMP_WRITE_FAILED when the
 * sink refused bytes (w->err holds the errno). Either way the caller must not
 * present the result as a complete dump.
 *
 * The walk's own state is freed on every path, a Zend bailout included (a fatal
 * error raised by a get_properties() or get_gc() handler mid-walk): it is
 * released, then the bailout continues to the caller, which must clean up what
 * it owns — the writer, the file — in a zend_try of its own.
 */
#define MEMINFO_DUMP_COMPLETE      0
#define MEMINFO_DUMP_INCOMPLETE    1
#define MEMINFO_DUMP_WRITE_FAILED  2

int meminfo_dump_to_writer(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table);

// The traversal itself (frames, static members, items and their children) is
// internal to meminfo.c: it is driven by context and bookkeeping types that
// callers of the extension have no business knowing about.

void meminfo_build_frame_label(char * frame_label, int frame_label_len, zend_execute_data* frame);

#endif
