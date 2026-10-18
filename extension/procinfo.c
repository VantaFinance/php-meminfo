/* Facts about another process: clock, /proc or sysctl, uids, start time, trust checks. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

/* ------------------------------------------------------------------------- *
 * Trigger side
 *
 * Asking another process for a dump. Works on every version and on ZTS: the
 * build doing the asking does not need to be able to serve a dump itself.
 * ------------------------------------------------------------------------- */

#ifndef PHP_WIN32

MEMINFO_HIDDEN uint64_t meminfo_now_usec(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) (ts.tv_nsec / 1000);
    }
#endif
    {
        struct timeval tv;

        gettimeofday(&tv, NULL);

        return (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
    }
}

MEMINFO_HIDDEN void meminfo_sleep_usec(uint64_t usec)
{
    struct timespec requested;

    requested.tv_sec = (time_t) (usec / 1000000ULL);
    requested.tv_nsec = (long) ((usec % 1000000ULL) * 1000);

    while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
        /* interrupted — finish the remaining time */
    }
}

/**
 * Read a small pseudo-file whose size is not known in advance.
 */
static ssize_t meminfo_read_small_file(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t total = 0;

    if (fd < 0) {
        return -1;
    }

    while (total < size - 1) {
        ssize_t got = read(fd, buffer + total, size - 1 - total);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (got == 0) {
            break;
        }

        total += (size_t) got;
    }

    close(fd);
    buffer[total] = '\0';

    return (ssize_t) total;
}

/**
 * Value of a "Name:\tvalue" line of /proc/<pid>/status, or NULL.
 */
static const char *meminfo_status_field(const char *status, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = status;

    while (line != NULL && *line != '\0') {
        if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;

            while (*value == ' ' || *value == '\t') {
                value++;
            }

            return value;
        }

        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }

    return NULL;
}

/**
 * Whether delivering this signal to a process that does not handle it is
 * harmless.
 *
 * These are the signals whose default action neither terminates nor stops the
 * receiver. Everything else — including SIGUSR1, SIGUSR2 and SIGIO, which look
 * like natural choices — would kill a target that is not listening.
 */
MEMINFO_HIDDEN int meminfo_signal_is_harmless(int signal_number)
{
    switch (signal_number) {
#ifdef SIGURG
        case SIGURG:        /* ignored by default */
#endif
#ifdef SIGCHLD
        case SIGCHLD:       /* ignored by default */
#endif
#ifdef SIGWINCH
        case SIGWINCH:      /* ignored by default */
#endif
#ifdef SIGCONT
        case SIGCONT:       /* resumes a stopped process; does not end it */
#endif
            return 1;
        default:
            return 0;
    }
}

#ifdef MEMINFO_HAVE_KINFO_PROC
/**
 * The kernel's record of a process, for systems without /proc.
 *
 * Readable for processes of other users without any privilege. Returns 0 when
 * the process does not exist.
 */
static int meminfo_kinfo_proc(pid_t pid, struct kinfo_proc *out)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int) pid };
    size_t size = sizeof(*out);

    memset(out, 0, sizeof(*out));

    if (sysctl(mib, 4, out, &size, NULL, 0) != 0 || size != sizeof(*out)) {
        return 0;
    }

    return out->kp_proc.p_pid == pid;
}
#endif

/**
 * Whether the target has a handler installed for this signal.
 *
 * This is the one cheap way to tell, from outside, that the target has meminfo
 * loaded *and* is currently inside a request — the handler only exists for the
 * duration of one. Returns 1 caught, 0 not caught, -1 when it cannot be known
 * (no /proc and no kernel process record to read it from).
 */
MEMINFO_HIDDEN int meminfo_signal_is_caught(pid_t pid, int signal_number)
{
    char path[64];
    char status[8192];
    const char *field;
    unsigned long long mask;

    if (signal_number < 1 || signal_number > 64) {
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);

    if (meminfo_read_small_file(path, status, sizeof(status)) < 0) {
#ifdef MEMINFO_HAVE_KINFO_PROC
        struct kinfo_proc info;

        // sigset_t is a 32-bit mask here, one bit per signal.
        if (signal_number <= 32 && meminfo_kinfo_proc(pid, &info)) {
            return ((unsigned long) info.kp_proc.p_sigcatch >> (signal_number - 1)) & 1UL ? 1 : 0;
        }
#endif
        return -1;
    }

    field = meminfo_status_field(status, "SigCgt");
    if (field == NULL) {
        return -1;
    }

    mask = strtoull(field, NULL, 16);

    return ((mask >> (signal_number - 1)) & 1ULL) ? 1 : 0;
}

MEMINFO_HIDDEN int meminfo_process_uids(pid_t pid, uid_t *real_uid, uid_t *effective_uid)
{
    char path[64];
    char status[8192];
    const char *field;
    char *end;

    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);

    if (meminfo_read_small_file(path, status, sizeof(status)) < 0) {
#ifdef MEMINFO_HAVE_KINFO_PROC
        struct kinfo_proc info;

        if (meminfo_kinfo_proc(pid, &info)) {
            *real_uid = info.kp_eproc.e_pcred.p_ruid;
            *effective_uid = info.kp_eproc.e_ucred.cr_uid;
            return 1;
        }
#endif
        return 0;
    }

    field = meminfo_status_field(status, "Uid");
    if (field == NULL) {
        return 0;
    }

    *real_uid = (uid_t) strtoul(field, &end, 10);
    *effective_uid = (uid_t) strtoul(end, NULL, 10);

    return 1;
}

/**
 * Start time of a process: clock ticks since boot from /proc, or microseconds
 * since the epoch from the kernel process record. Only ever compared with
 * itself.
 *
 * Used to notice that the pid has been reused by a different process while the
 * trigger was waiting, which would otherwise look like a dump that never
 * arrives.
 */
MEMINFO_HIDDEN int meminfo_process_start_time(pid_t pid, unsigned long long *out)
{
    char path[64];
    char buffer[4096];
    char *cursor;
    int field;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);

    if (meminfo_read_small_file(path, buffer, sizeof(buffer)) < 0) {
#ifdef MEMINFO_HAVE_KINFO_PROC
        struct kinfo_proc info;

        if (meminfo_kinfo_proc(pid, &info)) {
            *out = (unsigned long long) info.kp_proc.p_starttime.tv_sec * 1000000ULL
                + (unsigned long long) info.kp_proc.p_starttime.tv_usec;
            return 1;
        }
#endif
        return 0;
    }

    // The second field is the executable name in parentheses and may itself
    // contain spaces and parentheses, so fields are counted from the last ')'.
    cursor = strrchr(buffer, ')');
    if (cursor == NULL) {
        return 0;
    }
    cursor++;

    // Field 3 (state) starts here; starttime is field 22.
    for (field = 3; field < 22; field++) {
        while (*cursor == ' ') {
            cursor++;
        }
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            return 0;
        }
    }

    while (*cursor == ' ') {
        cursor++;
    }

    *out = strtoull(cursor, NULL, 10);

    return 1;
}

#ifdef HAVE_SIGQUEUE
/**
 * A non-zero 31-bit id naming this dump request.
 *
 * It only has to be unlikely to clash with another trigger running at the same
 * time; the target creates the file with O_EXCL, so a clash is noticed rather
 * than silently shared.
 */
MEMINFO_HIDDEN int meminfo_random_dump_id(void)
{
    int id = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        unsigned char bytes[4];

        if (read(fd, bytes, sizeof(bytes)) == (ssize_t) sizeof(bytes)) {
            id = (int) ((unsigned) bytes[0]
                | ((unsigned) bytes[1] << 8)
                | ((unsigned) bytes[2] << 16)
                | (((unsigned) bytes[3] & 0x7f) << 24));
        }

        close(fd);
    }

    if (id == 0) {
        struct timeval tv;

        gettimeofday(&tv, NULL);
        id = (int) ((((unsigned) tv.tv_usec << 8)
            ^ (unsigned) tv.tv_sec
            ^ ((unsigned) getpid() << 3)) & 0x7fffffff);
    }

    return id == 0 ? 1 : id;
}
#endif

/**
 * Check that a path really is the dump the target wrote.
 *
 * O_NOFOLLOW and the ownership check are what stop another user from parking a
 * symlink or a file of their own at the expected name and having it returned
 * as if it came from the target. A second link to a file the target owns is
 * refused as well: without protected_hardlinks (macOS has none) anyone can
 * create one, and it would make any file of that owner look like a dump.
 * O_NONBLOCK keeps a FIFO swapped in at that name from blocking the open.
 */
MEMINFO_HIDDEN int meminfo_dump_file_is_trustworthy(const char *path, pid_t pid)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat info;
    uid_t real_uid = 0;
    uid_t effective_uid = 0;

    if (fd < 0) {
        return 0;
    }

    if (fstat(fd, &info) != 0) {
        close(fd);
        return 0;
    }

    close(fd);

    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
        return 0;
    }

    if (meminfo_process_uids(pid, &real_uid, &effective_uid)) {
        return info.st_uid == real_uid || info.st_uid == effective_uid;
    }

    // The target's owner is unknown. Our own files are safe to return; as root
    // anything would pass, including a file another user planted, so refuse.
    return geteuid() != 0 && info.st_uid == geteuid();
}

#endif /* !PHP_WIN32 */
