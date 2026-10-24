/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Signal-triggered dumps: dump_signal parsing, the handler, the interrupt hook, dump to file.
 *
 * The target side of dumping a process that is already running: it takes a
 * dump on request without any change to the application's code. The signal
 * handler only records the request; the dump is written at the next safe point
 * between opcodes, to a file of its own, never into the process's output.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#if PHP_VERSION_ID >= 80000
#include "zend_observer.h"
#endif
#include "php_open_temporary_file.h"
#include "php_globals.h"

#ifndef PHP_WIN32
/**
 * Signal names accepted by meminfo.dump_signal, without the "SIG" prefix.
 *
 * Deliberately not a whitelist of *sensible* signals: an operator may have a
 * reason to pick an unusual one, and a signal that cannot be caught is
 * reported when sigaction() refuses it rather than being rejected here.
 */
typedef struct {
    const char *name;
    int number;
} meminfo_signal_name;

static const meminfo_signal_name meminfo_signal_names[] = {
#ifdef SIGURG
    { "URG", SIGURG },
#endif
#ifdef SIGUSR1
    { "USR1", SIGUSR1 },
#endif
#ifdef SIGUSR2
    { "USR2", SIGUSR2 },
#endif
#ifdef SIGHUP
    { "HUP", SIGHUP },
#endif
#ifdef SIGINT
    { "INT", SIGINT },
#endif
#ifdef SIGQUIT
    { "QUIT", SIGQUIT },
#endif
#ifdef SIGTERM
    { "TERM", SIGTERM },
#endif
#ifdef SIGWINCH
    { "WINCH", SIGWINCH },
#endif
#ifdef SIGIO
    { "IO", SIGIO },
#endif
#ifdef SIGPROF
    { "PROF", SIGPROF },
#endif
#ifdef SIGVTALRM
    { "VTALRM", SIGVTALRM },
#endif
#ifdef SIGALRM
    { "ALRM", SIGALRM },
#endif
#ifdef SIGCONT
    { "CONT", SIGCONT },
#endif
#ifdef SIGTRAP
    { "TRAP", SIGTRAP },
#endif
#ifdef SIGPWR
    { "PWR", SIGPWR },
#endif
#ifdef SIGKILL
    { "KILL", SIGKILL },
#endif
#ifdef SIGSTOP
    { "STOP", SIGSTOP },
#endif
    { NULL, 0 }
};

/**
 * Resolve a meminfo.dump_signal value to a signal number.
 *
 * Accepts a decimal number, or a name with or without the "SIG" prefix, in any
 * case. An empty value or "0" means "do not install a handler".
 *
 * Returns 0 when the value cannot be resolved, in which case *out is untouched.
 */
MEMINFO_HIDDEN int meminfo_parse_signal(const char *value, int *out)
{
    const meminfo_signal_name *entry;
    const char *name;
    size_t i;

    if (value == NULL) {
        *out = 0;
        return 1;
    }

    while (*value == ' ' || *value == '\t') {
        value++;
    }

    if (*value == '\0') {
        *out = 0;
        return 1;
    }

    if (*value >= '0' && *value <= '9') {
        int number = 0;

        for (i = 0; value[i] != '\0'; i++) {
            if (value[i] < '0' || value[i] > '9') {
                return 0;
            }
            number = number * 10 + (value[i] - '0');
            if (number > 255) {
                return 0;
            }
        }

        *out = number;
        return 1;
    }

    name = value;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    for (entry = meminfo_signal_names; entry->name != NULL; entry++) {
        if (strcasecmp(name, entry->name) == 0) {
            *out = entry->number;
            return 1;
        }
    }

    return 0;
}
#endif /* !PHP_WIN32 */

/**
 * Directory dump files are written to: the configured one, or the system
 * temporary directory when it is left empty.
 */
MEMINFO_HIDDEN const char *meminfo_dump_directory(void)
{
    const char *dir = MEMINFO_G(dump_dir);

    if (dir != NULL && dir[0] != '\0') {
        return dir;
    }

    return php_get_temporary_directory();
}

#ifndef PHP_WIN32
/**
 * Name of the request file for one dump: bound to the request id when the
 * signal carries one, otherwise one per target process.
 */
MEMINFO_HIDDEN int meminfo_request_file_name(char *buffer, size_t size, const char *base, int pid, int dump_id)
{
    int written = dump_id != 0
        ? snprintf(buffer, size, "%s/meminfo-request-%d-%d", base, pid, dump_id)
        : snprintf(buffer, size, "%s/meminfo-request-%d", base, pid);

    return written > 0 && (size_t) written < size;
}
#endif

#if MEMINFO_HAVE_SIGNAL_DUMP

/* The interrupt hook that was in place before this extension chained itself in. */
static void (*meminfo_previous_interrupt_function)(zend_execute_data *execute_data) = NULL;

/**
 * Signal handler.
 *
 * Does the bare minimum that is async-signal-safe: records the request and
 * raises the VM's interrupt flag. The engine clears that flag and calls
 * zend_interrupt_function at the next safe point between opcodes, which is
 * where the dump actually happens — walking the heap from inside a signal
 * handler would be unsafe in every possible way.
 */
static void meminfo_signal_handler(int signal_number, siginfo_t *info, void *context)
{
    (void) signal_number;
    (void) context;

    // sigqueue() carries the id the trigger is waiting for; a plain kill()
    // carries nothing, and the file is then named after the pid and the time.
    MEMINFO_G(dump_id) = (info != NULL && info->si_code == SI_QUEUE)
        ? (sig_atomic_t) info->si_value.sival_int
        : 0;

    MEMINFO_G(dump_requested) = 1;

#if PHP_VERSION_ID >= 80200
    zend_atomic_bool_store_ex(&EG(vm_interrupt), true);
#else
    EG(vm_interrupt) = 1;
#endif
}

/**
 * Build the name of the dump file for the pending request.
 *
 * A dump asked for with an id is named after it, so the trigger knows the exact
 * path to wait for and a resend of the same id is recognised as already served.
 */
static int meminfo_dump_file_name(char *buffer, size_t size, const char *dir, int dump_id)
{
    int written;

    if (dump_id != 0) {
        written = snprintf(buffer, size, "%s/" MEMINFO_DUMP_FILE_PREFIX "%d.json", dir, dump_id);
    } else {
        struct timeval now;

        gettimeofday(&now, NULL);
        written = snprintf(buffer, size, "%s/" MEMINFO_DUMP_FILE_PREFIX "%d-%ld-%ld.json",
            dir, (int) getpid(), (long) now.tv_sec, (long) now.tv_usec);
    }

    return written > 0 && (size_t) written < size;
}

/**
 * The directory a trigger asked this dump to be written to.
 *
 * Returns 1 and fills dir when a request names one; 0 when there is nothing to
 * honour — no request, or one not created by a user allowed to signal this
 * process (itself or root), which is ignored as if it were not there; -1 when a
 * genuine request cannot be honoured, with the reason in message.
 */
static int meminfo_read_dump_request(const char *request_path, char *dir, size_t dir_size, char *message, size_t message_size)
{
    int fd = open(request_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat info;
    ssize_t got;
    size_t len;

    if (fd < 0) {
        return 0;
    }

    if (fstat(fd, &info) != 0
        || !S_ISREG(info.st_mode)
        || info.st_nlink != 1
        || (info.st_uid != getuid() && info.st_uid != geteuid() && info.st_uid != 0)) {
        close(fd);
        return 0;
    }

    got = read(fd, dir, dir_size - 1);
    close(fd);

    if (got <= 0 || (size_t) got >= dir_size - 1) {
        snprintf(message, message_size, "meminfo: dump request %s is empty, unreadable or too long", request_path);
        return -1;
    }

    len = (size_t) got;
    dir[len] = '\0';

    while (len > 0 && dir[len - 1] == '\n') {
        dir[--len] = '\0';
    }

    if (len == 0 || dir[0] != '/' || strlen(dir) != len) {
        snprintf(message, message_size, "meminfo: dump request %s does not name an absolute directory", request_path);
        return -1;
    }

    if (stat(dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
        snprintf(message, message_size, "meminfo: requested dump directory %s is not a directory", dir);
        return -1;
    }

    return 1;
}

/**
 * Record why a dump could not be written: in the error log, and — when a
 * trigger is waiting on a request — next to that request, so the trigger can
 * report the reason at once instead of waiting out its timeout.
 */
static void meminfo_report_dump_failure(const char *request_path, const char *message)
{
    char error_path[MAXPATHLEN + 8];
    int fd;

    php_log_err((char *) message);

    if (request_path == NULL
        || snprintf(error_path, sizeof(error_path), "%s.error", request_path) >= (int) sizeof(error_path)) {
        return;
    }

    fd = open(error_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t written = write(fd, message, strlen(message));

        (void) written;
        close(fd);
    }
}

/**
 * zend_set_memory_limit(), reporting whether the limit was applied: PHP 8.0's
 * returns nothing (it never refuses there), 7.x and 8.1+ refuse a limit below
 * what is in use.
 */
static int meminfo_set_memory_limit(size_t limit)
{
#if PHP_VERSION_ID >= 80000 && PHP_VERSION_ID < 80100
    zend_set_memory_limit(limit);
    return 1;
#else
    return zend_set_memory_limit(limit) == SUCCESS;
#endif
}

/**
 * Hand the written dump over by renaming it into place — or, when writing it
 * failed or it is incomplete, report that and remove it. Returns 1 when the
 * dump is in place.
 */
static int meminfo_dump_file_finish(const char *request, const char *temp_path, const char *final_path, int write_errno, int status)
{
    char message[MAXPATHLEN + 192];
    int saved_errno;

    if (write_errno != 0) {
        snprintf(message, sizeof(message), "meminfo: dump to %s failed: %s",
            temp_path, strerror(write_errno));
        meminfo_report_dump_failure(request, message);
        unlink(temp_path);
        return 0;
    }

    if (status == MEMINFO_DUMP_INCOMPLETE) {
        // Valid JSON, but with items missing: not handed over as a dump.
        snprintf(message, sizeof(message),
            "meminfo: dump to %s is incomplete: out of memory for its bookkeeping", temp_path);
        meminfo_report_dump_failure(request, message);
        unlink(temp_path);
        return 0;
    }

    if (rename(temp_path, final_path) != 0) {
        saved_errno = errno;
        snprintf(message, sizeof(message), "meminfo: cannot rename %s: %s",
            temp_path, strerror(saved_errno));
        meminfo_report_dump_failure(request, message);
        unlink(temp_path);
        return 0;
    }

    return 1;
}

/**
 * Write the dump to a file, at a safe point in the VM.
 *
 * Nothing here may reach the output of the process being dumped: a stream
 * notice or a user error handler firing in the middle of someone's response is
 * exactly the kind of interference this feature must not cause. Failures go to
 * the error log only. On success, the path of the dump is copied to out_path
 * when one is given.
 */
static void meminfo_dump_to_file(zend_execute_data *execute_data, int dump_id, char *out_path, size_t out_size)
{
    char final_path[MAXPATHLEN];
    char temp_path[MAXPATHLEN + 8];
    char request_path[MAXPATHLEN];
    char request_dir[MAXPATHLEN];
    char message[MAXPATHLEN + 192];
    const char *request = NULL;
    const char *dir = meminfo_dump_directory();
    meminfo_writer *writer;
    int fd;
    int status;
    int write_errno;
    int saved_errno;
    int limit_lifted = 0;
#ifdef HAVE_SETITIMER
    struct itimerval no_timer;
    struct itimerval saved_timer;
    int timer_paused = 0;
#endif

    if (dir == NULL) {
        dir = "/tmp";
    }

    // A trigger that chose the directory left it in a request file. It is read
    // on every signal rather than consumed, so a resend lands in the same place;
    // the trigger removes it once it has its dump.
    if (meminfo_request_file_name(request_path, sizeof(request_path), MEMINFO_REQUEST_DIR, (int) getpid(), dump_id)) {
        switch (meminfo_read_dump_request(request_path, request_dir, sizeof(request_dir), message, sizeof(message))) {
            case 1:
                dir = request_dir;
                request = request_path;
                break;
            case -1:
                // Not written anywhere else: the trigger only looks where it
                // asked, so a dump in the default directory would be lost.
                meminfo_report_dump_failure(request_path, message);
                return;
            default:
                break;
        }
    }

    if (!meminfo_dump_file_name(final_path, sizeof(final_path), dir, dump_id)) {
        meminfo_report_dump_failure(request, "meminfo: dump directory path is too long");
        return;
    }

    // An already finished dump for this id means the trigger resent the signal
    // while waiting; serving it twice would only waste the target's time.
    if (access(final_path, F_OK) == 0) {
        return;
    }

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", final_path) < 0) {
        return;
    }

#ifdef HAVE_SETITIMER
    // A dump of a large heap can take seconds of CPU. max_execution_time is
    // measured on ITIMER_PROF, and exceeding it hard-kills the process with
    // _exit(124) — the dump would take the process down with it.
    memset(&no_timer, 0, sizeof(no_timer));
    if (setitimer(ITIMER_PROF, &no_timer, &saved_timer) == 0) {
        timer_paused = 1;
    }
#endif

    // O_EXCL so two triggers cannot write the same file; O_NOFOLLOW so a
    // planted symlink cannot redirect the dump; 0600 because a dump exposes
    // array keys, class and property names and heap addresses.
    fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (fd < 0) {
        saved_errno = errno;

        // EEXIST is reported too. A repeated signal for a dump that already
        // finished was handled by the check above, and a second dump cannot be
        // started while one is running, so something is left over at this path
        // — a partial dump from a process that died, or a planted entry.
        // O_NOFOLLOW means a symlink lands here as well, rather than being
        // followed somewhere it should not go.
        snprintf(message, sizeof(message),
            "meminfo: cannot create %s: %s%s",
            temp_path, strerror(saved_errno),
            saved_errno == EEXIST ? " (left over from an earlier dump, or not created by meminfo)" : "");
        meminfo_report_dump_failure(request, message);
    } else {
        writer = malloc(sizeof(*writer));

        if (writer == NULL) {
            close(fd);
            unlink(temp_path);
            meminfo_report_dump_failure(request, "meminfo: cannot allocate the dump buffer");
        } else {
            writer->stream = NULL;
            writer->fd = fd;
            writer->len = 0;
            writer->err = 0;

            // The process being inspected is often near memory_limit, which is
            // why it is being dumped. get_properties() and get_gc() handlers of
            // internal classes may allocate; hitting the limit there would turn
            // a dump request into a fatal error of the request being served.
            if (PG(memory_limit) > 0 && meminfo_set_memory_limit((size_t) ZEND_LONG_MAX)) {
                limit_lifted = 1;
            }

            // During shutdown the chain no longer reaches the main script's
            // frame, so the global scope is walked explicitly.
            //
            // Should a fatal error still bail out of the walk, which frees its
            // own state, the file, the timer, the limit and the dumping flag
            // are put back here before the bailout goes on.
            MEMINFO_G(dump_flags) = meminfo_dump_flags(NULL);
            if (MEMINFO_G(auto_mark)) {
                meminfo_mark();
            }
            if (meminfo_dump_to_writer_guarded(writer, execute_data, 1, &status)) {
                free(writer);
                close(fd);
                unlink(temp_path);
                meminfo_report_dump_failure(request, "meminfo: the dump was cut short by a fatal error");
#ifdef HAVE_SETITIMER
                if (timer_paused) {
                    setitimer(ITIMER_PROF, &saved_timer, NULL);
                }
#endif
                if (limit_lifted) {
                    meminfo_set_memory_limit((size_t) PG(memory_limit));
                }
                MEMINFO_G(dumping) = 0;
                zend_bailout();
            }

            write_errno = writer->err;
            free(writer);

            if (close(fd) != 0 && write_errno == 0) {
                write_errno = errno ? errno : EIO;
            }

            if (meminfo_dump_file_finish(request, temp_path, final_path, write_errno, status) && out_path != NULL) {
                snprintf(out_path, out_size, "%s", final_path);
            }
        }
    }

#ifdef HAVE_SETITIMER
    if (timer_paused) {
        setitimer(ITIMER_PROF, &saved_timer, NULL);
    }
#endif

    // Hand the pages the dump touched back, so the process does not keep its
    // resident size at the dump's peak for the rest of its life.
    zend_mm_gc(zend_mm_get_heap());

    // After the gc: the limit cannot be put back below what is in use.
    if (limit_lifted && !meminfo_set_memory_limit((size_t) PG(memory_limit))) {
        php_log_err((char *) "meminfo: cannot restore memory_limit after the dump; it stays lifted");
    }
}

/**
 * Serve a pending dump request, if there is one, at a point where the engine
 * is consistent. Returns 1 when a request was served; out_path receives the
 * dump's path when it was written.
 */
static int meminfo_serve_dump_request(zend_execute_data *execute_data, char *out_path, size_t out_size)
{
    if (MEMINFO_G(dump_requested) && !MEMINFO_G(dumping)) {
        int dump_id;

        // Cleared before anything else: a signal that arrives from here on
        // leaves the flag set and produces one more dump afterwards, rather
        // than being swallowed. Reading the id afterwards means a signal in
        // that window is served by this dump instead of being lost.
        MEMINFO_G(dump_requested) = 0;
        dump_id = (int) MEMINFO_G(dump_id);

        MEMINFO_G(dumping) = 1;

        // A request the code serves itself was waited for until a quiet point;
        // one served by the interrupt hook lands wherever the code was.
        if (MEMINFO_G(request_from_threshold)) {
            MEMINFO_G(dump_trigger) = out_path != NULL ? "threshold (deferred)" : "threshold";
        } else {
            MEMINFO_G(dump_trigger) = out_path != NULL ? "deferred" : "signal";
        }
        MEMINFO_G(request_from_threshold) = 0;

        meminfo_dump_to_file(execute_data, dump_id, out_path, out_size);

        // A request without an id that arrived during a dump without one is
        // most likely a resend of the request just served, and serving it
        // would write a second full dump nobody waits for. A trigger that sent
        // it after this dump started rejects this file by its timestamp and
        // asks again, so nothing is lost by dropping it. A request carrying an
        // id names its own file and is always kept.
        if (dump_id == 0 && MEMINFO_G(dump_requested) && MEMINFO_G(dump_id) == 0) {
            MEMINFO_G(dump_requested) = 0;
        }

        MEMINFO_G(dumping) = 0;
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- *
 * Dumps on memory pressure (meminfo.dump_on_limit, meminfo.dump_threshold)
 * ------------------------------------------------------------------------- */

/**
 * Dump on "Allowed memory size … exhausted", from the error notification,
 * before the fatal error ends the request: the frames are still there, which a
 * dump from a shutdown function after the bailout would no longer see. The
 * allocator is past its limit at that point, which is safe for the walk — it
 * allocates with malloc, and the limit is lifted for the dump anyway, as on a
 * signal. Once per request; reported in the error log.
 */
static void meminfo_dump_on_limit(const char *message, size_t length)
{
    static const char prefix[] = "Allowed memory size of";
    char path[MAXPATHLEN];
    char log_message[MAXPATHLEN + 64];

    if (!MEMINFO_G(dump_on_limit) || MEMINFO_G(limit_dumped) || MEMINFO_G(dumping)
        || length < sizeof(prefix) - 1 || memcmp(message, prefix, sizeof(prefix) - 1) != 0) {
        return;
    }

    MEMINFO_G(limit_dumped) = 1;
    MEMINFO_G(dumping) = 1;
    MEMINFO_G(dump_trigger) = "memory_limit";
    path[0] = '\0';
    meminfo_dump_to_file(EG(current_execute_data), 0, path, sizeof(path));
    MEMINFO_G(dumping) = 0;

    // meminfo.dump_defer cannot apply: the fatal error ends the request right
    // after this, so there is no quiet point left to wait for. The header says
    // "memory_limit" and the log says it, so a comparison treats this dump as
    // one taken in the middle of the work.
    if (path[0] != '\0') {
        snprintf(log_message, sizeof(log_message), "meminfo: memory_limit reached, dump written to %s%s", path,
            MEMINFO_G(dump_defer) ? " (in the middle of the work: meminfo.dump_defer cannot wait for a fatal error)" : "");
        php_log_err(log_message);
    }
}

#if PHP_VERSION_ID >= 80100
static void meminfo_error_observer(int type, zend_string *file, uint32_t line, zend_string *message)
{
    (void) file;
    (void) line;
    if (type & E_ERROR) {
        meminfo_dump_on_limit(ZSTR_VAL(message), ZSTR_LEN(message));
    }
}
#elif PHP_VERSION_ID >= 80000
static void meminfo_error_observer(int type, const char *file, uint32_t line, zend_string *message)
{
    (void) file;
    (void) line;
    if (type & E_ERROR) {
        meminfo_dump_on_limit(ZSTR_VAL(message), ZSTR_LEN(message));
    }
}
#else
static void (*meminfo_previous_error_cb)(int type, const char *file, const uint32_t line, const char *format, va_list args);

// Before 8.0 there are no error observers: zend_error_cb is wrapped. The
// format string of the limit error starts with the same words as its message.
static void meminfo_error_cb(int type, const char *file, const uint32_t line, const char *format, va_list args)
{
    if (type & E_ERROR) {
        meminfo_dump_on_limit(format, strlen(format));
    }
    meminfo_previous_error_cb(type, file, line, format, args);
}
#endif

#if PHP_VERSION_ID >= 80000
/**
 * The threshold in bytes for this request: "512M", "1G", "64K", a byte count,
 * or a percentage of memory_limit ("80%"). 0 when unset or meaningless.
 */
static size_t meminfo_threshold_bytes(void)
{
    const char *setting = MEMINFO_G(dump_threshold);
    size_t length;

    if (setting == NULL || setting[0] == '\0') {
        return 0;
    }

    length = strlen(setting);
    if (setting[length - 1] == '%') {
        zend_long percent = ZEND_STRTOL(setting, NULL, 10);

        return PG(memory_limit) > 0 && percent > 0 ? (size_t) PG(memory_limit) / 100 * (size_t) percent : 0;
    }

    {
        char *end = NULL;
        unsigned long long value = strtoull(setting, &end, 10);

        switch (end != NULL ? *end : '\0') {
            case 'g': case 'G': value <<= 30; break;
            case 'm': case 'M': value <<= 20; break;
            case 'k': case 'K': value <<= 10; break;
            default: break;
        }

        return (size_t) value;
    }
}

/**
 * Called on every function call while a threshold is set: one comparison.
 * 8.0 and 8.1 observe calls of user functions only, 8.2 and later internal
 * ones as well.
 * Crossing it requests a dump the way a signal does, served at the next safe
 * point (or by meminfo_dump_if_requested() with meminfo.dump_defer).
 */
static void meminfo_threshold_check(zend_execute_data *execute_data)
{
    size_t threshold;

    (void) execute_data;
    if (MEMINFO_G(threshold_crossed) || MEMINFO_G(dumping)) {
        return;
    }

    // Parsed once per memory_limit, not on every call.
    if (MEMINFO_G(threshold_limit_seen) != PG(memory_limit)) {
        MEMINFO_G(threshold_limit_seen) = PG(memory_limit);
        MEMINFO_G(threshold_bytes) = meminfo_threshold_bytes();
    }
    threshold = MEMINFO_G(threshold_bytes);
    if (threshold == 0 || zend_memory_usage(0) < threshold) {
        return;
    }

    MEMINFO_G(threshold_crossed) = 1;
    // A request already pending (a trigger waiting for the dump it named by
    // id) is served by the same dump; its id must not be replaced.
    if (!MEMINFO_G(dump_requested)) {
        MEMINFO_G(dump_id) = 0;
        MEMINFO_G(request_from_threshold) = 1;
    }
    MEMINFO_G(dump_requested) = 1;
#if PHP_VERSION_ID >= 80200
    zend_atomic_bool_store_ex(&EG(vm_interrupt), true);
#else
    EG(vm_interrupt) = 1;
#endif
    php_log_err((char *) "meminfo: memory usage passed meminfo.dump_threshold, dumping");
}

static zend_observer_fcall_handlers meminfo_threshold_observer(zend_execute_data *execute_data)
{
    zend_observer_fcall_handlers handlers = { meminfo_threshold_check, NULL };

    (void) execute_data;
    return handlers;
}
#endif

/**
 * Interrupt hook: runs between opcodes, with the engine in a consistent state.
 * With meminfo.dump_defer the request is left pending for
 * meminfo_dump_if_requested().
 */
static void meminfo_interrupt_handler(zend_execute_data *execute_data)
{
    if (!MEMINFO_G(dump_defer)) {
        meminfo_serve_dump_request(execute_data, NULL, 0);
    }

    // Timeouts and pcntl's signal dispatch run through the same hook.
    if (meminfo_previous_interrupt_function != NULL) {
        meminfo_previous_interrupt_function(execute_data);
    }
}

/**
 * Install the handler for the duration of a request.
 *
 * Plain sigaction(), not zend_sigaction(): the latter keeps its own deferring
 * handler registered in the kernel, so /proc/<pid>/status would still report
 * the signal as caught after it had been "restored". The trigger relies on
 * that bit to tell whether the target is in a request at all.
 */
MEMINFO_HIDDEN void meminfo_install_signal_handler(void)
{
    struct sigaction action;
    char message[160];

    if (MEMINFO_G(dump_signal) == 0
        || MEMINFO_G(handler_installed)
        || MEMINFO_G(install_failed)) {
        return;
    }

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = meminfo_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);

    if (sigaction(MEMINFO_G(dump_signal), &action, &MEMINFO_G(previous_action)) == 0) {
        MEMINFO_G(handler_installed) = 1;
        return;
    }

    // Once only: this would otherwise be logged on every request.
    MEMINFO_G(install_failed) = 1;

    snprintf(message, sizeof(message),
        "meminfo: cannot handle signal %d: %s; signal dumps are disabled",
        MEMINFO_G(dump_signal), strerror(errno));
    php_log_err(message);
}

MEMINFO_HIDDEN void meminfo_restore_signal_handler(void)
{
    if (!MEMINFO_G(handler_installed)) {
        return;
    }

    sigaction(MEMINFO_G(dump_signal), &MEMINFO_G(previous_action), NULL);
    MEMINFO_G(handler_installed) = 0;
}

/**
 * Chain the interrupt hook in at module startup, when a dump signal is set.
 *
 * Chained rather than replaced: the engine's own users of this hook (execution
 * timeouts, pcntl's signal dispatch) must keep working.
 */
void meminfo_signal_module_startup(void)
{
    int threshold = MEMINFO_G(dump_threshold) != NULL && MEMINFO_G(dump_threshold)[0] != '\0';

    if (MEMINFO_G(dump_signal) != 0 || threshold) {
        meminfo_previous_interrupt_function = zend_interrupt_function;
        zend_interrupt_function = meminfo_interrupt_handler;
    }

    if (MEMINFO_G(dump_on_limit)) {
#if PHP_VERSION_ID >= 80000
        zend_observer_error_register(meminfo_error_observer);
#else
        meminfo_previous_error_cb = zend_error_cb;
        zend_error_cb = meminfo_error_cb;
#endif
    }

    // Registered only when a threshold is set: every function call then pays
    // one comparison. Before 8.0 there is no observer to check it from.
    if (threshold) {
#if PHP_VERSION_ID >= 80000
        zend_observer_fcall_register(meminfo_threshold_observer);
#else
        php_log_err((char *) "meminfo: meminfo.dump_threshold needs PHP 8.0 or later; it is ignored");
#endif
    }
}

/**
 * Unchain the interrupt hook at module shutdown, if it is still ours.
 */
void meminfo_signal_module_shutdown(void)
{
    if (zend_interrupt_function == meminfo_interrupt_handler) {
        zend_interrupt_function = meminfo_previous_interrupt_function;
    }
#if PHP_VERSION_ID < 80000
    if (zend_error_cb == meminfo_error_cb) {
        zend_error_cb = meminfo_previous_error_cb;
    }
#endif
}

/**
 * Log a deferred request nobody served before the request ended: the trigger
 * waits for it until its timeout.
 */
MEMINFO_HIDDEN void meminfo_report_unserved_request(void)
{
    if (MEMINFO_G(dump_defer) && MEMINFO_G(dump_requested)) {
        php_log_err((char *) "meminfo: a dump was requested but meminfo_dump_if_requested() was not called before the request ended (meminfo.dump_defer=1)");
        MEMINFO_G(dump_requested) = 0;
    }
}

#endif /* MEMINFO_HAVE_SIGNAL_DUMP */

/**
 * meminfo_dump_if_requested(): ?string
 *
 * With meminfo.dump_defer=1, a dump signal only records the request; a worker
 * calls this between two jobs, where nothing of a job is left in memory, and
 * the dump is written then, as on a signal. Returns the dump's path, or null
 * when no dump was pending (or this build cannot serve one).
 */
PHP_FUNCTION(meminfo_dump_if_requested)
{
#if MEMINFO_HAVE_SIGNAL_DUMP
    char path[MAXPATHLEN];
    zend_execute_data *caller;
#endif

    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

#if MEMINFO_HAVE_SIGNAL_DUMP
    path[0] = '\0';
    caller = EG(current_execute_data) ? EG(current_execute_data)->prev_execute_data : NULL;
    if (meminfo_serve_dump_request(caller, path, sizeof(path)) && path[0] != '\0') {
        RETURN_STRING(path);
    }
#endif

    RETURN_NULL();
}
