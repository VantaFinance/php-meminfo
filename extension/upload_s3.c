/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * The "s3://" transport: dump files uploaded with aws-c-s3 (AWS CRT).
 *
 * Built only with --with-meminfo-s3. The client is created on the first upload
 * of the process, never at startup: PHP-FPM forks its workers after MINIT, and
 * the CRT's event loop threads would not survive that fork. It is sized for a
 * PHP worker, not for an EC2 throughput benchmark: one event loop thread, two
 * connections, 8 MiB parts and the smallest buffer pool the client accepts.
 *
 * Credentials come from the environment (AWS_ACCESS_KEY_ID,
 * AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN); meminfo.s3_endpoint and
 * meminfo.s3_path_style point it at S3-compatible stores (MinIO, Ceph, …).
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

#if MEMINFO_HAVE_SIGNAL_DUMP && defined(MEMINFO_HAVE_S3)

#include <pthread.h>

#include <aws/auth/credentials.h>
#include <aws/auth/signing_config.h>
#include <aws/common/thread.h>
#include <aws/common/uri.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/s3/s3.h>
#include <aws/s3/s3_client.h>

#define MEMINFO_S3_PART_SIZE (8 * 1024 * 1024)
/* The smallest buffer pool aws-c-s3 accepts; a reservation, not resident memory. */
#define MEMINFO_S3_MEMORY_LIMIT (256 * 1024 * 1024)
#define MEMINFO_S3_CONNECTIONS 2
#define MEMINFO_S3_CONNECT_TIMEOUT_MS 5000
#define MEMINFO_S3_MAX_RETRIES 3
#define MEMINFO_S3_DEFAULT_REGION "us-east-1"

/**
 * The client and what it is built on. Created on the first upload; lives
 * until MSHUTDOWN.
 */
static struct {
    int ready;
    struct aws_allocator *allocator;
    struct aws_event_loop_group *event_loop_group;
    struct aws_host_resolver *resolver;
    struct aws_client_bootstrap *bootstrap;
    struct aws_credentials_provider *credentials;
    struct aws_signing_config_aws signing_config;
    struct aws_s3_client *client;
    char region[64];

    /* uploads in flight, for cancelling them at exit */
    pthread_mutex_t lock;
    pthread_cond_t shutdown_done;
    int client_shut_down;
    struct meminfo_s3_upload *running;
} meminfo_s3 = { 0, NULL, NULL, NULL, NULL, NULL, { 0 }, NULL, { 0 },
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, NULL };

/**
 * One upload in flight.
 */
typedef struct meminfo_s3_upload {
    struct meminfo_s3_upload *prev;
    struct meminfo_s3_upload *next;
    meminfo_upload_job *job;
    struct aws_http_message *message;
    /* our hold on the meta request, dropped once it finishes */
    struct aws_s3_meta_request *meta_request;
    int finished;
    /* meminfo_s3_put() and the meta request each hold one: the request may be
     * over before make_meta_request returns */
    int refs;
    struct aws_uri endpoint;
    int endpoint_parsed;
} meminfo_s3_upload;

/**
 * The first non-empty of the setting and the environment variables.
 */
static const char *meminfo_s3_setting(const char *ini_value, const char *env1, const char *env2)
{
    const char *value;

    if (ini_value != NULL && ini_value[0] != '\0') {
        return ini_value;
    }
    value = env1 != NULL ? getenv(env1) : NULL;
    if (value != NULL && value[0] != '\0') {
        return value;
    }
    value = env2 != NULL ? getenv(env2) : NULL;

    return value != NULL && value[0] != '\0' ? value : NULL;
}

static void meminfo_s3_client_shutdown_callback(void *user_data)
{
    (void) user_data;

    pthread_mutex_lock(&meminfo_s3.lock);
    meminfo_s3.client_shut_down = 1;
    pthread_cond_broadcast(&meminfo_s3.shutdown_done);
    pthread_mutex_unlock(&meminfo_s3.lock);
}

/**
 * Release what meminfo_s3_init() built, in reverse order.
 */
static void meminfo_s3_release_base(void)
{
    if (meminfo_s3.credentials != NULL) {
        aws_credentials_provider_release(meminfo_s3.credentials);
        meminfo_s3.credentials = NULL;
    }
    if (meminfo_s3.bootstrap != NULL) {
        aws_client_bootstrap_release(meminfo_s3.bootstrap);
        meminfo_s3.bootstrap = NULL;
    }
    if (meminfo_s3.resolver != NULL) {
        aws_host_resolver_release(meminfo_s3.resolver);
        meminfo_s3.resolver = NULL;
    }
    if (meminfo_s3.event_loop_group != NULL) {
        aws_event_loop_group_release(meminfo_s3.event_loop_group);
        meminfo_s3.event_loop_group = NULL;
    }
}

/**
 * Create the client. Returns 1 when it is ready, 0 with the reason in error.
 */
static int meminfo_s3_init(char *error, size_t error_size)
{
    struct aws_event_loop_group_options loop_options;
    struct aws_host_resolver_default_options resolver_options;
    struct aws_client_bootstrap_options bootstrap_options;
    struct aws_credentials_provider_environment_options credentials_options;
    struct aws_s3_client_config config;
    const char *region;

    if (meminfo_s3.ready) {
        return 1;
    }

    region = meminfo_s3_setting(MEMINFO_G(s3_region), "AWS_REGION", "AWS_DEFAULT_REGION");
    snprintf(meminfo_s3.region, sizeof(meminfo_s3.region), "%s", region != NULL ? region : MEMINFO_S3_DEFAULT_REGION);

    meminfo_s3.allocator = aws_default_allocator();
    aws_s3_library_init(meminfo_s3.allocator);
    // A thread stuck in a transfer must not keep the process from exiting.
    aws_thread_set_managed_join_timeout_ns(2ULL * 1000 * 1000 * 1000);

    memset(&loop_options, 0, sizeof(loop_options));
    loop_options.loop_count = 1;
    meminfo_s3.event_loop_group = aws_event_loop_group_new(meminfo_s3.allocator, &loop_options);

    if (meminfo_s3.event_loop_group != NULL) {
        memset(&resolver_options, 0, sizeof(resolver_options));
        resolver_options.max_entries = 8;
        resolver_options.el_group = meminfo_s3.event_loop_group;
        meminfo_s3.resolver = aws_host_resolver_new_default(meminfo_s3.allocator, &resolver_options);
    }

    if (meminfo_s3.resolver != NULL) {
        memset(&bootstrap_options, 0, sizeof(bootstrap_options));
        bootstrap_options.event_loop_group = meminfo_s3.event_loop_group;
        bootstrap_options.host_resolver = meminfo_s3.resolver;
        meminfo_s3.bootstrap = aws_client_bootstrap_new(meminfo_s3.allocator, &bootstrap_options);
    }

    if (meminfo_s3.bootstrap != NULL) {
        memset(&credentials_options, 0, sizeof(credentials_options));
        meminfo_s3.credentials = aws_credentials_provider_new_environment(meminfo_s3.allocator, &credentials_options);
    }

    if (meminfo_s3.credentials == NULL) {
        snprintf(error, error_size, "cannot start the S3 client: %s", aws_error_debug_str(aws_last_error()));
        meminfo_s3_release_base();
        return 0;
    }

    aws_s3_init_default_signing_config(&meminfo_s3.signing_config,
        aws_byte_cursor_from_c_str(meminfo_s3.region), meminfo_s3.credentials);
    meminfo_s3.signing_config.flags.use_double_uri_encode = false;

    memset(&config, 0, sizeof(config));
    config.client_bootstrap = meminfo_s3.bootstrap;
    config.region = aws_byte_cursor_from_c_str(meminfo_s3.region);
    config.signing_config = &meminfo_s3.signing_config;
    // Enabled for the client; a request to an http:// endpoint goes without it.
    config.tls_mode = AWS_MR_TLS_ENABLED;
    config.part_size = MEMINFO_S3_PART_SIZE;
    config.memory_limit_in_bytes = MEMINFO_S3_MEMORY_LIMIT;
    config.max_active_connections_override = MEMINFO_S3_CONNECTIONS;
    config.throughput_target_gbps = 1.0;
    config.connect_timeout_ms = MEMINFO_S3_CONNECT_TIMEOUT_MS;
    config.retry_config.max_retries = MEMINFO_S3_MAX_RETRIES;
    config.shutdown_callback = meminfo_s3_client_shutdown_callback;

    meminfo_s3.client_shut_down = 0;
    meminfo_s3.client = aws_s3_client_new(meminfo_s3.allocator, &config);
    if (meminfo_s3.client == NULL) {
        snprintf(error, error_size, "cannot start the S3 client: %s", aws_error_debug_str(aws_last_error()));
        meminfo_s3_release_base();
        return 0;
    }

    meminfo_s3.ready = 1;

    return 1;
}

/**
 * The endpoint of a request: scheme, host and port, with the bucket in the
 * host name unless path-style addressing is asked for. The request path is
 * returned in path.
 */
static int meminfo_s3_endpoint(const meminfo_upload_job *job, char *endpoint, size_t endpoint_size,
    char *path, size_t path_size, char *error, size_t error_size)
{
    const char *configured = MEMINFO_G(s3_endpoint);
    int path_style = MEMINFO_G(s3_path_style);
    const char *scheme_end;
    const char *authority;
    size_t authority_len;
    int written;

    if (configured == NULL || configured[0] == '\0') {
        written = path_style
            ? snprintf(endpoint, endpoint_size, "https://s3.%s.amazonaws.com", meminfo_s3.region)
            : snprintf(endpoint, endpoint_size, "https://%s.s3.%s.amazonaws.com", job->bucket, meminfo_s3.region);
    } else {
        scheme_end = strstr(configured, "://");
        if (scheme_end == NULL) {
            snprintf(error, error_size, "meminfo.s3_endpoint \"%s\" is not a URL", configured);
            return 0;
        }
        authority = scheme_end + 3;
        authority_len = strcspn(authority, "/");
        written = path_style
            ? snprintf(endpoint, endpoint_size, "%.*s://%.*s", (int) (scheme_end - configured), configured,
                (int) authority_len, authority)
            : snprintf(endpoint, endpoint_size, "%.*s://%s.%.*s", (int) (scheme_end - configured), configured,
                job->bucket, (int) authority_len, authority);
    }

    if (written < 0 || (size_t) written >= endpoint_size) {
        snprintf(error, error_size, "%s", "the S3 endpoint is too long");
        return 0;
    }

    written = path_style
        ? snprintf(path, path_size, "/%s/%s", job->bucket, job->key)
        : snprintf(path, path_size, "/%s", job->key);
    if (written < 0 || (size_t) written >= path_size) {
        snprintf(error, error_size, "%s", "the object key is too long");
        return 0;
    }

    return 1;
}

static int meminfo_s3_add_header(struct aws_http_message *message, const char *name, const char *value)
{
    struct aws_http_header header;

    memset(&header, 0, sizeof(header));
    header.name = aws_byte_cursor_from_c_str(name);
    header.value = aws_byte_cursor_from_c_str(value);

    return aws_http_message_add_header(message, header) == AWS_OP_SUCCESS;
}

/**
 * The PUT request: path, content type and length, and what the dump is about
 * as object metadata.
 */
static struct aws_http_message *meminfo_s3_message(const meminfo_upload_job *job, const char *path)
{
    struct aws_http_message *message = aws_http_message_new_request(meminfo_s3.allocator);
    char length[24];
    char pid[16];

    if (message == NULL) {
        return NULL;
    }

    snprintf(length, sizeof(length), "%llu", (unsigned long long) job->size);
    snprintf(pid, sizeof(pid), "%d", (int) getpid());

    if (aws_http_message_set_request_method(message, aws_http_method_put) != AWS_OP_SUCCESS
        || aws_http_message_set_request_path(message, aws_byte_cursor_from_c_str(path)) != AWS_OP_SUCCESS
        || !meminfo_s3_add_header(message, "Content-Type", "application/json")
        || !meminfo_s3_add_header(message, "Content-Length", length)
        || !meminfo_s3_add_header(message, "x-amz-meta-meminfo-trigger", job->trigger)
        || !meminfo_s3_add_header(message, "x-amz-meta-meminfo-pid", pid)
        || !meminfo_s3_add_header(message, "x-amz-meta-meminfo-hostname", job->hostname)
        || !meminfo_s3_add_header(message, "x-amz-meta-meminfo-php-version", PHP_VERSION)
        || !meminfo_s3_add_header(message, "x-amz-meta-meminfo-version", MEMINFO_VERSION)) {
        aws_http_message_release(message);
        return NULL;
    }

    return message;
}

/**
 * Drop one hold on an upload; the last one unlinks it from the running list
 * and frees it.
 */
static void meminfo_s3_upload_release(meminfo_s3_upload *upload)
{
    pthread_mutex_lock(&meminfo_s3.lock);
    if (--upload->refs > 0) {
        pthread_mutex_unlock(&meminfo_s3.lock);
        return;
    }
    if (upload->prev != NULL) {
        upload->prev->next = upload->next;
    } else if (meminfo_s3.running == upload) {
        meminfo_s3.running = upload->next;
    }
    if (upload->next != NULL) {
        upload->next->prev = upload->prev;
    }
    pthread_mutex_unlock(&meminfo_s3.lock);

    if (upload->message != NULL) {
        aws_http_message_release(upload->message);
    }
    if (upload->endpoint_parsed) {
        aws_uri_clean_up(&upload->endpoint);
    }
    free(upload);
}

/**
 * The start of an error response body (S3 answers with XML) as one line of
 * text: not NUL-terminated in the buffer, and line breaks would split the log.
 */
static void meminfo_s3_error_body(const struct aws_byte_buf *body, char *out, size_t size)
{
    size_t length = body != NULL ? body->len : 0;
    size_t i;

    if (length > size - 1) {
        length = size - 1;
    }
    for (i = 0; i < length; i++) {
        unsigned char c = body->buffer[i];

        out[i] = c < 0x20 || c == 0x7f ? ' ' : (char) c;
    }
    out[length] = '\0';
}

/**
 * The meta request is over (CRT thread). Only plain C from here on.
 */
static void meminfo_s3_finish_callback(struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *result, void *user_data)
{
    meminfo_s3_upload *upload = user_data;
    char error[512];

    // The hold make_meta_request gave us. The client keeps its own while the
    // callback runs, so dropping ours here is safe — and keeps a long-lived
    // process from collecting one finished request per dump.
    pthread_mutex_lock(&meminfo_s3.lock);
    upload->finished = 1;
    upload->meta_request = NULL;
    pthread_mutex_unlock(&meminfo_s3.lock);
    aws_s3_meta_request_release(meta_request);

    if (result->error_code == AWS_ERROR_SUCCESS) {
        meminfo_upload_finished(upload->job, 1, NULL);
        upload->job = NULL;
        return;
    }

    if (result->response_status != 0) {
        char body[201];

        meminfo_s3_error_body(result->error_response_body, body, sizeof(body));
        snprintf(error, sizeof(error), "%s (HTTP %d%s%s)", aws_error_debug_str(result->error_code),
            result->response_status, body[0] != '\0' ? ": " : "", body);
    } else {
        snprintf(error, sizeof(error), "%s", aws_error_debug_str(result->error_code));
    }

    meminfo_upload_finished(upload->job, 0, error);
    upload->job = NULL;
}

/**
 * The meta request is gone (CRT thread): nothing refers to the upload any more.
 */
static void meminfo_s3_shutdown_callback(void *user_data)
{
    meminfo_s3_upload_release(user_data);
}

/**
 * Start a PUT of job->path to job->bucket/job->key. Returns 1 when it is on
 * its way — meminfo_upload_finished() then follows from a CRT thread — or 0
 * with the reason in error, the job still belonging to the caller.
 */
MEMINFO_HIDDEN int meminfo_s3_put(meminfo_upload_job *job, char *error, size_t error_size)
{
    struct aws_s3_meta_request_options options;
    struct aws_s3_meta_request *meta_request;
    meminfo_s3_upload *upload;
    char endpoint[512];
    char path[sizeof(job->key) + 80];
    struct aws_byte_cursor endpoint_cursor;

    // Refused here rather than by a signing failure after connecting: the
    // usual cause is PHP-FPM's clear_env, and the message says so.
    if (getenv("AWS_ACCESS_KEY_ID") == NULL || getenv("AWS_SECRET_ACCESS_KEY") == NULL) {
        snprintf(error, error_size, "%s",
            "no credentials: AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY are not in the environment "
            "(PHP-FPM clears it unless the pool sets env[AWS_...] or clear_env = no)");
        return 0;
    }

    if (!meminfo_s3_init(error, error_size)) {
        return 0;
    }

    if (!meminfo_s3_endpoint(job, endpoint, sizeof(endpoint), path, sizeof(path), error, error_size)) {
        return 0;
    }

    upload = calloc(1, sizeof(*upload));
    if (upload == NULL) {
        snprintf(error, error_size, "%s", "cannot allocate the upload");
        return 0;
    }
    upload->job = job;
    upload->refs = 2;

    endpoint_cursor = aws_byte_cursor_from_c_str(endpoint);
    if (aws_uri_init_parse(&upload->endpoint, meminfo_s3.allocator, &endpoint_cursor) != AWS_OP_SUCCESS) {
        snprintf(error, error_size, "cannot parse the S3 endpoint %s: %s", endpoint, aws_error_debug_str(aws_last_error()));
        free(upload);
        return 0;
    }
    upload->endpoint_parsed = 1;

    upload->message = meminfo_s3_message(job, path);
    if (upload->message == NULL) {
        snprintf(error, error_size, "cannot build the S3 request: %s", aws_error_debug_str(aws_last_error()));
        aws_uri_clean_up(&upload->endpoint);
        free(upload);
        return 0;
    }

    memset(&options, 0, sizeof(options));
    options.type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT;
    options.message = upload->message;
    options.send_filepath = aws_byte_cursor_from_c_str(job->path);
    options.endpoint = &upload->endpoint;
    options.user_data = upload;
    options.finish_callback = meminfo_s3_finish_callback;
    options.shutdown_callback = meminfo_s3_shutdown_callback;

    // Linked in before the request exists: its callbacks may run at once.
    pthread_mutex_lock(&meminfo_s3.lock);
    upload->next = meminfo_s3.running;
    if (meminfo_s3.running != NULL) {
        meminfo_s3.running->prev = upload;
    }
    meminfo_s3.running = upload;
    pthread_mutex_unlock(&meminfo_s3.lock);

    meta_request = aws_s3_client_make_meta_request(meminfo_s3.client, &options);
    if (meta_request == NULL) {
        snprintf(error, error_size, "cannot start the S3 upload to %s%s: %s",
            endpoint, path, aws_error_debug_str(aws_last_error()));
        // No callbacks follow: both holds are ours.
        upload->job = NULL;
        upload->refs = 1;
        meminfo_s3_upload_release(upload);
        return 0;
    }

    // Kept for cancelling at exit, unless it already finished.
    pthread_mutex_lock(&meminfo_s3.lock);
    if (!upload->finished) {
        upload->meta_request = meta_request;
    }
    pthread_mutex_unlock(&meminfo_s3.lock);
    meminfo_s3_upload_release(upload);

    return 1;
}

/**
 * Cancel every upload in flight; each still finishes, with an error.
 */
MEMINFO_HIDDEN void meminfo_s3_cancel_all(void)
{
    struct aws_s3_meta_request *held[64];
    meminfo_s3_upload *upload;
    size_t count = 0;
    size_t i;

    // Held outside our lock: cancelling may run callbacks that take it.
    pthread_mutex_lock(&meminfo_s3.lock);
    for (upload = meminfo_s3.running; upload != NULL && count < sizeof(held) / sizeof(held[0]); upload = upload->next) {
        if (upload->meta_request != NULL) {
            held[count++] = aws_s3_meta_request_acquire(upload->meta_request);
        }
    }
    pthread_mutex_unlock(&meminfo_s3.lock);

    for (i = 0; i < count; i++) {
        aws_s3_meta_request_cancel(held[i]);
        aws_s3_meta_request_release(held[i]);
    }
}

/**
 * Tear the client down once no upload is pending (MSHUTDOWN).
 */
MEMINFO_HIDDEN void meminfo_s3_shutdown(void)
{
    struct timespec deadline;
    struct timeval now;

    if (!meminfo_s3.ready) {
        return;
    }

    // Every upload has finished and dropped its meta request; the client
    // shuts down once the last of them is destroyed.
    aws_s3_client_release(meminfo_s3.client);
    meminfo_s3.client = NULL;

    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + 5;
    deadline.tv_nsec = (long) now.tv_usec * 1000;
    pthread_mutex_lock(&meminfo_s3.lock);
    while (!meminfo_s3.client_shut_down
        && pthread_cond_timedwait(&meminfo_s3.shutdown_done, &meminfo_s3.lock, &deadline) == 0) {
    }
    pthread_mutex_unlock(&meminfo_s3.lock);

    meminfo_s3_release_base();
    aws_s3_library_clean_up();
    meminfo_s3.ready = 0;
}

#endif /* MEMINFO_HAVE_SIGNAL_DUMP && MEMINFO_HAVE_S3 */
