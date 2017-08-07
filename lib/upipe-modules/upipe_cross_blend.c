/*
 * Copyright (C) 2017 OpenHeadend S.A.R.L.
 *
 * Authors: Arnaud de Turckheim
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_urefcount_real.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe/upipe_helper_flow_def.h>
#include <upipe/upipe_helper_input.h>

#include <upipe/uclock.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/uref_sound.h>
#include <upipe/upipe.h>

#include <upipe-modules/upipe_cross_blend.h>

#define UPIPE_XBLEND_IN_SIGNATURE       UBASE_FOURCC('x','b','l','i')
/** by default cross-blend for 200 ms */
#define CROSSBLEND_PERIOD (UCLOCK_FREQ / 5)

/** @internal @This is the private structure of an cross blend input
 * subpipe. */
struct upipe_xblend_in {
    /** public pipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** chain to the super pipe list */
    struct uchain uchain;
    /** retained urefs */
    struct uchain urefs;
    /** number of retained urefs */
    unsigned int nb_urefs;
    /** maximum number of retained urefs */
    unsigned int max_urefs;
    /** blocker list */
    struct uchain blockers;
    /** input flow def */
    struct uref *flow_def_input;
    /** flow def attributes */
    struct uref *flow_def_attr;
};

/** @hidden */
static bool upipe_xblend_in_output(struct upipe *upipe,
                                   struct uref *uref,
                                   struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_xblend_in, upipe, UPIPE_XBLEND_IN_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_xblend_in, urefcount, upipe_xblend_in_free);
UPIPE_HELPER_VOID(upipe_xblend_in);
UPIPE_HELPER_INPUT(upipe_xblend_in, urefs, nb_urefs, max_urefs, blockers,
                   upipe_xblend_in_output);
UPIPE_HELPER_FLOW_DEF(upipe_xblend_in, flow_def_input, flow_def_attr);

/** @internal @This is the private structure of a cross blend pipe. */
struct upipe_xblend {
    /** public pipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** internal refcount structure */
    struct urefcount urefcount_real;
    /** output pipe */
    struct upipe *output;
    /** output flow def */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** request list */
    struct uchain requests;
    /** subpipes list */
    struct uchain inputs;
    /** subpipes manager */
    struct upipe_mgr input_mgr;
    /** current input */
    struct upipe *current;
    /** previous input */
    struct upipe *previous;
    /** current crossblend value */
    float crossblend;
    /** crossblend step */
    float step;
    /** input sample rate */
    uint64_t sample_rate;
    /** crossblend duration */
    uint64_t period;
};

UPIPE_HELPER_UPIPE(upipe_xblend, upipe, UPIPE_XBLEND_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_xblend, urefcount, upipe_xblend_no_ref);
UPIPE_HELPER_UREFCOUNT_REAL(upipe_xblend, urefcount_real, upipe_xblend_free);
UPIPE_HELPER_VOID(upipe_xblend);
UPIPE_HELPER_OUTPUT(upipe_xblend, output, flow_def, output_state, requests);
UPIPE_HELPER_SUBPIPE(upipe_xblend, upipe_xblend_in, input, input_mgr,
                     inputs, uchain);

/** @internal @This frees a cross blend input.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_xblend_in_free(struct upipe *upipe)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(upipe->mgr);
    struct upipe *super = upipe_xblend_to_upipe(upipe_xblend);

    if (upipe_xblend->current == upipe) {
        struct upipe *previous = upipe_xblend->previous;
        upipe_xblend->current = NULL;
        upipe_xblend->previous = NULL;
        if (previous)
            upipe_throw_sink_end(previous);
    }
    else if (upipe_xblend->previous == upipe)
        upipe_xblend->previous = NULL;

    upipe_throw_dead(upipe);

    upipe_xblend_in_clean_flow_def(upipe);
    upipe_xblend_in_clean_input(upipe);
    upipe_xblend_in_clean_sub(upipe);
    upipe_xblend_in_clean_urefcount(upipe);
    upipe_xblend_in_free_void(upipe);
}

/** @internal @This allocates a cross blend input.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe.
 */
static struct upipe *upipe_xblend_in_alloc(struct upipe_mgr *mgr,
                                           struct uprobe *uprobe,
                                           uint32_t signature,
                                           va_list args)
{
    struct upipe *upipe =
        upipe_xblend_in_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(!upipe))
        return NULL;

    upipe_xblend_in_init_urefcount(upipe);
    upipe_xblend_in_init_sub(upipe);
    upipe_xblend_in_init_input(upipe);
    upipe_xblend_in_init_flow_def(upipe);

    upipe_throw_ready(upipe);

    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(mgr);
    struct upipe *previous = upipe_xblend->previous;
    upipe_xblend->previous = upipe_xblend->current;
    upipe_xblend->current = upipe;
    upipe_xblend->crossblend = 0.;

    if (previous)
        upipe_throw_sink_end(previous);

    if (upipe_xblend->previous)
        upipe_dbg(upipe, "start crossblending");

    return upipe;
}

/** @internal @This checks if the previous input has enough data to 
 * crossblend.
 *
 * @param upipe description structure of the previous pipe
 * @param size need size to crossblend
 * @param sample_size sample size targeted
 * @return true if crossblend is possible, false otherwise
 */
static bool upipe_xblend_check_previous(struct upipe *upipe,
                                        size_t size,
                                        uint8_t sample_size)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_upipe(upipe);
    struct upipe_xblend_in *previous = upipe_xblend->previous ?
        upipe_xblend_in_from_upipe(upipe_xblend->previous) : NULL;
    struct uref *flow_def = upipe_xblend->flow_def;

    /* check output flow format */
    if (unlikely(!flow_def))
        return false;

    size_t available = 0;
    struct uchain *uchain, *uchain_tmp;
    float crossblend = upipe_xblend->crossblend;
    ulist_delete_foreach(&previous->urefs, uchain, uchain_tmp) {
        /* check is available size is enough for crossblend completion */
        if (crossblend >= 1. || available >= size)
            break;

        /* check if the next uref is a flow format definition */
        struct uref *uref= uref_from_uchain(uchain);
        const char *def;
        int ret = uref_flow_get_def(uref, &def);
        if (unlikely(ubase_check(ret))) {
            /* must be compatible (see upipe_xblend_in_set_flow_def) */
            ubase_assert(uref_sound_flow_compare_format(uref, flow_def));
            ulist_delete(uchain);
            uref_free(uref);
            continue;
        }

        /* get the next input buffer size */
        size_t uref_size;
        uint8_t uref_sample_size;
        ret = uref_sound_size(uref, &uref_size, &uref_sample_size);
        /* is it valid? */
        if (unlikely(!ubase_check(ret)) || sample_size != uref_sample_size) {
            /* no, so remove it and continue */
            ulist_delete(uchain);
            uref_free(uref);
            continue;
        }
        available += uref_size;
        crossblend += uref_size * upipe_xblend->step;
    }

    return crossblend >= 1. || available >= size;
}

/** @internal @This sets a new input flow definition to the subpipe for real.
 *
 * @param upipe description structure of the subpipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_xblend_in_set_flow_def_real(struct upipe *upipe,
                                             struct uref *flow_def)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(upipe->mgr);

    assert(upipe == upipe_xblend->current);
    upipe_xblend_in_store_flow_def_input(upipe, flow_def);
    uint64_t rate;
    ubase_assert(uref_sound_flow_get_rate(flow_def, &rate));
    upipe_xblend->step = (float)UCLOCK_FREQ / rate / upipe_xblend->period;

    if (likely(upipe_xblend->current == upipe)) {
        struct uref *flow_def_dup = uref_dup(flow_def);
        if (unlikely(!flow_def_dup)) {
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
            return UBASE_ERR_ALLOC;
        }
        upipe_xblend_store_flow_def(upipe_xblend_to_upipe(upipe_xblend),
                                    flow_def_dup);
    }

    return UBASE_ERR_NONE;
}

/** @internal @This tries to output a sound buffer.
 *
 * @param upipe description structure of the current input pipe
 * @param uref sound buffer to output
 * @param upump_p reference to pump that generated the buffer
 * @return true if buffer was output, false otherwise.
 */
static bool upipe_xblend_in_output(struct upipe *upipe,
                                   struct uref *uref,
                                   struct upump **upump_p)
{
    struct upipe_xblend_in *upipe_xblend_in = upipe_xblend_in_from_upipe(upipe);
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(upipe->mgr);
    struct upipe *super = upipe_xblend_to_upipe(upipe_xblend);

    /* if the input uref is a new flow definition, apply it and return. */
    const char *def;
    if (unlikely(ubase_check(uref_flow_get_def(uref, &def)))) {
        upipe_xblend_in_set_flow_def_real(upipe, uref);
        return true;
    }

    /* if no input flow format is set, drop */
    if (unlikely(!upipe_xblend_in->flow_def_input)) {
        upipe_warn(upipe, "no input flow format set");
        uref_free(uref);
        return true;
    }

    /* if no output flow format is set, drop */
    if (unlikely(!upipe_xblend->flow_def)) {
        upipe_warn(upipe, "no output flow format set");
        uref_free(uref);
        return true;
    }

    /* if no previous input to crossblend output */
    if (likely(!upipe_xblend->previous)) {
        upipe_xblend_output(super, uref, upump_p);
        return true;
    }

    /* get input sound buffer size */
    size_t size;
    uint8_t sample_size;
    int ret = uref_sound_size(uref, &size, &sample_size);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "invalid sound buffer");
        uref_free(uref);
        return true;
    }

    /* check if we have enough previous input to output */
    if (!upipe_xblend_check_previous(super, size, sample_size))
        return false;

    /* prepare for crossblend */
    uint8_t planes = 0;
    ubase_assert(uref_sound_flow_get_planes(upipe_xblend->flow_def, &planes));
    float *dst[planes];
    ret = uref_sound_write_float(uref, 0, size, dst, planes);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "unable to write into sound buffer");
        uref_free(uref);
        return true;
    }

    /* crossblend */
    upipe_dbg_va(upipe, "crossblend: %f", upipe_xblend->crossblend);
    size_t offset = 0;
    while (size && upipe_xblend->crossblend < 1.) {
        struct uref *uref_prev =
            upipe_xblend_in_pop_input(upipe_xblend->previous);
        assert(uref_prev);
        size_t available = 0;
        ubase_assert(uref_sound_size(uref_prev, &available, NULL));
        size_t extract = available > size ? size : available;
        const float *src[planes];
        ubase_assert(uref_sound_read_float(uref_prev, 0, extract,
                                           src, planes));
        for (uint8_t plane = 0; plane < planes; plane++) {
            float crossblend = upipe_xblend->crossblend;
            for (size_t i = 0; i < extract && crossblend < 1.; i++) {
                dst[plane][offset + i] =
                    dst[plane][offset + i] * crossblend +
                    src[plane][i] * (1. - crossblend);
                crossblend += upipe_xblend->step;
            }
        }
        uref_sound_unmap(uref_prev, 0, extract, planes);
        upipe_xblend->crossblend += extract * upipe_xblend->step;
        offset += extract;
        size -= extract;

        if (extract < available) {
            uref_sound_resize(uref_prev, extract, -1);
            upipe_xblend_in_unshift_input(upipe_xblend->previous, uref_prev);
        }
        else
            uref_free(uref_prev);
    }
    uref_sound_unmap(uref, 0, size, planes);

    /* if crossblend is done, we don't need previous input anymore */
    if (upipe_xblend->crossblend >= 1.) {
        upipe_throw_sink_end(upipe_xblend->previous);
        upipe_xblend->previous = NULL;
    }

    /* output crossblended sound buffer */
    upipe_xblend_output(super, uref, upump_p);
    return true;
}

/** @internal @This outputs input buffers.
 *
 * @param upipe description structure of the subpipe
 * @param uref input buffer to output
 * @param upump_p reference to pump that generated the buffer
 * @return true if the buffer was output, false otherwise
 */
static void upipe_xblend_in_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_xblend_in *upipe_xblend_in = upipe_xblend_in_from_upipe(upipe);
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(upipe->mgr);

    if (upipe == upipe_xblend->current) {
        if (!upipe_xblend_in_check_input(upipe) ||
            !upipe_xblend_in_output(upipe, uref, upump_p))
            upipe_xblend_in_hold_input(upipe, uref);
    }
    else if (upipe == upipe_xblend->previous) {
        upipe_xblend_in_hold_input(upipe, uref);
        upipe_xblend_in_output_input(upipe_xblend->current);
    }
    else {
        uref_free(uref);
    }
}

/** @internal @This sets a new input flow definition to the subpipe.
 *
 * @param upipe description structure of the subpipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_xblend_in_set_flow_def(struct upipe *upipe,
                                        struct uref *flow_def)
{
    struct upipe_xblend_in *upipe_xblend_in = upipe_xblend_in_from_upipe(upipe);
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_input_mgr(upipe->mgr);
    uint8_t planes;
    uint64_t rate;

    UBASE_RETURN(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF));
    UBASE_RETURN(uref_sound_flow_get_planes(flow_def, &planes));
    UBASE_RETURN(uref_sound_flow_get_rate(flow_def, &rate));
    if (upipe_xblend->flow_def &&
        (!uref_sound_flow_compare_format(upipe_xblend->flow_def, flow_def) ||
         uref_sound_flow_cmp_rate(upipe_xblend->flow_def, flow_def))) {
        upipe_warn(upipe, "crossblend does not support flow def changes");
        return UBASE_ERR_INVALID;
    }

    struct uref *flow_def_dup = uref_dup(flow_def);
    if (unlikely(!flow_def_dup)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }

    upipe_input(upipe, flow_def_dup, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This handles the subpipe control commands.
 *
 * @param upipe description structure of the subpipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_xblend_in_control(struct upipe *upipe,
                                   int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_xblend_in_control_super(upipe, command, args));

    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_xblend_in_set_flow_def(upipe, flow_def);
        }
    }

    return UBASE_ERR_UNHANDLED;
}

/** @internal @This frees a cross blend pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_xblend_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_xblend_clean_sub_inputs(upipe);
    upipe_xblend_clean_output(upipe);
    upipe_xblend_clean_urefcount_real(upipe);
    upipe_xblend_clean_urefcount(upipe);
    upipe_xblend_free_void(upipe);
}

/** @internal @This is called when there is no more external reference.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_xblend_no_ref(struct upipe *upipe)
{
    upipe_xblend_release_urefcount_real(upipe);
}

/** @internal @This initializes the subpipe manager.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_xblend_init_input_mgr(struct upipe *upipe)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_upipe(upipe);
    struct upipe_mgr *mgr = upipe_xblend_to_input_mgr(upipe_xblend);

    mgr->signature = UPIPE_XBLEND_IN_SIGNATURE;
    mgr->refcount = upipe_xblend_to_urefcount_real(upipe_xblend);
    mgr->upipe_alloc = upipe_xblend_in_alloc;
    mgr->upipe_control = upipe_xblend_in_control;
    mgr->upipe_input = upipe_xblend_in_input;
}

/** @internal @This allocates a cross blend pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe
 */
static struct upipe *upipe_xblend_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature,
                                        va_list args)
{
    struct upipe *upipe = upipe_xblend_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(!upipe))
        return NULL;

    upipe_xblend_init_urefcount(upipe);
    upipe_xblend_init_urefcount_real(upipe);
    upipe_xblend_init_output(upipe);
    upipe_xblend_init_sub_inputs(upipe);
    upipe_xblend_init_input_mgr(upipe);

    struct upipe_xblend *upipe_xblend = upipe_xblend_from_upipe(upipe);
    upipe_xblend->current = NULL;
    upipe_xblend->previous = NULL;
    upipe_xblend->crossblend = 1.;
    upipe_xblend->step = 0.;
    upipe_xblend->period = CROSSBLEND_PERIOD;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This gets the cross blend duration.
 *
 * @param upipe description structure of the pipe
 * @param duration_p filled with the cross blend duration
 * @return an error code
 */
static int upipe_xblend_get_duration_real(struct upipe *upipe,
                                          uint64_t *duration_p)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_upipe(upipe);
    if (duration_p)
        *duration_p = upipe_xblend->period;
    return UBASE_ERR_NONE;
}

/** @internal @This sets the cross blend duration.
 *
 * @param upipe description structure of the pipe
 * @param duration cross blend duration to set
 * @return an error code
 */
static int upipe_xblend_set_duration_real(struct upipe *upipe,
                                          uint64_t duration)
{
    struct upipe_xblend *upipe_xblend = upipe_xblend_from_upipe(upipe);
    upipe_xblend->period = duration;
    return UBASE_ERR_NONE;
}

/** @internal @This handles cross blend pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_xblend_control(struct upipe *upipe, int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_xblend_control_output(upipe, command, args));
    UBASE_HANDLED_RETURN(upipe_xblend_control_inputs(upipe, command, args));

    switch (command) {
        case UPIPE_XBLEND_GET_DURATION: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_XBLEND_SIGNATURE);
            uint64_t *duration_p = va_arg(args, uint64_t *);
            return upipe_xblend_get_duration_real(upipe, duration_p);
        }
        case UPIPE_XBLEND_SET_DURATION: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_XBLEND_SIGNATURE);
            uint64_t duration = va_arg(args, uint64_t);
            return upipe_xblend_set_duration_real(upipe, duration);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This is the management structure for cross blend pipes. */
static struct upipe_mgr upipe_xblend_mgr = {
    .refcount = NULL,
    .signature = UPIPE_XBLEND_SIGNATURE,
    .upipe_alloc = upipe_xblend_alloc,
    .upipe_input = NULL,
    .upipe_control = upipe_xblend_control,
};

/** @This returns the cross blend pipes management structure.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_xblend_mgr_alloc(void)
{
    return &upipe_xblend_mgr;
}
