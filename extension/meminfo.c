/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Module entry of the meminfo extension.
 *
 * meminfo dumps everything a PHP process holds in memory — every object, array
 * and value reachable from the running code — as JSON, so that a memory leak
 * can be tracked down by looking at what is actually kept alive. This file
 * wires the extension into PHP: arginfo, ini settings, the MINIT…MINFO
 * lifecycle and meminfo_dump(); the work itself lives in the other modules.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#include "ext/standard/info.h"
#include "php_ini.h"
#include "php_open_temporary_file.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
#if PHP_VERSION_ID >= 80100
# include "zend_observer.h"
#endif

#if PHP_VERSION_ID >= 80000
ZEND_BEGIN_ARG_INFO_EX(arginfo_meminfo_dump, 0, 0, 1)
    ZEND_ARG_INFO(0, output_stream)
ZEND_END_ARG_INFO()

// Always a string: every failure path throws instead of returning a value.
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_meminfo_dump_process, 0, 1, IS_STRING, 0)
    ZEND_ARG_INFO(0, pid)
    ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

const zend_function_entry meminfo_functions[] = {
    PHP_FE(meminfo_dump, arginfo_meminfo_dump)
#ifndef PHP_WIN32
    PHP_FE(meminfo_dump_process, arginfo_meminfo_dump_process)
#endif
    PHP_FE_END
};
#else
const zend_function_entry meminfo_functions[] = {
    PHP_FE(meminfo_dump, NULL)
#ifndef PHP_WIN32
    PHP_FE(meminfo_dump_process, NULL)
#endif
    PHP_FE_END
};
#endif

/* ------------------------------------------------------------------------- *
 * Module globals, configuration and lifecycle
 * ------------------------------------------------------------------------- */

ZEND_DECLARE_MODULE_GLOBALS(meminfo)

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("meminfo.dump_signal", MEMINFO_DEFAULT_DUMP_SIGNAL, PHP_INI_SYSTEM, OnUpdateString,
        dump_signal_setting, zend_meminfo_globals, meminfo_globals)
    STD_PHP_INI_ENTRY("meminfo.dump_dir", "", PHP_INI_SYSTEM, OnUpdateString,
        dump_dir, zend_meminfo_globals, meminfo_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(meminfo)
{
#if defined(COMPILE_DL_MEMINFO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    memset(meminfo_globals, 0, sizeof(*meminfo_globals));
}

static PHP_GSHUTDOWN_FUNCTION(meminfo)
{
#if PHP_VERSION_ID >= 80100
    free(meminfo_globals->suspended.slots);
    meminfo_globals->suspended.slots = NULL;
    meminfo_globals->suspended.capacity = 0;
    meminfo_globals->suspended.count = 0;
#else
    (void) meminfo_globals;
#endif
}

static PHP_MINIT_FUNCTION(meminfo)
{
    REGISTER_INI_ENTRIES();

#if PHP_VERSION_ID >= 80100
    zend_observer_fiber_switch_register(meminfo_fiber_switch);
    zend_observer_fiber_destroy_register(meminfo_fiber_destroy);
#endif

#ifndef PHP_WIN32
    {
        int signal_number = 0;

        if (!meminfo_parse_signal(MEMINFO_G(dump_signal_setting), &signal_number)) {
            // Startup is the only place this can be reported: it happens before
            // any request, so it cannot corrupt a response body.
            php_error_docref(NULL, E_WARNING,
                "meminfo.dump_signal: unknown signal \"%s\"; signal dumps are disabled",
                MEMINFO_G(dump_signal_setting) ? MEMINFO_G(dump_signal_setting) : "");
            signal_number = 0;
        }

        MEMINFO_G(dump_signal) = signal_number;

# if !MEMINFO_HAVE_SIGNAL_DUMP
        // Only complain when an operator asked for a signal explicitly. The
        // setting has a default, so warning whenever it is non-zero would mean
        // warning on every single startup of an unsupported build.
        if (signal_number != 0
            && MEMINFO_G(dump_signal_setting) != NULL
            && strcmp(MEMINFO_G(dump_signal_setting), MEMINFO_DEFAULT_DUMP_SIGNAL) != 0) {

            php_error_docref(NULL, E_WARNING,
                "meminfo.dump_signal is set but this build cannot dump on a signal "
                "(needs PHP 7.1 or later, non-ZTS, POSIX); meminfo_dump_process() "
                "can still be used from here to dump other processes");
        }

        MEMINFO_G(dump_signal) = 0;
# endif
    }
#else
    MEMINFO_G(dump_signal) = 0;
#endif

#if MEMINFO_HAVE_SIGNAL_DUMP
    meminfo_signal_module_startup();
#endif

    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(meminfo)
{
#if MEMINFO_HAVE_SIGNAL_DUMP
    meminfo_signal_module_shutdown();
#endif

    UNREGISTER_INI_ENTRIES();

    return SUCCESS;
}

static PHP_RINIT_FUNCTION(meminfo)
{
#if defined(COMPILE_DL_MEMINFO) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

#if MEMINFO_HAVE_SIGNAL_DUMP
    // Installed per request on purpose. Outside a request there is no VM to
    // interrupt, and the trigger uses the "signal caught" bit in
    // /proc/<pid>/status to decide whether asking is worth anything.
    MEMINFO_G(dump_requested) = 0;
    MEMINFO_G(dump_id) = 0;
    MEMINFO_G(dumping) = 0;

    meminfo_install_signal_handler();
#endif

    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(meminfo)
{
#if MEMINFO_HAVE_SIGNAL_DUMP
    meminfo_restore_signal_handler();
#endif

#if PHP_VERSION_ID >= 80100
    // Every context of the request is gone with it. The slots are kept for the
    // next request and freed with the globals.
    if (MEMINFO_G(suspended).count > 0) {
        memset(MEMINFO_G(suspended).slots, 0, MEMINFO_G(suspended).capacity * sizeof(meminfo_suspended_context));
        MEMINFO_G(suspended).count = 0;
    }
#endif

    return SUCCESS;
}

static PHP_MINFO_FUNCTION(meminfo)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Version", MEMINFO_VERSION);
    php_info_print_table_row(2, "Copyright", MEMINFO_COPYRIGHT);
    php_info_print_table_row(2, "Signal dumps supported",
        MEMINFO_HAVE_SIGNAL_DUMP ? "yes" : "no (needs PHP 7.1+, non-ZTS, POSIX)");

    if (MEMINFO_G(dump_signal) != 0) {
        char signal_text[32];

        snprintf(signal_text, sizeof(signal_text), "%d (%s)",
            MEMINFO_G(dump_signal),
            MEMINFO_G(dump_signal_setting) ? MEMINFO_G(dump_signal_setting) : "");
        php_info_print_table_row(2, "Dump signal", signal_text);
    } else {
        php_info_print_table_row(2, "Dump signal", "disabled");
    }

    php_info_print_table_row(2, "Dump directory", meminfo_dump_directory());
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

zend_module_entry meminfo_module_entry = {
    STANDARD_MODULE_HEADER,
    "meminfo",
    meminfo_functions,
    PHP_MINIT(meminfo),
    PHP_MSHUTDOWN(meminfo),
    PHP_RINIT(meminfo),
    PHP_RSHUTDOWN(meminfo),
    PHP_MINFO(meminfo),
    MEMINFO_VERSION,
    PHP_MODULE_GLOBALS(meminfo),
    PHP_GINIT(meminfo),
    PHP_GSHUTDOWN(meminfo),
    NULL,                           /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};

/**
 * Generate a JSON output of the list of items in memory (objects, arrays, string, etc...)
 * with their sizes and other information
 */
/**
 * Pick how meminfo_dump() writes to the caller's stream.
 *
 * Nothing may run PHP code, or switch to another coroutine, while the heap is
 * being walked: the traversal holds raw pointers into hash tables that such
 * code could free or resize. A php_stream write can do either — a userspace
 * stream wrapper runs its stream_write() method, and Swoole's file and socket
 * hooks yield to other coroutines (seen in 54 of 90 dumps under
 * SWOOLE_HOOK_ALL, the likely cause of GitHub issue #117). So:
 *
 *  - php://memory and php://temp are written through the stream, as they
 *    neither run code nor yield (and are what tests dump into);
 *  - a stream over a regular file, with no write filter, is written with
 *    write(2) on its descriptor — Swoole's hooked file streams included, which
 *    are not PHP_STREAM_IS_STDIO but still hand out their descriptor;
 *  - anything else (sockets, pipes, filtered or compressed streams, userspace
 *    wrappers, whose very cast would run PHP code) goes to an anonymous
 *    temporary file first, copied into the stream once the walk is over. The
 *    dump is never held in memory.
 *
 * Without a temporary file the stream is written directly, as before.
 */
static void meminfo_dump_target_open(meminfo_writer *w, php_stream *stream, FILE **spool)
{
#ifndef PHP_WIN32
    int fd = -1;
    struct stat info;

    if (php_stream_is(stream, PHP_STREAM_IS_MEMORY) || php_stream_is(stream, PHP_STREAM_IS_TEMP)) {
        w->stream = stream;
        return;
    }

    // The stream's own cast handler, not php_stream_cast(): that one also
    // seeks through the stream's ops to drop its read buffer, and under
    // Swoole's file hook such a seek is a coroutine operation that yields.
    if (!php_stream_is(stream, PHP_STREAM_IS_USERSPACE)
        && stream->writefilters.head == NULL
        && stream->ops->cast != NULL
        && stream->ops->cast(stream, PHP_STREAM_AS_FD, (void **) &fd) == SUCCESS
        && fd >= 0
        && fstat(fd, &info) == 0
        && S_ISREG(info.st_mode)) {
        // What php_stream_cast() does before handing out a descriptor: write
        // out what the stream buffers, and put the descriptor where the stream
        // thinks it is (a read buffer leaves it further on), with plain lseek.
        php_stream_flush(stream);
        if (lseek(fd, (off_t) stream->position, SEEK_SET) >= 0) {
            stream->readpos = 0;
            stream->writepos = 0;
            w->fd = fd;
            return;
        }
    }

    *spool = tmpfile();
    if (*spool != NULL) {
        w->fd = fileno(*spool);
        return;
    }
#else
    (void) spool;
#endif

    w->stream = stream;
}

/**
 * Finish what meminfo_dump_target_open() started, once the walk is over.
 */
static void meminfo_dump_target_close(meminfo_writer *w, php_stream *stream, FILE *spool)
{
#ifndef PHP_WIN32
    if (spool != NULL) {
        int fd = fileno(spool);

        // The staging buffer is free again: reuse it for the copy.
        if (!w->err && lseek(fd, 0, SEEK_SET) == 0) {
            ssize_t got;

            while ((got = read(fd, w->buf, sizeof(w->buf))) != 0) {
                if (got < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    w->err = errno ? errno : EIO;
                    break;
                }
                errno = 0;
                if ((ssize_t) php_stream_write(stream, w->buf, (size_t) got) != got) {
                    w->err = errno ? errno : EIO;
                    break;
                }
            }
        } else if (!w->err) {
            w->err = errno ? errno : EIO;
        }

        fclose(spool);
        return;
    }

    if (w->fd >= 0) {
        // Written behind the stream's back: move its idea of the position to
        // where the descriptor is now, so that ftell() and further writes
        // agree. Pipes and the like have no position to keep.
        off_t position = lseek(w->fd, 0, SEEK_CUR);

        if (position >= 0) {
            stream->position = (zend_off_t) position;
        }
    }
#else
    (void) w;
    (void) stream;
    (void) spool;
#endif
}

PHP_FUNCTION(meminfo_dump)
{
    zval *zval_stream;

    php_stream *stream;
    meminfo_writer *writer;
    zend_execute_data *caller_frame;
    FILE *spool = NULL;
    int status;
    int write_errno;

    // No throw here: PHP 8 has already thrown a TypeError or ArgumentCountError
    // of its own by this point, and 7.x has emitted a warning.
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zval_stream) == FAILURE) {
        return;
    }

    php_stream_from_zval(stream, zval_stream);

    // malloc, not emalloc: the staging buffer must not count towards the
    // memory_limit of the process being dumped.
    writer = malloc(sizeof(*writer));
    if (writer == NULL) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump(): cannot allocate the dump buffer");
        return;
    }

    writer->stream = NULL;
    writer->fd = -1;
    writer->len = 0;
    writer->err = 0;

    meminfo_dump_target_open(writer, stream, &spool);

    // The frame of the meminfo_dump() call itself holds no user variables; the
    // dump starts at its caller.
    caller_frame = EG(current_execute_data) ? EG(current_execute_data)->prev_execute_data : NULL;

    // The global scope is not always on the caller's chain: a coroutine of
    // Swoole starts a frame chain of its own. When it is, its variables are
    // already recorded by then and skipped by address.
    //
    // A fatal error mid-walk (memory_limit in an object's handler) bails out
    // of the walk, which frees its own state; the writer and the spool file
    // are this function's to release before the bailout goes on.
    if (meminfo_dump_to_writer_guarded(writer, caller_frame, 1, &status)) {
        free(writer);
        if (spool != NULL) {
            fclose(spool);
        }
        zend_bailout();
    }

    meminfo_dump_target_close(writer, stream, spool);

    write_errno = writer->err;
    free(writer);

    // Reported rather than swallowed: a caller that is told nothing will go on
    // to analyse a cut-short dump as if it were complete. Unlike the signal
    // path, which unlinks its temporary file, there is nothing to roll back
    // here — the bytes are already in the caller's stream.
    if (write_errno) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump(): writing the dump failed (%s); the stream holds a partial dump",
            strerror(write_errno));
    } else if (status == MEMINFO_DUMP_INCOMPLETE) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump(): the dump is incomplete: out of memory for its bookkeeping; "
            "the stream holds valid JSON with items missing");
    }
}

#ifdef COMPILE_DL_MEMINFO
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(meminfo)
#endif
