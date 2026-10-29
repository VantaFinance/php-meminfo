/* Test-only get_gc faults for dump cleanup. Never linked into meminfo.so. */
#include "php.h"
#include "zend_exceptions.h"
#include "zend_object_handlers.h"
#include "zend_objects.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#if PHP_VERSION_ID < 80000
#error "The dump fault fixture requires PHP 8.0+"
#endif

static zend_class_entry *fault_class;
static zend_object_handlers fault_handlers;
static int fault_mode;
static void *fault_memory;

/* Linux-only fault fixture: replace this child's open dump sink with /dev/full.
 * Only private .json.tmp descriptors are candidates; normal output is untouched. */
static int fault_fail_dump_write(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    int found = 0;
    int full;

    if (directory == NULL) {
        return 0;
    }
    full = open("/dev/full", O_WRONLY | O_CLOEXEC);
    if (full < 0) {
        closedir(directory);
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char link[64];
        char path[MAXPATHLEN];
        ssize_t length;
        char *suffix;
        int fd = atoi(entry->d_name);

        if (fd < 3) {
            continue;
        }
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        length = readlink(link, path, sizeof(path) - 1);
        if (length < 0) {
            continue;
        }
        path[length] = '\0';
        suffix = strstr(path, ".json.tmp");
        if (suffix != NULL && strcmp(suffix, ".json.tmp") == 0) {
            found = dup2(full, fd) == fd;
            break;
        }
    }
    close(full);
    closedir(directory);
    return found;
}

static HashTable *fault_get_gc(zend_object *object, zval **table, int *count)
{
    int mode = fault_mode;
    fault_mode = 0;
    *table = NULL;
    *count = 0;
    if (mode == 1) {
        zend_error(E_ERROR, "controlled dump traversal failure");
    }
    if (mode == 2) {
        /* The file dump temporarily lifts the internal limit. Retain enough
         * native heap memory to make restoring the original 16 MiB fail. */
        fault_memory = emalloc(24 * 1024 * 1024);
        php_log_err("OOM_FAULT: retained native allocation during traversal");
    }
    if (mode == 3 && !fault_fail_dump_write()) {
        zend_error(E_ERROR, "controlled write fault could not locate the dump descriptor");
    }
    return zend_std_get_properties(object);
}

PHP_FUNCTION(oom_fault_create)
{
    char *mode;
    size_t length;
    zend_object *object;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &mode, &length) == FAILURE) {
        return;
    }
    if (fault_mode != 0 || fault_memory != NULL) {
        zend_throw_exception(NULL, "Only one fault object per request", 0);
        return;
    }
    if (length == 7 && memcmp(mode, "bailout", 7) == 0) {
        fault_mode = 1;
    } else if (length == 7 && memcmp(mode, "restore", 7) == 0) {
        fault_mode = 2;
    } else if (length == 5 && memcmp(mode, "write", 5) == 0) {
        fault_mode = 3;
    } else {
        zend_throw_exception(NULL, "Unknown fault mode", 0);
        return;
    }
    object = zend_objects_new(fault_class);
    object->handlers = &fault_handlers;
    ZVAL_OBJ(return_value, object);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_fault_create, 0, 0, 1)
    ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO()

static const zend_function_entry fault_functions[] = {
    PHP_FE(oom_fault_create, arginfo_fault_create)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(oom_fault)
{
    zend_class_entry entry;
    INIT_CLASS_ENTRY(entry, "OomFaultObject", NULL);
    fault_class = zend_register_internal_class(&entry);
    memcpy(&fault_handlers, zend_get_std_object_handlers(), sizeof(fault_handlers));
    fault_handlers.get_gc = fault_get_gc;
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(oom_fault)
{
    if (fault_memory != NULL) {
        efree(fault_memory);
        fault_memory = NULL;
    }
    fault_mode = 0;
    return SUCCESS;
}

zend_module_entry oom_fault_module_entry = {
    STANDARD_MODULE_HEADER,
    "oom_fault", fault_functions,
    PHP_MINIT(oom_fault), NULL, NULL, PHP_RSHUTDOWN(oom_fault), NULL,
    "0.0.1", STANDARD_MODULE_PROPERTIES
};
ZEND_GET_MODULE(oom_fault)
