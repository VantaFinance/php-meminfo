/* Research-only, NTS PHP 7.4-8.1 allocator monitor. Not part of meminfo.so. */
#include "php.h"
#include "php_streams.h"
#include "zend_exceptions.h"
#include "zend_vm_opcodes.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ZTS
#error "The allocator prototype supports NTS builds only"
#endif

#if PHP_VERSION_ID < 70400 || PHP_VERSION_ID >= 80200
#error "This research prototype has only been validated against PHP 7.4 through 8.1"
#endif

/* Use libc formatting on the allocation path, outside the Zend heap. */
#undef snprintf

static struct {
    zend_mm_heap *heap;
    zval stream;
    size_t threshold;
    unsigned long allocations;
    unsigned long reallocations;
    int active;
    int pending;
    int handling;
    int dumped;
    int log_fd;
} probe;

static void (*previous_interrupt)(zend_execute_data *) = NULL;
static user_opcode_handler_t previous_opcodes[256];
static int opcode_mode;
static int notify_only;
static int internal_mode;
static void (*previous_execute_internal)(zend_execute_data *, zval *) = NULL;

static void probe_execute_internal(zend_execute_data *execute_data, zval *return_value);

static void probe_log(const char *phase, size_t requested)
{
    char line[320];
    int saved_errno = errno;
    int length = snprintf(line, sizeof(line),
        "[ALLOC_PROBE] phase=%s pid=%ld requested=%zu real=%zu used=%zu pending=%d dumped=%d allocations=%lu reallocations=%lu shutdown=%d\n",
        phase, (long) getpid(), requested, zend_memory_usage(1), zend_memory_usage(0),
        probe.pending, probe.dumped, probe.allocations, probe.reallocations,
        (EG(flags) & EG_FLAGS_IN_SHUTDOWN) != 0);

    if (probe.log_fd >= 0 && length > 0 && (size_t) length < sizeof(line)) {
        /* Best effort only: no PHP calls, heap allocation or retry loop. */
        ssize_t result = write(probe.log_fd, line, (size_t) length);
        (void) result;
    }
    errno = saved_errno;
}

static void probe_notify(size_t requested)
{
    size_t real;

    if (!probe.active || probe.handling || probe.pending || probe.dumped) {
        return;
    }
    real = zend_memory_usage(internal_mode ? 0 : 1);
    /* Requested realloc size is deliberately a conservative estimate, not
     * the exact growth in mapped pages. No heap structures are walked here. */
    if (real < probe.threshold && requested < probe.threshold - real) {
        return;
    }
    probe.pending = 1;
    probe_log("notify", requested);
    EG(vm_interrupt) = 1;
}

static void *probe_malloc(size_t size)
{
    probe.allocations++;
    probe_notify(size);
    return _zend_mm_alloc(probe.heap, size ZEND_FILE_LINE_EMPTY_CC ZEND_FILE_LINE_EMPTY_CC);
}

/* Alternative backend: observe a completed internal call without touching MM.
 * Only request an interrupt here; execution is still inside the call frame. */
static void probe_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    if (previous_execute_internal != NULL) {
        previous_execute_internal(execute_data, return_value);
    } else {
        execute_internal(execute_data, return_value);
    }
    if (EG(exception) == NULL && !CG(in_compilation) && !(EG(flags) & EG_FLAGS_IN_SHUTDOWN)) {
        probe_notify(0);
    }
}

static void probe_free(void *ptr)
{
    _zend_mm_free(probe.heap, ptr ZEND_FILE_LINE_EMPTY_CC ZEND_FILE_LINE_EMPTY_CC);
}

static void *probe_realloc(void *ptr, size_t size)
{
    probe.reallocations++;
    probe_notify(size);
    return _zend_mm_realloc(probe.heap, ptr, size ZEND_FILE_LINE_EMPTY_CC ZEND_FILE_LINE_EMPTY_CC);
}

static int probe_set_limit(size_t limit)
{
#if PHP_VERSION_ID >= 80000 && PHP_VERSION_ID < 80100
    zend_set_memory_limit(limit);
    return 1;
#else
    return zend_set_memory_limit(limit) == SUCCESS;
#endif
}

/* This is called only from VM hooks, never from an allocator callback. */
static void probe_safe_point(const char *phase)
{
    zval function_name;
    zval result;
    size_t limit;
    int status;

    if (!probe.active || !probe.pending || probe.handling || probe.dumped || notify_only
        || EG(exception) != NULL || CG(in_compilation)
        || (EG(flags) & EG_FLAGS_IN_SHUTDOWN)) {
        return;
    }
    probe.handling = 1;
    probe.pending = 0;
    probe_log(phase, 0);
    limit = PG(memory_limit) > 0 ? (size_t) PG(memory_limit) : (size_t) -1;
    /* The same temporary internal limit lift as meminfo's signal path. The
     * application's configured PG(memory_limit) and final OOM stay unchanged. */
    if (!probe_set_limit((size_t) ZEND_LONG_MAX)) {
        probe_log("limit-lift-failed", 0);
        probe.handling = 0;
        return;
    }

    ZVAL_STRING(&function_name, "meminfo_dump");
    ZVAL_UNDEF(&result);
    status = call_user_function(EG(function_table), NULL, &function_name, &result, 1, &probe.stream);
    zval_ptr_dtor(&function_name);
    if (!Z_ISUNDEF(result)) {
        zval_ptr_dtor(&result);
    }
    probe.dumped = status == SUCCESS && EG(exception) == NULL;
    if (!probe_set_limit(limit)) {
        probe_log("limit-restore-failed", 0);
    }
    probe_log(probe.dumped ? "dump-ok" : "dump-failed", 0);
    probe.handling = 0;
}

static void probe_interrupt(zend_execute_data *execute_data)
{
    probe_safe_point("interrupt");
    if (previous_interrupt != NULL) {
        previous_interrupt(execute_data);
    }
}

static int probe_opcode(zend_execute_data *execute_data)
{
    unsigned int opcode = execute_data->opline->opcode;

    probe_safe_point("opcode");
    if (previous_opcodes[opcode] != NULL) {
        return previous_opcodes[opcode](execute_data);
    }
    return ZEND_USER_OPCODE_DISPATCH;
}

static int probe_owns_handlers(void)
{
    void *(*malloc_handler)(size_t) = NULL;
    void (*free_handler)(void *) = NULL;
    void *(*realloc_handler)(void *, size_t) = NULL;

    if (!probe.active || probe.heap != zend_mm_get_heap()) {
        return 0;
    }
    if (internal_mode) {
        return zend_execute_internal == probe_execute_internal;
    }
    zend_mm_get_custom_handlers(probe.heap, &malloc_handler, &free_handler, &realloc_handler);
    return malloc_handler == probe_malloc && free_handler == probe_free
        && realloc_handler == probe_realloc;
}

static void probe_disarm(void)
{
    if (!probe.active) {
        return;
    }
    /* A later extension must not have its handlers silently removed. This
     * experiment cannot support another extension retaining our callbacks. */
    if (!probe_owns_handlers()) {
        probe_log("disarm-ownership-lost", 0);
        return;
    }
    if (!internal_mode) {
        zend_mm_set_custom_handlers(probe.heap, NULL, NULL, NULL);
    }
    if (!internal_mode && zend_interrupt_function == probe_interrupt) {
        zend_interrupt_function = previous_interrupt;
    }
    probe.active = 0;
    probe_log("disarm", 0);
    zval_ptr_dtor(&probe.stream);
    ZVAL_UNDEF(&probe.stream);
}

PHP_FUNCTION(oom_probe_arm)
{
    zval *stream;
    zend_long threshold;
    zend_function *dump;
    const char *log_path;
    php_stream *stream_handle;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &stream, &threshold) == FAILURE) {
        return;
    }
    php_stream_from_zval(stream_handle, stream);
    (void) stream_handle;
    dump = zend_hash_str_find_ptr(EG(function_table), "meminfo_dump", sizeof("meminfo_dump") - 1);
    if (probe.active || threshold <= 0 || dump == NULL || dump->type != ZEND_INTERNAL_FUNCTION
        || zend_mm_is_custom_heap(zend_mm_get_heap())) {
        zend_throw_exception(NULL, "Prototype needs meminfo, a positive threshold and the default NTS allocator", 0);
        return;
    }
    if (probe.log_fd >= 0) {
        close(probe.log_fd);
    }
    memset(&probe, 0, sizeof(probe));
    probe.log_fd = -1;
    log_path = getenv("MEMINFO_ALLOC_PROBE_LOG");
    if (log_path != NULL) {
        probe.log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    probe.heap = zend_mm_get_heap();
    probe.threshold = (size_t) threshold;
    ZVAL_COPY(&probe.stream, stream);
    if (!internal_mode) {
        previous_interrupt = zend_interrupt_function;
        zend_interrupt_function = probe_interrupt;
    }
    probe.active = 1;
    if (!internal_mode) {
        zend_mm_set_custom_handlers(probe.heap, probe_malloc, probe_free, probe_realloc);
    }
    probe_log("arm", 0);
    RETURN_TRUE;
}

PHP_FUNCTION(oom_probe_disarm)
{
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }
    probe_disarm();
}

PHP_FUNCTION(oom_probe_stats)
{
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }
    array_init(return_value);
    add_assoc_bool(return_value, "active", probe.active);
    add_assoc_bool(return_value, "pending", probe.pending);
    add_assoc_bool(return_value, "dumped", probe.dumped);
    add_assoc_bool(return_value, "custom_heap", zend_mm_is_custom_heap(zend_mm_get_heap()));
    add_assoc_bool(return_value, "owns_handlers", probe_owns_handlers());
    add_assoc_long(return_value, "allocations", (zend_long) probe.allocations);
    add_assoc_long(return_value, "reallocations", (zend_long) probe.reallocations);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_probe_arm, 0, 0, 2)
    ZEND_ARG_INFO(0, stream)
    ZEND_ARG_INFO(0, threshold)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_probe_none, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry probe_functions[] = {
    PHP_FE(oom_probe_arm, arginfo_probe_arm)
    PHP_FE(oom_probe_disarm, arginfo_probe_none)
    PHP_FE(oom_probe_stats, arginfo_probe_none)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(oom_probe)
{
    const char *mode = getenv("MEMINFO_ALLOC_PROBE_MODE");
    int opcode;

    probe.log_fd = -1;
    ZVAL_UNDEF(&probe.stream);
    opcode_mode = mode != NULL && strcmp(mode, "opcode") == 0;
    internal_mode = mode != NULL && (strcmp(mode, "internal") == 0 || strcmp(mode, "internal-notify") == 0);
    notify_only = mode != NULL && (strcmp(mode, "notify") == 0 || strcmp(mode, "internal-notify") == 0);
    if (internal_mode) {
        previous_execute_internal = zend_execute_internal;
        zend_execute_internal = probe_execute_internal;
        /* JIT can embed the current hook in generated stubs. Match meminfo's
         * module-lifetime interrupt chain rather than installing at arm time. */
        previous_interrupt = zend_interrupt_function;
        zend_interrupt_function = probe_interrupt;
    }
    if (opcode_mode) {
        for (opcode = 0; opcode < 256; opcode++) {
            previous_opcodes[opcode] = zend_get_user_opcode_handler((zend_uchar) opcode);
            zend_set_user_opcode_handler((zend_uchar) opcode, probe_opcode);
        }
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(oom_probe)
{
    probe_log("rshutdown-enter", 0);
    probe_disarm();
    probe_log("rshutdown-finished", 0);
    if (probe.log_fd >= 0) {
        close(probe.log_fd);
        probe.log_fd = -1;
    }
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(oom_probe)
{
    int opcode;

    if (internal_mode && zend_execute_internal == probe_execute_internal) {
        zend_execute_internal = previous_execute_internal;
    }
    if (internal_mode && zend_interrupt_function == probe_interrupt) {
        zend_interrupt_function = previous_interrupt;
    }
    if (opcode_mode) {
        for (opcode = 0; opcode < 256; opcode++) {
            if (zend_get_user_opcode_handler((zend_uchar) opcode) == probe_opcode) {
                zend_set_user_opcode_handler((zend_uchar) opcode, previous_opcodes[opcode]);
            }
        }
    }
    return SUCCESS;
}

zend_module_entry oom_probe_module_entry = {
    STANDARD_MODULE_HEADER,
    "oom_probe", probe_functions,
    PHP_MINIT(oom_probe), PHP_MSHUTDOWN(oom_probe), NULL,
    PHP_RSHUTDOWN(oom_probe), NULL,
    "0.0.1", STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(oom_probe)
