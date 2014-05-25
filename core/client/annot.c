#include "../globals.h"
#include "../hashtable.h"
#include "../x86/instr.h"
#include "../x86/instr_create.h"
#include "../x86/instrument.h"
#include "../x86/disassemble.h"
#include "annot.h"

#include "../lib/annotation/valgrind.h"
#include "../lib/annotation/memcheck.h"

#define KEY(addr) ((ptr_uint_t) addr)
static generic_table_t *handlers;

// locked under the `handlers` table lock
static annotation_handler_t *vg_handlers[VG_ID__LAST];

/* Immediate operands to the special rol instructions.
 * See __SPECIAL_INSTRUCTION_PREAMBLE in valgrind.h.
 */
#ifdef X64
static const int
expected_rol_immeds[VG_PATTERN_LENGTH] = {
    3,
    13,
    61,
    51
};
#else
static const int
expected_rol_immeds[VG_PATTERN_LENGTH] = {
    3,
    13,
    29,
    19
};
#endif

#define VALGRIND_ANNOTATION_ROL_COUNT 4

/**** Private Function Declarations ****/

/* Handles a valgrind client request, if we understand it.
 */
static void
handle_vg_annotation(app_pc request_args);

static void
event_module_load(void *drcontext, const module_data_t *info, bool loaded);

static void
event_module_unload(void *drcontext, const module_data_t *info);

static void
convert_va_list_to_opnd(opnd_t *args, uint num_args, va_list ap);

static valgrind_request_id_t
lookup_valgrind_request(ptr_uint_t request);

static void
free_annotation_handler(void *p);

/**** Public Function Definitions ****/

void
annot_init()
{
    handlers = generic_hash_create(GLOBAL_DCONTEXT, 8, 80,
                                   HASHTABLE_ENTRY_SHARED | HASHTABLE_SHARED |
                                   HASHTABLE_RELAX_CLUSTER_CHECKS,
                                   free_annotation_handler
                                   _IF_DEBUG("annotation hashtable"));

    dr_register_module_load_event(event_module_load);
    dr_register_module_unload_event(event_module_unload);
}

void
annot_exit()
{
    uint i;

    for (i = 0; i < VG_ID__LAST; i++) {
        if (vg_handlers[i] != NULL)
            HEAP_ARRAY_FREE(GLOBAL_DCONTEXT, vg_handlers[i], annotation_handler_t,
                            1, ACCT_OTHER, UNPROTECTED);
    }

    generic_hash_destroy(GLOBAL_DCONTEXT, handlers);
}

void
annot_register_call(void *drcontext, void *annotation_func,
                    void *callback, bool save_fpstate, uint num_args, ...)
{
    TABLE_RWLOCK(handlers, write, lock);
    annotation_handler_t *handler =
        (annotation_handler_t *) generic_hash_lookup(GLOBAL_DCONTEXT, handlers,
                                                     KEY(annotation_func));
    if (handler == NULL) {
        // TODO: what's "PROTECTED"?
        handler = HEAP_TYPE_ALLOC(GLOBAL_DCONTEXT, annotation_handler_t,
                                  ACCT_OTHER, UNPROTECTED);
        handler->type = ANNOT_HANDLER_CALL;
        handler->id.annotation_func = (app_pc) annotation_func;
        handler->instrumentation.callback = callback;
        handler->save_fpstate = save_fpstate;
        handler->num_args = num_args;
        handler->next_handler = NULL;

        if (num_args == 0) {
            handler->args = NULL;
        } else {
            va_list args;
            va_start(args, num_args);
            handler->args = HEAP_ARRAY_ALLOC(drcontext,
                opnd_t, num_args, ACCT_OTHER, UNPROTECTED);
            convert_va_list_to_opnd(handler->args, num_args, args);
            va_end(args);
        }

        generic_hash_add(GLOBAL_DCONTEXT, handlers, KEY(annotation_func), handler);
    } // else ignore duplicate registration
    TABLE_RWLOCK(handlers, write, unlock);
}

void
annot_register_return(void *drcontext, void *annotation_func, void *return_value)
{
    TABLE_RWLOCK(handlers, write, lock);
    annotation_handler_t *handler =
        (annotation_handler_t *) generic_hash_lookup(GLOBAL_DCONTEXT, handlers,
                                                     KEY(annotation_func));
    if (handler == NULL) {
        handler = HEAP_TYPE_ALLOC(GLOBAL_DCONTEXT, annotation_handler_t,
                                  ACCT_OTHER, UNPROTECTED);
        handler->type = ANNOT_HANDLER_RETURN_VALUE;
        handler->id.annotation_func = (app_pc) annotation_func;
        handler->instrumentation.return_value = return_value;
        handler->save_fpstate = false;
        handler->num_args = 0;
        handler->next_handler = NULL;

        generic_hash_add(GLOBAL_DCONTEXT, handlers, KEY(annotation_func), handler);
    } // else ignore duplicate registration
    TABLE_RWLOCK(handlers, write, unlock);
}

void
annot_register_valgrind(void *drcontext, valgrind_request_id_t request_id,
    ptr_uint_t (*annotation_callback)(vg_client_request_t *request))
{
    if (request_id >= VG_ID__LAST)
        return;

    TABLE_RWLOCK(handlers, write, lock);
    annotation_handler_t *handler = vg_handlers[request_id];
    if (handler == NULL) {
        handler = HEAP_TYPE_ALLOC(GLOBAL_DCONTEXT, annotation_handler_t,
                                  ACCT_OTHER, UNPROTECTED);
        handler->type = ANNOT_HANDLER_VALGRIND;
        handler->id.vg_request_id = request_id;
        handler->instrumentation.vg_callback = annotation_callback;
        handler->save_fpstate = false;
        handler->num_args = 0;
        handler->next_handler = NULL;

        vg_handlers[request_id] = handler;
    }
    TABLE_RWLOCK(handlers, write, unlock);
}

instr_t *
annot_match(dcontext_t *dcontext, instr_t *instr)
{
    instr_t *first_call = NULL, *prev_call = NULL;

    if (instr_is_call_direct(instr)) {
        app_pc target = instr_get_branch_target_pc(instr);
        annotation_handler_t *handler = NULL;

        TABLE_RWLOCK(handlers, read, lock);
        handler = (annotation_handler_t *) generic_hash_lookup(GLOBAL_DCONTEXT, handlers,
                                                               (ptr_uint_t)target);
        while (handler != NULL) {
            instr_t *call = INSTR_CREATE_label(dcontext);

            call->flags |= INSTR_ANNOTATION;
            instr_set_note(call, (void *) handler); // Collision with other notes?
            instr_set_ok_to_mangle(call, false);

            if (first_call == NULL) {
                first_call = prev_call = call;
            } else {
                instr_set_next(prev_call, call);
                instr_set_prev(call, prev_call);
                prev_call = call;
            }

            handler = handler->next_handler;
        }
        TABLE_RWLOCK(handlers, read, unlock);
    }

    return first_call;
}

bool
match_valgrind_pattern(dcontext_t *dcontext, instrlist_t *bb, instr_t *instr)
{
    int i;
    app_pc xchg_xl8;
    instr_t *instr_walk;

    /* Already know that `instr` is `OP_xchg`, per `IS_VALGRIND_ANNOTATION_SHAPE`.
     * Check the operands of the xchg for the Valgrind signature: both xbx. */
    opnd_t src = instr_get_src(instr, 0);
    opnd_t dst = instr_get_dst(instr, 0);
    opnd_t xbx = opnd_create_reg(DR_REG_XBX);
    if (!opnd_same(src, xbx) || !opnd_same(dst, xbx))
        return false;

    /* If it's a Valgrind annotation, the preceding `VALGRIND_ANNOTATION_ROL_COUNT`
     * instructions will be `OP_rol` having `expected_rol_immeds` operands. */
    instr_walk = instrlist_last(bb);
    for (i = (VALGRIND_ANNOTATION_ROL_COUNT - 1); i >= 0; i--) {
        if (instr_get_opcode(instr_walk) != OP_rol) {
            return false;
        } else {
            opnd_t src = instr_get_src(instr_walk, 0);
            opnd_t dst = instr_get_dst(instr_walk, 0);
            if (!opnd_is_immed(src) ||
                opnd_get_immed_int(src) != expected_rol_immeds[i])
                return false;
            if (!opnd_same(dst, opnd_create_reg(DR_REG_XDI)))
                return false;
        }
        instr_walk = instr_get_prev(instr_walk);
    }

    /* We have matched the pattern. */
    DOLOG(4, LOG_INTERP, {
        LOG(THREAD, LOG_INTERP, 4, "Matched valgrind client request pattern at "PFX":\n",
            instr_get_app_pc(instr));
        /*
        for (i = 0; i < BUFFER_SIZE_ELEMENTS(instrs); i++) {
            instr_disassemble(dcontext, instr, THREAD);
            LOG(THREAD, LOG_INTERP, 4, "\n");
        }
        */
        LOG(THREAD, LOG_INTERP, 4, "\n");
    });

    /* We leave the argument gathering code (typically "lea _zzq_args -> %xax"
     * and "mov _zzq_default -> %xdx") as app instructions, as it writes to app
     * registers (xref i#1423).
     */
    xchg_xl8 = instr_get_app_pc(instr);
    instr_destroy(dcontext, instr);

    /* Delete rol and xchg instructions. Note: reusing parameter `instr`. */
    instr = instrlist_last(bb);
    for (i = 0; i < VALGRIND_ANNOTATION_ROL_COUNT; i++) {
        instr_walk = instr_get_prev(instr);
        instrlist_remove(bb, instr);
        instr_destroy(dcontext, instr);
        instr = instr_walk;
    }

    // TODO: check request id, and ignore if we don't support that one?

    /* Append a write to %xbx, both to ensure it's marked defined by DrMem
     * and to avoid confusion with register analysis code (%xbx is written
     * by the clean callee).
     */
    instrlist_append(bb, INSTR_XL8(INSTR_CREATE_xor(dcontext, opnd_create_reg(DR_REG_XBX),
                                                    opnd_create_reg(DR_REG_XBX)),
                                   xchg_xl8));

    dr_insert_clean_call(dcontext, bb, NULL, (void*)handle_vg_annotation,
                         /*fpstate=*/false, 1, opnd_create_reg(DR_REG_XAX));

    return true;
}

/**** Private Function Definitions ****/

static void
handle_vg_annotation(app_pc request_args)
{
    dcontext_t *dcontext = (dcontext_t *) dr_get_current_drcontext();
    valgrind_request_id_t request_id;
    annotation_handler_t *handler;
    vg_client_request_t request;
    dr_mcontext_t mcontext;
    ptr_uint_t result;

    if (!safe_read(request_args, sizeof(request), &request))
        return;

    result = request.default_result;

    request_id = lookup_valgrind_request(request.request);
    if (request_id < VG_ID__LAST) {
        TABLE_RWLOCK(handlers, read, lock);
        handler = vg_handlers[request_id];
        if (handler != NULL) // TODO: multiple handlers? Then what result?
            result = handler->instrumentation.vg_callback(&request);
        TABLE_RWLOCK(handlers, read, unlock);
    }

    /* The result code goes in xbx. */
    mcontext.size = sizeof(mcontext);
    mcontext.flags = DR_MC_INTEGER;
    dr_get_mcontext(dcontext, &mcontext);
    mcontext.xbx = result;
    dr_set_mcontext(dcontext, &mcontext);
}

static void
event_module_load(void *drcontext, const module_data_t *info, bool loaded)
{
    generic_func_t target =
        dr_get_proc_address(info->handle, "dynamorio_annotate_running_on_dynamorio");
    if (target != NULL)
        annot_register_return(drcontext, target, (void *) true);
}

static void
event_module_unload(void *drcontext, const module_data_t *info)
{
    int iter = 0;
    ptr_uint_t key;
    void *handler;
    TABLE_RWLOCK(handlers, write, lock);
    do {
        iter = generic_hash_iterate_next(GLOBAL_DCONTEXT, handlers,
                                         iter, &key, &handler);
        if (iter < 0)
            break;
        if ((key > (ptr_uint_t) info->start) && (key < (ptr_uint_t) info->end))
            iter = generic_hash_iterate_remove(GLOBAL_DCONTEXT, handlers,
                                               iter, key);
    } while (true);
    TABLE_RWLOCK(handlers, write, unlock);
}

/* Stolen from instrument.c--should it become a utility? */
static void
convert_va_list_to_opnd(opnd_t *args, uint num_args, va_list ap)
{
    uint i;
    /* There's no way to check num_args vs actual args passed in */
    for (i = 0; i < num_args; i++) {
        args[i] = va_arg(ap, opnd_t);
        CLIENT_ASSERT(opnd_is_valid(args[i]),
                      "Call argument: bad operand. Did you create a valid opnd_t?");
    }
}

static valgrind_request_id_t
lookup_valgrind_request(ptr_uint_t request)
{
    switch (request) {
        case VG_USERREQ__MAKE_MEM_DEFINED_IF_ADDRESSABLE:
            return VG_ID__MAKE_MEM_DEFINED_IF_ADDRESSABLE;
    }

    return VG_ID__LAST;
}

static void
free_annotation_handler(void *p)
{
    annotation_handler_t *handler = (annotation_handler_t *) p;
    if (handler->num_args > 0) {
        HEAP_ARRAY_FREE(GLOBAL_DCONTEXT, handler->args, opnd_t, handler->num_args,
                        ACCT_OTHER, UNPROTECTED);
    }
    HEAP_ARRAY_FREE(GLOBAL_DCONTEXT, p, annotation_handler_t,
                    1, ACCT_OTHER, UNPROTECTED);
}
