/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Item emission: one dump item and its "children" block per zval.
 *
 * Turns a single value into the JSON a reader of the dump relies on: type,
 * size, class, and the links to what it holds. Objects are identified by their
 * zend_object so that every reference to one object leads to the same item,
 * which is what makes reference paths and retained sizes computable.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#include "zend_closures.h"
#include "zend_generators.h"
#include "php_memory_streams.h"
#if PHP_VERSION_ID >= 80400
#include "zend_lazy_objects.h"
#endif

/**
 * The address an item is identified by in the dump: one item per allocation.
 *
 * References and indirect slots are followed first. An object is keyed by its
 * zend_object, a refcounted string or array by its zend_string / zend_array,
 * so that every zval sharing one allocation (a string assigned to many
 * variables, a copy-on-write array, a foreach copy) leads to the same item and
 * its size is counted once. Values that are not refcounted (scalars, interned
 * strings, immutable arrays) have no allocation of their own on the request
 * heap and keep the address of the zval.
 */
static zend_always_inline int meminfo_hash_initialized(const HashTable *ht);

static const void *meminfo_item_identity(zval *value)
{
    if (Z_TYPE_P(value) == IS_INDIRECT) {
        value = Z_INDIRECT_P(value);
    }

    if (Z_ISREF_P(value)) {
        ZVAL_DEREF(value);
    }

    if (Z_TYPE_P(value) == IS_OBJECT) {
        return (const void *) Z_OBJ_P(value);
    }

    if (!Z_REFCOUNTED_P(value)) {
        return (const void *) value;
    }

    if (Z_TYPE_P(value) == IS_STRING) {
        return (const void *) Z_STR_P(value);
    }

    if (Z_TYPE_P(value) == IS_ARRAY) {
        return (const void *) Z_ARR_P(value);
    }

    return (const void *) value;
}

/**
 * Begin one entry of a "children" block, up to the opening quote of its name.
 *
 * The block is opened as `"children" : {` with no newline, so the first entry
 * starts its own line and the next ones are preceded by a comma.
 */
static void meminfo_child_entry_open(meminfo_context *ctx, int *first_child)
{
    meminfo_writer *w = ctx->w;

    if (!*first_child) {
        MEMINFO_WRITE_LIT(w, ",");
    }
    MEMINFO_WRITE_LIT(w, "\n");
    *first_child = 0;

    MEMINFO_WRITE_LIT(w, "            \"");
}

/**
 * Finish one entry of a "children" block: the name is already written, this
 * adds the identifier it points at.
 */
static void meminfo_child_entry_close(meminfo_context *ctx, const void *child)
{
    meminfo_writer *w = ctx->w;

    MEMINFO_WRITE_LIT(w, "\":\"");
    meminfo_write_ptr(w, child);
    MEMINFO_WRITE_LIT(w, "\"");
}

/**
 * Close a "children" block: on its own line after entries, `{}` when empty.
 */
static void meminfo_children_close(meminfo_context *ctx, int first_child)
{
    meminfo_writer *w = ctx->w;

    if (first_child) {
        MEMINFO_WRITE_LIT(w, "}");
        return;
    }

    MEMINFO_WRITE_LIT(w, "\n        }");
}

static void meminfo_hash_write_entries(meminfo_context *ctx, HashTable *ht, zend_bool is_object, int *first_child);

/**
 * Write the "children" block of an item: one key -> child identifier pair per
 * entry of ht. The children themselves are visited later, by the traversal
 * loop in meminfo_zval_dump().
 */
static void meminfo_hash_write_children(meminfo_context *ctx, HashTable *ht, zend_bool is_object)
{
    int first_child = 1;

    MEMINFO_WRITE_LIT(ctx->w, "        \"children\" : {");
    meminfo_hash_write_entries(ctx, ht, is_object, &first_child);
    meminfo_children_close(ctx, first_child);
}

/**
 * Write one "children" entry per element of ht, into a block already open.
 */
static void meminfo_hash_write_entries(meminfo_context *ctx, HashTable *ht, zend_bool is_object, int *first_child)
{
    meminfo_writer *w = ctx->w;
    zval *entry;

    zend_string *key;
    HashPosition pos;
    zend_ulong num_key;

    zend_hash_internal_pointer_reset_ex(ht, &pos);
    while ((entry = zend_hash_get_current_data_ex(ht, &pos)) != NULL) {
        const void *child = meminfo_item_identity(entry);

        meminfo_child_entry_open(ctx, first_child);

        switch (zend_hash_get_current_key_ex(ht, &key, &num_key, &pos)) {
            case HASH_KEY_IS_STRING:

                meminfo_write_json_escaped_key(w, key, is_object);

                break;
            case HASH_KEY_IS_LONG:
                meminfo_write_int(w, (int64_t) (zend_long) num_key);
                break;
            default:
                // Unreachable while the entry exists; leaves an empty name.
                break;
        }

        meminfo_child_entry_close(ctx, child);

        zend_hash_move_forward_ex(ht, &pos);
    }
}

/**
 * Whether an object's declared properties can be read without building its
 * property table.
 *
 * Only plain objects qualify. A class with its own get_properties handler
 * (ArrayObject, SplFixedArray, DateInterval, protobuf messages, …) may expose
 * something other than its declared slots, and an object that already has a
 * property table may additionally carry dynamic properties — both keep the
 * handler path.
 */
static int meminfo_object_has_plain_properties(zend_object *zobj)
{
    return zobj->properties == NULL
        && zobj->handlers->get_properties == zend_std_get_properties;
}

/**
 * List the declared properties of an object, in the order the engine itself
 * would have put them into the object's property table.
 *
 * Calling get_properties() instead would run rebuild_object_properties(),
 * which attaches that table to the object and leaves it there: tens of
 * megabytes of permanent growth over a large object graph, charged to the
 * process the dump is supposed to observe without disturbing.
 *
 * The order differs by version and must be reproduced exactly, because
 * unmangled private names can collide in the JSON and the last duplicate wins
 * when the dump is decoded. Returns 0 if the list could not be allocated.
 */
static int meminfo_collect_declared_properties(zend_object *zobj, meminfo_prop_slot **out_slots, uint32_t *out_count)
{
    zend_class_entry *ce = zobj->ce;
    meminfo_prop_slot *slots;
    uint32_t capacity;
    uint32_t count = 0;

    *out_slots = NULL;
    *out_count = 0;

    if (ce->default_properties_count <= 0) {
        return 1;
    }

#if PHP_VERSION_ID >= 80100
    // properties_info_table has exactly one entry per slot, so this is exact.
    capacity = (uint32_t) ce->default_properties_count;
#else
    // An upper bound derived from what the two passes below actually iterate,
    // rather than from the assumption that each emitted property owns a
    // distinct slot. That assumption holds, but if it ever stopped holding the
    // list would silently lose a child instead of saying so.
    {
        zend_class_entry *parent;

        capacity = zend_hash_num_elements(&ce->properties_info);

        for (parent = ce->parent; parent && parent->default_properties_count; parent = parent->parent) {
            capacity += zend_hash_num_elements(&parent->properties_info);
        }
    }
#endif

    if (capacity == 0) {
        return 1;
    }

    slots = malloc((size_t) capacity * sizeof(*slots));
    if (slots == NULL) {
        return 0;
    }

#if PHP_VERSION_ID >= 80100
    // 8.1 fills the property table straight from properties_info_table, i.e.
    // in slot order, parent classes first.
    {
        uint32_t i;

        for (i = 0; i < capacity; i++) {
            zend_property_info *info = ce->properties_info_table[i];

            if (info == NULL) {
                // Dead slot: left behind by a property a child class redeclared.
                continue;
            }

            slots[count].name = info->name;
            slots[count].value = OBJ_PROP(zobj, info->offset);
            count++;
        }
    }
#else
    // 7.0 - 8.0 walk ce->properties_info in hash order, which lists a class's
    // own properties before the inherited ones.
    //
    // That hash is keyed by the *unmangled* name, so when a class redeclares a
    // private property of a parent, the parent's entry is displaced and its
    // slot would be missed. The engine appends those separately, and so must
    // this: a shadowed parent private still exists as its own slot and still
    // appears in the dump.
    {
        zend_property_info *info;
        HashPosition pos;
        uint32_t flags_seen = 0;

        zend_hash_internal_pointer_reset_ex(&ce->properties_info, &pos);
        while (count < capacity
            && (info = zend_hash_get_current_data_ptr_ex(&ce->properties_info, &pos)) != NULL) {

            if (!(info->flags & ZEND_ACC_STATIC)) {
                slots[count].name = info->name;
                slots[count].value = OBJ_PROP(zobj, info->offset);
                count++;
                flags_seen |= info->flags;
            }

            zend_hash_move_forward_ex(&ce->properties_info, &pos);
        }

        // ZEND_ACC_CHANGED marks a property that redeclares an inherited one;
        // without it nothing can have been displaced.
        if (flags_seen & ZEND_ACC_CHANGED) {
            zend_class_entry *parent;

            for (parent = ce->parent; parent && parent->default_properties_count; parent = parent->parent) {
                HashPosition parent_pos;

                zend_hash_internal_pointer_reset_ex(&parent->properties_info, &parent_pos);
                while (count < capacity
                    && (info = zend_hash_get_current_data_ptr_ex(&parent->properties_info, &parent_pos)) != NULL) {

                    zend_string *key = NULL;
                    zend_ulong num_key;

                    zend_hash_get_current_key_ex(&parent->properties_info, &key, &num_key, &parent_pos);

                    if (key != NULL
                        && info->ce == parent
                        && (info->flags & ZEND_ACC_PRIVATE)
                        && !(info->flags & ZEND_ACC_STATIC)) {

                        zend_property_info *visible = zend_hash_find_ptr(&ce->properties_info, key);

                        // Emitted by the walk above already unless the name now
                        // resolves to a different slot, i.e. it was redeclared.
                        if (visible == NULL
                            || (visible->flags & ZEND_ACC_STATIC)
                            || visible->offset != info->offset) {

                            slots[count].name = info->name;
                            slots[count].value = OBJ_PROP(zobj, info->offset);
                            count++;
                        }
                    }

                    zend_hash_move_forward_ex(&parent->properties_info, &parent_pos);
                }
            }
        }
    }
#endif

    *out_slots = slots;
    *out_count = count;

    return 1;
}

/**
 * Write the "children" block from a list of declared property slots.
 *
 * Mirrors the object branch of meminfo_hash_write_children() so that both
 * sources of children produce the same bytes.
 */
/**
 * Write one "children" entry per declared property slot, into a block already
 * open.
 */
static void meminfo_slots_write_entries(meminfo_context *ctx, meminfo_prop_slot *slots, uint32_t count, int *first_child)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        const void *child = meminfo_item_identity(slots[i].value);

        meminfo_child_entry_open(ctx, first_child);

        meminfo_write_json_escaped_key(ctx->w, slots[i].name, 1);

        meminfo_child_entry_close(ctx, child);
    }
}

#if PHP_VERSION_ID >= 80400
#define MEMINFO_OBJECT_IS_LAZY(zobj) zend_object_is_lazy(zobj)
#else
#define MEMINFO_OBJECT_IS_LAZY(zobj) 0
#endif

#if PHP_VERSION_ID >= 80100
/**
 * Add what a Fiber holds besides its frames to its "children" block: the
 * callable it was created with, as "[callable]", until that callable returns,
 * and its return value, as "[result]", once it has. Its get_gc() handler
 * reports both but is not called (see below).
 */
static void meminfo_fiber_write_gc_entries(meminfo_context *ctx, zend_fiber *fiber, meminfo_level *level, int *first_child)
{
    static const char *const names[] = { "[callable]", "[result]" };
    zval *values[2];
    meminfo_gc_block *block;
    uint32_t i;

    values[0] = &fiber->fci.function_name;
    values[1] = &fiber->result;

    block = malloc(sizeof(meminfo_gc_block) + sizeof(zval));
    if (block == NULL) {
        ctx->aborted = 1;
        return;
    }

    block->count = 0;
    block->next = ctx->gc_blocks;
    ctx->gc_blocks = block;

    for (i = 0; i < 2; i++) {
        if (!Z_REFCOUNTED_P(values[i])) {
            continue;
        }

        ZVAL_COPY_VALUE(&block->values[block->count], values[i]);
        meminfo_child_entry_open(ctx, first_child);
        meminfo_write_raw(ctx->w, names[i], strlen(names[i]));
        meminfo_child_entry_close(ctx, meminfo_item_identity(&block->values[block->count]));
        block->count++;
    }

    if (block->count > 0) {
        level->gc_values = block->values;
        level->gc_count = block->count;
    }
}
#endif

/**
 * Add what an object holds outside its properties to its "children" block,
 * and record it in level to be visited after the properties.
 *
 * Closures (bound $this, use variables, static variables), generators (the
 * locals of a suspended generator), SPL containers (ArrayObject storage,
 * SplObjectStorage, SplDoublyLinkedList, SplHeap, SplFixedArray), WeakMap
 * values and objects of other extensions expose that data only to the garbage
 * collector, through get_gc(). Objects using the standard handler have nothing
 * there that their properties do not already show, so it is not called for
 * them — except lazy objects (8.4+), whose initializer and, once a proxy is
 * initialized, real instance live outside the object and are reported by the
 * standard handler without initializing anything. A Fiber's get_gc() is not
 * called either: its frames are walked as frames of their own, and the rest is
 * read by meminfo_fiber_write_gc_entries().
 *
 * get_gc() handlers are engine or extension code; none of them runs PHP code.
 * Entries from the returned table keep their keys (a closure's variable names);
 * the loose zvals are named "[0]", "[1]"… in the order the handler gave them.
 */
static void meminfo_object_write_gc_entries(meminfo_context *ctx, zval *zv, HashTable *properties, meminfo_level *level, int *first_child)
{
    zend_object *zobj = Z_OBJ_P(zv);
    zval *table = NULL;
    int n = 0;
    HashTable *gc_ht;

    if (zobj->handlers->get_gc == NULL) {
        return;
    }
    if (zobj->handlers->get_gc == std_object_handlers.get_gc && !MEMINFO_OBJECT_IS_LAZY(zobj)) {
        return;
    }
#if PHP_VERSION_ID >= 80100
    if (zobj->ce == zend_ce_fiber) {
        meminfo_fiber_write_gc_entries(ctx, (zend_fiber *) zobj, level, first_child);
        return;
    }
#endif

#if PHP_VERSION_ID >= 80000
    gc_ht = zobj->handlers->get_gc(zobj, &table, &n);
#else
    gc_ht = zobj->handlers->get_gc(zv, &table, &n);
#endif

    // Copy the loose zvals before anything else can call a get_gc() handler:
    // on PHP 8 most of them fill one buffer shared by the whole engine.
    if (table != NULL && n > 0) {
        meminfo_gc_block *block;
        int i;

        block = malloc(sizeof(meminfo_gc_block) + (size_t) (n - 1) * sizeof(zval));
        if (block == NULL) {
            ctx->aborted = 1;
            return;
        }

        block->count = 0;
        for (i = 0; i < n; i++) {
            // Only refcounted values can lead anywhere or take up memory of
            // their own; the rest would be noise.
            if (Z_REFCOUNTED(table[i])) {
                ZVAL_COPY_VALUE(&block->values[block->count], &table[i]);
                block->count++;
            }
        }

        block->next = ctx->gc_blocks;
        ctx->gc_blocks = block;

        if (block->count > 0) {
            uint32_t k;

            for (k = 0; k < block->count; k++) {
                meminfo_child_entry_open(ctx, first_child);
                MEMINFO_WRITE_LIT(ctx->w, "[");
                meminfo_write_int(ctx->w, (int64_t) k);
                MEMINFO_WRITE_LIT(ctx->w, "]");
                meminfo_child_entry_close(ctx, meminfo_item_identity(&block->values[k]));
            }

            level->gc_values = block->values;
            level->gc_count = block->count;
        }
    }

    // The standard properties table again, for handlers that start from it.
    if (gc_ht != NULL && gc_ht != properties && gc_ht != zobj->properties && zend_hash_num_elements(gc_ht) > 0) {
        meminfo_hash_write_entries(ctx, gc_ht, 0, first_child);

        level->gc_ht = gc_ht;
        zend_hash_internal_pointer_reset_ex(gc_ht, &level->gc_ht_pos);
    }
}

typedef struct {
    const char *name;
    size_t len;
} meminfo_type_name;

#define MEMINFO_TYPE_NAME(s) { s, sizeof(s) - 1 }

/**
 * The "type" of an item: what zend_get_type_by_const() returns for each value
 * type on the running version ("boolean"/"integer" before 7.3), with no strlen
 * and no escaping. Anything past IS_RESOURCE is "unknown" — on 8.1+ the engine
 * function would hit EMPTY_SWITCH_DEFAULT_CASE() there instead.
 */
static const meminfo_type_name meminfo_type_names[] = {
    /* IS_UNDEF */    MEMINFO_TYPE_NAME("null"),
    /* IS_NULL */     MEMINFO_TYPE_NAME("null"),
#if PHP_VERSION_ID >= 70300
    /* IS_FALSE */    MEMINFO_TYPE_NAME("bool"),
    /* IS_TRUE */     MEMINFO_TYPE_NAME("bool"),
    /* IS_LONG */     MEMINFO_TYPE_NAME("int"),
#else
    /* IS_FALSE */    MEMINFO_TYPE_NAME("boolean"),
    /* IS_TRUE */     MEMINFO_TYPE_NAME("boolean"),
    /* IS_LONG */     MEMINFO_TYPE_NAME("integer"),
#endif
    /* IS_DOUBLE */   MEMINFO_TYPE_NAME("float"),
    /* IS_STRING */   MEMINFO_TYPE_NAME("string"),
    /* IS_ARRAY */    MEMINFO_TYPE_NAME("array"),
    /* IS_OBJECT */   MEMINFO_TYPE_NAME("object"),
    /* IS_RESOURCE */ MEMINFO_TYPE_NAME("resource"),
};
static const meminfo_type_name meminfo_type_unknown = MEMINFO_TYPE_NAME("unknown");

static zend_always_inline const meminfo_type_name *meminfo_type_name_of(zval *zv)
{
    zend_uchar type = Z_TYPE_P(zv);

    return type <= IS_RESOURCE ? &meminfo_type_names[type] : &meminfo_type_unknown;
}

/**
 * Where a closure comes from: "function" (the method or function it wraps, or
 * {closure}, with its scope) and, for user code, "declared_at" (file:line of
 * the declaration). A listener left in a static array is a Closure among
 * thousands; this is what points at the code that registered it.
 */
static void meminfo_closure_write_location(meminfo_writer *w, zval *zv)
{
#if PHP_VERSION_ID >= 80000
    const zend_function *func = zend_get_closure_method_def(Z_OBJ_P(zv));
#else
    const zend_function *func = zend_get_closure_method_def(zv);
#endif

    if (func == NULL || func->common.function_name == NULL) {
        return;
    }

    MEMINFO_WRITE_LIT(w, "        \"function\" : \"");
    if (func->common.scope) {
        meminfo_write_json_escaped_len(w, ZSTR_VAL(func->common.scope->name), ZSTR_LEN(func->common.scope->name));
        MEMINFO_WRITE_LIT(w, "::");
    }
    meminfo_write_json_escaped_len(w, ZSTR_VAL(func->common.function_name), ZSTR_LEN(func->common.function_name));
    MEMINFO_WRITE_LIT(w, "\",\n");

    if (func->type == ZEND_USER_FUNCTION && func->op_array.filename) {
        MEMINFO_WRITE_LIT(w, "        \"declared_at\" : \"");
        meminfo_write_json_escaped_len(w, ZSTR_VAL(func->op_array.filename), ZSTR_LEN(func->op_array.filename));
        MEMINFO_WRITE_LIT(w, ":");
        meminfo_write_int(w, (int64_t) func->op_array.line_start);
        MEMINFO_WRITE_LIT(w, "\",\n");
    }
}

/**
 * The object part of an item: its class, handle and one "children" block for
 * its properties — read from the declared slots when the object has no
 * property table, from get_properties() otherwise — and what get_gc() exposes.
 * *level is set up to visit those children; a fiber's suspended frames are
 * left in level->pending_fiber.
 */
static void meminfo_item_write_object(meminfo_context *ctx, zval *zv, meminfo_level *level)
{
    meminfo_writer *w = ctx->w;
    HashTable *properties;
    zend_string *class_name;

    properties = NULL;
    class_name = Z_OBJCE_P(zv)->name;

    MEMINFO_WRITE_LIT(w, ",\n        \"class\" : \"");
    meminfo_write_json_escaped(w, ZSTR_VAL(class_name));
    MEMINFO_WRITE_LIT(w, "\",\n        \"object_handle\" : \"");
    meminfo_write_int(w, (int64_t) (int) Z_OBJ_HANDLE_P(zv));
    MEMINFO_WRITE_LIT(w, "\",\n");

    if (Z_OBJCE_P(zv) == zend_ce_closure) {
        meminfo_closure_write_location(w, zv);
    }

    if (MEMINFO_G(track_allocations)) {
        zend_string *file;
        uint32_t line = meminfo_object_site(Z_OBJ_P(zv), &file);

        if (file != NULL) {
            MEMINFO_WRITE_LIT(w, "        \"site\" : \"");
            meminfo_write_json_escaped_len(w, ZSTR_VAL(file), ZSTR_LEN(file));
            MEMINFO_WRITE_LIT(w, ":");
            meminfo_write_int(w, (int64_t) line);
            MEMINFO_WRITE_LIT(w, "\",\n");
        }
    }

    if (MEMINFO_G(epoch) > 0) {
        MEMINFO_WRITE_LIT(w, "        \"generation\" : ");
        meminfo_write_int(w, (int64_t) meminfo_object_generation(Z_OBJ_P(zv)));
        MEMINFO_WRITE_LIT(w, ",\n");
    }

    zend_object *zobj = Z_OBJ_P(zv);
    int first_child = 1;

    // One block for properties and get_gc() entries alike.
    MEMINFO_WRITE_LIT(w, "        \"children\" : {");

    if (meminfo_object_has_plain_properties(zobj)) {
        // Read the declared slots directly: asking the handler would
        // materialise a property table on the inspected object and leave
        // it there.
        meminfo_prop_slot *slots;
        uint32_t slot_count;

        if (meminfo_collect_declared_properties(zobj, &slots, &slot_count)) {
            meminfo_slots_write_entries(ctx, slots, slot_count, &first_child);

            level->slots = slots;
            level->slot_count = slot_count;
        } else {
            ctx->aborted = 1;
        }
    } else {
#if PHP_VERSION_ID >= 80000
        properties = zobj->handlers->get_properties(zobj);
        if (properties) {
            GC_TRY_ADDREF(properties);
        }
#elif PHP_VERSION_ID >= 70400
        /* PHP 7.4: get_properties takes a zval and GC_TRY_ADDREF does not exist yet */
        properties = Z_OBJ_HANDLER_P(zv, get_properties) ? Z_OBJ_HANDLER_P(zv, get_properties)(zv) : NULL;
        if (properties &&!(GC_FLAGS(properties) & GC_IMMUTABLE)) {
            GC_ADDREF(properties);
        }
#else
        /* Borrowed reference: get_properties never calls __debugInfo and needs no release */
        properties = Z_OBJ_HANDLER_P(zv, get_properties) ? Z_OBJPROP_P(zv) : NULL;
#endif

        if (properties != NULL) {
            meminfo_hash_write_entries(ctx, properties, 1, &first_child);

            // The properties table has to stay alive — and, on 7.4+, stay
            // referenced — until the whole subtree below it has been visited.
            level->ht = properties;
            zend_hash_internal_pointer_reset_ex(properties, &level->pos);
#if PHP_VERSION_ID >= 70400
            level->properties_to_release = properties;
#endif
        }
        // Some internal classes (e.g. ext-protobuf messages) return NULL
        // from get_properties: the block then holds get_gc()'s entries at
        // most, and is still written so that the item stays closed.
    }

    if (!meminfo_aborted(ctx)) {
        meminfo_object_write_gc_entries(ctx, zv, properties, level, &first_child);
    }
    meminfo_children_close(ctx, first_child);
#if PHP_VERSION_ID >= 80100
    if (Z_OBJCE_P(zv) == zend_ce_fiber) {
        zend_object *object = Z_OBJ_P(zv);
        zend_fiber *fiber = (zend_fiber *) object;
        if (fiber->execute_data) {
            level->pending_fiber = fiber->execute_data;
        }
    }
#endif
}

/**
 * Strings shorter than this get no hash: a hash of a short value can be
 * reversed by trying values, and the dump must not leak them that way.
 */
#define MEMINFO_HASHED_STRING_MIN 32

/**
 * Write "hash" for a string (option "string_hashes"), so that identical
 * strings held apart can be found: the hash the engine already cached, or one
 * computed without storing it in the string.
 */
static void meminfo_write_string_hash(meminfo_writer *w, zend_string *str)
{
    char hex[24];
    zend_ulong hash = ZSTR_H(str) ? ZSTR_H(str) : zend_hash_func(ZSTR_VAL(str), ZSTR_LEN(str));
    int len = snprintf(hex, sizeof(hex), "%016llx", (unsigned long long) hash);

    MEMINFO_WRITE_LIT(w, "        \"hash\" : \"");
    meminfo_write_raw(w, hex, (size_t) len);
    MEMINFO_WRITE_LIT(w, "\",\n");
}

/**
 * Write one item and report what is left to visit underneath it.
 *
 * Returns 0 when the item had already been visited (or when the traversal has
 * been stopped), in which case nothing was written and *level is meaningless.
 * Otherwise the item and its "children" block are written and *level describes
 * the children to visit and the resources to release afterwards.
 */
static int meminfo_item_open(meminfo_context *ctx, char *frame_label, const char *symbol_name, size_t symbol_name_len, zval *zv, meminfo_level *level)
{
    meminfo_writer *w = ctx->w;
    const void *item_ptr;

    level->ht = NULL;
    level->pos = 0;
    level->slots = NULL;
    level->slot_count = 0;
    level->slot_pos = 0;
    level->gc_ht = NULL;
    level->gc_ht_pos = 0;
    level->gc_values = NULL;
    level->gc_count = 0;
    level->gc_pos = 0;
#if PHP_VERSION_ID >= 70400
    level->properties_to_release = NULL;
#endif
#if PHP_VERSION_ID >= 80100
    level->pending_fiber = NULL;
#endif

    if (meminfo_aborted(ctx)) {
        return 0;
    }

    if (Z_TYPE_P(zv) == IS_INDIRECT) {
        zv = Z_INDIRECT_P(zv);
    }

    if (Z_ISREF_P(zv)) {
        ZVAL_DEREF(zv);
    }

    item_ptr = meminfo_item_identity(zv);

    if (meminfo_visit_item(&ctx->visited_items, item_ptr)) {
        return 0;
    }

    if (! ctx->first_element) {
        MEMINFO_WRITE_LIT(w, "\n    },\n");
    }
    ctx->first_element = 0;

    const meminfo_type_name *type_name = meminfo_type_name_of(zv);

    MEMINFO_WRITE_LIT(w, "    \"");
    meminfo_write_ptr(w, item_ptr);
    MEMINFO_WRITE_LIT(w, "\" : {\n        \"type\" : \"");
    meminfo_write_raw(w, type_name->name, type_name->len);
    MEMINFO_WRITE_LIT(w, "\",\n        \"size\" : \"");
    meminfo_write_int(w, (int64_t) meminfo_get_element_size(zv));
    MEMINFO_WRITE_LIT(w, "\",\n");

    if ((MEMINFO_G(dump_flags) & MEMINFO_DUMP_STRING_HASHES) && Z_TYPE_P(zv) == IS_STRING
        && Z_STRLEN_P(zv) >= MEMINFO_HASHED_STRING_MIN) {
        meminfo_write_string_hash(w, Z_STR_P(zv));
    }

    if (frame_label) {
        if (symbol_name) {
            MEMINFO_WRITE_LIT(w, "        \"symbol_name\" : \"");
            meminfo_write_json_escaped_len(w, symbol_name, symbol_name_len);
            MEMINFO_WRITE_LIT(w, "\",\n");
        }

        MEMINFO_WRITE_LIT(w, "        \"is_root\" : true,\n        \"frame\" : \"");
        meminfo_write_json_escaped(w, frame_label);
        MEMINFO_WRITE_LIT(w, "\"");
        if (ctx->frame_line > 0) {
            MEMINFO_WRITE_LIT(w, ",\n        \"line\" : ");
            meminfo_write_int(w, (int64_t) ctx->frame_line);
        }
    } else {
        MEMINFO_WRITE_LIT(w, "        \"is_root\" : false");
    }

    if (Z_TYPE_P(zv) == IS_RESOURCE) {
        const char *resource_type = zend_rsrc_list_get_rsrc_type(Z_RES_P(zv));

        if (resource_type) {
            MEMINFO_WRITE_LIT(w, ",\n        \"resource_type\" : \"");
            meminfo_write_json_escaped(w, resource_type);
            MEMINFO_WRITE_LIT(w, "\"");
        }
    }

    if (Z_TYPE_P(zv) == IS_OBJECT) {
        meminfo_item_write_object(ctx, zv, level);
        return 1;
    }

    if (Z_TYPE_P(zv) == IS_ARRAY) {
        // Slots allocated and slots used (deleted ones included), for spotting
        // arrays that grew and were emptied: the memory stays allocated.
        if (Z_REFCOUNTED_P(zv) && meminfo_hash_initialized(Z_ARRVAL_P(zv))) {
            MEMINFO_WRITE_LIT(w, ",\n        \"capacity\" : ");
            meminfo_write_int(w, (int64_t) Z_ARRVAL_P(zv)->nTableSize);
            MEMINFO_WRITE_LIT(w, ",\n        \"used\" : ");
            meminfo_write_int(w, (int64_t) Z_ARRVAL_P(zv)->nNumUsed);
        }
        MEMINFO_WRITE_LIT(w, ",\n");
        meminfo_hash_write_children(ctx, Z_ARRVAL_P(zv), 0);

        level->ht = Z_ARRVAL_P(zv);
        zend_hash_internal_pointer_reset_ex(level->ht, &level->pos);
    }

    return 1;
}

/**
 * Move the level meminfo_item_open() just filled onto the work stack, or close
 * it right away when the stack cannot grow. ctx->opening gives up what it owns
 * first: closing may walk fiber frames, which opens further items through it.
 */
static void meminfo_level_push_opening(meminfo_context *ctx)
{
    meminfo_level opened;
    meminfo_level *pushed;

    if (!meminfo_level_is_pending(&ctx->opening)) {
        return;
    }

    opened = ctx->opening;
    ctx->opening.slots = NULL;
#if PHP_VERSION_ID >= 70400
    ctx->opening.properties_to_release = NULL;
#endif

    pushed = meminfo_stack_push(&ctx->stack);
    if (!pushed) {
        meminfo_level_close(ctx, &opened);
        return;
    }

    *pushed = opened;
}

/**
 * Write the item behind zv and everything reachable from it.
 *
 * The object graph is walked with an explicit stack rather than by recursing:
 * a linked structure a few tens of thousands of levels deep used to overflow
 * the C stack and kill the process being inspected.
 *
 * Visiting order is identical to the recursive version — an item, then its
 * "children" block, then each child's subtree in table order — so the dump is
 * byte for byte the same.
 */
MEMINFO_HIDDEN void meminfo_zval_dump(meminfo_context *ctx, char *frame_label, const char *symbol_name, size_t symbol_name_len, zval *zv)
{
    // Levels below this one belong to an outer call: the fiber hook re-enters
    // this function while levels are already stacked, and must not drain them.
    size_t base = ctx->stack.len;

    if (meminfo_item_open(ctx, frame_label, symbol_name, symbol_name_len, zv, &ctx->opening)) {
        meminfo_level_push_opening(ctx);
    }

    while (ctx->stack.len > base) {
        meminfo_level *top = &ctx->stack.levels[ctx->stack.len - 1];
        zval *child = NULL;

        // Advance before visiting: visiting may grow — and therefore move —
        // the stack, which would leave top dangling.
        if (top->slots) {
            if (top->slot_pos < top->slot_count) {
                child = top->slots[top->slot_pos].value;
                top->slot_pos++;
            }
        } else if (top->ht) {
            child = zend_hash_get_current_data_ex(top->ht, &top->pos);

            if (child) {
                zend_hash_move_forward_ex(top->ht, &top->pos);
            }
        }

        // Then what get_gc() exposed, in the order the children block lists it.
        if (child == NULL && top->gc_values && top->gc_pos < top->gc_count) {
            child = &top->gc_values[top->gc_pos];
            top->gc_pos++;
        }
        if (child == NULL && top->gc_ht) {
            child = zend_hash_get_current_data_ex(top->gc_ht, &top->gc_ht_pos);

            if (child) {
                zend_hash_move_forward_ex(top->gc_ht, &top->gc_ht_pos);
            }
        }

        if (child == NULL) {
            meminfo_level done = *top;

            ctx->stack.len--;
            meminfo_level_close(ctx, &done);
            continue;
        }

        if (meminfo_item_open(ctx, NULL, NULL, 0, child, &ctx->opening)) {
            meminfo_level_push_opening(ctx);
        }
    }
}

/**
 * Whether a HashTable has its data block allocated yet.
 */
static zend_always_inline int meminfo_hash_initialized(const HashTable *ht)
{
#if PHP_VERSION_ID >= 70400
    return (HT_FLAGS(ht) & HASH_FLAG_UNINITIALIZED) == 0;
#else
    return (ht->u.flags & HASH_FLAG_INITIALIZED) != 0;
#endif
}

static zend_always_inline int meminfo_hash_packed(const HashTable *ht)
{
#if PHP_VERSION_ID >= 70300
    return HT_IS_PACKED(ht);
#else
    return (ht->u.flags & HASH_FLAG_PACKED) != 0;
#endif
}

/**
 * Size of a refcounted string's allocation: header and characters, rounded as
 * the allocator rounds small blocks.
 */
static zend_always_inline size_t meminfo_string_size(const zend_string *str)
{
    return ZEND_MM_ALIGNED_SIZE(_ZSTR_STRUCT_SIZE(ZSTR_LEN(str)));
}

/**
 * Size of a HashTable: its header, its data block (buckets, or zvals for a
 * packed array from 8.2, plus the hash index) and a share of every
 * non-interned string key — the whole key when only this table holds it, a
 * 1/refcount part when it is shared, so a key used by many arrays is not
 * counted many times over.
 */
static size_t meminfo_hash_size(const HashTable *ht)
{
    size_t size = sizeof(HashTable);
    Bucket *bucket, *end;

    if (!meminfo_hash_initialized(ht)) {
        return size;
    }

#if PHP_VERSION_ID >= 80200
    if (meminfo_hash_packed(ht)) {
        return size + HT_PACKED_SIZE(ht);
    }
#endif

    size += HT_SIZE(ht);
    if (meminfo_hash_packed(ht)) {
        return size;
    }

    bucket = ht->arData;
    end = bucket + ht->nNumUsed;
    for (; bucket != end; bucket++) {
        if (bucket->key == NULL || Z_TYPE(bucket->val) == IS_UNDEF || ZSTR_IS_INTERNED(bucket->key)) {
            continue;
        }
        size += meminfo_string_size(bucket->key) / GC_REFCOUNT(bucket->key);
    }

    return size;
}

/**
 * Size of a memory stream's content (php://memory, or php://temp while it is
 * still below its limit and in memory), read without calling the stream.
 */
static size_t meminfo_memory_stream_content(php_stream *stream)
{
#if PHP_VERSION_ID >= 80100
    zend_string *data;
#else
    size_t length = 0;
#endif

    if (stream->ops == &php_stream_temp_ops) {
        // php_stream_temp_data starts with the inner stream in every version.
        stream = stream->abstract ? *(php_stream **) stream->abstract : NULL;
        if (stream == NULL) {
            return 0;
        }
    }
    if (stream->ops != &php_stream_memory_ops) {
        return 0;
    }

#if PHP_VERSION_ID >= 80100
    data = php_stream_memory_get_buffer(stream);
    return data ? ZEND_MM_ALIGNED_SIZE(_ZSTR_STRUCT_SIZE(ZSTR_LEN(data))) : 0;
#else
    php_stream_memory_get_buffer(stream, &length);
    return length;
#endif
}

/**
 * What a stream resource holds besides the zend_resource: the php_stream, its
 * read buffer and, for a memory stream, its content. 0 for other resources.
 */
static size_t meminfo_stream_size(zend_resource *resource)
{
    php_stream *stream;

    if ((resource->type != php_file_le_stream() && resource->type != php_file_le_pstream()) || resource->ptr == NULL) {
        return 0;
    }

    stream = (php_stream *) resource->ptr;

    return sizeof(php_stream) + stream->readbuflen + meminfo_memory_stream_content(stream);
}

/**
 * zend_closure as declared in Zend/zend_closures.c, the same in every
 * supported version: only its size is used.
 */
typedef struct {
    zend_object std;
    zend_function func;
    zval this_ptr;
    zend_class_entry *called_scope;
    void (*orig_internal_handler)(INTERNAL_FUNCTION_PARAMETERS);
} meminfo_closure_layout;

/**
 * The struct an object's class allocates. Internal classes usually keep their
 * own data before the zend_object, handlers->offset bytes of it; the engine's
 * Closure, Generator and Fiber put the zend_object first and their data after.
 */
static size_t meminfo_object_struct_size(const zend_object *object)
{
    if (object->ce == zend_ce_closure) {
        return sizeof(meminfo_closure_layout);
    }
    if (object->ce == zend_ce_generator) {
        return sizeof(zend_generator);
    }
#if PHP_VERSION_ID >= 80100
    if (object->ce == zend_ce_fiber) {
        return sizeof(zend_fiber);
    }
#endif

    return (size_t) object->handlers->offset + sizeof(zend_object);
}

/**
 * Size of the allocation an item stands for (header "size_model" 2).
 *
 *  - object: the struct its class allocates (meminfo_object_struct_size(); a
 *    closure is some 330 bytes) with the declared property slots, and the
 *    property table when one is attached;
 *  - refcounted array: meminfo_hash_size();
 *  - refcounted string: header and characters;
 *  - resource: the zend_resource;
 *  - anything else: 0. Scalars live inside the storage of what holds them
 *    (bucket, property slot, frame), interned strings and immutable arrays
 *    outside the request heap.
 */
zend_ulong meminfo_get_element_size(zval *zv)
{
    zend_object *object;
    size_t size;

    if (!Z_REFCOUNTED_P(zv)) {
        return 0;
    }

    switch (Z_TYPE_P(zv)) {
        case IS_STRING:
            return (zend_ulong) meminfo_string_size(Z_STR_P(zv));

        case IS_ARRAY:
            return (zend_ulong) meminfo_hash_size(Z_ARRVAL_P(zv));

        case IS_OBJECT:
            object = Z_OBJ_P(zv);
            size = meminfo_object_struct_size(object) + zend_object_properties_size(object->ce);
            if (object->properties != NULL) {
                size += meminfo_hash_size(object->properties);
            }
            return (zend_ulong) size;

        case IS_RESOURCE:
            return (zend_ulong) (sizeof(zend_resource) + meminfo_stream_size(Z_RES_P(zv)));
    }

    return 0;
}

/**
 * Write a root that is not a zval: memory PHP holds outside any value, such as
 * an output buffer, as an item of the given type and size with no children.
 * identity is the address of that memory.
 */
MEMINFO_HIDDEN void meminfo_raw_root_dump(meminfo_context *ctx, const char *frame_label, const char *symbol_name, const void *identity, const char *type, size_t size)
{
    meminfo_writer *w = ctx->w;

    if (identity == NULL || meminfo_aborted(ctx) || meminfo_visit_item(&ctx->visited_items, identity)) {
        return;
    }

    if (! ctx->first_element) {
        MEMINFO_WRITE_LIT(w, "\n    },\n");
    }
    ctx->first_element = 0;

    MEMINFO_WRITE_LIT(w, "    \"");
    meminfo_write_ptr(w, identity);
    MEMINFO_WRITE_LIT(w, "\" : {\n        \"type\" : \"");
    meminfo_write_json_escaped(w, type);
    MEMINFO_WRITE_LIT(w, "\",\n        \"size\" : \"");
    meminfo_write_int(w, (int64_t) size);
    MEMINFO_WRITE_LIT(w, "\",\n        \"symbol_name\" : \"");
    meminfo_write_json_escaped(w, symbol_name);
    MEMINFO_WRITE_LIT(w, "\",\n        \"is_root\" : true,\n        \"frame\" : \"");
    meminfo_write_json_escaped(w, frame_label);
    MEMINFO_WRITE_LIT(w, "\"");
}
