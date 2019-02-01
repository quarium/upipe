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

/** @file
 * @short Upipe module generating blank audio for void urefs
 */

#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_flow_def.h>
#include <upipe/upipe_helper_input.h>

#include <upipe/uref_sound_flow.h>
#include <upipe/uref_void_flow.h>
#include <upipe/uref_clock.h>

#include <upipe/ubuf_sound.h>

#include <upipe/uclock.h>

#include <upipe/uref_dump.h>

#include <upipe-modules/upipe_audio_blank.h>

#include <stdlib.h>

/** @internal @This is the private structure of audio blank pipe. */
struct upipe_ablk {
    /** public pipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** output pipe */
    struct upipe *output;
    /** output flow def */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** urequest list */
    struct uchain requests;
    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** odd blank sound */
    struct ubuf *ubuf_odd;
    /** even blank sound */
    struct ubuf *ubuf_even;
    /** ubuf flow format */
    struct uref *flow_format;
    /** ubuf manager request */
    struct urequest ubuf_mgr_request;
    /** list of retained urefs */
    struct uchain urefs;
    /** number of retained urefs */
    unsigned nb_urefs;
    /** maximum number of retained urefs */
    unsigned max_urefs;
    /** list of blockers */
    struct uchain blockers;
    /** number of samples */
    uint64_t samples;
    /** sample size */
    uint8_t sample_size;
    /** input rate */
    uint64_t rate;
    /** input duration */
    uint64_t duration;
    /** fractional part */
    uint64_t remainder;
};

/** @hidden */
static int upipe_ablk_check(struct upipe *upipe, struct uref *flow_format);

/** @hidden */
static bool upipe_ablk_handle(struct upipe *upipe,
                              struct uref *uref,
                              struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_ablk, upipe, UPIPE_ABLK_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_ablk, urefcount, upipe_ablk_free);
UPIPE_HELPER_FLOW(upipe_ablk, UREF_SOUND_FLOW_DEF);
UPIPE_HELPER_OUTPUT(upipe_ablk, output, flow_def, output_state, requests);
UPIPE_HELPER_UBUF_MGR(upipe_ablk, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_ablk_check,
                      upipe_ablk_register_output_request,
                      upipe_ablk_unregister_output_request);
UPIPE_HELPER_INPUT(upipe_ablk, urefs, nb_urefs, max_urefs, blockers,
                   upipe_ablk_handle);

/** @internal @This frees an audio blank pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ablk_free(struct upipe *upipe)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);

    upipe_throw_dead(upipe);

    if (upipe_ablk->ubuf_even)
        ubuf_free(upipe_ablk->ubuf_even);
    if (upipe_ablk->ubuf_odd)
        ubuf_free(upipe_ablk->ubuf_odd);
    upipe_ablk_clean_input(upipe);
    upipe_ablk_clean_ubuf_mgr(upipe);
    upipe_ablk_clean_output(upipe);
    upipe_ablk_clean_urefcount(upipe);
    upipe_ablk_free_flow(upipe);
}

/** @internal @This checks the validity of a void flow def.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition to check
 * @return an error code
 */
static int upipe_ablk_check_void_flow_def(struct upipe *upipe,
                                          struct uref *flow_def)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);

    UBASE_RETURN(uref_flow_match_def(flow_def, UREF_VOID_FLOW_DEF));
    upipe_ablk->duration = UINT64_MAX;
    uref_clock_get_duration(flow_def, &upipe_ablk->duration);
    return UBASE_ERR_NONE;
}

/** @internal @This checks the validity of a sound flow def.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition to check
 * @return an error code
 */
static int upipe_ablk_check_sound_flow_def(struct upipe *upipe,
                                           struct uref *flow_def)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);
    uint8_t planes, channels;

    UBASE_RETURN(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF));
    UBASE_RETURN(uref_sound_flow_get_planes(flow_def, &planes));
    UBASE_RETURN(uref_sound_flow_get_channels(flow_def, &channels));
    UBASE_RETURN(uref_sound_flow_get_rate(flow_def, &upipe_ablk->rate));
    UBASE_RETURN(uref_sound_flow_get_sample_size(
            flow_def, &upipe_ablk->sample_size));
    upipe_ablk->samples = UINT64_MAX;
    uref_sound_flow_get_samples(flow_def, &upipe_ablk->samples);
    upipe_ablk->duration = UINT64_MAX;
    uref_clock_get_duration(flow_def, &upipe_ablk->duration);
    return UBASE_ERR_NONE;
}

/** @internal @This allocates an audio blank pipe.
 *
 * @param mgr management structure for this pipe type
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe
 */
static struct upipe *upipe_ablk_alloc(struct upipe_mgr *mgr,
                                      struct uprobe *uprobe,
                                      uint32_t signature,
                                      va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_ablk_alloc_flow(mgr, uprobe, signature,
                                                args, &flow_def);
    if (unlikely(!upipe))
        return NULL;

    upipe_ablk_init_urefcount(upipe);
    upipe_ablk_init_output(upipe);
    upipe_ablk_init_ubuf_mgr(upipe);
    upipe_ablk_init_input(upipe);

    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);
    upipe_ablk->ubuf_even = NULL;
    upipe_ablk->ubuf_odd = NULL;
    upipe_ablk->samples = UINT64_MAX;
    upipe_ablk->rate = UINT64_MAX;
    upipe_ablk->duration = UINT64_MAX;
    upipe_ablk->remainder = 0;

    upipe_throw_ready(upipe);

    if (unlikely(
            !ubase_check(upipe_ablk_check_sound_flow_def(upipe, flow_def)))) {
        uref_free(flow_def);
        upipe_release(upipe);
        return NULL;
    }

    upipe_ablk_store_flow_def(upipe, flow_def);

    return upipe;
}

/** @internal @This handles input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static bool upipe_ablk_handle(struct upipe *upipe,
                              struct uref *uref,
                              struct upump **upump_p)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);
    struct uref *flow_def = upipe_ablk->flow_def;

    if (uref->ubuf) {
        upipe_ablk_output(upipe, uref, upump_p);
        return true;
    }

    if (unlikely(!flow_def)) {
        upipe_warn(upipe, "no output flow definition set");
        uref_free(uref);
        return true;
    }

    if (unlikely(!upipe_ablk->ubuf_mgr))
        return false;

    uint64_t samples = upipe_ablk->samples;
    if (samples == UINT64_MAX) {
        uint64_t duration = upipe_ablk->duration;
        if (unlikely(duration == UINT64_MAX)) {
            upipe_warn(upipe, "input flow def has no samples or duration set");
            uref_free(uref);
            return true;
        }
        lldiv_t q = lldiv(duration * upipe_ablk->rate + upipe_ablk->remainder,
                          UCLOCK_FREQ);
        upipe_ablk->remainder = q.rem;
        samples = q.quot;
    }

    struct ubuf **ubuf_p =
        samples % 2 ? &upipe_ablk->ubuf_odd : &upipe_ablk->ubuf_even;
    if (!*ubuf_p) {
        upipe_verbose(upipe, "allocate blank sound");

        struct ubuf *ubuf = ubuf_sound_alloc(upipe_ablk->ubuf_mgr, samples);
        if (unlikely(!ubuf)) {
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
            uref_free(uref);
            return true;
        }

        const char *channel;
        uint8_t *buf = NULL;
        ubuf_sound_foreach_plane(ubuf, channel) {
            ubuf_sound_plane_write_uint8_t(ubuf, channel, 0, -1, &buf);
            memset(buf, 0, upipe_ablk->sample_size * samples);
            ubuf_sound_plane_unmap(ubuf, channel, 0, -1);
        }

        *ubuf_p = ubuf;
    }

    struct ubuf *ubuf = ubuf_dup(*ubuf_p);
    if (unlikely(!ubuf)) {
        upipe_err(upipe, "fail to duplicate blank buffer");
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return true;
    }

    uref_attach_ubuf(uref, ubuf);
    upipe_ablk_output(upipe, uref, upump_p);
    return true;
}

/** @internal @This handles the input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_ablk_input(struct upipe *upipe,
                             struct uref *uref,
                             struct upump **upump_p)
{
    if (!upipe_ablk_check_input(upipe)) {
        upipe_ablk_hold_input(upipe, uref);
        upipe_ablk_block_input(upipe, upump_p);
    }
    else if (!upipe_ablk_handle(upipe, uref, upump_p)) {
        upipe_ablk_hold_input(upipe, uref);
        upipe_ablk_block_input(upipe, upump_p);
        upipe_use(upipe);
    }
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def input flow definition to set
 * @return an error code
 */
static int upipe_ablk_set_flow_def(struct upipe *upipe,
                                   struct uref *flow_def)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);

    if (ubase_check(upipe_ablk_check_void_flow_def(upipe, flow_def))) {
    }
    else if (ubase_check(upipe_ablk_check_sound_flow_def(upipe, flow_def))) {
        struct uref *flow_def_dup = uref_dup(flow_def);
        if (unlikely(!flow_def_dup)) {
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
            return UBASE_ERR_ALLOC;
        }

        upipe_ablk_store_flow_def(upipe, flow_def_dup);
    }
    else {
        return UBASE_ERR_INVALID;
    }

    if (upipe_ablk->ubuf_even) {
        ubuf_free(upipe_ablk->ubuf_even);
        upipe_ablk->ubuf_even = NULL;
    }
    if (upipe_ablk->ubuf_odd) {
        ubuf_free(upipe_ablk->ubuf_odd);
        upipe_ablk->ubuf_odd = NULL;
    }

    if (upipe_ablk->ubuf_mgr &&
        !ubase_check(ubuf_mgr_check(upipe_ablk->ubuf_mgr, flow_def))) {
        ubuf_mgr_release(upipe_ablk->ubuf_mgr);
        upipe_ablk->ubuf_mgr = NULL;
    }

    return UBASE_ERR_NONE;
}

/** @internal @This sets the reference sound.
 *
 * @param upipe description structure of the pipe
 * @param uref reference sound buffer
 * @return an error code
 */
static int upipe_ablk_set_sound_real(struct upipe *upipe, struct uref *uref)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);
    if (upipe_ablk->ubuf_even)
        ubuf_free(upipe_ablk->ubuf_even);
    upipe_ablk->ubuf_even = uref->ubuf;
    uref->ubuf = NULL;
    uref_free(uref);
    return UBASE_ERR_NONE;
}

/** @internal @This handles pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_ablk_control_real(struct upipe *upipe,
                                   int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_ablk_control_output(upipe, command, args));

    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_ablk_set_flow_def(upipe, flow_def);
        }
        case UPIPE_ABLK_SET_SOUND: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_ABLK_SIGNATURE);
            struct uref *uref = va_arg(args, struct uref *);
            return upipe_ablk_set_sound_real(upipe, uref);
        }
    }

    return UBASE_ERR_UNHANDLED;
}

/** @internal @This checks if the ubuf manager need to be required.
 *
 * @param upipe description structure of the pipe
 * @param flow_format requested flow format
 * @return an error code
 */
static int upipe_ablk_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_ablk *upipe_ablk = upipe_ablk_from_upipe(upipe);

    if (flow_format)
        upipe_ablk_store_flow_def(upipe, flow_format);

    if (!upipe_ablk->flow_def)
        return UBASE_ERR_NONE;

    if (!upipe_ablk->ubuf_mgr) {
        uref_dump(upipe_ablk->flow_def, upipe->uprobe);
        upipe_ablk_require_ubuf_mgr(upipe, uref_dup(upipe_ablk->flow_def));
        return UBASE_ERR_NONE;
    }

    bool need_release = !upipe_ablk_check_input(upipe);
    if (upipe_ablk_output_input(upipe) && need_release)
        upipe_release(upipe);

    return UBASE_ERR_NONE;
}

/** @internal @This handles control commands and checks the ubuf manager.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_ablk_control(struct upipe *upipe,
                              int command, va_list args)
{
    UBASE_RETURN(upipe_ablk_control_real(upipe, command, args));
    return upipe_ablk_check(upipe, NULL);
}

/** @internal @This is the static audio blank pipe manager. */
static struct upipe_mgr upipe_ablk_mgr = {
    .refcount = NULL,
    .signature = UPIPE_ABLK_SIGNATURE,
    .upipe_alloc = upipe_ablk_alloc,
    .upipe_input = upipe_ablk_input,
    .upipe_control = upipe_ablk_control,
};

/** @This returns the audio blank pipe manager.
 *
 * @return a pipe manager
 */
struct upipe_mgr *upipe_ablk_mgr_alloc(void)
{
    return &upipe_ablk_mgr;
}
