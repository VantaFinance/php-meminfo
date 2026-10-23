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
static void meminfo_browse_function_static_variables(meminfo_context *ctx);
static void meminfo_browse_zvals_from_symbol_table(meminfo_context *ctx, char *frame_label, HashTable *p_symbol_table);

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
    memset(&ctx.opening, 0, sizeof(ctx.opening));

    bailed_out = meminfo_dump_walk_guarded(&ctx, start_frame, include_global_symbol_table);

    if (w->err) {
        status = MEMINFO_DUMP_WRITE_FAILED;
    } else if (meminfo_aborted(&ctx)) {
        status = MEMINFO_DUMP_INCOMPLETE;
    } else {
        status = MEMINFO_DUMP_COMPLETE;
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

    char frame_label[500];
    exec_frame = start_frame;

    while (exec_frame && !meminfo_aborted(ctx)) {
        // Internal frames (a call into a C function) declare no PHP variables.
        // Visiting them would attribute the variables of the surrounding user
        // function to the internal call, e.g. "outerVar@array_map()".
        if (!exec_frame->func || !ZEND_USER_CODE(exec_frame->func->common.type)) {
            goto handle_next;
        }

        if (meminfo_visit_item(&ctx->visited_items, exec_frame)) {
            goto handle_next;
        }

        // A frame with nothing below it is the main script — or the first
        // frame of a coroutine (OpenSwoole), which still has a function name.
        if (exec_frame->prev_execute_data || exec_frame->func->common.function_name) {
            meminfo_build_frame_label(frame_label, sizeof(frame_label), exec_frame);
        } else {
            snprintf(frame_label, sizeof(frame_label), "<GLOBAL>");
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
        } else {
            // Read the compiled variables straight out of the frame. Calling
            // zend_rebuild_symbol_table() instead would allocate a table per
            // frame and leave ZEND_CALL_HAS_SYMBOL_TABLE set on the process
            // being inspected, which the dump must not do.
            zend_op_array *op_array = &exec_frame->func->op_array;
            // last_var is a signed int in every supported version, and
            // ZEND_CALL_VAR_NUM casts its index to int as well.
            int i;

            for (i = 0; i < op_array->last_var; i++) {
                meminfo_zval_dump(ctx, frame_label, ZSTR_VAL(op_array->vars[i]), ZSTR_LEN(op_array->vars[i]),
                    ZEND_CALL_VAR_NUM(exec_frame, i));
            }
        }

handle_next:
        exec_frame = exec_frame->prev_execute_data;
    }
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

#if PHP_VERSION_ID >= 80500
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
#endif

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
 * Dump callables the engine holds on to for later (GitHub issue #78): error
 * and exception handlers, shutdown functions with their arguments, and user
 * output-buffer handlers. A closure registered there keeps everything it uses
 * alive, often as the only reference left.
 *
 * Not covered: spl_autoload_register() callbacks. SPL keeps them in a table
 * that is private to ext/spl, and the only way to list them is to call
 * spl_autoload_functions(), which a dump must not do.
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

    if (frame->func) {
        func = frame->func;

#if PHP_VERSION_ID >= 80000
        if (func->common.function_name) {
            function_name = ZSTR_VAL(func->common.function_name);
        } else {
            function_name = NULL;
        }
#else
        function_name = (func->common.scope &&
                         func->common.scope->trait_aliases) ?
            ZSTR_VAL(zend_resolve_method_name(
                (object ? object->ce : func->common.scope), func)) :
            (func->common.function_name ?
                ZSTR_VAL(func->common.function_name) : NULL);
#endif
    } else {
        func = NULL;
        function_name = NULL;
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
