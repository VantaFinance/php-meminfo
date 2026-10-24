/**
 * PHP Meminfo
 *
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Object generations: meminfo_mark() and the "generation" of each object, and
 * meminfo_histogram(), both read from the object store.
 *
 * A mark records which objects are alive; an object's generation is the mark
 * that first saw it, and objects created after the last mark are one newer.
 * Objects of an old generation still alive in a later dump are what a leak
 * looks like (YourKit generations, JProfiler's mark heap). Nothing hooks object
 * creation: the cost is paid at mark time, one pass over the object store.
 *
 * The table is indexed by handle and remembers the object and its class at
 * the time it was seen, so a handle freed and reused by another object starts
 * a generation of its own. It is malloc'ed, outside memory_limit, kept for the
 * request and reset with it: handles mean nothing in the next one.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

/**
 * Record every live object not seen yet under a new generation. Returns the
 * new generation, or 0 when the table could not grow (nothing is recorded).
 */
MEMINFO_HIDDEN uint32_t meminfo_mark(void)
{
    uint32_t top = EG(objects_store).top;
    uint32_t handle;
    uint32_t epoch;

    if (top > MEMINFO_G(generations_capacity)) {
        uint32_t capacity = EG(objects_store).size > top ? EG(objects_store).size : top;
        meminfo_generation_slot *slots = realloc(MEMINFO_G(generations), (size_t) capacity * sizeof(*slots));

        if (slots == NULL) {
            return 0;
        }
        memset(slots + MEMINFO_G(generations_capacity), 0, (size_t) (capacity - MEMINFO_G(generations_capacity)) * sizeof(*slots));
        MEMINFO_G(generations) = slots;
        MEMINFO_G(generations_capacity) = capacity;
    }

    epoch = ++MEMINFO_G(epoch);

    for (handle = 1; handle < top; handle++) {
        zend_object *object = EG(objects_store).object_buckets[handle];
        meminfo_generation_slot *slot = &MEMINFO_G(generations)[handle];

        if (object == NULL || !IS_OBJ_VALID(object)) {
            continue;
        }
        if (slot->object == object && slot->ce == object->ce) {
            continue;
        }
        slot->object = object;
        slot->ce = object->ce;
        slot->generation = epoch;
    }

    return epoch;
}

/**
 * The generation of an object: the mark that first saw it, or one past the
 * last mark for an object created since. 0 when nothing was ever marked.
 */
MEMINFO_HIDDEN uint32_t meminfo_object_generation(const zend_object *object)
{
    const meminfo_generation_slot *slot;

    if (MEMINFO_G(epoch) == 0) {
        return 0;
    }
    if (object->handle >= MEMINFO_G(generations_capacity)) {
        return MEMINFO_G(epoch) + 1;
    }

    slot = &MEMINFO_G(generations)[object->handle];

    return slot->object == object && slot->ce == object->ce ? slot->generation : MEMINFO_G(epoch) + 1;
}

/** Forget every mark: called when the request ends. */
MEMINFO_HIDDEN void meminfo_generations_reset(void)
{
    if (MEMINFO_G(generations) != NULL) {
        memset(MEMINFO_G(generations), 0, (size_t) MEMINFO_G(generations_capacity) * sizeof(meminfo_generation_slot));
    }
    MEMINFO_G(epoch) = 0;
}

/**
 * meminfo_mark(): int
 *
 * Start a new generation and return its number: objects alive now keep the
 * generation they had, or get this one; objects created from now on belong to
 * the next.
 */
PHP_FUNCTION(meminfo_mark)
{
    uint32_t epoch;

    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    epoch = meminfo_mark();
    if (epoch == 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "meminfo_mark(): cannot allocate the generation table");
        return;
    }

    RETURN_LONG((zend_long) epoch);
}

typedef struct {
    zend_class_entry *ce;
    zend_long count;
    zend_long size;
} meminfo_histogram_row;

/**
 * meminfo_histogram(): array
 *
 * Instances and bytes per class of every live object, from the object store
 * alone: no graph, no JSON, cheap enough to call every few minutes in a worker
 * and watch for a class that only grows (jmap -histo). Bytes are what
 * meminfo_dump() reports as the objects' own size.
 */
PHP_FUNCTION(meminfo_histogram)
{
    meminfo_histogram_row *rows = NULL;
    HashTable by_class;
    uint32_t handle, count = 0, i;

    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    // Counted by class entry first — one integer-keyed lookup per object —
    // and turned into the array keyed by class name once at the end.
    zend_hash_init(&by_class, 64, NULL, NULL, 0);

    for (handle = 1; handle < EG(objects_store).top; handle++) {
        zend_object *object = EG(objects_store).object_buckets[handle];
        zval object_zval, *slot, index;

        if (object == NULL || !IS_OBJ_VALID(object)) {
            continue;
        }

        slot = zend_hash_index_find(&by_class, (zend_ulong) (uintptr_t) object->ce);
        if (slot == NULL) {
            if ((count & (count - 1)) == 0) {
                meminfo_histogram_row *grown = erealloc(rows, (size_t) (count ? count * 2 : 16) * sizeof(*rows));
                rows = grown;
            }
            rows[count].ce = object->ce;
            rows[count].count = 0;
            rows[count].size = 0;
            ZVAL_LONG(&index, (zend_long) count);
            slot = zend_hash_index_add_new(&by_class, (zend_ulong) (uintptr_t) object->ce, &index);
            count++;
        }

        ZVAL_OBJ(&object_zval, object);
        rows[Z_LVAL_P(slot)].count++;
        rows[Z_LVAL_P(slot)].size += (zend_long) meminfo_get_element_size(&object_zval);
    }

    zend_hash_destroy(&by_class);

    array_init_size(return_value, count);
    for (i = 0; i < count; i++) {
        zval row;

        array_init_size(&row, 2);
        add_assoc_long(&row, "count", rows[i].count);
        add_assoc_long(&row, "size", rows[i].size);
        zend_hash_update(Z_ARRVAL_P(return_value), rows[i].ce->name, &row);
    }

    if (rows != NULL) {
        efree(rows);
    }
}
