/*
 * Copyright (C) 2018 OpenHeadend S.A.R.L.
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
 * @short timecode rendering
 */

#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_urefcount_real.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_uprobe.h>
#include <upipe/upipe_helper_inner.h>
#include <upipe/upipe_helper_bin_input.h>
#include <upipe/upipe_helper_bin_output.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uclock.h>

#include <upipe-modules/uprobe_blit_prepare.h>
#include <upipe-modules/upipe_blit.h>

#include <upipe-freetype/upipe_freetype.h>
#include <upipe-freetype/upipe_timecode.h>

/** expected input flow format */
#define EXPECTED_FLOW   UREF_PIC_FLOW_DEF

/** @internal @This is the private structure of timecode pipe. */
struct upipe_tc {
    /** public structure */
    struct upipe upipe;
    /** external reference */
    struct urefcount urefcount;
    /** reference counter */
    struct urefcount urefcount_real;
    /** inner blit pipe probe */
    struct uprobe blit_probe;
    /** inner text probe */
    struct uprobe text_probe;
    /** inner freetype probe */
    struct uprobe freetype_probe;
    /** inner blit pipe */
    struct upipe *blit;
    /** list of input requests */
    struct uchain input_requests;
    /** list of output requests */
    struct uchain output_requests;
    /** alloc flow format */
    struct uref *flow_def;
    /** output pipe */
    struct upipe *output;
    /** timecode text */
    struct upipe *text;
    /** inner freetype pipe */
    struct upipe *freetype;
    /** uclock */
    struct uclock *uclock;
    /** uclock request */
    struct urequest uclock_request;
};

/** @hidden */
static int upipe_tc_check(struct upipe *upipe, struct uref *flow_format);

UPIPE_HELPER_UPIPE(upipe_tc, upipe, UPIPE_TC_SIGNATURE);
UPIPE_HELPER_FLOW(upipe_tc, EXPECTED_FLOW);
UPIPE_HELPER_UREFCOUNT(upipe_tc, urefcount, upipe_tc_noref);
UPIPE_HELPER_UREFCOUNT_REAL(upipe_tc, urefcount_real, upipe_tc_free);
UPIPE_HELPER_UPROBE(upipe_tc, urefcount_real, blit_probe, NULL);
UPIPE_HELPER_UPROBE(upipe_tc, urefcount_real, text_probe, NULL);
UPIPE_HELPER_UPROBE(upipe_tc, urefcount_real, freetype_probe, NULL);
UPIPE_HELPER_INNER(upipe_tc, blit);
UPIPE_HELPER_INNER(upipe_tc, text);
UPIPE_HELPER_INNER(upipe_tc, freetype);
UPIPE_HELPER_BIN_INPUT(upipe_tc, blit, input_requests);
UPIPE_HELPER_BIN_OUTPUT(upipe_tc, blit, output, output_requests);
UPIPE_HELPER_UCLOCK(upipe_tc, uclock, uclock_request,
                    upipe_tc_check,
                    upipe_tc_register_bin_request,
                    upipe_tc_unregister_bin_request);

/** @internal @This checks the timecode flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_format timecode flow format
 * @return an error code
 */
static int upipe_tc_check_flow_format(struct upipe *upipe,
                                      struct uref *flow_def)
{
    UBASE_RETURN(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF));
    UBASE_RETURN(uref_pic_flow_get_hsize(flow_def, NULL));
    UBASE_RETURN(uref_pic_flow_get_vsize(flow_def, NULL));
    UBASE_RETURN(uref_pic_get_hposition(flow_def, NULL));
    UBASE_RETURN(uref_pic_get_vposition(flow_def, NULL));
    return UBASE_ERR_NONE;
}

/** @internal @This allocates a timecode pipe.
 *
 * @param mgr pointer to manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe or NULL
 */
static struct upipe *upipe_tc_alloc(struct upipe_mgr *mgr,
                                    struct uprobe *uprobe,
                                    uint32_t signature,
                                    va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_tc_alloc_flow(mgr, uprobe, signature, args,
                                              &flow_def);
    if (unlikely(!upipe))
        return NULL;

    upipe_tc_init_urefcount(upipe);
    upipe_tc_init_urefcount_real(upipe);
    upipe_tc_init_bin_input(upipe);
    upipe_tc_init_bin_output(upipe);
    upipe_tc_init_blit_probe(upipe);
    upipe_tc_init_text_probe(upipe);
    upipe_tc_init_freetype_probe(upipe);
    upipe_tc_init_text(upipe);
    upipe_tc_init_freetype(upipe);
    upipe_tc_init_uclock(upipe);

    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);
    upipe_tc->flow_def = flow_def;

    upipe_throw_ready(upipe);

    if (unlikely(!ubase_check(upipe_tc_check_flow_format(upipe, flow_def)))) {
        upipe_err(upipe, "invalid flow format");
        upipe_release(upipe);
        return NULL;
    }

    struct upipe_mgr *upipe_blit_mgr = upipe_blit_mgr_alloc();
    if (unlikely(!upipe_blit_mgr)) {
        upipe_err(upipe, "fail to allocate blit manager");
        upipe_release(upipe);
        return NULL;
    }
    struct upipe *blit = upipe_void_alloc(
        upipe_blit_mgr,
        uprobe_pfx_alloc(
            uprobe_blit_prepare_alloc(
                uprobe_use(upipe_tc_to_blit_probe(upipe_tc))),
            UPROBE_LOG_VERBOSE, "blit"));
    upipe_mgr_release(upipe_blit_mgr);
    if (unlikely(!blit)) {
        upipe_err(upipe, "fail to allocate blit pipe");
        upipe_release(upipe);
        return NULL;
    }
    upipe_attach_upump_mgr(blit);
    upipe_tc_store_bin_input(upipe, blit);
    upipe_tc_store_bin_output(upipe, upipe_use(blit));

    struct upipe_mgr *upipe_freetype_mgr = upipe_freetype_mgr_alloc();
    if (unlikely(!upipe_freetype_mgr)) {
        upipe_err(upipe, "fail to allocate freetype manager");
        upipe_release(upipe);
        return NULL;
    }
    struct upipe *freetype = upipe_flow_alloc(
        upipe_freetype_mgr,
        uprobe_pfx_alloc(
            uprobe_use(upipe_tc_to_freetype_probe(upipe_tc)),
            UPROBE_LOG_VERBOSE, "freetype"),
        flow_def);
    upipe_mgr_release(upipe_freetype_mgr);
    if (unlikely(!freetype)) {
        upipe_err(upipe, "fail to allocate freetype pipe");
        upipe_release(upipe);
        return NULL;
    }
    upipe_tc_store_freetype(upipe, freetype);
    struct uref *flow_def_freetype = uref_sibling_alloc_control(flow_def);
    if (unlikely(!flow_def_freetype)) {
        upipe_err(upipe, "fail to allocate freetype flow format");
        upipe_release(upipe);
        return NULL;
    }
    if (unlikely(!ubase_check(uref_flow_set_def(
                    flow_def_freetype, "void.text.")))) {
        upipe_err(upipe, "fail to set freetype flow def");
        uref_free(flow_def_freetype);
        upipe_release(upipe);
        return NULL;
    }
    upipe_set_flow_def(freetype, flow_def_freetype);
    uref_free(flow_def_freetype);

    struct upipe *text = upipe_void_alloc_sub(
        blit,
        uprobe_pfx_alloc(
            uprobe_use(upipe_tc_to_text_probe(upipe_tc)),
            UPROBE_LOG_VERBOSE, "text"));
    if (unlikely(!text)) {
        upipe_err(upipe, "fail to allocate text");
        upipe_release(upipe);
        return NULL;
    }
    upipe_tc_store_text(upipe, text);
    if (unlikely(!ubase_check(upipe_blit_sub_set_alpha_threshold(text, 0xff)))) {
        upipe_err(upipe, "fail to set alpha threshold");
        upipe_release(upipe);
        return NULL;
    }
    if (unlikely(!ubase_check(upipe_blit_sub_set_z_index(text, 2)))) {
        upipe_err(upipe, "fail to set z index");
        upipe_release(upipe);
        return NULL;
    }
    if (unlikely(!ubase_check(upipe_set_output(freetype, text)))) {
        upipe_err(upipe, "fail to link freetype to blit");
        upipe_release(upipe);
        return NULL;
    }

    return upipe;
}

/** @internal @This frees a timecode pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_tc_free(struct upipe *upipe)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(upipe_tc->flow_def);
    upipe_tc_clean_uclock(upipe);
    upipe_tc_clean_freetype(upipe);
    upipe_tc_clean_text(upipe);
    upipe_tc_clean_bin_output(upipe);
    upipe_tc_clean_bin_input(upipe);
    upipe_tc_clean_freetype_probe(upipe);
    upipe_tc_clean_text_probe(upipe);
    upipe_tc_clean_blit_probe(upipe);
    upipe_tc_clean_urefcount_real(upipe);
    upipe_tc_clean_urefcount(upipe);
    upipe_tc_free_flow(upipe);
}

/** @internal @This is called when there is no more external reference.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_tc_noref(struct upipe *upipe)
{
    upipe_tc_store_bin_output(upipe, NULL);
    upipe_tc_store_bin_input(upipe, NULL);
    upipe_tc_store_freetype(upipe, NULL);
    upipe_tc_store_text(upipe, NULL);
    upipe_tc_release_urefcount_real(upipe);
}

/** @internal @This handles input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_tc_input(struct upipe *upipe,
                           struct uref *uref,
                           struct upump **upump_p)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);

    uint64_t pts = 0;
    int ret = uref_clock_get_pts_sys(uref, &pts);
    if (unlikely(!ubase_check(ret)))
        upipe_warn(upipe, "non dated uref");

    unsigned h = 0, m = 0, s = 0, ms = 0;
    uint64_t real = 0;
    if (upipe_tc->uclock)
        real = uclock_to_real(upipe_tc->uclock, pts);
    else
        upipe_warn(upipe, "no clock");
    h = (real % (UCLOCK_FREQ * 60 * 60 * 24)) / (UCLOCK_FREQ * 60 * 60);
    m = (real % (UCLOCK_FREQ * 60 * 60)) / (UCLOCK_FREQ * 60);
    s = (real % (UCLOCK_FREQ * 60)) / UCLOCK_FREQ;
    ms = (real % UCLOCK_FREQ) / (UCLOCK_FREQ / 1000);
    assert(h < 24 && m < 60 && s < 60 && ms < 1000);
    char buffer[strlen("00:00:00.000") + 1];
    sprintf(buffer, "%02u:%02u:%02u.%03u", h, m, s, ms);
    struct uref *ft_txt = uref_sibling_alloc_control(uref);
    uref_attr_set_string(ft_txt, buffer, UDICT_TYPE_STRING, "text");
    upipe_input(upipe_tc->freetype, ft_txt, NULL);

    upipe_tc_bin_input(upipe, uref, upump_p);
}

/** @internal @This checks the internal state of the timecode pipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_format provided flow format
 * @return an error code
 */
static int upipe_tc_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);

    if (flow_format)
        uref_free(flow_format);

    if (unlikely(!upipe_tc->uclock)) {
        upipe_tc_require_uclock(upipe);
        return UBASE_ERR_NONE;
    }

    return UBASE_ERR_NONE;
}

/** @internal @This sets the timecode position.
 *
 * @param upipe description structure of the pipe
 * @param x horizontal offset
 * @param y vertical offset
 * @return an error code
 */
static int upipe_tc_set_position(struct upipe *upipe, int x, int y)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);
    UBASE_RETURN(uref_pic_set_hposition(upipe_tc->flow_def, x));
    UBASE_RETURN(uref_pic_set_vposition(upipe_tc->flow_def, y));
    return upipe_set_flow_def(upipe_tc->freetype, upipe_tc->flow_def);
}

/** @internal @This sets the freetype font size.
 *
 * @param upipe description structure of the pipe
 * @param size font size in pixel
 * @return an error code
 */
static int upipe_tc_set_size(struct upipe *upipe, uint64_t size)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);
    UBASE_RETURN(uref_pic_flow_set_hsize(upipe_tc->flow_def, size));
    return upipe_set_flow_def(upipe_tc->freetype, upipe_tc->flow_def);
}

/** @internal @This sets the input floe definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def input flow format
 * @return an error code
 */
static int upipe_tc_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_tc *upipe_tc = upipe_tc_from_upipe(upipe);
    UBASE_RETURN(uref_pic_flow_copy_hsize(upipe_tc->flow_def, flow_def));
    return upipe_set_flow_def(upipe_tc->blit, flow_def);
}

/** @internal @This handles the control commands.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_tc_control_real(struct upipe *upipe,
                                 int command, va_list args)
{
    switch (command) {
        case UPIPE_SET_OPTION:
            return upipe_tc_control_freetype(upipe, command, args);
        case UPIPE_GET_OUTPUT:
        case UPIPE_SET_OUTPUT:
        case UPIPE_BIN_GET_LAST_INNER:
            return upipe_tc_control_bin_output(upipe, command, args);
        case UPIPE_REGISTER_REQUEST:
        case UPIPE_UNREGISTER_REQUEST:
        case UPIPE_SET_FLOW_DEF:
        case UPIPE_BIN_GET_FIRST_INNER:
            return upipe_tc_control_bin_input(upipe, command, args);
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This handles the control commands and checks the internal state.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_tc_control(struct upipe *upipe,
                            int command, va_list args)
{
    UBASE_RETURN(upipe_tc_control_real(upipe, command, args));
    return upipe_tc_check(upipe, NULL);
}

/** @internal @This the static management structure for timecode pipes. */
static struct upipe_mgr upipe_tc_mgr = {
    .signature = UPIPE_TC_SIGNATURE,
    .refcount = NULL,
    .upipe_alloc = upipe_tc_alloc,
    .upipe_input = upipe_tc_input,
    .upipe_control = upipe_tc_control,
};

/** @This returns the managemenet structure for timecode pipes.
 *
 * @return a pointer to the timecode management structure
 */
struct upipe_mgr *upipe_tc_mgr_alloc(void)
{
    return &upipe_tc_mgr;
}
