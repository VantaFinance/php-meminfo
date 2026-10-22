/*
 * Declarations shared by the extension's source files; not part of any API.
 *
 * Every function declared here is internal to meminfo.so: MEMINFO_HIDDEN keeps
 * it out of the dynamic symbol table, so that it can neither clash with nor be
 * interposed by a symbol of another extension loaded into the same process.
 */
#ifndef MEMINFO_INTERNAL_H
#define MEMINFO_INTERNAL_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#ifndef PHP_WIN32
# include <dirent.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/types.h>
# include <time.h>
# include <unistd.h>
#endif
#ifdef __APPLE__
// No /proc: the facts the trigger needs about the target come from sysctl.
# include <sys/sysctl.h>
# define MEMINFO_HAVE_KINFO_PROC 1
#endif

#if PHP_VERSION_ID >= 80100
// zend_fiber_context, in the fiber observer prototypes below.
# include "zend_fibers.h"
#endif

#if (defined(__GNUC__) && __GNUC__ >= 4) || defined(__clang__)
# define MEMINFO_HIDDEN __attribute__((visibility("hidden")))
#else
# define MEMINFO_HIDDEN
#endif

#if defined(ZTS) && defined(COMPILE_DL_MEMINFO)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/* Append a string literal, without a strlen call. */
#define MEMINFO_WRITE_LIT(w, literal) \
    meminfo_write_raw((w), (literal), sizeof(literal) - 1)

/*
 * Where a trigger asking for a dump in a directory of its choice leaves that
 * directory for the target: the one place both know without any configuration.
 */
#define MEMINFO_REQUEST_DIR "/tmp"

// Default action of SIGURG is to ignore it, which makes it the only common
// choice that cannot kill a process that is not listening. php-fpm resets
// SIGUSR2 to SIG_DFL in every worker, where its default action is to
// terminate, so an idle worker signalled with it would simply die.
#define MEMINFO_DEFAULT_DUMP_SIGNAL "SIGURG"

/* How often the trigger looks for the dump file: first after the minimum, then
 * twice as long each time up to the maximum, so that a small dump is picked up
 * within milliseconds and a long one does not cost a stat() every few ms. */
#define MEMINFO_POLL_MIN_INTERVAL_USEC 5000
#define MEMINFO_POLL_INTERVAL_USEC 50000
/* How often the signal is resent while the target has not started writing. */
#define MEMINFO_RESEND_INTERVAL_USEC 1000000

/* Dump files are named <prefix><id>.json, or <prefix><pid>-<sec>-<usec>.json
 * without an id: written by signal.c, looked for by trigger.c. */
#define MEMINFO_DUMP_FILE_PREFIX "meminfo-"
/**
 * Set of already-visited item addresses.
 *
 * Open addressing with linear probing over a power-of-two table of raw
 * pointers. A zend_hash keyed by the printed "%p" identifier was doing the same
 * job, but it allocated a zend_string per lookup and charged the whole table to
 * the process being inspected; a persistent zend_hash would instead pemalloc
 * and abort the process when memory runs out. Slot value 0 means empty, which
 * is unambiguous because no item ever lives at address 0.
 */
typedef struct {
    uintptr_t *slots;
    size_t capacity;    /* power of two, 0 before the first insert */
    size_t count;
    int failed;         /* allocation failed: the dump has to stop */
} meminfo_ptr_set;

/**
 * A declared property read straight out of an object.
 */
typedef struct {
    zend_string *name;      /* borrowed from the class entry; still mangled */
    zval *value;            /* borrowed: the property slot inside the object */
} meminfo_prop_slot;

/**
 * One level of the traversal: an item whose children still have to be visited.
 *
 * The children of an item are written as a flat "children" block when the item
 * itself is written; the zvals behind them are visited afterwards, which is
 * what this level records. Children come either from a HashTable or, for a
 * plain object whose property table does not exist yet, from a list of
 * declared property slots — and then, for objects that keep data outside their
 * properties (closures, generators, SPL containers…), from what get_gc() lists.
 */
typedef struct {
    HashTable *ht;                      /* children left to visit, NULL when not iterating a table */
    HashPosition pos;                   /* cursor into ht */

    meminfo_prop_slot *slots;           /* declared properties, NULL when not iterating slots */
    uint32_t slot_count;
    uint32_t slot_pos;

    HashTable *gc_ht;                   /* children only get_gc() exposes, visited after the above */
    HashPosition gc_ht_pos;
    zval *gc_values;                    /* copies of get_gc()'s zvals, owned by a meminfo_gc_block */
    uint32_t gc_count;
    uint32_t gc_pos;

#if PHP_VERSION_ID >= 70400
    HashTable *properties_to_release;   /* get_properties() result, released once the subtree is done */
#endif
#if PHP_VERSION_ID >= 80100
    zend_execute_data *pending_fiber;   /* fiber frames to walk once the subtree is done */
#endif
} meminfo_level;

/**
 * Explicit work stack replacing C recursion over the object graph.
 *
 * It is deliberately allocated with malloc rather than emalloc: the dump runs
 * inside a process that is being inspected for memory problems and must not
 * push it over memory_limit, and a dump triggered from an interrupt may run
 * while the Zend allocator is already near its limit.
 */
typedef struct {
    meminfo_level *levels;
    size_t len;
    size_t capacity;
    int exhausted;      /* an allocation failed: stop descending, still close the JSON */
} meminfo_stack;

/**
 * Copies of the zvals an object's get_gc() handler listed.
 *
 * The handler hands back a buffer the engine reuses on the next call, so the
 * zvals are copied out at once. The copies are kept until the dump ends rather
 * than until their subtree is done: a scalar item is identified by the address
 * of its zval, and a freed block whose memory came back for another one would
 * make new items look already visited.
 */
typedef struct meminfo_gc_block {
    struct meminfo_gc_block *next;
    uint32_t count;
    zval values[1];
} meminfo_gc_block;

/**
 * State threaded through the whole traversal.
 */
typedef struct {
    meminfo_writer *w;
    meminfo_ptr_set visited_items;
    int first_element;
    meminfo_stack stack;
    int aborted;        /* an allocation outside the stack and the set failed */
    meminfo_gc_block *gc_blocks;    /* every get_gc() copy made so far, freed at the end */
    /* The level meminfo_item_open() is filling, until it is pushed or closed.
     * Kept here rather than on the C stack so that what it already owns (the
     * slot array, a property table reference) can still be released after a
     * bailout out of a get_properties() or get_gc() handler. */
    meminfo_level opening;
} meminfo_context;

/**
 * Whether the traversal has given up (an allocation failed).
 *
 * The dump is still closed as valid JSON when this is set — a truncated but
 * parseable dump is far more useful than a corrupt one.
 */
static zend_always_inline int meminfo_aborted(const meminfo_context *ctx)
{
    // A failed write counts too: once the output cannot be stored there is no
    // point walking the rest of the heap.
    return ctx->aborted || ctx->stack.exhausted || ctx->visited_items.failed || ctx->w->err;
}

/* writer.c */
MEMINFO_HIDDEN void meminfo_writer_flush(meminfo_writer *w);
MEMINFO_HIDDEN zend_never_inline void meminfo_write_raw_slow(meminfo_writer *w, const char *ptr, size_t len);

/**
 * Append len bytes to the writer.
 *
 * Inline fast path: room in the buffer, one memcpy. It does not look at err:
 * once a write has failed, the next flush discards whatever was staged, so
 * bytes appended here after that point never reach the sink.
 */
static zend_always_inline void meminfo_write_raw(meminfo_writer *w, const char *ptr, size_t len)
{
    if (EXPECTED(len <= sizeof(w->buf) - w->len)) {
        memcpy(w->buf + w->len, ptr, len);
        w->len += len;
        return;
    }
    meminfo_write_raw_slow(w, ptr, len);
}
MEMINFO_HIDDEN void meminfo_write_json_escaped_len(meminfo_writer *w, const char *s, size_t len);
MEMINFO_HIDDEN void meminfo_write_json_escaped(meminfo_writer *w, const char *s);
MEMINFO_HIDDEN void meminfo_write_json_escaped_key(meminfo_writer *w, zend_string *key, zend_bool is_property);
MEMINFO_HIDDEN void meminfo_write_int(meminfo_writer *w, int64_t value);
MEMINFO_HIDDEN void meminfo_write_ptr(meminfo_writer *w, const void *ptr);

/* ptr_set.c */
MEMINFO_HIDDEN size_t meminfo_ptr_slot(uintptr_t ptr, size_t mask);
MEMINFO_HIDDEN int meminfo_visit_item(meminfo_ptr_set *set, const void *ptr);

/* traverse.c */
MEMINFO_HIDDEN meminfo_level *meminfo_stack_push(meminfo_stack *stack);
MEMINFO_HIDDEN void meminfo_level_close(meminfo_context *ctx, meminfo_level *level);
MEMINFO_HIDDEN int meminfo_level_is_pending(const meminfo_level *level);
MEMINFO_HIDDEN void meminfo_visit_exec_frame(meminfo_context *ctx, zend_execute_data *start_frame);
MEMINFO_HIDDEN int meminfo_dump_to_writer_guarded(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table, int *status);

/* items.c */
MEMINFO_HIDDEN void meminfo_zval_dump(meminfo_context *ctx, char *frame_label, const char *symbol_name, size_t symbol_name_len, zval *zv);

/* fibers.c */
#if PHP_VERSION_ID >= 80100
MEMINFO_HIDDEN void meminfo_fiber_switch(zend_fiber_context *from, zend_fiber_context *to);
MEMINFO_HIDDEN void meminfo_fiber_destroy(zend_fiber_context *context);
MEMINFO_HIDDEN void meminfo_visit_suspended_contexts(meminfo_context *ctx);
#endif

/* signal.c */
#if !defined(PHP_WIN32)
MEMINFO_HIDDEN int meminfo_parse_signal(const char *value, int *out);
#endif
MEMINFO_HIDDEN const char *meminfo_dump_directory(void);
#if !defined(PHP_WIN32)
MEMINFO_HIDDEN int meminfo_request_file_name(char *buffer, size_t size, const char *base, int pid, int dump_id);
#endif
#if MEMINFO_HAVE_SIGNAL_DUMP
MEMINFO_HIDDEN void meminfo_install_signal_handler(void);
MEMINFO_HIDDEN void meminfo_restore_signal_handler(void);
#endif

/* procinfo.c */
#if !defined(PHP_WIN32)
MEMINFO_HIDDEN uint64_t meminfo_now_usec(void);
MEMINFO_HIDDEN void meminfo_sleep_usec(uint64_t usec);
MEMINFO_HIDDEN int meminfo_signal_is_harmless(int signal_number);
MEMINFO_HIDDEN int meminfo_signal_is_caught(pid_t pid, int signal_number);
MEMINFO_HIDDEN int meminfo_process_uids(pid_t pid, uid_t *real_uid, uid_t *effective_uid);
MEMINFO_HIDDEN int meminfo_process_start_time(pid_t pid, unsigned long long *out);
#endif
#if !defined(PHP_WIN32) && defined(HAVE_SIGQUEUE)
MEMINFO_HIDDEN int meminfo_random_dump_id(void);
#endif
#if !defined(PHP_WIN32)
MEMINFO_HIDDEN int meminfo_dump_file_is_trustworthy(const char *path, pid_t pid);
#endif
#if MEMINFO_HAVE_SIGNAL_DUMP
/* signal.c, called by MINIT/MSHUTDOWN */
MEMINFO_HIDDEN void meminfo_signal_module_startup(void);
MEMINFO_HIDDEN void meminfo_signal_module_shutdown(void);
#endif

#endif /* MEMINFO_INTERNAL_H */
