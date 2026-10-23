/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Set of visited item addresses.
 *
 * The heap is a graph full of shared values and cycles; recording every
 * address once written is what lets each item appear in the dump exactly once
 * and lets the walk terminate. Allocated outside the Zend heap, so a dump does
 * not count against memory_limit.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

/* Multiplier and shift for the pointer hash, per pointer width. */
#if defined(UINTPTR_MAX) && UINTPTR_MAX > 0xFFFFFFFFU
# define MEMINFO_PTR_HASH_MULT ((uintptr_t) 0x9E3779B97F4A7C15ULL)
# define MEMINFO_PTR_HASH_SHIFT 29
#else
# define MEMINFO_PTR_HASH_MULT ((uintptr_t) 0x9E3779B9U)
# define MEMINFO_PTR_HASH_SHIFT 15
#endif

/**
 * Fibonacci hash of an item address.
 *
 * Allocator addresses are aligned, so their low bits are constant and would
 * collide in long probe chains if used directly. The alignment bits are dropped
 * and the result mixed so that the low bits taken by the mask vary.
 */
MEMINFO_HIDDEN size_t meminfo_ptr_slot(uintptr_t ptr, size_t mask)
{
    uintptr_t h = ptr >> 3;

    h *= MEMINFO_PTR_HASH_MULT;
    h ^= h >> MEMINFO_PTR_HASH_SHIFT;

    return (size_t) h & mask;
}

/**
 * Double the set, or allocate it on first use. Returns 0 on failure.
 */
static int meminfo_ptr_set_grow(meminfo_ptr_set *set)
{
    size_t capacity = set->capacity ? set->capacity * 2 : 1024;
    uintptr_t *slots;
    size_t i;

    if (capacity < set->capacity) {   /* size_t overflow, far beyond any real heap */
        set->failed = 1;
        return 0;
    }

    slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        set->failed = 1;
        return 0;
    }

    for (i = 0; i < set->capacity; i++) {
        uintptr_t key = set->slots[i];

        if (key != 0) {
            size_t slot = meminfo_ptr_slot(key, capacity - 1);

            while (slots[slot] != 0) {
                slot = (slot + 1) & (capacity - 1);
            }
            slots[slot] = key;
        }
    }

    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;

    return 1;
}

/**
 * Record an item address as visited.
 *
 * Returns 1 when the address had already been recorded, which is also what is
 * reported once an allocation has failed, so that the traversal stops.
 */
MEMINFO_HIDDEN int meminfo_visit_item(meminfo_ptr_set *set, const void *ptr)
{
    uintptr_t key = (uintptr_t) ptr;
    size_t mask;
    size_t slot;

    if (set->failed || key == 0) {
        // Reporting "already visited" is what stops the traversal: nothing more
        // is written, and the caller still closes the JSON.
        return 1;
    }

    // Grow at half load; linear probing degrades sharply past that.
    if ((set->count + 1) * 2 > set->capacity) {
        if (!meminfo_ptr_set_grow(set)) {
            return 1;
        }
    }

    mask = set->capacity - 1;
    slot = meminfo_ptr_slot(key, mask);

    while (set->slots[slot] != 0) {
        if (set->slots[slot] == key) {
            return 1;
        }
        slot = (slot + 1) & mask;
    }

    set->slots[slot] = key;
    set->count++;

    return 0;
}
