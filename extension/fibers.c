/* Suspended fiber contexts, tracked through the fiber observers (PHP 8.1+). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

#if PHP_VERSION_ID >= 80100
/* ------------------------------------------------------------------------- *
 * Suspended fiber contexts
 *
 * A dump starts from the running frame and walks down its chain. Frames of a
 * context that is not running are on a stack of their own, reachable only
 * through whatever suspended them: a Fiber object for native fibers, nothing
 * at all for Swoole coroutines, which keep their stacks in C++ structures.
 *
 * The fiber observers see every switch, native fibers and — with
 * swoole.enable_fiber_mock=1 — Swoole coroutines alike, so the context being
 * left is recorded with its top frame, and forgotten again as soon as it runs
 * or is destroyed. The frames stay valid in between: nothing runs on a stack
 * while it is suspended.
 * ------------------------------------------------------------------------- */

static size_t meminfo_suspended_find(const meminfo_suspended_map *map, const void *context)
{
    size_t mask = map->capacity - 1;
    size_t i = meminfo_ptr_slot((uintptr_t) context, mask);

    while (map->slots[i].context != NULL && map->slots[i].context != context) {
        i = (i + 1) & mask;
    }

    return i;
}

static void meminfo_suspended_remove(meminfo_suspended_map *map, const void *context)
{
    size_t mask, i, j;

    if (map->count == 0) {
        return;
    }

    mask = map->capacity - 1;
    i = meminfo_suspended_find(map, context);
    if (map->slots[i].context == NULL) {
        return;
    }

    // Backward-shift deletion: pull later entries of the same probe run into
    // the hole, so that lookups never need tombstones.
    j = i;
    for (;;) {
        size_t home;

        j = (j + 1) & mask;
        if (map->slots[j].context == NULL) {
            break;
        }

        home = meminfo_ptr_slot((uintptr_t) map->slots[j].context, mask);
        if ((i <= j) ? (home <= i || home > j) : (home <= i && home > j)) {
            map->slots[i] = map->slots[j];
            i = j;
        }
    }

    map->slots[i].context = NULL;
    map->slots[i].frame = NULL;
    map->count--;
}

static int meminfo_suspended_grow(meminfo_suspended_map *map)
{
    size_t capacity = map->capacity ? map->capacity * 2 : 64;
    meminfo_suspended_context *old = map->slots;
    size_t old_capacity = map->capacity;
    size_t i;

    map->slots = calloc(capacity, sizeof(*map->slots));
    if (map->slots == NULL) {
        map->slots = old;
        return 0;
    }
    map->capacity = capacity;

    for (i = 0; i < old_capacity; i++) {
        if (old[i].context != NULL) {
            map->slots[meminfo_suspended_find(map, old[i].context)] = old[i];
        }
    }

    free(old);
    return 1;
}

static void meminfo_suspended_put(meminfo_suspended_map *map, void *context, void *frame)
{
    size_t i;

    // Kept at most half full.
    if ((map->count + 1) * 2 > map->capacity && !meminfo_suspended_grow(map)) {
        // Out of memory: that context will be missing from dumps, nothing worse.
        return;
    }

    i = meminfo_suspended_find(map, context);
    if (map->slots[i].context == NULL) {
        map->slots[i].context = context;
        map->count++;
    }
    map->slots[i].frame = frame;
}

/**
 * Whether ex lies on the VM stack in use. Swoole announces a new coroutine
 * with a frame of its own on the C stack, gone once the switch returns; only
 * frames on a VM stack outlive it.
 */
static int meminfo_frame_on_vm_stack(const zend_execute_data *ex)
{
    zend_vm_stack page;

    for (page = EG(vm_stack); page != NULL; page = page->prev) {
        if ((const zval *) ex >= ZEND_VM_STACK_ELEMENTS(page) && (const zval *) ex < page->end) {
            return 1;
        }
    }

    return 0;
}

MEMINFO_HIDDEN void meminfo_fiber_switch(zend_fiber_context *from, zend_fiber_context *to)
{
    meminfo_suspended_map *map = &MEMINFO_G(suspended);

    if (to != NULL) {
        meminfo_suspended_remove(map, to);
    }
    if (from == NULL) {
        return;
    }

    meminfo_suspended_remove(map, from);

    if (from->status == ZEND_FIBER_STATUS_DEAD
        || EG(current_execute_data) == NULL
        || !meminfo_frame_on_vm_stack(EG(current_execute_data))) {
        return;
    }

    meminfo_suspended_put(map, from, EG(current_execute_data));
}

MEMINFO_HIDDEN void meminfo_fiber_destroy(zend_fiber_context *context)
{
    meminfo_suspended_remove(&MEMINFO_G(suspended), context);
}

/**
 * Walk the frames of every suspended context. Frames a Fiber object leads to
 * as well are skipped the second time, by address.
 */
MEMINFO_HIDDEN void meminfo_visit_suspended_contexts(meminfo_context *ctx)
{
    meminfo_suspended_map *map = &MEMINFO_G(suspended);
    size_t i;

    for (i = 0; i < map->capacity && !meminfo_aborted(ctx); i++) {
        if (map->slots[i].context != NULL) {
            meminfo_visit_exec_frame(ctx, (zend_execute_data *) map->slots[i].frame);
        }
    }
}
#endif
