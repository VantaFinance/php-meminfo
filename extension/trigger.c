/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * meminfo_dump_process(): asking another process for a dump and waiting for it.
 *
 * The trigger side behind the dump-meminfo command: signals the target, then
 * finds the file it wrote and waits until it is complete, so that a leak can
 * be investigated in a live worker without restarting or modifying it.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#include "php_open_temporary_file.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#ifndef PHP_WIN32

/**
 * One place the dump may turn up.
 *
 * A target in its own mount namespace (systemd PrivateTmp, a container) writes
 * to a /tmp the trigger cannot see under that name, but which is reachable
 * through /proc/<pid>/root for the same user or for root with CAP_SYS_PTRACE.
 */
typedef struct {
    char dir[MAXPATHLEN];
    char final_path[MAXPATHLEN];
    char temp_path[MAXPATHLEN + 8];
    int usable;
} meminfo_dump_location;

/**
 * Without an id (dump_id 0) the names are not known in advance: they are
 * filled in by meminfo_find_pid_dump() on every look.
 */
static void meminfo_dump_location_init(meminfo_dump_location *location, const char *prefix, const char *dir, int dump_id)
{
    int written;

    location->usable = 0;
    location->final_path[0] = '\0';
    location->temp_path[0] = '\0';

    written = snprintf(location->dir, sizeof(location->dir), "%s%s", prefix, dir);

    if (written <= 0 || (size_t) written >= sizeof(location->dir)) {
        return;
    }

    if (dump_id == 0) {
        location->usable = 1;
        return;
    }

    written = snprintf(location->final_path, sizeof(location->final_path),
        "%s%s/" MEMINFO_DUMP_FILE_PREFIX "%d.json", prefix, dir, dump_id);

    if (written <= 0 || (size_t) written >= sizeof(location->final_path)) {
        return;
    }

    written = snprintf(location->temp_path, sizeof(location->temp_path),
        "%s.tmp", location->final_path);

    if (written <= 0 || (size_t) written >= sizeof(location->temp_path)) {
        return;
    }

    location->usable = 1;
}

#ifndef HAVE_SIGQUEUE

static int meminfo_timestamp_compare(long a_sec, long a_usec, long b_sec, long b_usec)
{
    if (a_sec != b_sec) {
        return a_sec < b_sec ? -1 : 1;
    }
    if (a_usec != b_usec) {
        return a_usec < b_usec ? -1 : 1;
    }
    return 0;
}

/**
 * Whether a directory entry is named exactly meminfo-<pid>-<sec>-<usec>.json,
 * or the same with .tmp, for this pid.
 *
 * The usec part is not zero-padded, so the timestamp is returned as numbers to
 * be compared as such, never as text.
 */
static int meminfo_parse_pid_dump_name(const char *name, pid_t pid, long *sec, long *usec, int *is_temp)
{
    char prefix[32];
    const char *cursor;
    char *end;
    int written = snprintf(prefix, sizeof(prefix), MEMINFO_DUMP_FILE_PREFIX "%d-", (int) pid);

    if (written <= 0 || (size_t) written >= sizeof(prefix)
        || strncmp(name, prefix, (size_t) written) != 0) {
        return 0;
    }

    // Digits only: strtol would also take a sign or leading blanks.
    cursor = name + written;
    if (*cursor < '0' || *cursor > '9') {
        return 0;
    }

    errno = 0;
    *sec = strtol(cursor, &end, 10);
    if (errno != 0 || *end != '-') {
        return 0;
    }

    cursor = end + 1;
    if (*cursor < '0' || *cursor > '9') {
        return 0;
    }

    *usec = strtol(cursor, &end, 10);
    if (errno != 0 || *usec >= 1000000) {
        return 0;
    }

    if (strcmp(end, ".json") == 0) {
        *is_temp = 0;
        return 1;
    }

    if (strcmp(end, ".json.tmp") == 0) {
        *is_temp = 1;
        return 1;
    }

    return 0;
}

/**
 * Look for the dump a target wrote in answer to a plain kill().
 *
 * The target names it after its pid and the time the dump started, so only
 * names timestamped between the request and now are considered: an earlier
 * one is left over from a previous request, a later one cannot be genuine.
 * Each candidate must pass the same ownership check as a named dump; one that
 * does not is skipped rather than refused, so a file planted in a shared
 * directory cannot block the request. The earliest finished dump and the
 * earliest partial one are reported, either may be empty.
 *
 * Returns how many candidates were skipped as untrustworthy.
 */
static int meminfo_find_pid_dump(meminfo_dump_location *location, pid_t pid, const struct timeval *since)
{
    DIR *dir;
    struct dirent *entry;
    struct timeval now;
    long best_sec[2] = { 0, 0 };
    long best_usec[2] = { 0, 0 };
    int untrusted = 0;

    location->final_path[0] = '\0';
    location->temp_path[0] = '\0';

    dir = opendir(location->dir);
    if (dir == NULL) {
        return 0;
    }

    gettimeofday(&now, NULL);

    while ((entry = readdir(dir)) != NULL) {
        char path[MAXPATHLEN + 8];
        char *best_path;
        size_t best_size;
        long sec;
        long usec;
        int is_temp;
        int written;

        if (!meminfo_parse_pid_dump_name(entry->d_name, pid, &sec, &usec, &is_temp)) {
            continue;
        }

        if (meminfo_timestamp_compare(sec, usec, (long) since->tv_sec, (long) since->tv_usec) < 0
            || meminfo_timestamp_compare(sec, usec, (long) now.tv_sec, (long) now.tv_usec) > 0) {
            continue;
        }

        best_path = is_temp ? location->temp_path : location->final_path;
        best_size = is_temp ? sizeof(location->temp_path) : sizeof(location->final_path);

        if (best_path[0] != '\0'
            && meminfo_timestamp_compare(sec, usec, best_sec[is_temp], best_usec[is_temp]) >= 0) {
            continue;
        }

        written = snprintf(path, sizeof(path), "%s/%s", location->dir, entry->d_name);
        if (written <= 0 || (size_t) written >= best_size) {
            continue;
        }

        if (!meminfo_dump_file_is_trustworthy(path, pid)) {
            untrusted++;
            continue;
        }

        memcpy(best_path, path, (size_t) written + 1);
        best_sec[is_temp] = sec;
        best_usec[is_temp] = usec;
    }

    closedir(dir);

    return untrusted;
}

#endif /* !HAVE_SIGQUEUE */

#ifndef HAVE_SIGQUEUE
/* Resends without an id: each one served is a full dump, so only a few. */
#define MEMINFO_MAX_RESENDS_WITHOUT_ID 3
#endif

/**
 * Tell the target which directory to write the dump to.
 *
 * The request goes into the target's own /tmp: the same as ours, or the one of
 * its mount namespace on Linux. A request left behind by a trigger that died is
 * replaced once it is older than any wait could be; one still in use makes this
 * call fail instead of racing it.
 *
 * Returns 1, or 0 with an exception thrown.
 */
static int meminfo_create_dump_request(pid_t pid, int dump_id, const char *dir, zend_long timeout_seconds, char *path, size_t size)
{
    const char *base = MEMINFO_REQUEST_DIR;
    int attempt;
#ifdef __linux__
    char proc_tmp[64];
    struct stat ours;
    struct stat theirs;

    if (snprintf(proc_tmp, sizeof(proc_tmp), "/proc/%d/root" MEMINFO_REQUEST_DIR, (int) pid) > 0
        && stat(proc_tmp, &theirs) == 0
        && stat(MEMINFO_REQUEST_DIR, &ours) == 0
        && (theirs.st_dev != ours.st_dev || theirs.st_ino != ours.st_ino)) {
        base = proc_tmp;
    }
#endif

    if (!meminfo_request_file_name(path, size, base, (int) pid, dump_id)) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): dump request path is too long");
        return 0;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat info;

        if (fd >= 0) {
            size_t len = strlen(dir);
            int ok = write(fd, dir, len) == (ssize_t) len && write(fd, "\n", 1) == 1;

            if (close(fd) != 0) {
                ok = 0;
            }

            if (!ok) {
                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                    "meminfo_dump_process(): cannot write the dump request %s: %s", path, strerror(errno));
                unlink(path);
                return 0;
            }

            return 1;
        }

        if (errno != EEXIST) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): cannot create the dump request %s: %s", path, strerror(errno));
            return 0;
        }

        if (attempt == 0
            && lstat(path, &info) == 0
            && (info.st_uid == geteuid() || geteuid() == 0)
            && time(NULL) - info.st_mtime > (time_t) timeout_seconds) {
            char error_path[MAXPATHLEN + 8];

            unlink(path);
            if (snprintf(error_path, sizeof(error_path), "%s.error", path) < (int) sizeof(error_path)) {
                unlink(error_path);
            }
            continue;
        }

        break;
    }

    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
        "meminfo_dump_process(): another dump request for process %d is pending (%s); "
        "retry once it has finished",
        (int) pid, path);
    return 0;
}

/**
 * The reason the target gave for not writing a requested dump, if it gave one.
 *
 * Only a file that passes the same ownership check as a dump is believed, so
 * nobody else can make a request fail by planting one.
 */
static int meminfo_read_dump_failure(const char *error_path, pid_t pid, char *reason, size_t size)
{
    int fd;
    ssize_t got;

    if (access(error_path, F_OK) != 0 || !meminfo_dump_file_is_trustworthy(error_path, pid)) {
        return 0;
    }

    fd = open(error_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    got = read(fd, reason, size - 1);
    close(fd);

    reason[got > 0 ? got : 0] = '\0';

    return 1;
}

/**
 * One meminfo_dump_process() call: what was asked for, where the dump will
 * appear and what has to be cleaned up afterwards. Shared by its steps.
 */
typedef struct {
    pid_t pid;
    int signal_number;
    int dump_id;
    zend_long timeout_seconds;
    int force;
    const char *dir;
    char dir_buffer[MAXPATHLEN];
    char resolved_dir[MAXPATHLEN];
    int explicit_dir;
    char request_path[MAXPATHLEN];
    char request_error_path[MAXPATHLEN + 8];
    int request_created;

    meminfo_dump_location locations[2];
    int location_count;

    unsigned long long start_time;
    int have_start_time;
#ifndef HAVE_SIGQUEUE
    struct timeval sent_at;
#endif
} meminfo_dump_request;

/**
 * How a step of meminfo_dump_process() ended.
 */
typedef enum {
    MEMINFO_STEP_CONTINUE,          /* go on with the next step */
    MEMINFO_STEP_DONE,              /* stop: the dump was found, or an exception is pending */
    MEMINFO_STEP_REMOVE_PARTIAL     /* stop with an exception; the partial dump is of no use */
} meminfo_step;

/* --- options ----------------------------------------------------------- */

/**
 * Read the "dir", "signal", "timeout" and "force" options over the ini
 * defaults. Throws InvalidArgumentException on a value that cannot be used.
 */
static meminfo_step meminfo_dump_process_options(meminfo_dump_request *request, HashTable *option_table)
{
    zval *option;

    option = zend_hash_str_find(option_table, "dir", sizeof("dir") - 1);
    if (option != NULL && Z_TYPE_P(option) == IS_STRING && Z_STRLEN_P(option) > 0) {
        if (Z_STRLEN_P(option) >= sizeof(request->dir_buffer)) {
            zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
                "meminfo_dump_process(): dir is too long");
            return MEMINFO_STEP_DONE;
        }
        memcpy(request->dir_buffer, Z_STRVAL_P(option), Z_STRLEN_P(option) + 1);
        request->dir = request->dir_buffer;
        request->explicit_dir = 1;
    }

    option = zend_hash_str_find(option_table, "signal", sizeof("signal") - 1);
    if (option != NULL && Z_TYPE_P(option) != IS_STRING) {
        request->signal_number = (int) zval_get_long(option);
    }
    if (option != NULL && Z_TYPE_P(option) == IS_STRING) {
        int parsed = 0;

        if (!meminfo_parse_signal(Z_STRVAL_P(option), &parsed)) {
            zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
                "meminfo_dump_process(): unknown signal \"%s\"", Z_STRVAL_P(option));
            return MEMINFO_STEP_DONE;
        }
        request->signal_number = parsed;
    }

    option = zend_hash_str_find(option_table, "timeout", sizeof("timeout") - 1);
    if (option != NULL) {
        request->timeout_seconds = zval_get_long(option);
    }

    option = zend_hash_str_find(option_table, "force", sizeof("force") - 1);
    if (option != NULL) {
        request->force = zend_is_true(option);
    }

    return MEMINFO_STEP_CONTINUE;
}

/**
 * Check the pid, the directory and the signal. Throws InvalidArgumentException.
 */
static meminfo_step meminfo_dump_process_validate(meminfo_dump_request *request, zend_long pid_arg)
{
    struct stat info;

    if (pid_arg <= 0) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): pid must be greater than 0");
        return MEMINFO_STEP_DONE;
    }

    request->pid = (pid_t) pid_arg;

    if (request->pid == getpid()) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): cannot dump the calling process; use meminfo_dump()");
        return MEMINFO_STEP_DONE;
    }

    // Sent to the target as an absolute path: its working directory is not ours.
    if (request->explicit_dir) {
        if (realpath(request->dir_buffer, request->resolved_dir) == NULL
            || stat(request->resolved_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
            zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
                "meminfo_dump_process(): dir %s is not an existing directory", request->dir_buffer);
            return MEMINFO_STEP_DONE;
        }

        request->dir = request->resolved_dir;
    }

    if (request->signal_number <= 0) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): no dump signal configured; set meminfo.dump_signal "
            "or pass the \"signal\" option");
        return MEMINFO_STEP_DONE;
    }

    if (request->timeout_seconds < 1) {
        request->timeout_seconds = 1;
    }

    return MEMINFO_STEP_CONTINUE;
}

/* --- is the target listening? ------------------------------------------ */

/**
 * Refuse to signal a target that cannot serve the dump, or whose dump could
 * not be trusted. Throws RuntimeException.
 */
static meminfo_step meminfo_dump_process_check_target(meminfo_dump_request *request)
{
    pid_t pid = request->pid;
    int signal_number = request->signal_number;
    int caught = meminfo_signal_is_caught(pid, signal_number);
    int default_signal = 0;
    uid_t real_uid;
    uid_t effective_uid;

    if (caught == 0 && !meminfo_signal_is_harmless(signal_number)) {
        // Deliberately not overridable by "force". The target is known not to
        // handle this signal, so delivering it cannot produce a dump — it can
        // only terminate or stop the target. Allowing it would turn a
        // diagnostic function into a way to kill arbitrary processes, which is
        // a capability this extension has no business providing.
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d does not handle signal %d, whose default "
            "action would terminate or stop it; refusing to send it. The target may not have "
            "meminfo loaded, may be a PHP 7.0 or ZTS build, or may not be running a request",
            (int) pid, signal_number);
        return MEMINFO_STEP_DONE;
    }

    if (caught == 0 && !request->force) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d does not handle signal %d: meminfo is not "
            "loaded, the target is PHP 7.0 or a ZTS build, or no request is running. "
            "Pass \"force\" => true to send it anyway",
            (int) pid, signal_number);
        return MEMINFO_STEP_DONE;
    }

    meminfo_parse_signal(MEMINFO_DEFAULT_DUMP_SIGNAL, &default_signal);

    // Without /proc there is no way to know whether the signal is handled.
    // Sending the default is safe because its default action is to ignore;
    // anything else could kill a target that is not listening.
    if (caught < 0 && !request->force && signal_number != default_signal) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): cannot tell whether process %d handles signal %d "
            "(no /proc on this system) and its default action may terminate the target. "
            "Pass \"force\" => true to send it anyway",
            (int) pid, signal_number);
        return MEMINFO_STEP_DONE;
    }

    // As root, a dump would pass the ownership check whoever wrote it, so
    // without knowing who runs the target there is no dump that can be trusted.
    // Better to say so now than after the target has spent the time writing one.
    if (geteuid() == 0 && !meminfo_process_uids(pid, &real_uid, &effective_uid)) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): cannot determine who runs process %d, so a dump could "
            "not be verified as coming from it; refusing to ask as root", (int) pid);
        return MEMINFO_STEP_DONE;
    }

    request->have_start_time = meminfo_process_start_time(pid, &request->start_time);

    return MEMINFO_STEP_CONTINUE;
}

/* --- where the dump will appear ---------------------------------------- */

/**
 * Work out where the dump will appear and, for an explicit directory, leave
 * the request file that tells the target to write there.
 */
static meminfo_step meminfo_dump_process_locations(meminfo_dump_request *request)
{
    char proc_root_prefix[64];

#ifdef HAVE_SIGQUEUE
    request->dump_id = meminfo_random_dump_id();
#else
    // Cannot be delivered; the dump is looked for by pid and time instead.
    request->dump_id = 0;
#endif

    meminfo_dump_location_init(&request->locations[0], "", request->dir, request->dump_id);

#ifdef __linux__
    if (snprintf(proc_root_prefix, sizeof(proc_root_prefix), "/proc/%d/root", (int) request->pid) > 0) {
        meminfo_dump_location_init(&request->locations[1], proc_root_prefix, request->dir, request->dump_id);
        if (request->locations[1].usable) {
            request->location_count = 2;
        }
    }
#else
    (void) proc_root_prefix;
#endif

    if (!request->locations[0].usable) {
        zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0,
            "meminfo_dump_process(): dump path is too long");
        return MEMINFO_STEP_DONE;
    }

    // Without a request the target writes to its own meminfo.dump_dir, which
    // may well not be the directory asked for here.
    if (!request->explicit_dir) {
        return MEMINFO_STEP_CONTINUE;
    }

    if (!meminfo_create_dump_request(request->pid, request->dump_id, request->dir, request->timeout_seconds,
            request->request_path, sizeof(request->request_path))) {
        return MEMINFO_STEP_DONE;
    }

    request->request_created = 1;
    snprintf(request->request_error_path, sizeof(request->request_error_path), "%s.error", request->request_path);

    return MEMINFO_STEP_CONTINUE;
}

/* --- ask --------------------------------------------------------------- */

/**
 * Send the dump signal: with the request id where sigqueue() can carry one,
 * plainly otherwise. Throws RuntimeException when it cannot be sent.
 */
static meminfo_step meminfo_dump_process_ask(meminfo_dump_request *request)
{
#ifdef HAVE_SIGQUEUE
    union sigval value;

    memset(&value, 0, sizeof(value));
    value.sival_int = request->dump_id;

    if (sigqueue(request->pid, request->signal_number, value) == 0) {
        return MEMINFO_STEP_CONTINUE;
    }
#else
    // Without sigqueue() the id cannot be delivered, so the target names the
    // dump after its pid and the time the dump started. Only a dump started
    // after this moment can answer this request. Two requests running at the
    // same time cannot be told apart and may be handed the same file.
    gettimeofday(&request->sent_at, NULL);

    if (kill(request->pid, request->signal_number) == 0) {
        return MEMINFO_STEP_CONTINUE;
    }
#endif

    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
        "meminfo_dump_process(): cannot signal process %d: %s",
        (int) request->pid, strerror(errno));
    return MEMINFO_STEP_DONE;
}

/* --- wait -------------------------------------------------------------- */

/**
 * The location holding the finished dump, under either name, or -1.
 */
static int meminfo_dump_process_find_finished(const meminfo_dump_request *request)
{
    struct stat info;
    int i;

    for (i = 0; i < request->location_count; i++) {
        const meminfo_dump_location *location = &request->locations[i];

        if (location->usable
            && location->final_path[0] != '\0'
            && stat(location->final_path, &info) == 0
            && S_ISREG(info.st_mode)) {
            return i;
        }
    }

    return -1;
}

/**
 * The size of the partial dump, or -1 while there is none.
 */
static off_t meminfo_dump_process_partial_size(const meminfo_dump_request *request)
{
    struct stat info;
    int i;

    for (i = 0; i < request->location_count; i++) {
        const meminfo_dump_location *location = &request->locations[i];

        if (location->usable
            && location->temp_path[0] != '\0'
            && stat(location->temp_path, &info) == 0) {
            return info.st_size;
        }
    }

    return -1;
}

/**
 * Throw for a dump that stopped growing for the whole timeout.
 */
static void meminfo_dump_process_throw_stalled(const meminfo_dump_request *request, int untrusted)
{
    if (untrusted > 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): the dump of process %d stopped growing for %ld "
            "seconds; giving up (%d matching file(s) in %s were ignored: not owned "
            "by the target, or not regular files with a single link)",
            (int) request->pid, (long) request->timeout_seconds, untrusted, request->locations[0].dir);
        return;
    }

    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
        "meminfo_dump_process(): the dump of process %d stopped growing for %ld "
        "seconds; giving up", (int) request->pid, (long) request->timeout_seconds);
}

/**
 * Throw for a dump that did not start within the timeout.
 *
 * Two usual reasons. The target ran no PHP code meanwhile: the dump starts at
 * the next opcode, and an idle event loop (Swoole, ReactPHP, AMPHP) or a long
 * blocking call never gets there. Or a directory mismatch: the target writes
 * to its own meminfo.dump_dir, and only builds that know about requests can
 * be told otherwise.
 */
static void meminfo_dump_process_throw_not_started(const meminfo_dump_request *request, int untrusted)
{
    const char *hint = request->explicit_dir
        ? "either it ran no PHP code in that time (an idle event loop or a long blocking call: "
          "retry while it is busy), or it runs an older meminfo that cannot be told where to "
          "write: look in its meminfo.dump_dir"
        : "either it ran no PHP code in that time (an idle event loop or a long blocking call: "
          "retry while it is busy), or it wrote to its own meminfo.dump_dir: pass \"dir\" to "
          "choose where";

    if (untrusted > 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d did not start a dump in %s within %ld seconds "
            "(%d matching file(s) were ignored: not owned by the target, or not "
            "regular files with a single link)",
            (int) request->pid, request->locations[0].dir, (long) request->timeout_seconds, untrusted);
        return;
    }

    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
        "meminfo_dump_process(): process %d did not start a dump in %s within %ld seconds; %s",
        (int) request->pid, request->locations[0].dir, (long) request->timeout_seconds, hint);
}

/**
 * Has the target gone away? Throws RuntimeException when it exited, or when
 * its pid now belongs to another process.
 */
static meminfo_step meminfo_dump_process_target_gone(const meminfo_dump_request *request)
{
    unsigned long long current_start_time = 0;

    if (kill(request->pid, 0) != 0 && errno == ESRCH) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d exited during the dump", (int) request->pid);
        return MEMINFO_STEP_REMOVE_PARTIAL;
    }

    if (request->have_start_time
        && meminfo_process_start_time(request->pid, &current_start_time)
        && current_start_time != request->start_time) {

        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "meminfo_dump_process(): process %d exited during the dump and its pid "
            "was reused", (int) request->pid);
        return MEMINFO_STEP_REMOVE_PARTIAL;
    }

    return MEMINFO_STEP_CONTINUE;
}

/**
 * Send the signal again.
 *
 * Standard signals coalesce, so one sent while the target was busy may be the
 * only one it ever sees, and one arriving between two requests is lost. With
 * an id, resending is harmless: the file name stays the same and the target
 * serves it once.
 */
static void meminfo_dump_process_resend(meminfo_dump_request *request, int *resends)
{
#ifdef HAVE_SIGQUEUE
    union sigval value;

    (void) resends;
    memset(&value, 0, sizeof(value));
    value.sival_int = request->dump_id;
    sigqueue(request->pid, request->signal_number, value);
#else
    // Without one, every signal served is a separate dump: look once more
    // right before asking again, as the dump may have started since the last
    // look, and ask only a few times. A target that writes somewhere this call
    // cannot see would otherwise get a full dump out of every resend.
    int pending = 0;
    int i;

    for (i = 0; i < request->location_count; i++) {
        if (request->locations[i].usable) {
            meminfo_find_pid_dump(&request->locations[i], request->pid, &request->sent_at);
            pending |= request->locations[i].temp_path[0] != '\0' || request->locations[i].final_path[0] != '\0';
        }
    }

    if (!pending && *resends < MEMINFO_MAX_RESENDS_WITHOUT_ID) {
        kill(request->pid, request->signal_number);
        (*resends)++;
    }
#endif
}

/**
 * Poll until the dump is complete, the target fails or goes away, or the
 * timeout runs out. Returns MEMINFO_STEP_DONE with *finished set to the
 * location of the dump on success.
 */
static meminfo_step meminfo_dump_process_wait(meminfo_dump_request *request, int *finished)
{
    uint64_t began = meminfo_now_usec();
    uint64_t last_resend = began;
    uint64_t last_growth = began;
    uint64_t poll_interval = MEMINFO_POLL_MIN_INTERVAL_USEC;
    uint64_t timeout_usec = (uint64_t) request->timeout_seconds * 1000000ULL;
    off_t last_size = -1;
    off_t size;
    int temp_seen = 0;
    int untrusted = 0;
    int resends = 0;
    char reason[512];
    meminfo_step step;
    int found;
#ifndef HAVE_SIGQUEUE
    int i;
#endif

    for (;;) {
        if (request->request_created
            && meminfo_read_dump_failure(request->request_error_path, request->pid, reason, sizeof(reason))) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): process %d could not write the dump: %s",
                (int) request->pid, reason[0] != '\0' ? reason : "no reason given");
            return MEMINFO_STEP_DONE;
        }

#ifndef HAVE_SIGQUEUE
        untrusted = 0;
        for (i = 0; i < request->location_count; i++) {
            if (request->locations[i].usable) {
                untrusted += meminfo_find_pid_dump(&request->locations[i], request->pid, &request->sent_at);
            }
        }
#endif

        // A partial dump means the target is working; from here on the wait
        // is bounded by progress rather than by the timeout, because a large
        // heap on a slow disk can legitimately take minutes.
        size = meminfo_dump_process_partial_size(request);
        if (size >= 0) {
            temp_seen = 1;
        }
        if (size >= 0 && size != last_size) {
            last_size = size;
            last_growth = meminfo_now_usec();
        }

        // Looked for after the partial one, never before: the other way
        // round, a rename in between would make the dump look absent and the
        // request be sent again.
        found = meminfo_dump_process_find_finished(request);
        if (found >= 0 && !meminfo_dump_file_is_trustworthy(request->locations[found].final_path, request->pid)) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): %s is not a regular file owned by process %d; "
                "refusing to use it",
                request->locations[found].final_path, (int) request->pid);
            return MEMINFO_STEP_DONE;
        }
        if (found >= 0) {
            *finished = found;
            return MEMINFO_STEP_DONE;
        }

        if (temp_seen && meminfo_now_usec() - last_growth > timeout_usec) {
            meminfo_dump_process_throw_stalled(request, untrusted);
            return MEMINFO_STEP_DONE;
        }

        if (!temp_seen && meminfo_now_usec() - began > timeout_usec) {
            meminfo_dump_process_throw_not_started(request, untrusted);
            return MEMINFO_STEP_DONE;
        }

        step = meminfo_dump_process_target_gone(request);
        if (step != MEMINFO_STEP_CONTINUE) {
            return step;
        }

        // The handler is only installed while a request runs. If it has gone
        // and nothing was written, the request finished before reaching a
        // safe point.
        if (!temp_seen && meminfo_signal_is_caught(request->pid, request->signal_number) == 0) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "meminfo_dump_process(): the request in process %d ended before the dump "
                "started", (int) request->pid);
            return MEMINFO_STEP_DONE;
        }

        if (!temp_seen && meminfo_now_usec() - last_resend >= MEMINFO_RESEND_INTERVAL_USEC) {
            meminfo_dump_process_resend(request, &resends);
            last_resend = meminfo_now_usec();
        }

        meminfo_sleep_usec(poll_interval);
        poll_interval *= 2;
        if (poll_interval > MEMINFO_POLL_INTERVAL_USEC) {
            poll_interval = MEMINFO_POLL_INTERVAL_USEC;
        }
    }
}

/**
 * Remove the partial dump the target left behind. Called with an exception
 * already pending: it is of no use to anyone and goes before unwinding.
 */
static void meminfo_dump_process_remove_partial(const meminfo_dump_request *request)
{
    int i;

    for (i = 0; i < request->location_count; i++) {
        if (request->locations[i].usable && request->locations[i].temp_path[0] != '\0') {
            unlink(request->locations[i].temp_path);
        }
    }
}

/**
 * Run the steps in order, stopping at the first that does not continue.
 */
static meminfo_step meminfo_dump_process_run(meminfo_dump_request *request, zend_long pid_arg, zval *options, int *finished)
{
    meminfo_step step;

    if (options != NULL) {
        step = meminfo_dump_process_options(request, Z_ARRVAL_P(options));
        if (step != MEMINFO_STEP_CONTINUE) {
            return step;
        }
    }

    step = meminfo_dump_process_validate(request, pid_arg);
    if (step != MEMINFO_STEP_CONTINUE) {
        return step;
    }

    step = meminfo_dump_process_check_target(request);
    if (step != MEMINFO_STEP_CONTINUE) {
        return step;
    }

    step = meminfo_dump_process_locations(request);
    if (step != MEMINFO_STEP_CONTINUE) {
        return step;
    }

    step = meminfo_dump_process_ask(request);
    if (step != MEMINFO_STEP_CONTINUE) {
        return step;
    }

    return meminfo_dump_process_wait(request, finished);
}

/**
 * Ask a running process to dump its memory, and wait for the result.
 *
 * The target does the work at the next safe point between opcodes, so this
 * returns as soon as the dump file has been completely written and renamed
 * into place — or throws, explaining which step failed.
 */
PHP_FUNCTION(meminfo_dump_process)
{
    zend_long pid_arg;
    zval *options = NULL;
    meminfo_dump_request request;
    meminfo_step step;
    int finished = -1;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|a", &pid_arg, &options) == FAILURE) {
        return;
    }

    memset(&request, 0, sizeof(request));
    request.timeout_seconds = 30;
    request.location_count = 1;
    request.signal_number = MEMINFO_G(dump_signal);
#if !MEMINFO_HAVE_SIGNAL_DUMP
    // This build cannot serve a dump, so the ini value was forced to 0; the
    // default signal is still the right thing to send to a target that can.
    if (request.signal_number == 0) {
        meminfo_parse_signal(MEMINFO_DEFAULT_DUMP_SIGNAL, &request.signal_number);
    }
#endif
    request.dir = meminfo_dump_directory();

    step = meminfo_dump_process_run(&request, pid_arg, options, &finished);

    if (finished >= 0) {
        RETVAL_STRING(request.locations[finished].final_path);
    }

    if (step == MEMINFO_STEP_REMOVE_PARTIAL) {
        meminfo_dump_process_remove_partial(&request);
    }

    // Ours to remove whatever happened; the target only reads it.
    if (request.request_created) {
        unlink(request.request_path);
        unlink(request.request_error_path);
    }
}

#endif /* !PHP_WIN32 */
