/*
 * Linux-only LD_PRELOAD prototype: observe every _exit(), then preserve exit.
 * This records a terminal notification, not an OOM diagnosis or a heap dump.
 *
 * MEMINFO_EXIT_PROBE_LOG is opened before PHP starts. The termination path
 * uses a fixed stack buffer and write/syscall only: no PHP, allocation,
 * stdio, dynamic symbol lookup, bailout, or attempt to resume execution.
 */
#ifndef __linux__
#error "exit_probe requires Linux exit_group semantics"
#endif

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int probe_fd = -1;
static atomic_flag probe_writing = ATOMIC_FLAG_INIT;

__attribute__((constructor))
static void exit_probe_startup(void)
{
    const char *path = getenv("MEMINFO_EXIT_PROBE_LOG");
    struct stat info;
    int fd;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    /* Nonblocking open also avoids waiting on an accidentally supplied FIFO. */
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) {
        return;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_uid != geteuid() || (info.st_mode & 0777) != 0600) {
        close(fd);
        return;
    }

    probe_fd = fd;
}

static size_t exit_probe_uint(char *buffer, unsigned long value)
{
    char reversed[3 * sizeof(value)];
    size_t count = 0;
    size_t i;

    do {
        reversed[count++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);

    for (i = 0; i < count; i++) {
        buffer[i] = reversed[count - i - 1];
    }

    return count;
}

static void exit_probe_record(int status)
{
    static const char prefix[] = "[EXIT_PROBE] pid=";
    static const char separator[] = " status=";
    char buffer[128];
    unsigned int magnitude = (unsigned int) status;
    size_t length = 0;
    size_t i;
    ssize_t written;

    if (probe_fd < 0 || atomic_flag_test_and_set_explicit(&probe_writing, memory_order_relaxed)) {
        return;
    }

    for (i = 0; i < sizeof(prefix) - 1; i++) {
        buffer[length++] = prefix[i];
    }
    length += exit_probe_uint(buffer + length, (unsigned long) getpid());
    for (i = 0; i < sizeof(separator) - 1; i++) {
        buffer[length++] = separator[i];
    }
    if (status < 0) {
        buffer[length++] = '-';
        magnitude = 0U - magnitude;
    }
    length += exit_probe_uint(buffer + length, magnitude);
    buffer[length++] = '\n';

    /* Best effort, one bounded write. A diagnostic failure must not delay exit. */
    written = write(probe_fd, buffer, length);
    (void) written;
}

__attribute__((noreturn, visibility("default")))
void _exit(int status)
{
    exit_probe_record(status);
    (void) syscall(SYS_exit_group, status);

    /* Only reachable if the syscall was denied; never return into PHP. */
    (void) syscall(SYS_exit, status);
    __builtin_trap();
}
