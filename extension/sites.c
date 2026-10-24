/**
 * PHP Meminfo
 *
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Allocation sites: where each object was created (meminfo.track_allocations).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

/*
 * A user opcode handler on ZEND_NEW records the file and line of the "new"
 * against the handle the object is about to get — the head of the free list,
 * or the top of the store — and the next "new" (or a dump) matches it to the
 * object then in that slot, with its class, so a slot reused later by an
 * object created otherwise (unserialize(), Reflection, PDO) is not credited to
 * the old site. Objects created without "new" have no site. Off by default:
 * every "new" pays for it, and the opcache JIT does not run with user opcode
 * handlers registered.
 */

static user_opcode_handler_t meminfo_previous_new_handler = NULL;

static int meminfo_sites_reserve(uint32_t handle)
{
    uint32_t capacity;
    meminfo_site_slot *slots;

    if (handle < MEMINFO_G(sites_capacity)) {
        return 1;
    }
    capacity = EG(objects_store).size > handle ? EG(objects_store).size : handle + 1;
    slots = realloc(MEMINFO_G(sites), (size_t) capacity * sizeof(*slots));
    if (slots == NULL) {
        if (!MEMINFO_G(sites_failed)) {
            MEMINFO_G(sites_failed) = 1;
            php_log_err((char *) "meminfo: cannot grow the allocation site table; new objects get no site");
        }
        return 0;
    }
    memset(slots + MEMINFO_G(sites_capacity), 0, (size_t) (capacity - MEMINFO_G(sites_capacity)) * sizeof(*slots));
    MEMINFO_G(sites) = slots;
    MEMINFO_G(sites_capacity) = capacity;
    return 1;
}

/** Match the last "new" to the object that took its handle. */
static void meminfo_commit_pending_site(void)
{
    int handle = MEMINFO_G(pending_site_handle);
    zend_object *object;
    meminfo_site_slot *slot;

    if (handle <= 0) {
        return;
    }
    MEMINFO_G(pending_site_handle) = 0;

    if ((uint32_t) handle >= EG(objects_store).top || !meminfo_sites_reserve((uint32_t) handle)) {
        return;
    }
    object = EG(objects_store).object_buckets[handle];
    if (object == NULL || !IS_OBJ_VALID(object) || object->handle != (uint32_t) handle) {
        return;
    }
    // The "new" was for another class: its object is gone and the slot reused.
    if (MEMINFO_G(pending_site_ce) != NULL && object->ce != MEMINFO_G(pending_site_ce)) {
        return;
    }

    slot = &MEMINFO_G(sites)[handle];
    slot->object = object;
    slot->ce = object->ce;
    slot->file = MEMINFO_G(pending_site_file);
    slot->line = MEMINFO_G(pending_site_line);
}

/**
 * The class a "new" creates, when it is known without running anything: a
 * loaded class named in the code, self or parent, or a class in a variable.
 * NULL otherwise (static, a class still to be autoloaded).
 */
static zend_class_entry *meminfo_new_class(zend_execute_data *execute_data)
{
    const zend_op *opline = execute_data->opline;

    if (opline->op1_type == IS_CONST) {
#if PHP_VERSION_ID >= 70300
        zval *name = RT_CONSTANT(opline, opline->op1);
#else
        zval *name = EX_CONSTANT(opline->op1);
#endif
        // The literal after the class name is its lowercased key.
        return zend_hash_find_ptr(EG(class_table), Z_STR_P(name + 1));
    }
    if (opline->op1_type == IS_VAR) {
        return Z_CE_P(EX_VAR(opline->op1.var));
    }
    if (opline->op1_type == IS_UNUSED && execute_data->func->common.scope != NULL) {
        switch (opline->op1.num & ZEND_FETCH_CLASS_MASK) {
            case ZEND_FETCH_CLASS_SELF:
                return execute_data->func->common.scope;
            case ZEND_FETCH_CLASS_PARENT:
                return execute_data->func->common.scope->parent;
            default:
                break;
        }
    }

    return NULL;
}

static int meminfo_new_handler(zend_execute_data *execute_data)
{
    int handle;

    meminfo_commit_pending_site();

    handle = EG(objects_store).free_list_head;
#ifdef EG_FLAGS_OBJECT_STORE_NO_REUSE
    if (EG(flags) & EG_FLAGS_OBJECT_STORE_NO_REUSE) {
        handle = -1;
    }
#endif
    if (handle == -1) {
        handle = (int) EG(objects_store).top;
    }

    MEMINFO_G(pending_site_handle) = handle;
    MEMINFO_G(pending_site_file) = execute_data->func->op_array.filename;
    MEMINFO_G(pending_site_line) = execute_data->opline->lineno;
    MEMINFO_G(pending_site_ce) = meminfo_new_class(execute_data);

    return meminfo_previous_new_handler != NULL ? meminfo_previous_new_handler(execute_data) : ZEND_USER_OPCODE_DISPATCH;
}

/** Install the "new" handler at module startup, when tracking is on. */
MEMINFO_HIDDEN void meminfo_sites_module_startup(void)
{
    if (!MEMINFO_G(track_allocations)) {
        return;
    }
    meminfo_previous_new_handler = zend_get_user_opcode_handler(ZEND_NEW);
    zend_set_user_opcode_handler(ZEND_NEW, meminfo_new_handler);
}

/**
 * Where an object was created, or 0 with *file NULL when unknown.
 */
MEMINFO_HIDDEN uint32_t meminfo_object_site(const zend_object *object, zend_string **file)
{
    const meminfo_site_slot *slot;

    *file = NULL;
    meminfo_commit_pending_site();
    if (object->handle >= MEMINFO_G(sites_capacity)) {
        return 0;
    }
    slot = &MEMINFO_G(sites)[object->handle];
    if (slot->object != object || slot->ce != object->ce || slot->file == NULL) {
        return 0;
    }
    *file = slot->file;
    return slot->line;
}

/** Forget every site: the files and handles mean nothing in the next request. */
MEMINFO_HIDDEN void meminfo_sites_reset(void)
{
    if (MEMINFO_G(sites) != NULL) {
        memset(MEMINFO_G(sites), 0, (size_t) MEMINFO_G(sites_capacity) * sizeof(meminfo_site_slot));
    }
    MEMINFO_G(pending_site_handle) = 0;
}

