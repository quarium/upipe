/*
 * Copyright (c) 2015 Arnaud de Turckheim <quarium@gmail.com>
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
 * @short Upipe rtp module to convert planar 16 bits linear audio to
 * block 16 bits linear audio
 */

#include <upipe-modules/upipe_rtp_l16.h>

#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_input.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_upipe.h>

#include <upipe/upipe.h>

#include <upipe/uref_sound_flow.h>
#include <upipe/uref_sound.h>
#include <upipe/uref_clock.h>

#include <upipe/ubuf_block.h>

#define EXPECTED_FLOW_DEF     "sound.s16."
#define OUTPUT_FLOW_DEF       "block.sound.s16."
/** we only accept 44100 Hz */
#define SAMPLE_RATE 44100

/** @internal @This is the private context of a rtp l16 pipe. */
struct upipe_rtp_l16 {
    /** public upipe structure */
    struct upipe upipe;
    /** refcount management structure */
    struct urefcount urefcount;
    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** request for ubuf manager */
    struct urequest request_ubuf_mgr;
    /** output request list */
    struct uchain requests;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** input flow format */
    struct uref *flow_def_input;
    /** output flow format */
    struct uref *flow_def;
    /** ubuf flow format */
    struct uref *flow_format;
    /** output pipe */
    struct upipe *output;
    /** list of urefs */
    struct uchain urefs;
    /** number of uref in the list */
    unsigned nb_urefs;
    /** maximum of uref in the list */
    unsigned max_urefs;
    /** list of blockers */
    struct uchain blockers;
};

/** @hidden */
static int upipe_rtp_l16_check(struct upipe *upipe, struct uref *flow_format);

/** @hidden */
static bool upipe_rtp_l16_process(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_rtp_l16, upipe, UPIPE_RTP_L16_SIGNATURE);
UPIPE_HELPER_VOID(upipe_rtp_l16);
UPIPE_HELPER_UREFCOUNT(upipe_rtp_l16, urefcount, upipe_rtp_l16_no_ref);
UPIPE_HELPER_INPUT(upipe_rtp_l16, urefs, nb_urefs, max_urefs, blockers,
                   upipe_rtp_l16_process);
UPIPE_HELPER_OUTPUT(upipe_rtp_l16, output, flow_def, output_state, requests);
UPIPE_HELPER_UBUF_MGR(upipe_rtp_l16, ubuf_mgr, flow_format, request_ubuf_mgr,
                      upipe_rtp_l16_check,
                      upipe_rtp_l16_register_output_request,
                      upipe_rtp_l16_unregister_output_request);

/** @internal @This allocates a rtp_l16 pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe
 */
static struct upipe *upipe_rtp_l16_alloc(struct upipe_mgr *mgr,
                                         struct uprobe *uprobe,
                                         uint32_t signature,
                                         va_list args)
{
    struct upipe *upipe =
        upipe_rtp_l16_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_rtp_l16_init_urefcount(upipe);
    upipe_rtp_l16_init_input(upipe);
    upipe_rtp_l16_init_output(upipe);
    upipe_rtp_l16_init_ubuf_mgr(upipe);

    struct upipe_rtp_l16 *upipe_rtp_l16 = upipe_rtp_l16_from_upipe(upipe);
    upipe_rtp_l16->flow_def_input = NULL;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This is called when there is no external reference to the pipe
 * and frees the pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_rtp_l16_no_ref(struct upipe *upipe)
{
    struct upipe_rtp_l16 *upipe_rtp_l16 = upipe_rtp_l16_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(upipe_rtp_l16->flow_def_input);
    upipe_rtp_l16_clean_ubuf_mgr(upipe);
    upipe_rtp_l16_clean_output(upipe);
    upipe_rtp_l16_clean_input(upipe);
    upipe_rtp_l16_clean_urefcount(upipe);
    upipe_rtp_l16_free_void(upipe);
}

static void upipe_rtp_l16_store_flow_def_input(struct upipe *upipe,
                                               struct uref *flow_def_input)
{
    struct upipe_rtp_l16 *upipe_rtp_l16 = upipe_rtp_l16_from_upipe(upipe);
    if (upipe_rtp_l16->flow_def_input)
        uref_free(upipe_rtp_l16->flow_def_input);
    upipe_rtp_l16->flow_def_input = flow_def_input;
}

/** @internal @This sets the real output flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_def the flow format to set
 * @return an error code
 */
static int upipe_rtp_l16_set_flow_def_real(struct upipe *upipe,
                                           struct uref *flow_def_input)
{
    upipe_rtp_l16_store_flow_def(upipe, NULL);
    upipe_rtp_l16_store_flow_def_input(upipe, flow_def_input);
    struct uref *flow_def = uref_sibling_alloc_control(flow_def_input);
    UBASE_ALLOC_RETURN(flow_def);
    int ret = uref_flow_set_def(flow_def, OUTPUT_FLOW_DEF);
    if (unlikely(!ubase_check(ret))) {
        uref_free(flow_def);
        return ret;
    }
    upipe_rtp_l16_require_ubuf_mgr(upipe, flow_def);
    return UBASE_ERR_NONE;
}

/** @internal @This handles data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref carrying the data
 * @param upump_p reference to pump that generated the buffer
 * @return false if the input must be blocked
 */
static bool upipe_rtp_l16_process(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_rtp_l16 *upipe_rtp_l16 = upipe_rtp_l16_from_upipe(upipe);
    int ret;

    const char *def;
    if (unlikely(ubase_check(uref_flow_get_def(uref, &def)))) {
        ret = upipe_rtp_l16_set_flow_def_real(upipe, uref);
        if (unlikely(!ubase_check(ret)))
            upipe_throw_fatal(upipe, ret);
        return true;
    }

    if (unlikely(upipe_rtp_l16->flow_def == NULL))
        return false;
    struct uref *flow_def_input = upipe_rtp_l16->flow_def_input;

    uint8_t planes;
    ret = uref_sound_flow_get_planes(flow_def_input, &planes);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "sound planes not found");
        uref_free(uref);
        return true;
    }

    uint8_t sample_size;
    ret = uref_sound_flow_get_sample_size(flow_def_input, &sample_size);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "sample size not found");
        uref_free(uref);
        return true;
    }

    uint64_t samples;
    ret = uref_sound_flow_get_samples(flow_def_input, &samples);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "number of samples not found");
        uref_free(uref);
        return true;
    }

    int size = samples * sample_size * planes;
    if (unlikely(size == 0)) {
        upipe_warn(upipe, "invalid flow format");
        uref_free(uref);
        return true;
    }

    struct ubuf *ubuf = ubuf_block_alloc(upipe_rtp_l16->ubuf_mgr, size);
    if (unlikely(ubuf == NULL)) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return true;
    }

    int ubuf_size = size;
    uint8_t *buffer;
    ret = ubuf_block_write(ubuf, 0, &ubuf_size, &buffer);
    if (unlikely(!ubase_check(ret)) || unlikely(ubuf_size != size)) {
        uref_free(uref);
        ubuf_free(ubuf);
        upipe_throw_fatal(upipe, UBASE_ERR_INVALID);
        return true;
    }

    ret = uref_sound_interleave(uref, buffer, 0, samples, sample_size, planes);
    for (unsigned i = 0; ubase_check(ret) && i < (unsigned)size; i += 2) {
        uint8_t tmp = buffer[i];
        buffer[i] = buffer[i + 1];
        buffer[i + 1] = tmp;
    }
    ubuf_block_unmap(ubuf, 0);
    if (unlikely(!ubase_check(ret))) {
        uref_free(uref);
        ubuf_free(ubuf);
        upipe_err(upipe, "fail to interleave uref");
        return true;
    }

    struct ubuf *ubuf_tmp = uref_detach_ubuf(uref);
    if (likely(ubuf_tmp != NULL))
        ubuf_free(ubuf_tmp);
    uref_attach_ubuf(uref, ubuf);

    uref_clock_set_cr_dts_delay(uref, 0);
    upipe_rtp_l16_output(upipe, uref, upump_p);
    return true;
}

/** @internal @This is called when there is input data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref carrying the data
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_rtp_l16_input(struct upipe *upipe,
                                struct uref *uref,
                                struct upump **upump_p)
{
    if (unlikely(!upipe_rtp_l16_check_input(upipe))) {
        upipe_rtp_l16_hold_input(upipe, uref);
        upipe_rtp_l16_block_input(upipe, upump_p);
    }
    else if (unlikely(!upipe_rtp_l16_process(upipe, uref, upump_p))) {
        upipe_rtp_l16_hold_input(upipe, uref);
        upipe_rtp_l16_block_input(upipe, upump_p);
        /* Increment upipe refcount to avoid disappearing before all packets
         * have been sent. */
        upipe_use(upipe);
    }
}

/** @internal @This receives a provided ubuf manager.
 *
 * @param upipe description structure of the pipe
 * @param flow_format amended flow format
 * @return an error code
 */
static int upipe_rtp_l16_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_rtp_l16 *upipe_rtp_l16 = upipe_rtp_l16_from_upipe(upipe);

    if (flow_format != NULL)
        upipe_rtp_l16_store_flow_def(upipe, flow_format);

    if (upipe_rtp_l16->flow_def == NULL)
        return UBASE_ERR_NONE;

    bool was_buffered = !upipe_rtp_l16_check_input(upipe);
    upipe_rtp_l16_output_input(upipe);
    upipe_rtp_l16_unblock_input(upipe);
    if (was_buffered && upipe_rtp_l16_check_input(upipe)) {
        /* All packets have been output, release again the pipe that has been
         * used in @ref upipe_rtp_l16_input. */
        upipe_release(upipe);
    }
    return UBASE_ERR_NONE;
}

/** @internal @This sets the output flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_def input flow definition format
 * @return an error code
 */
static int upipe_rtp_l16_set_flow_def(struct upipe *upipe,
                                      struct uref *flow_def)
{
    if (unlikely(flow_def == NULL))
        return UBASE_ERR_INVALID;

    UBASE_RETURN(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF));
    uint8_t planes;
    UBASE_RETURN(uref_sound_flow_get_planes(flow_def, &planes));
    uint8_t sample_size;
    UBASE_RETURN(uref_sound_flow_get_sample_size(flow_def, &sample_size));
    uint64_t samples;
    UBASE_RETURN(uref_sound_flow_get_samples(flow_def, &samples));

    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);
    upipe_input(upipe, flow_def_dup, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This provides a flow format suggestion.
 *
 * @param upipe description structure of the pipe
 * @param request description structure of the request
 * @return an error code
 */
static int upipe_rtp_l16_provide_flow_format(struct upipe *upipe,
                                             struct urequest *request)
{
    struct uref *flow_format = uref_dup(request->uref);
    UBASE_ALLOC_RETURN(flow_format);
    uref_sound_flow_clear_format(flow_format);
    uref_flow_set_def(flow_format, EXPECTED_FLOW_DEF);
    uref_sound_flow_set_channels(flow_format, 2);
    uref_sound_flow_set_samples(flow_format, 256);
    uref_sound_flow_set_sample_size(flow_format, 4);
    uref_sound_flow_set_planes(flow_format, 0);
    uref_sound_flow_add_plane(flow_format, "lr");
    uref_sound_flow_set_rate(flow_format, SAMPLE_RATE);
    return urequest_provide_flow_format(request, flow_format);
}

/** @internal @This dispatches the control commands.
 *
 * @param upipe description structure of the pipe
 * @param command command to dispatch
 * @param args optional arguments
 * @return an error code
 */
static int upipe_rtp_l16_control(struct upipe *upipe,
                                 int command,
                                 va_list args)
{
    switch (command) {
    case UPIPE_REGISTER_REQUEST: {
        struct urequest *request = va_arg(args, struct urequest *);
        if (request->type == UREQUEST_UBUF_MGR)
            return upipe_throw_provide_request(upipe, request);
        if (request->type == UREQUEST_FLOW_FORMAT)
            return upipe_rtp_l16_provide_flow_format(upipe, request);
        return upipe_rtp_l16_alloc_output_proxy(upipe, request);
    }
    case UPIPE_UNREGISTER_REQUEST: {
        struct urequest *request = va_arg(args, struct urequest *);
        if (request->type == UREQUEST_UBUF_MGR ||
            request->type == UREQUEST_FLOW_FORMAT)
            return UBASE_ERR_NONE;
        return upipe_rtp_l16_alloc_output_proxy(upipe, request);
    }

    case UPIPE_SET_FLOW_DEF: {
        struct uref *flow_def = va_arg(args, struct uref *);
        return upipe_rtp_l16_set_flow_def(upipe, flow_def);
    }
    case UPIPE_GET_FLOW_DEF: {
        struct uref **flow_def_p = va_arg(args, struct uref **);
        return upipe_rtp_l16_get_flow_def(upipe, flow_def_p);
    }
    case UPIPE_SET_OUTPUT: {
        struct upipe *output = va_arg(args, struct upipe *);
        return upipe_rtp_l16_set_output(upipe, output);
    }
    case UPIPE_GET_OUTPUT: {
        struct upipe **output_p = va_arg(args, struct upipe **);
        return upipe_rtp_l16_get_output(upipe, output_p);
    }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This is the static structure for rtp_l16 manager. */
static struct upipe_mgr upipe_rtp_l16_mgr = {
    .refcount = NULL,
    .signature = UPIPE_RTP_L16_SIGNATURE,
    .upipe_alloc = upipe_rtp_l16_alloc,
    .upipe_input = upipe_rtp_l16_input,
    .upipe_control = upipe_rtp_l16_control,
};

/** @This returns the management structure for rtp_l16 pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_rtp_l16_mgr_alloc(void)
{
    return &upipe_rtp_l16_mgr;
}
