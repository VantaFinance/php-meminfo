/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Transports for finished dump files: meminfo.dump_upload.
 *
 * A dump the process writes on its own (signal, threshold, memory_limit) is
 * always a local file first; with a transport configured it is then shipped
 * somewhere that outlives the process — today "s3://", see upload_s3.c.
 *
 * Uploads run in the background and nothing of a request ever waits for one:
 * in PHP-FPM the client's connection is only released after the whole request
 * shutdown. The process waits for them only when it exits (MSHUTDOWN), bounded
 * by meminfo.dump_upload_timeout.
 *
 * Transports complete on threads of their own, which must not touch PHP. They
 * write the outcome next to the dump (<dump>.uploaded / <dump>.upload-error,
 * plain C file calls) and queue a log line; the PHP thread hands the queued
 * lines to php_log_err() at its next poll point.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

/* php.h maps these to PHP's own formatter, which is not for threads PHP does
 * not know about — and transport callbacks run on such threads. The C
 * library's versions serve both sides here. */
#undef snprintf
#undef vsnprintf

#if MEMINFO_HAVE_SIGNAL_DUMP
# include <pthread.h>
#endif

/* "s3://" + bucket name (3-63 characters) */
#define MEMINFO_S3_SCHEME "s3://"
#define MEMINFO_S3_BUCKET_MAX 63

/**
 * The parsed meminfo.dump_upload, shared by every request of the process.
 */
static struct {
    int enabled;
    char bucket[MEMINFO_S3_BUCKET_MAX + 1];
    char prefix[512];
    char text[600];             /* what phpinfo() shows */
} meminfo_upload_target_info;

/**
 * Whether a bucket name follows the S3 rules: 3-63 characters out of lower
 * case letters, digits, dots and hyphens, starting and ending with a letter or
 * a digit. Stricter names keep the virtual-hosted address a valid host name.
 */
static int meminfo_upload_bucket_is_valid(const char *bucket, size_t len)
{
    size_t i;

    if (len < 3 || len > MEMINFO_S3_BUCKET_MAX) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        char c = bucket[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (!alnum && c != '.' && c != '-') {
            return 0;
        }
        if (!alnum && (i == 0 || i == len - 1)) {
            return 0;
        }
    }

    return 1;
}

/**
 * Whether a key prefix needs no URL encoding: letters, digits and "/._-".
 * Keys are put into the request path as they are, so anything else is refused
 * at startup rather than encoded.
 */
static int meminfo_upload_prefix_is_valid(const char *prefix)
{
    const char *p;

    for (p = prefix; *p != '\0'; p++) {
        char c = *p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '/' || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }

    return strstr(prefix, "//") == NULL && strstr(prefix, "..") == NULL;
}

/**
 * Parse meminfo.dump_upload. Returns 1 for "s3://bucket[/prefix]", 0 for an
 * empty setting and -1 (with the reason in error) for anything else.
 */
static int meminfo_upload_parse_target(const char *value, char *error, size_t error_size)
{
    const char *bucket;
    const char *slash;
    size_t bucket_len;
    size_t prefix_len;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    if (strncmp(value, MEMINFO_S3_SCHEME, sizeof(MEMINFO_S3_SCHEME) - 1) != 0) {
        snprintf(error, error_size, "\"%s\" is not a supported target; expected s3://bucket[/prefix]", value);
        return -1;
    }

    bucket = value + sizeof(MEMINFO_S3_SCHEME) - 1;
    slash = strchr(bucket, '/');
    bucket_len = slash != NULL ? (size_t) (slash - bucket) : strlen(bucket);

    if (!meminfo_upload_bucket_is_valid(bucket, bucket_len)) {
        snprintf(error, error_size, "\"%s\": the bucket name is not valid for S3", value);
        return -1;
    }

    memcpy(meminfo_upload_target_info.bucket, bucket, bucket_len);
    meminfo_upload_target_info.bucket[bucket_len] = '\0';
    meminfo_upload_target_info.prefix[0] = '\0';

    if (slash != NULL) {
        // Leading and trailing slashes carry no meaning in a key prefix.
        while (*slash == '/') {
            slash++;
        }
        prefix_len = strlen(slash);
        while (prefix_len > 0 && slash[prefix_len - 1] == '/') {
            prefix_len--;
        }
        if (prefix_len >= sizeof(meminfo_upload_target_info.prefix)) {
            snprintf(error, error_size, "\"%s\": the key prefix is too long", value);
            return -1;
        }
        memcpy(meminfo_upload_target_info.prefix, slash, prefix_len);
        meminfo_upload_target_info.prefix[prefix_len] = '\0';

        if (!meminfo_upload_prefix_is_valid(meminfo_upload_target_info.prefix)) {
            snprintf(error, error_size,
                "\"%s\": the key prefix may only hold letters, digits and \"/._-\"", value);
            return -1;
        }
    }

    return 1;
}

/* The settings meminfo_upload_target_info was worked out from. */
static struct {
    int done;
    char key[1200];
} meminfo_upload_configured;

/**
 * Report a configuration problem: as a startup warning in MINIT, the only
 * point where one cannot end up in somebody's response, and to the error log
 * afterwards.
 */
static void meminfo_upload_config_problem(int at_startup, const char *message)
{
    char line[1100];

    if (at_startup) {
        php_error_docref(NULL, E_WARNING, "%s", message);
        return;
    }

    snprintf(line, sizeof(line), "meminfo: %s", message);
    php_log_err(line);
}

/**
 * Work out the target from meminfo.dump_upload and meminfo.s3_endpoint, and
 * again whenever they change: PHP-FPM applies a pool's php_admin_value after
 * startup, so each pool can upload somewhere else. Problems are reported once
 * per value.
 */
static void meminfo_upload_configure(int at_startup)
{
    char error[600];
    char message[700];
    char key[sizeof(meminfo_upload_configured.key)];
    const char *value = MEMINFO_G(dump_upload) != NULL ? MEMINFO_G(dump_upload) : "";
    const char *endpoint = MEMINFO_G(s3_endpoint) != NULL ? MEMINFO_G(s3_endpoint) : "";
    int parsed;

    snprintf(key, sizeof(key), "%s\n%s", value, endpoint);
    if (meminfo_upload_configured.done && strcmp(key, meminfo_upload_configured.key) == 0) {
        return;
    }
    meminfo_upload_configured.done = 1;
    memcpy(meminfo_upload_configured.key, key, sizeof(key));

    meminfo_upload_target_info.enabled = 0;
    snprintf(meminfo_upload_target_info.text, sizeof(meminfo_upload_target_info.text), "%s", "disabled");

    parsed = meminfo_upload_parse_target(value, error, sizeof(error));
    if (parsed == 0) {
        return;
    }
    if (parsed < 0) {
        snprintf(message, sizeof(message), "meminfo.dump_upload: %s; dumps stay local", error);
        meminfo_upload_config_problem(at_startup, message);
        return;
    }

#if !MEMINFO_HAVE_SIGNAL_DUMP
    meminfo_upload_config_problem(at_startup,
        "meminfo.dump_upload is set but this build cannot dump on its own "
        "(needs PHP 7.1 or later, non-ZTS, POSIX); it is ignored");
#elif !defined(MEMINFO_HAVE_S3)
    meminfo_upload_config_problem(at_startup,
        "meminfo.dump_upload is set but this build has no S3 transport "
        "(build with --with-meminfo-s3); dumps stay local");
#else
    if (endpoint[0] != '\0' && strncmp(endpoint, "http://", 7) != 0 && strncmp(endpoint, "https://", 8) != 0) {
        snprintf(message, sizeof(message),
            "meminfo.s3_endpoint: \"%s\" must start with http:// or https://; dumps stay local", endpoint);
        meminfo_upload_config_problem(at_startup, message);
        return;
    }

    meminfo_upload_target_info.enabled = 1;
    snprintf(meminfo_upload_target_info.text, sizeof(meminfo_upload_target_info.text), "%s%s%s%s",
        MEMINFO_S3_SCHEME, meminfo_upload_target_info.bucket,
        meminfo_upload_target_info.prefix[0] != '\0' ? "/" : "", meminfo_upload_target_info.prefix);
#endif
}

/**
 * Validate the upload settings at startup.
 */
MEMINFO_HIDDEN void meminfo_upload_module_startup(void)
{
    meminfo_upload_configure(1);
}

/**
 * The upload target as phpinfo() shows it.
 */
MEMINFO_HIDDEN const char *meminfo_upload_target(void)
{
    meminfo_upload_configure(0);

    return meminfo_upload_target_info.text[0] != '\0' ? meminfo_upload_target_info.text : "disabled";
}

#if MEMINFO_HAVE_SIGNAL_DUMP

/**
 * A line for the error log, written by a transport thread and handed to
 * php_log_err() by the PHP thread.
 */
typedef struct meminfo_upload_note {
    struct meminfo_upload_note *next;
    char text[MAXPATHLEN + 1024];
} meminfo_upload_note;

/**
 * State shared between the PHP thread and the transport threads. Builds that
 * dump on their own are NTS, so this is per process.
 */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t idle;            /* signalled whenever pending drops */
    unsigned int pending;
    meminfo_upload_note *notes;     /* oldest first */
    meminfo_upload_note *last_note;
    /* the process whose transport threads exist; 0 before the first upload */
    pid_t owner;
    /* told the log once that this forked process cannot upload */
    int fork_reported;
} meminfo_upload_state = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, NULL, NULL, 0, 0
};

/**
 * Queue a log line. Safe on any thread; never calls into PHP.
 */
static void meminfo_upload_note_add(const char *format, ...)
{
    meminfo_upload_note *note = malloc(sizeof(*note));
    va_list args;

    if (note == NULL) {
        return;
    }

    note->next = NULL;
    va_start(args, format);
    vsnprintf(note->text, sizeof(note->text), format, args);
    va_end(args);

    pthread_mutex_lock(&meminfo_upload_state.lock);
    if (meminfo_upload_state.last_note != NULL) {
        meminfo_upload_state.last_note->next = note;
    } else {
        meminfo_upload_state.notes = note;
    }
    meminfo_upload_state.last_note = note;
    pthread_mutex_unlock(&meminfo_upload_state.lock);
}

/**
 * Hand the queued lines to the error log. PHP thread only; never waits for an
 * upload.
 */
MEMINFO_HIDDEN void meminfo_upload_poll(void)
{
    meminfo_upload_note *note;

    if (meminfo_upload_state.owner != getpid()) {
        return;
    }

    pthread_mutex_lock(&meminfo_upload_state.lock);
    note = meminfo_upload_state.notes;
    meminfo_upload_state.notes = NULL;
    meminfo_upload_state.last_note = NULL;
    pthread_mutex_unlock(&meminfo_upload_state.lock);

    while (note != NULL) {
        meminfo_upload_note *next = note->next;

        php_log_err(note->text);
        free(note);
        note = next;
    }
}

/**
 * Whether dumps of this process are uploaded.
 */
MEMINFO_HIDDEN int meminfo_upload_enabled(void)
{
    meminfo_upload_configure(0);

    return meminfo_upload_target_info.enabled;
}

/**
 * Write a small file next to the dump the way the dump itself is written: a
 * temporary name created exclusively, without following links, readable by
 * the owner only, then renamed into place. Any thread.
 */
static int meminfo_upload_write_sidecar(const char *dump_path, const char *suffix, const char *content)
{
    char path[MAXPATHLEN + 32];
    char temp_path[MAXPATHLEN + 40];
    size_t len = strlen(content);
    ssize_t written;
    int fd;

    if (snprintf(path, sizeof(path), "%s%s", dump_path, suffix) >= (int) sizeof(path)
        || snprintf(temp_path, sizeof(temp_path), "%s.tmp", path) >= (int) sizeof(temp_path)) {
        return 0;
    }

    fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        // Left over by a process that died between creating and renaming it:
        // ours to replace, the name embeds the dump's own.
        unlink(temp_path);
        fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    if (fd < 0) {
        return 0;
    }

    written = write(fd, content, len);
    if (close(fd) != 0 || written != (ssize_t) len || rename(temp_path, path) != 0) {
        unlink(temp_path);
        return 0;
    }

    return 1;
}

/**
 * strerror() for transport threads: the text of errno in buf. glibc with
 * _GNU_SOURCE has the strerror_r that returns the text, everything else the
 * POSIX one that fills buf.
 */
static const char *meminfo_upload_strerror(int error, char *buf, size_t size)
{
#if defined(__GLIBC__) && defined(__USE_GNU)
    return strerror_r(error, buf, size);
#else
    if (strerror_r(error, buf, size) != 0) {
        snprintf(buf, size, "error %d", error);
    }

    return buf;
#endif
}

/**
 * Called by a transport once an upload is over, on whatever thread it
 * completes on. Records the outcome next to the dump, removes the local copy
 * when asked to, and queues the log line. Takes ownership of the job.
 */
MEMINFO_HIDDEN void meminfo_upload_finished(meminfo_upload_job *job, int ok, const char *error)
{
    char content[sizeof(job->url) + 2];
    char reason[128];
    double seconds = (double) (meminfo_now_usec() - job->started_usec) / 1000000.0;
    int removed = 0;

    if (ok) {
        snprintf(content, sizeof(content), "%s\n", job->url);
        if (!meminfo_upload_write_sidecar(job->path, MEMINFO_UPLOADED_SUFFIX, content)) {
            meminfo_upload_note_add("meminfo: uploaded %s to %s but could not write %s%s: %s",
                job->path, job->url, job->path, MEMINFO_UPLOADED_SUFFIX,
                meminfo_upload_strerror(errno, reason, sizeof(reason)));
        } else if (!job->keep_local) {
            if (unlink(job->path) == 0) {
                removed = 1;
            } else {
                meminfo_upload_note_add("meminfo: uploaded %s but could not remove it: %s", job->path,
                    meminfo_upload_strerror(errno, reason, sizeof(reason)));
            }
        }
        meminfo_upload_note_add("meminfo: uploaded %s to %s in %.1fs (%llu bytes)%s",
            job->path, job->url, seconds, (unsigned long long) job->size,
            removed ? ", local copy removed" : "");
    } else {
        meminfo_upload_write_sidecar(job->path, MEMINFO_UPLOAD_ERROR_SUFFIX, error);
        meminfo_upload_note_add("meminfo: upload of %s to %s failed after %.1fs: %s; the dump stays local",
            job->path, job->url, seconds, error);
    }

    free(job);

    pthread_mutex_lock(&meminfo_upload_state.lock);
    if (meminfo_upload_state.pending > 0) {
        meminfo_upload_state.pending--;
    }
    pthread_cond_broadcast(&meminfo_upload_state.idle);
    pthread_mutex_unlock(&meminfo_upload_state.lock);
}

/**
 * Whether this process may start uploads. Transport threads do not survive
 * fork(), and re-initialising a transport in a child is not safe either: its
 * library state (locks, thread lists, RNG) may have been copied mid-update
 * from a thread that no longer exists. A process forked after its parent
 * started uploading keeps its dumps local; one forked before uploads its own.
 */
static int meminfo_upload_claim_process(void)
{
    pid_t pid = getpid();

    if (meminfo_upload_state.owner == pid) {
        return 1;
    }

    if (meminfo_upload_state.owner == 0) {
        meminfo_upload_state.owner = pid;
        return 1;
    }

    // A child of a process that had transport threads. Its copy of the shared
    // state belongs to threads that do not exist here: start it afresh so that
    // nothing waits for uploads this process never started.
    if (!meminfo_upload_state.fork_reported) {
        pid_t parent = meminfo_upload_state.owner;

        pthread_mutex_init(&meminfo_upload_state.lock, NULL);
        pthread_cond_init(&meminfo_upload_state.idle, NULL);
        meminfo_upload_state.pending = 0;
        meminfo_upload_state.notes = NULL;
        meminfo_upload_state.last_note = NULL;
        meminfo_upload_state.fork_reported = 1;

        {
            char message[256];

            snprintf(message, sizeof(message),
                "meminfo: dump uploads are off in process %d: it was forked after process %d "
                "started uploading; its dumps stay local", (int) pid, (int) parent);
            php_log_err(message);
        }
    }

    return 0;
}

/**
 * The host name for the object key, reduced to characters that need no
 * encoding in a request path.
 */
static void meminfo_upload_hostname(char *buffer, size_t size)
{
    char *p;

    if (gethostname(buffer, size) != 0 || buffer[0] == '\0') {
        snprintf(buffer, size, "%s", "unknown-host");
        return;
    }
    buffer[size - 1] = '\0';

    for (p = buffer; *p != '\0'; p++) {
        char c = *p;

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-')) {
            *p = '_';
        }
    }
}

/**
 * Start uploading a finished dump file, in the background. PHP thread; never
 * fails the dump: problems end up in the log and in <dump>.upload-error.
 */
MEMINFO_HIDDEN void meminfo_upload_start(const char *path, const char *trigger)
{
    meminfo_upload_job *job;
    const char *base;
    struct stat info;
    char error[512];

    if (!meminfo_upload_enabled() || !meminfo_upload_claim_process()) {
        return;
    }

    meminfo_upload_poll();

    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        php_log_err((char *) "meminfo: cannot allocate an upload; the dump stays local");
        return;
    }

    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;

    snprintf(job->path, sizeof(job->path), "%s", path);
    snprintf(job->bucket, sizeof(job->bucket), "%s", meminfo_upload_target_info.bucket);
    snprintf(job->trigger, sizeof(job->trigger), "%s", trigger != NULL ? trigger : "");
    meminfo_upload_hostname(job->hostname, sizeof(job->hostname));
    snprintf(job->key, sizeof(job->key), "%s%s%s/%s",
        meminfo_upload_target_info.prefix, meminfo_upload_target_info.prefix[0] != '\0' ? "/" : "",
        job->hostname, base);
    snprintf(job->url, sizeof(job->url), "%s%s/%s", MEMINFO_S3_SCHEME, job->bucket, job->key);
    job->keep_local = MEMINFO_G(dump_upload_keep_local);
    job->size = stat(path, &info) == 0 ? (uint64_t) info.st_size : 0;
    job->started_usec = meminfo_now_usec();

    pthread_mutex_lock(&meminfo_upload_state.lock);
    meminfo_upload_state.pending++;
    pthread_mutex_unlock(&meminfo_upload_state.lock);

    {
        char message[MAXPATHLEN + 700];

        snprintf(message, sizeof(message), "meminfo: uploading %s to %s (%llu bytes)",
            job->path, job->url, (unsigned long long) job->size);
        php_log_err(message);
    }

    error[0] = '\0';
#ifdef MEMINFO_HAVE_S3
    if (meminfo_s3_put(job, error, sizeof(error))) {
        return;
    }
#else
    snprintf(error, sizeof(error), "%s", "no S3 transport in this build");
#endif

    // Not started: finish it right here, so the sidecar and the log tell why.
    meminfo_upload_finished(job, 0, error[0] != '\0' ? error : "the upload could not be started");
    meminfo_upload_poll();
}

/**
 * Wait for the uploads still running, up to meminfo.dump_upload_timeout, then
 * shut the transport down. MSHUTDOWN of the process that started them only:
 * a forked child never touches its parent's transport.
 */
MEMINFO_HIDDEN void meminfo_upload_module_shutdown(void)
{
    zend_long timeout = MEMINFO_G(dump_upload_timeout);
    unsigned int left;
    struct timespec deadline;
    struct timeval now;

    if (meminfo_upload_state.owner != getpid()) {
        return;
    }

    pthread_mutex_lock(&meminfo_upload_state.lock);
    left = meminfo_upload_state.pending;
    pthread_mutex_unlock(&meminfo_upload_state.lock);

    if (left > 0) {
        char message[128];

        snprintf(message, sizeof(message), "meminfo: waiting up to %lds for %u dump upload(s) before exiting",
            (long) (timeout > 0 ? timeout : 0), left);
        php_log_err(message);

        gettimeofday(&now, NULL);
        deadline.tv_sec = now.tv_sec + (timeout > 0 ? (time_t) timeout : 0);
        deadline.tv_nsec = (long) now.tv_usec * 1000;

        pthread_mutex_lock(&meminfo_upload_state.lock);
        while (meminfo_upload_state.pending > 0
            && pthread_cond_timedwait(&meminfo_upload_state.idle, &meminfo_upload_state.lock, &deadline) == 0) {
        }
        left = meminfo_upload_state.pending;
        pthread_mutex_unlock(&meminfo_upload_state.lock);
    }

#ifdef MEMINFO_HAVE_S3
    if (left > 0) {
        char message[128];

        snprintf(message, sizeof(message), "meminfo: gave up on %u dump upload(s) after %lds; the dumps stay local",
            left, (long) timeout);
        php_log_err(message);
        // Cancelled uploads still complete, with an error, which records them
        // in <dump>.upload-error before the transport goes.
        meminfo_s3_cancel_all();

        gettimeofday(&now, NULL);
        deadline.tv_sec = now.tv_sec + 5;
        deadline.tv_nsec = (long) now.tv_usec * 1000;
        pthread_mutex_lock(&meminfo_upload_state.lock);
        while (meminfo_upload_state.pending > 0
            && pthread_cond_timedwait(&meminfo_upload_state.idle, &meminfo_upload_state.lock, &deadline) == 0) {
        }
        left = meminfo_upload_state.pending;
        pthread_mutex_unlock(&meminfo_upload_state.lock);
    }

    meminfo_upload_poll();

    // With an upload still stuck in a transport thread, tearing the transport
    // down would wait for that thread: leave it to the exit instead.
    if (left == 0) {
        meminfo_s3_shutdown();
    }
#else
    meminfo_upload_poll();
#endif
}

#endif /* MEMINFO_HAVE_SIGNAL_DUMP */
