/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Traversal: work stack, execution frames and the other roots of the dump.
 *
 * Decides what the dump starts from — the frames of the running code, globals,
 * class statics, static variables, registered callbacks, constants — and walks
 * everything they reach with an explicit work stack instead of recursion, so
 * that arbitrarily deep structures cannot overflow the C stack.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"
#include "ext/standard/basic_functions.h"
#include "ext/standard/file.h"
#include "zend_exceptions.h"
#include "SAPI.h"

// The session save handler is read from the session module's globals, found
// through the module registry rather than linked against: ext/session may be a
// shared module loaded after this one. Only in non-thread-safe builds, where
// the module entry points straight at its globals.
#if !defined(ZTS) && defined(__has_include)
#if __has_include("ext/session/php_session.h")
#include "ext/session/php_session.h"
#define MEMINFO_HAVE_SESSION_GLOBALS 1
#endif
#endif

// Below this much room left under memory_limit, internal functions that list
// roots are not called: their result is allocated from the Zend heap.
#define MEMINFO_CALL_HEADROOM (256 * 1024)

// Signal numbers asked of pcntl_signal_get_handler(): the range it accepts
// without a warning (up to 8.1, printed into the process's output) or a
// ValueError (8.2, 8.3): 1 to 32 before PHP 8.4, 1 to NSIG - 1 since.
#if PHP_VERSION_ID < 80400
#define MEMINFO_SIGNAL_COUNT 33
#elif defined(NSIG)
#define MEMINFO_SIGNAL_COUNT NSIG
#else
#define MEMINFO_SIGNAL_COUNT 65
#endif

/**
 * Reserve one more level on the work stack.
 *
 * Returns NULL when the stack could not grow; the caller must then stop
 * descending, but the dump still has to be closed as valid JSON.
 */
MEMINFO_HIDDEN meminfo_level *meminfo_stack_push(meminfo_stack *stack)
{
    if (stack->len == stack->capacity) {
        size_t capacity = stack->capacity ? stack->capacity * 2 : 64;
        meminfo_level *levels = realloc(stack->levels, capacity * sizeof(*levels));

        if (levels == NULL) {
            stack->exhausted = 1;
            return NULL;
        }

        stack->levels = levels;
        stack->capacity = capacity;
    }

    return &stack->levels[stack->len++];
}

/**
 * Finish a level once its whole subtree has been written.
 */
MEMINFO_HIDDEN void meminfo_level_close(meminfo_context *ctx, meminfo_level *level)
{
    free(level->slots);
    level->slots = NULL;

#if PHP_VERSION_ID >= 70400
    if (level->properties_to_release) {
        zend_release_properties(level->properties_to_release);
    }
#endif
#if PHP_VERSION_ID >= 80100
    if (level->pending_fiber) {
        meminfo_visit_exec_frame(ctx, level->pending_fiber);
    }
#else
    (void) ctx;
#endif
}

/**
 * Whether a level still owns work: children to visit, or something to close.
 */
MEMINFO_HIDDEN int meminfo_level_is_pending(const meminfo_level *level)
{
    if (level->ht) {
        return 1;
    }
    if (level->slots) {
        return 1;
    }
    if (level->gc_ht || level->gc_values) {
        return 1;
    }
#if PHP_VERSION_ID >= 70400
    if (level->properties_to_release) {
        return 1;
    }
#endif
#if PHP_VERSION_ID >= 80100
    if (level->pending_fiber) {
        return 1;
    }
#endif
    return 0;
}

static void meminfo_browse_callbacks(meminfo_context *ctx);
static void meminfo_browse_class_static_members(meminfo_context *ctx);
static void meminfo_browse_constants(meminfo_context *ctx);
static void meminfo_browse_output_buffers(meminfo_context *ctx);
static void meminfo_browse_unreachable_objects(meminfo_context *ctx);
static void meminfo_visit_frame_extras(meminfo_context *ctx, char *frame_label, zend_execute_data *frame);
#if PHP_VERSION_ID >= 80100
static void meminfo_browse_class_constants(meminfo_context *ctx);
#endif
static void meminfo_browse_function_static_variables(meminfo_context *ctx);
static void meminfo_browse_zvals_from_symbol_table(meminfo_context *ctx, char *frame_label, HashTable *p_symbol_table);

/**
 * Header keys on the process the dump comes from: what produced it, when, and
 * memory as the system and the garbage collector see it.
 */
static void meminfo_write_header_process(meminfo_writer *w)
{
    size_t rss = meminfo_self_rss();
#if PHP_VERSION_ID >= 70300
    zend_gc_status gc;
#endif

    MEMINFO_WRITE_LIT(w, ",\n    \"php_version\" : \"");
    meminfo_write_json_escaped(w, PHP_VERSION);
    MEMINFO_WRITE_LIT(w, "\",\n    \"sapi\" : \"");
    meminfo_write_json_escaped(w, sapi_module.name ? sapi_module.name : "");
    MEMINFO_WRITE_LIT(w, "\"");
    if (MEMINFO_G(dump_trigger) != NULL) {
        MEMINFO_WRITE_LIT(w, ",\n    \"trigger\" : \"");
        meminfo_write_json_escaped(w, MEMINFO_G(dump_trigger));
        MEMINFO_WRITE_LIT(w, "\"");
    }
#ifndef PHP_WIN32
    MEMINFO_WRITE_LIT(w, ",\n    \"pid\" : ");
    meminfo_write_int(w, (int64_t) getpid());
#endif
    MEMINFO_WRITE_LIT(w, ",\n    \"timestamp\" : ");
    meminfo_write_int(w, (int64_t) time(NULL));
    MEMINFO_WRITE_LIT(w, ",\n    \"memory_limit\" : ");
    meminfo_write_int(w, (int64_t) PG(memory_limit));
    if (rss > 0) {
        MEMINFO_WRITE_LIT(w, ",\n    \"rss\" : ");
        meminfo_write_int(w, (int64_t) rss);
    }
    MEMINFO_WRITE_LIT(w, ",\n    \"object_handles\" : ");
    meminfo_write_int(w, (int64_t) EG(objects_store).top);
#if PHP_VERSION_ID >= 70300
    zend_gc_get_status(&gc);
    MEMINFO_WRITE_LIT(w, ",\n    \"gc_runs\" : ");
    meminfo_write_int(w, (int64_t) gc.runs);
    MEMINFO_WRITE_LIT(w, ",\n    \"gc_collected\" : ");
    meminfo_write_int(w, (int64_t) gc.collected);
    MEMINFO_WRITE_LIT(w, ",\n    \"gc_threshold\" : ");
    meminfo_write_int(w, (int64_t) gc.threshold);
    MEMINFO_WRITE_LIT(w, ",\n    \"gc_roots\" : ");
    meminfo_write_int(w, (int64_t) gc.num_roots);
#endif
}

/**
 * Header keys on memory no item accounts for: the VM stack, and the classes
 * and functions compiled so far — code declared at runtime (eval(), proxy
 * generators) is never freed, and a count growing from dump to dump shows it.
 */
static void meminfo_write_header_stats(meminfo_writer *w)
{
    zend_vm_stack page;
    zend_class_entry *ce;
    zend_function *func;
    size_t vm_stack = 0;
    uint32_t user_classes = 0, user_functions = 0;

    for (page = EG(vm_stack); page != NULL; page = page->prev) {
        vm_stack += (size_t) ((char *) page->end - (char *) page);
    }

    ZEND_HASH_FOREACH_PTR(CG(class_table), ce) {
        user_classes += ce->type == ZEND_USER_CLASS;
    } ZEND_HASH_FOREACH_END();
    ZEND_HASH_FOREACH_PTR(CG(function_table), func) {
        user_functions += func->type == ZEND_USER_FUNCTION;
    } ZEND_HASH_FOREACH_END();

    MEMINFO_WRITE_LIT(w, ",\n    \"vm_stack_size\" : ");
    meminfo_write_int(w, (int64_t) vm_stack);
    MEMINFO_WRITE_LIT(w, ",\n    \"class_count\" : ");
    meminfo_write_int(w, (int64_t) zend_hash_num_elements(CG(class_table)));
    MEMINFO_WRITE_LIT(w, ",\n    \"user_class_count\" : ");
    meminfo_write_int(w, (int64_t) user_classes);
    MEMINFO_WRITE_LIT(w, ",\n    \"user_function_count\" : ");
    meminfo_write_int(w, (int64_t) user_functions);

    meminfo_write_header_process(w);
}

/**
 * Write the whole document: header, every root and what it reaches, closing.
 */
static void meminfo_dump_walk(meminfo_context *ctx, zend_execute_data *start_frame, int include_global_symbol_table)
{
    MEMINFO_WRITE_LIT(ctx->w, "{\n");

    MEMINFO_WRITE_LIT(ctx->w, "  \"header\" : {\n");
    MEMINFO_WRITE_LIT(ctx->w, "    \"memory_usage\" : ");
    meminfo_write_int(ctx->w, (int64_t) zend_memory_usage(0));
    MEMINFO_WRITE_LIT(ctx->w, ",\n    \"memory_usage_real\" : ");
    meminfo_write_int(ctx->w, (int64_t) zend_memory_usage(1));
    MEMINFO_WRITE_LIT(ctx->w, ",\n    \"peak_memory_usage\" : ");
    meminfo_write_int(ctx->w, (int64_t) zend_memory_peak_usage(0));
    MEMINFO_WRITE_LIT(ctx->w, ",\n    \"peak_memory_usage_real\" : ");
    meminfo_write_int(ctx->w, (int64_t) zend_memory_peak_usage(1));
    // 2: one item per allocation (zend_object, refcounted zend_string or
    // zend_array). Dumps without the key keyed strings and arrays by zval.
    MEMINFO_WRITE_LIT(ctx->w, ",\n    \"identity_model\" : 2");
    // 2: "size" is the allocation an item stands for (meminfo_get_element_size()),
    // 0 for values stored inside their holder. Before: a zval plus a header.
    MEMINFO_WRITE_LIT(ctx->w, ",\n    \"size_model\" : 2");
    meminfo_write_header_stats(ctx->w);
    MEMINFO_WRITE_LIT(ctx->w, "\n  },\n");

    MEMINFO_WRITE_LIT(ctx->w, "  \"items\": {\n");
    meminfo_visit_exec_frame(ctx, start_frame);
#if PHP_VERSION_ID >= 80100
    meminfo_visit_suspended_contexts(ctx);
#endif

    if (include_global_symbol_table) {
        char global_label[] = "<GLOBAL>";

        // Harmless when the main frame was on the chain: those zvals are
        // already recorded and get skipped by address.
        meminfo_browse_zvals_from_symbol_table(ctx, global_label, &EG(symbol_table));
    }

    meminfo_browse_class_static_members(ctx);
    meminfo_browse_function_static_variables(ctx);
    meminfo_browse_callbacks(ctx);
    meminfo_browse_constants(ctx);
#if PHP_VERSION_ID >= 80100
    meminfo_browse_class_constants(ctx);
#endif
    if (MEMINFO_G(dump_flags) & MEMINFO_DUMP_UNREACHABLE) {
        meminfo_browse_unreachable_objects(ctx);
    }

    // Every item leaves its body open without a trailing newline: close the
    // last one here, if there was one, then "items" and the document.
    if (! ctx->first_element) {
        MEMINFO_WRITE_LIT(ctx->w, "\n    }\n");
    }
    MEMINFO_WRITE_LIT(ctx->w, "  }\n}\n");

    meminfo_writer_flush(ctx->w);
}

/**
 * Run the walk, catching a Zend bailout. Returns 1 if one happened.
 *
 * A fatal error raised mid-walk — memory_limit hit in a get_properties() or
 * get_gc() handler — longjmps out of the walk. The zend_try lives here rather
 * than in meminfo_dump_to_writer() so that ctx, which the walk changes, belongs
 * to a frame other than the one calling setjmp and keeps its values.
 */
static int meminfo_dump_walk_guarded(meminfo_context *ctx, zend_execute_data *start_frame, int include_global_symbol_table)
{
    int bailed_out = 0;

    zend_try {
        meminfo_dump_walk(ctx, start_frame, include_global_symbol_table);
    } zend_catch {
        bailed_out = 1;
    } zend_end_try();

    return bailed_out;
}

/* Release what a level owns without walking anything (no pending fiber). */
static void meminfo_level_release(meminfo_level *level)
{
    free(level->slots);
    level->slots = NULL;
#if PHP_VERSION_ID >= 70400
    if (level->properties_to_release) {
        zend_release_properties(level->properties_to_release);
        level->properties_to_release = NULL;
    }
#endif
}

/**
 * Free everything the walk allocated. Levels are left on the stack, and the
 * level being opened still owns something, only when the walk was cut short by
 * a bailout: their resources are released, nothing more is walked.
 */
static void meminfo_context_free(meminfo_context *ctx)
{
    while (ctx->stack.len > 0) {
        meminfo_level_release(&ctx->stack.levels[--ctx->stack.len]);
    }

    // Only owns something when a bailout hit meminfo_item_open() mid-way.
    meminfo_level_release(&ctx->opening);

    free(ctx->stack.levels);
    free(ctx->visited_items.slots);

    while (ctx->gc_blocks) {
        meminfo_gc_block *next = ctx->gc_blocks->next;

        free(ctx->gc_blocks);
        ctx->gc_blocks = next;
    }

    zval_ptr_dtor(&ctx->call_results);
    ZVAL_UNDEF(&ctx->call_results);
}

int meminfo_dump_to_writer(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table)
{
    meminfo_context ctx;
    int bailed_out;
    int status;

    ctx.w = w;
    ctx.visited_items.slots = NULL;
    ctx.visited_items.capacity = 0;
    ctx.visited_items.count = 0;
    ctx.visited_items.failed = 0;
    ctx.first_element = 1;
    ctx.stack.levels = NULL;
    ctx.stack.len = 0;
    ctx.stack.capacity = 0;
    ctx.stack.exhausted = 0;
    ctx.aborted = 0;
    ctx.gc_blocks = NULL;
    ZVAL_UNDEF(&ctx.call_results);
    ctx.frame_line = 0;
    memset(&ctx.opening, 0, sizeof(ctx.opening));

    bailed_out = meminfo_dump_walk_guarded(&ctx, start_frame, include_global_symbol_table);

    // In increasing order of precedence: a failed write outranks a cut-short walk.
    status = MEMINFO_DUMP_COMPLETE;
    if (meminfo_aborted(&ctx)) {
        status = MEMINFO_DUMP_INCOMPLETE;
    }
    if (w->err) {
        status = MEMINFO_DUMP_WRITE_FAILED;
    }

    meminfo_context_free(&ctx);

    if (bailed_out) {
        zend_bailout();
    }

    return status;
}

/**
 * Walk a chain of execution frames and record the variables each one declares.
 *
 * EG(current_execute_data) is deliberately never written here. Repointing it
 * corrupts the engine state when the chain being walked is not the running one
 * — most visibly for a suspended Fiber, whose frames are on a foreign stack.
 */
MEMINFO_HIDDEN void meminfo_visit_exec_frame(meminfo_context *ctx, zend_execute_data *start_frame)
{
    zend_execute_data *exec_frame;
    zend_op_array *op_array;
    // last_var is a signed int in every supported version, and
    // ZEND_CALL_VAR_NUM casts its index to int as well.
    int i;

    char frame_label[500];
    // Fiber frames are walked from inside the walk of another frame's roots.
    uint32_t saved_line = ctx->frame_line;

    for (exec_frame = start_frame; exec_frame && !meminfo_aborted(ctx); exec_frame = exec_frame->prev_execute_data) {
        int internal;

        if (!exec_frame->func) {
            continue;
        }

        // An internal frame (a call into a C function) declares no PHP
        // variables: only its arguments, $this and closure are its own. The
        // frame a dump starts from, when internal, is the meminfo function
        // doing the dump.
        // A nameless internal frame is a placeholder of the engine, such as
        // the bottom frame of a fiber's stack, with nothing in it.
        internal = !ZEND_USER_CODE(exec_frame->func->common.type);
        if (internal && (exec_frame == start_frame || exec_frame->func->common.function_name == NULL)) {
            continue;
        }

        if (meminfo_visit_item(&ctx->visited_items, exec_frame)) {
            continue;
        }

        // A frame with nothing below it is the main script — or the first
        // frame of a coroutine (OpenSwoole), which still has a function name.
        // The line the frame is at, written with each of its roots.
        ctx->frame_line = !internal && exec_frame->opline ? exec_frame->opline->lineno : 0;

        if (internal || exec_frame->prev_execute_data || exec_frame->func->common.function_name) {
            meminfo_build_frame_label(frame_label, sizeof(frame_label), exec_frame);
        } else {
            snprintf(frame_label, sizeof(frame_label), "<GLOBAL>");
        }

        if (internal) {
            meminfo_visit_frame_extras(ctx, frame_label, exec_frame);
            continue;
        }

        // The global scope, include/eval and any frame reached by $$var or
        // extract() keep their variables in a real symbol table, which can hold
        // names beyond the compiled variables below.
        //
        // From 7.1 on, the presence of that table is recorded in the call info:
        // ex->symbol_table is left uninitialised when there is none, so the
        // pointer alone must not be trusted. PHP 7.0 has no such flag and
        // zeroes the pointer instead, which is what the engine checks there.
#if PHP_VERSION_ID >= 70100
        if (ZEND_CALL_INFO(exec_frame) & ZEND_CALL_HAS_SYMBOL_TABLE) {
#else
        if (exec_frame->symbol_table != NULL) {
#endif
            if (exec_frame->symbol_table) {
                meminfo_browse_zvals_from_symbol_table(ctx, frame_label, exec_frame->symbol_table);
            }
            meminfo_visit_frame_extras(ctx, frame_label, exec_frame);
            continue;
        }

        // Read the compiled variables straight out of the frame. Calling
        // zend_rebuild_symbol_table() instead would allocate a table per
        // frame and leave ZEND_CALL_HAS_SYMBOL_TABLE set on the process
        // being inspected, which the dump must not do.
        op_array = &exec_frame->func->op_array;

        for (i = 0; i < op_array->last_var; i++) {
            meminfo_zval_dump(ctx, frame_label, ZSTR_VAL(op_array->vars[i]), ZSTR_LEN(op_array->vars[i]),
                ZEND_CALL_VAR_NUM(exec_frame, i));
        }

        meminfo_visit_frame_extras(ctx, frame_label, exec_frame);
    }

    ctx->frame_line = saved_line;
}

/**
 * Go through static members of classes
 */
static void meminfo_browse_class_static_members(meminfo_context *ctx)
{
    HashPosition ce_pos;
    HashPosition prop_pos;
    zend_class_entry *class_entry;
    zend_property_info * prop_info;

    char frame_label[500];
    char symbol_name[500];
    const char *prop_name, *class_name;
    zval * prop;

    zend_hash_internal_pointer_reset_ex(CG(class_table), &ce_pos);
    while (!meminfo_aborted(ctx)
        && (class_entry = zend_hash_get_current_data_ptr_ex(CG(class_table), &ce_pos)) != NULL) {

#if PHP_VERSION_ID >= 70400
        if (class_entry->default_static_members_count > 0 && ZEND_MAP_PTR(class_entry->static_members_table) && CE_STATIC_MEMBERS(class_entry)) {
#else
        if (class_entry->static_members_table) {
#endif

            HashTable *properties_info = &(class_entry->properties_info);

            zend_hash_internal_pointer_reset_ex(properties_info, &prop_pos);

            while ((prop_info = zend_hash_get_current_data_ptr_ex(properties_info, &prop_pos)) != NULL) {

                if (prop_info->flags & ZEND_ACC_STATIC) {
                    snprintf(frame_label, sizeof(frame_label), "<CLASS_STATIC_MEMBER>");
#if PHP_VERSION_ID >= 70400
                    prop = CE_STATIC_MEMBERS(class_entry) + prop_info->offset;
#else
                    prop = &class_entry->static_members_table[prop_info->offset];
#endif

                    zend_unmangle_property_name(prop_info->name, &class_name, &prop_name);

                    if (class_name) {
                        snprintf(symbol_name, sizeof(symbol_name), "%s::%s",  class_name, prop_name);
                    } else {
                        snprintf(symbol_name, sizeof(symbol_name), "%s::%s",  ZSTR_VAL(class_entry->name), ZSTR_VAL(prop_info->name));
                    }

                    // A plain buffer, not a zend_string: the walk allocates
                    // nothing from the Zend heap, so it cannot hit memory_limit.
                    meminfo_zval_dump(ctx, frame_label, symbol_name, strlen(symbol_name), prop);
                }

                zend_hash_move_forward_ex(properties_info, &prop_pos);
            }
        }

        zend_hash_move_forward_ex(CG(class_table), &ce_pos);
    }
}

/**
 * Dump one root that lives outside any frame, symbol table or class, under a
 * symbol name made up on the spot. Undefined slots are skipped.
 */
static void meminfo_dump_named_root(meminfo_context *ctx, char *frame_label, const char *name, zval *zv)
{
    if (zv == NULL || Z_TYPE_P(zv) == IS_UNDEF || meminfo_aborted(ctx)) {
        return;
    }

    meminfo_zval_dump(ctx, frame_label, name, strlen(name), zv);
}

/**
 * Same, for a root the engine keeps as a bare zend_object. Objects are
 * identified by their zend_object, so a zval on the C stack is enough.
 */
static void meminfo_dump_named_object_root(meminfo_context *ctx, char *frame_label, const char *name, zend_object *object)
{
    zval holder;

    if (object == NULL) {
        return;
    }

    ZVAL_OBJ(&holder, object);
    meminfo_dump_named_root(ctx, frame_label, name, &holder);
}

#if PHP_VERSION_ID >= 80200
/**
 * Dump the arguments already passed to calls a frame is preparing. Which slots
 * of such a call hold an argument yet is only known from the opcodes, so the
 * engine's own zend_unfinished_calls_gc() lists them, into the engine's shared
 * gc buffer, which is copied out at once like a get_gc() result.
 */
static void meminfo_visit_pending_call_args(meminfo_context *ctx, char *frame_label, zend_execute_data *frame, uint32_t op_num)
{
    zend_get_gc_buffer *buffer = zend_get_gc_buffer_create();
    meminfo_gc_block *block;
    zval *table;
    int n, i;

    zend_unfinished_calls_gc(frame, frame->call, op_num, buffer);
    zend_get_gc_buffer_use(buffer, &table, &n);
    if (n <= 0) {
        return;
    }

    block = malloc(sizeof(meminfo_gc_block) + (size_t) (n - 1) * sizeof(zval));
    if (block == NULL) {
        ctx->aborted = 1;
        return;
    }
    block->count = 0;
    block->next = ctx->gc_blocks;
    ctx->gc_blocks = block;

    for (i = 0; i < n; i++) {
        if (Z_REFCOUNTED(table[i])) {
            ZVAL_COPY_VALUE(&block->values[block->count], &table[i]);
            block->count++;
        }
    }

    for (i = 0; i < (int) block->count && !meminfo_aborted(ctx); i++) {
        meminfo_dump_named_root(ctx, frame_label, "[pending arg]", &block->values[i]);
    }
}
#endif

#ifndef ZEND_CLOSURE_OBJECT
// Before 7.3: a closure's function is its first member after the object.
#define ZEND_CLOSURE_OBJECT(func) ((zend_object *) ((char *) (func) - sizeof(zend_object)))
#endif

/**
 * Dump what a frame holds besides its compiled variables, the same values the
 * engine's own garbage collector counts for an unfinished frame
 * (zend_unfinished_execution_gc_ex()):
 *   "$this"         the object a method runs on, when the frame holds it — a
 *                   temporary receiver such as (new A)->run() is held only here
 *   "{closure}"     the closure being run
 *   "[arg N]"       arguments beyond the declared parameters, and every
 *                   argument of an internal function (array_map($f, load()))
 *   "[named args]"  named arguments collected by a variadic parameter (8.0+)
 *   "[tmp]"         live temporaries at the current opline (7.1+): the array a
 *                   foreach iterates over a copy of, intermediate results
 *   "[pending arg]" arguments already passed to a call not made yet (8.2+)
 */
static void meminfo_visit_frame_extras(meminfo_context *ctx, char *frame_label, zend_execute_data *frame)
{
    char symbol_name[64];
    uint32_t call_info = ZEND_CALL_INFO(frame);
    uint32_t num_args = ZEND_CALL_NUM_ARGS(frame);
    uint32_t i;

    if ((call_info & ZEND_CALL_RELEASE_THIS) && Z_TYPE(frame->This) == IS_OBJECT) {
        meminfo_dump_named_object_root(ctx, frame_label, "$this", Z_OBJ(frame->This));
    }

    if (call_info & ZEND_CALL_CLOSURE) {
        meminfo_dump_named_object_root(ctx, frame_label, "{closure}", ZEND_CLOSURE_OBJECT(frame->func));
    }

    if (!ZEND_USER_CODE(frame->func->common.type)) {
        for (i = 0; i < num_args && !meminfo_aborted(ctx); i++) {
            snprintf(symbol_name, sizeof(symbol_name), "[arg %u]", i);
            meminfo_dump_named_root(ctx, frame_label, symbol_name, ZEND_CALL_ARG(frame, i + 1));
        }
        return;
    }

    {
        zend_op_array *op_array = &frame->func->op_array;

        if ((call_info & ZEND_CALL_FREE_EXTRA_ARGS) && num_args > op_array->num_args) {
            zval *extra = ZEND_CALL_VAR_NUM(frame, op_array->last_var + op_array->T);

            for (i = 0; i < num_args - op_array->num_args && !meminfo_aborted(ctx); i++) {
                snprintf(symbol_name, sizeof(symbol_name), "[arg %u]", op_array->num_args + i);
                meminfo_dump_named_root(ctx, frame_label, symbol_name, &extra[i]);
            }
        }

#if PHP_VERSION_ID >= 80000
        if (call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) {
            zval named;

            ZVAL_ARR(&named, frame->extra_named_params);
            meminfo_dump_named_root(ctx, frame_label, "[named args]", &named);
        }
#endif

#if PHP_VERSION_ID >= 70100
        if (frame->opline != NULL && frame->opline != op_array->opcodes) {
            const zend_op *opline = frame->opline->opcode == ZEND_HANDLE_EXCEPTION && EG(opline_before_exception)
                ? EG(opline_before_exception) : frame->opline;
            uint32_t op_num = (uint32_t) (opline - op_array->opcodes);

            for (i = 0; i < (uint32_t) op_array->last_live_range && !meminfo_aborted(ctx); i++) {
                const zend_live_range *range = &op_array->live_range[i];
                uint32_t kind = range->var & ZEND_LIVE_MASK;

                if (range->start > op_num) {
                    break;
                }
                if (op_num >= range->end || (kind != ZEND_LIVE_TMPVAR && kind != ZEND_LIVE_LOOP)) {
                    continue;
                }
                meminfo_dump_named_root(ctx, frame_label, "[tmp]", ZEND_CALL_VAR(frame, range->var & ~ZEND_LIVE_MASK));
            }

#if PHP_VERSION_ID >= 80200
            if (frame->call) {
                meminfo_visit_pending_call_args(ctx, frame_label, frame, op_num);
            }
#endif
        }
#endif
    }
}

/**
 * Walk the static variables of one user function or method.
 *
 * Only refcounted values are dumped: a variable that never ran its static
 * declaration still holds its scalar default, and a counter or a flag would
 * add a root per function without holding anything.
 */
static void meminfo_browse_static_variables_of(meminfo_context *ctx, zend_class_entry *scope, zend_function *func)
{
    char frame_label[] = "<STATIC_VARIABLE>";
    char symbol_name[500];
    HashTable *statics;
    zend_string *var_name;
    zval *value;

    if (func->type != ZEND_USER_FUNCTION) {
        return;
    }

#if PHP_VERSION_ID >= 70400
    // NULL until the function has run once.
    statics = ZEND_MAP_PTR(func->op_array.static_variables_ptr) ? ZEND_MAP_PTR_GET(func->op_array.static_variables_ptr) : NULL;
#else
    statics = func->op_array.static_variables;
#endif
    if (statics == NULL) {
        return;
    }

    ZEND_HASH_FOREACH_STR_KEY_VAL(statics, var_name, value) {
        if (meminfo_aborted(ctx)) {
            break;
        }
        if (var_name == NULL || !Z_REFCOUNTED_P(value)) {
            continue;
        }
#if PHP_VERSION_ID < 70400
        // Before 7.4 the table exists from compile time and holds the
        // declared defaults; a variable is bound (a reference) once it ran.
        if (!Z_ISREF_P(value)) {
            continue;
        }
#endif

        if (scope) {
            snprintf(symbol_name, sizeof(symbol_name), "%s::%s()::$%s",
                ZSTR_VAL(scope->name), ZSTR_VAL(func->common.function_name), ZSTR_VAL(var_name));
        } else {
            snprintf(symbol_name, sizeof(symbol_name), "%s()::$%s",
                ZSTR_VAL(func->common.function_name), ZSTR_VAL(var_name));
        }

        meminfo_dump_named_root(ctx, frame_label, symbol_name, value);
    } ZEND_HASH_FOREACH_END();
}

/**
 * Walk the static variables of every user function and method (GitHub issue
 * #75). Those of closures belong to each closure object and are reached
 * through it.
 */
static void meminfo_browse_function_static_variables(meminfo_context *ctx)
{
    zend_function *func;
    zend_class_entry *class_entry;

    ZEND_HASH_FOREACH_PTR(CG(function_table), func) {
        if (meminfo_aborted(ctx)) {
            return;
        }
        meminfo_browse_static_variables_of(ctx, NULL, func);
    } ZEND_HASH_FOREACH_END();

    // An inherited method may share its statics with the parent's: the copy
    // seen second is skipped by address.
    ZEND_HASH_FOREACH_PTR(CG(class_table), class_entry) {
        if (class_entry->type != ZEND_USER_CLASS) {
            continue;
        }
        ZEND_HASH_FOREACH_PTR(&class_entry->function_table, func) {
            if (meminfo_aborted(ctx)) {
                return;
            }
            meminfo_browse_static_variables_of(ctx, class_entry, func);
        } ZEND_HASH_FOREACH_END();
    } ZEND_HASH_FOREACH_END();
}

/**
 * Dump the handlers stacked by nested set_error_handler() or
 * set_exception_handler() calls, oldest first.
 */
static void meminfo_browse_handler_stack(meminfo_context *ctx, char *frame_label, const char *name, zend_stack *stack)
{
    char symbol_name[128];
    zval *handlers = (zval *) zend_stack_base(stack);
    int count = zend_stack_count(stack);
    int i;

    for (i = 0; handlers != NULL && i < count; i++) {
        snprintf(symbol_name, sizeof(symbol_name), "%s[%d]", name, i);
        meminfo_dump_named_root(ctx, frame_label, symbol_name, &handlers[i]);
    }
}

/**
 * Call an internal function and keep its result until the dump ends, for
 * roots only such a function can list: SPL keeps its autoloaders in a table
 * private to ext/spl, pcntl its signal handlers in its own globals.
 *
 * The functions called run no PHP code. Their result is still allocated from
 * the Zend heap, so nothing is called when the process is within
 * MEMINFO_CALL_HEADROOM of memory_limit, when an exception is pending, or when
 * the name is not an internal function (a user function may take the name of
 * an extension that is not loaded). The result is kept rather than released
 * at once: items are identified by address, and memory freed and reused during
 * the dump would make new items look already visited.
 *
 * Returns the kept result, or NULL when nothing was called.
 */
static zval *meminfo_call_internal(meminfo_context *ctx, const char *name, zval *args, uint32_t arg_count)
{
    zend_function *func;
    zval function_name;
    zval result;
    int called;

    if (EG(exception) || meminfo_aborted(ctx)) {
        return NULL;
    }
    if (PG(memory_limit) > 0 && zend_memory_usage(0) + MEMINFO_CALL_HEADROOM > (size_t) PG(memory_limit)) {
        return NULL;
    }

    func = zend_hash_str_find_ptr(CG(function_table), name, strlen(name));
    if (func == NULL || func->type != ZEND_INTERNAL_FUNCTION) {
        return NULL;
    }

    if (Z_TYPE(ctx->call_results) == IS_UNDEF) {
        array_init(&ctx->call_results);
    }

    ZVAL_STRING(&function_name, name);
    ZVAL_UNDEF(&result);
    called = call_user_function(CG(function_table), NULL, &function_name, &result, arg_count, args) == SUCCESS;
    zval_ptr_dtor(&function_name);

    if (EG(exception)) {
        zend_clear_exception();
        called = 0;
    }
    if (!called || Z_TYPE(result) == IS_UNDEF) {
        zval_ptr_dtor(&result);
        return NULL;
    }

    return zend_hash_next_index_insert(Z_ARRVAL(ctx->call_results), &result);
}

/**
 * Dump every refcounted element of a list a function returned, as
 * "<name>[<key>]".
 */
static void meminfo_dump_list_roots(meminfo_context *ctx, char *frame_label, const char *name, zval *list)
{
    char symbol_name[128];
    zend_string *key;
    zend_ulong index;
    zval *entry;

    if (list == NULL || Z_TYPE_P(list) != IS_ARRAY) {
        return;
    }

    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(list), index, key, entry) {
        if (!Z_REFCOUNTED_P(entry)) {
            continue;
        }
        if (key) {
            snprintf(symbol_name, sizeof(symbol_name), "%s[%s]", name, ZSTR_VAL(key));
        } else {
            snprintf(symbol_name, sizeof(symbol_name), "%s[" ZEND_ULONG_FMT "]", name, index);
        }
        meminfo_dump_named_root(ctx, frame_label, symbol_name, entry);
    } ZEND_HASH_FOREACH_END();
}

/**
 * The private entry of a register_tick_function() callback, as declared in
 * ext/standard/basic_functions.c of each version.
 */
#if PHP_VERSION_ID >= 80500
typedef struct {
    zend_fcall_info_cache fci_cache;
    zval *params;
    uint32_t param_count;
    bool calling;
} meminfo_tick_entry;
#elif PHP_VERSION_ID >= 80100
typedef struct {
    zend_fcall_info fci;
    zend_fcall_info_cache fci_cache;
    bool calling;
} meminfo_tick_entry;
#else
typedef struct {
    zval *arguments;
    int arg_count;
    int calling;
} meminfo_tick_entry;
#endif

/**
 * Dump register_tick_function() callbacks and their arguments.
 */
static void meminfo_browse_tick_functions(meminfo_context *ctx, char *frame_label)
{
    char symbol_name[128];
    zend_llist_position pos;
    meminfo_tick_entry *entry;
    uint32_t index = 0;

    if (BG(user_tick_functions) == NULL) {
        return;
    }

    for (entry = zend_llist_get_first_ex(BG(user_tick_functions), &pos); entry != NULL;
         entry = zend_llist_get_next_ex(BG(user_tick_functions), &pos)) {
        zval *args = NULL;
        uint32_t arg_count = 0, i;

        snprintf(symbol_name, sizeof(symbol_name), "register_tick_function[%u]", index);
#if PHP_VERSION_ID >= 80500
        meminfo_dump_named_object_root(ctx, frame_label, symbol_name,
            entry->fci_cache.closure ? entry->fci_cache.closure : entry->fci_cache.object);
        args = entry->params;
        arg_count = entry->param_count;
#elif PHP_VERSION_ID >= 80100
        meminfo_dump_named_root(ctx, frame_label, symbol_name, &entry->fci.function_name);
        args = entry->fci.params;
        arg_count = entry->fci.param_count;
#else
        // The callable is the first "argument".
        if (entry->arg_count > 0) {
            meminfo_dump_named_root(ctx, frame_label, symbol_name, &entry->arguments[0]);
            args = entry->arguments + 1;
            arg_count = (uint32_t) entry->arg_count - 1;
        }
#endif
        for (i = 0; args != NULL && i < arg_count; i++) {
            snprintf(symbol_name, sizeof(symbol_name), "register_tick_function[%u]($%u)", index, i);
            meminfo_dump_named_root(ctx, frame_label, symbol_name, &args[i]);
        }

        index++;
    }
}

#ifdef MEMINFO_HAVE_SESSION_GLOBALS
/**
 * Dump the callables of session_set_save_handler(): an object handler appears
 * as [$handler, 'open'], … arrays, so the handler is reached through them.
 */
static void meminfo_browse_session_handler(meminfo_context *ctx, char *frame_label)
{
    char symbol_name[128];
    zend_module_entry *module;
    php_ps_globals *ps;
    zval *names;
    size_t count, i;

    module = zend_hash_str_find_ptr(&module_registry, "session", sizeof("session") - 1);
    if (module == NULL || module->globals_ptr == NULL) {
        return;
    }

    ps = (php_ps_globals *) module->globals_ptr;
    names = (zval *) &ps->mod_user_names;
    count = sizeof(ps->mod_user_names) / sizeof(zval);

    for (i = 0; i < count; i++) {
        if (!Z_REFCOUNTED(names[i])) {
            continue;
        }
        snprintf(symbol_name, sizeof(symbol_name), "session_set_save_handler[%zu]", i);
        meminfo_dump_named_root(ctx, frame_label, symbol_name, &names[i]);
    }
}
#endif

/**
 * Dump the callbacks held by ext/standard, the SAPI and other extensions:
 * autoloaders, tick functions, header_register_callback(), the session save
 * handler, pcntl signal handlers, the libxml external entity loader (8.2+) and
 * the options of the default stream context.
 *
 * Not covered: libxml_set_streams_context() and stream notification callbacks,
 * which have no reader and whose storage is private to their extension.
 */
static void meminfo_browse_more_callbacks(meminfo_context *ctx, char *frame_label)
{
    meminfo_dump_list_roots(ctx, frame_label, "spl_autoload_register", meminfo_call_internal(ctx, "spl_autoload_functions", NULL, 0));

    meminfo_browse_tick_functions(ctx, frame_label);

    if (Z_REFCOUNTED(SG(callback_func))) {
        meminfo_dump_named_root(ctx, frame_label, "header_register_callback", &SG(callback_func));
    }

#ifdef MEMINFO_HAVE_SESSION_GLOBALS
    meminfo_browse_session_handler(ctx, frame_label);
#endif

    if (zend_hash_str_exists(CG(function_table), "pcntl_signal_get_handler", sizeof("pcntl_signal_get_handler") - 1)) {
        char symbol_name[128];
        int signo;

        for (signo = 1; signo < MEMINFO_SIGNAL_COUNT && !meminfo_aborted(ctx); signo++) {
            zval arg;
            zval *handler;

            ZVAL_LONG(&arg, signo);
            handler = meminfo_call_internal(ctx, "pcntl_signal_get_handler", &arg, 1);
            if (handler == NULL || !Z_REFCOUNTED_P(handler)) {
                continue;
            }
            snprintf(symbol_name, sizeof(symbol_name), "pcntl_signal[%d]", signo);
            meminfo_dump_named_root(ctx, frame_label, symbol_name, handler);
        }
    }

    {
        zval *loader = meminfo_call_internal(ctx, "libxml_get_external_entity_loader", NULL, 0);

        if (loader != NULL && Z_REFCOUNTED_P(loader)) {
            meminfo_dump_named_root(ctx, frame_label, "libxml_set_external_entity_loader", loader);
        }
    }

    // Any stream opened without a context creates the default one, empty.
    if (FG(default_context) && Z_TYPE(FG(default_context)->options) == IS_ARRAY
        && zend_hash_num_elements(Z_ARRVAL(FG(default_context)->options)) > 0) {
        meminfo_dump_named_root(ctx, frame_label, "stream_context_set_default", &FG(default_context)->options);
    }
}

/**
 * Dump the objects still alive that no root reached, as roots of
 * "<UNREACHABLE>" named after their handle: cycles waiting for the cycle
 * collector, objects an extension holds a reference to (or leaked one to), and
 * whatever is held from a place this walk does not know. Runs after every
 * other root, so an object reached from one is never listed here. Reads the
 * object store only; no handler is called.
 */
static void meminfo_browse_unreachable_objects(meminfo_context *ctx)
{
    char frame_label[] = "<UNREACHABLE>";
    char symbol_name[32];
    uint32_t handle;

    for (handle = 1; handle < EG(objects_store).top && !meminfo_aborted(ctx); handle++) {
        zend_object *object = EG(objects_store).object_buckets[handle];
        zval holder;

        if (object == NULL || !IS_OBJ_VALID(object)) {
            continue;
        }

        snprintf(symbol_name, sizeof(symbol_name), "#%u", handle);
        ZVAL_OBJ(&holder, object);
        meminfo_zval_dump(ctx, frame_label, symbol_name, strlen(symbol_name), &holder);
    }
}

/**
 * Dump the content of output buffers (ob_start(), or output_buffering) as
 * "buffer" items: a CLI worker that buffers its output and never flushes holds
 * all of it, and no zval shows that memory.
 */
static void meminfo_browse_output_buffers(meminfo_context *ctx)
{
    char symbol_name[64];
    php_output_handler **handlers;
    int count, i;

    if (!OG(handlers).elements) {
        return;
    }

    handlers = (php_output_handler **) zend_stack_base(&OG(handlers));
    count = zend_stack_count(&OG(handlers));

    for (i = 0; handlers != NULL && i < count; i++) {
        if (handlers[i] == NULL || handlers[i]->buffer.data == NULL) {
            continue;
        }
        snprintf(symbol_name, sizeof(symbol_name), "ob_start[%d]", i);
        meminfo_raw_root_dump(ctx, "<OUTPUT_BUFFER>", symbol_name, handlers[i]->buffer.data, "buffer", handlers[i]->buffer.size);
    }
}

/**
 * Dump callables the engine holds on to for later (GitHub issue #78): error
 * and exception handlers, shutdown functions with their arguments, user
 * output-buffer handlers, then the rest of meminfo_browse_more_callbacks(). A
 * closure registered there keeps everything it uses alive, often as the only
 * reference left.
 */
static void meminfo_browse_callbacks(meminfo_context *ctx)
{
    char frame_label[] = "<CALLBACK>";
    char symbol_name[128];

    meminfo_dump_named_root(ctx, frame_label, "set_error_handler", &EG(user_error_handler));
    meminfo_browse_handler_stack(ctx, frame_label, "set_error_handler", &EG(user_error_handlers));
    meminfo_dump_named_root(ctx, frame_label, "set_exception_handler", &EG(user_exception_handler));
    meminfo_browse_handler_stack(ctx, frame_label, "set_exception_handler", &EG(user_exception_handlers));

    if (BG(user_shutdown_function_names)) {
        php_shutdown_function_entry *entry;
        uint32_t index = 0;

        ZEND_HASH_FOREACH_PTR(BG(user_shutdown_function_names), entry) {
            zval *args = NULL;
            uint32_t arg_count = 0, i;

            if (meminfo_aborted(ctx)) {
                return;
            }

            snprintf(symbol_name, sizeof(symbol_name), "register_shutdown_function[%u]", index);
#if PHP_VERSION_ID >= 80500
            meminfo_dump_named_object_root(ctx, frame_label, symbol_name,
                entry->fci_cache.closure ? entry->fci_cache.closure : entry->fci_cache.object);
            args = entry->params;
            arg_count = entry->param_count;
#elif PHP_VERSION_ID >= 80100
            meminfo_dump_named_root(ctx, frame_label, symbol_name, &entry->fci.function_name);
            args = entry->fci.params;
            arg_count = entry->fci.param_count;
#elif PHP_VERSION_ID >= 80000
            meminfo_dump_named_root(ctx, frame_label, symbol_name, &entry->function_name);
            args = entry->arguments;
            arg_count = (uint32_t) entry->arg_count;
#else
            // The callable is the first "argument".
            if (entry->arg_count > 0) {
                meminfo_dump_named_root(ctx, frame_label, symbol_name, &entry->arguments[0]);
                args = entry->arguments + 1;
                arg_count = (uint32_t) entry->arg_count - 1;
            }
#endif
            for (i = 0; args != NULL && i < arg_count; i++) {
                snprintf(symbol_name, sizeof(symbol_name), "register_shutdown_function[%u]($%u)", index, i);
                meminfo_dump_named_root(ctx, frame_label, symbol_name, &args[i]);
            }

            index++;
        } ZEND_HASH_FOREACH_END();
    }

    if (OG(handlers).elements) {
        php_output_handler **handlers = (php_output_handler **) zend_stack_base(&OG(handlers));
        int count = zend_stack_count(&OG(handlers));
        int i;

        for (i = 0; handlers != NULL && i < count; i++) {
            if (handlers[i] && (handlers[i]->flags & PHP_OUTPUT_HANDLER_USER) && handlers[i]->func.user) {
                snprintf(symbol_name, sizeof(symbol_name), "ob_start[%d]", i);
                meminfo_dump_named_root(ctx, frame_label, symbol_name, &handlers[i]->func.user->zoh);
            }
        }
    }

    meminfo_browse_more_callbacks(ctx, frame_label);
    meminfo_browse_output_buffers(ctx);
}

/**
 * Dump user constants holding values of their own (const X = new Foo since
 * PHP 8.1, define() with a string or an array built at runtime). Constants of
 * PHP itself and of extensions stay out of the dump.
 */
static void meminfo_browse_constants(meminfo_context *ctx)
{
    char frame_label[] = "<CONSTANT>";
    zend_constant *constant;

    ZEND_HASH_FOREACH_PTR(EG(zend_constants), constant) {
        if (meminfo_aborted(ctx)) {
            return;
        }
#if PHP_VERSION_ID >= 70300
        if (ZEND_CONSTANT_MODULE_NUMBER(constant) != PHP_USER_CONSTANT) {
#else
        if (constant->module_number != PHP_USER_CONSTANT) {
#endif
            continue;
        }
        if (constant->name && Z_REFCOUNTED(constant->value)) {
            meminfo_dump_named_root(ctx, frame_label, ZSTR_VAL(constant->name), &constant->value);
        }
    } ZEND_HASH_FOREACH_END();
}

#if PHP_VERSION_ID >= 80100
/**
 * The constants table of a class as it stands, without separating it: a
 * class whose constant expressions were never evaluated keeps them as ASTs,
 * which are skipped, and evaluating them here would run autoloaders.
 */
static HashTable *meminfo_class_constants_table(zend_class_entry *ce)
{
    if ((ce->ce_flags & ZEND_ACC_HAS_AST_CONSTANTS) && ZEND_MAP_PTR(ce->mutable_data)) {
        zend_class_mutable_data *mutable_data = (zend_class_mutable_data *) ZEND_MAP_PTR_GET_IMM(ce->mutable_data);

        if (mutable_data && mutable_data->constants_table) {
            return mutable_data->constants_table;
        }
    }

    return &ce->constants_table;
}

/**
 * Dump class constants holding values of their own: enum cases once used
 * (objects created on first access), object-valued constants and constants
 * evaluated at runtime into a refcounted string or array. A constant inherited
 * by a subclass is the same entry, seen once.
 */
static void meminfo_browse_class_constants(meminfo_context *ctx)
{
    char frame_label[] = "<CLASS_CONSTANT>";
    char symbol_name[500];
    zend_class_entry *class_entry;
    zend_class_constant *constant;
    zend_string *name;

    ZEND_HASH_FOREACH_PTR(CG(class_table), class_entry) {
        ZEND_HASH_FOREACH_STR_KEY_PTR(meminfo_class_constants_table(class_entry), name, constant) {
            if (meminfo_aborted(ctx)) {
                return;
            }
            if (name == NULL || !Z_REFCOUNTED(constant->value) || Z_TYPE(constant->value) == IS_CONSTANT_AST) {
                continue;
            }
            snprintf(symbol_name, sizeof(symbol_name), "%s::%s", ZSTR_VAL(constant->ce->name), ZSTR_VAL(name));
            meminfo_zval_dump(ctx, frame_label, symbol_name, strlen(symbol_name), &constant->value);
        } ZEND_HASH_FOREACH_END();
    } ZEND_HASH_FOREACH_END();
}
#endif

static void meminfo_browse_zvals_from_symbol_table(meminfo_context *ctx, char *frame_label, HashTable *p_symbol_table)
{
    zval *zval_to_dump;
    HashPosition pos;

    zend_string *key;
    zend_ulong index;

    zend_hash_internal_pointer_reset_ex(p_symbol_table, &pos);

    while (!meminfo_aborted(ctx)
        && (zval_to_dump = zend_hash_get_current_data_ex(p_symbol_table, &pos)) != NULL) {

        zend_hash_get_current_key_ex(p_symbol_table, &key, &index, &pos);

        meminfo_zval_dump(ctx, frame_label, key ? ZSTR_VAL(key) : NULL, key ? ZSTR_LEN(key) : 0, zval_to_dump);

        zend_hash_move_forward_ex(p_symbol_table, &pos);
    }
}

/**
 * meminfo_dump_to_writer() for callers that own resources to release when a
 * fatal error bails out of the walk: returns 1 after such a bailout — the
 * caller cleans up, then calls zend_bailout() — and 0 with *status set
 * otherwise. The zend_try is here so that no local of the caller is read after
 * the longjmp.
 */
MEMINFO_HIDDEN int meminfo_dump_to_writer_guarded(meminfo_writer *w, zend_execute_data *start_frame, int include_global_symbol_table, int *status)
{
    int bailed_out = 0;

    zend_try {
        *status = meminfo_dump_to_writer(w, start_frame, include_global_symbol_table);
    } zend_catch {
        bailed_out = 1;
    } zend_end_try();

    return bailed_out;
}

/**
 * Build the current frame label based on function name and object class
 * if necessary.
 *
 * Most code comes from the debug_print_backtrace implementation.
 */
void meminfo_build_frame_label(char* frame_label, int frame_label_len, zend_execute_data* frame)
{
    zend_function *func;
    const char *function_name;
    char * call_type;
    zend_string *class_name = NULL;
    zend_object *object;
    zend_execute_data *ptr;

    object = Z_OBJ(frame->This);
    ptr = frame->prev_execute_data;
    func = NULL;
    function_name = NULL;

    if (frame->func) {
        func = frame->func;

#if PHP_VERSION_ID >= 80000
        function_name = func->common.function_name ? ZSTR_VAL(func->common.function_name) : NULL;
#else
        function_name = (func->common.scope &&
                         func->common.scope->trait_aliases) ?
            ZSTR_VAL(zend_resolve_method_name(
                (object ? object->ce : func->common.scope), func)) :
            (func->common.function_name ?
                ZSTR_VAL(func->common.function_name) : NULL);
#endif
    }

    if (function_name) {
        if (object) {
            if (func->common.scope) {
                class_name = func->common.scope->name;
            } else {
                class_name = object->ce->name;
            }

            call_type = "->";
        } else if (func->common.scope) {
            class_name = func->common.scope->name;
            call_type = "::";
        } else {
            class_name = NULL;
            call_type = NULL;
        }
    } else {

        if (!ptr->func || !ZEND_USER_CODE(ptr->func->common.type) || ptr->opline->opcode != ZEND_INCLUDE_OR_EVAL) {
            /* can happen when calling eval from a custom sapi */
            function_name = "unknown";
        } else
        switch (ptr->opline->extended_value) {
            case ZEND_EVAL:
                function_name = "eval";
                break;
            case ZEND_INCLUDE:
                function_name = "include";
                break;
            case ZEND_REQUIRE:
                function_name = "require";
                break;
            case ZEND_INCLUDE_ONCE:
                function_name = "include_once";
                break;
            case ZEND_REQUIRE_ONCE:
                function_name = "require_once";
                break;
            default:
                /* this can actually happen if you're in your error_handler and
                 * you're in the top-scope */
                function_name = "unknown";
                break;
        }
    }
    if (class_name) {
        snprintf(frame_label, frame_label_len, "%s%s%s()", ZSTR_VAL(class_name), call_type, function_name);
    } else {
        snprintf(frame_label, frame_label_len, "%s()", function_name);
    }
}
