/*
 * Copyright (C) 2026 EasyTools
 *
 * Authors: Arnaud de Turckheim
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module to force scan
 */

#include "upipe/uref_pic.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_flow.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_inner.h"
#include "upipe/upipe_helper_uprobe.h"
#include "upipe/upipe_helper_urefcount_real.h"
#include "upipe/upipe_helper_bin_input.h"
#include "upipe/upipe_helper_bin_output.h"
#include "upipe-modules/upipe_setscan.h"
#include "upipe-modules/upipe_setflowdef.h"
#include "upipe-modules/upipe_probe_uref.h"

/** @internal @This is the private structure for a setscan pipe. */
struct upipe_setscan {
    /** public refcount management structure */
    struct urefcount urefcount;
    /** private refcount managment structure */
    struct urefcount urefcount_real;
    /** input request list */
    struct uchain input_requests;
    /** output request list */
    struct uchain output_requests;
    /** output pipe */
    struct upipe *output;
    /** setflowdef proxy probe */
    struct uprobe setflowdef_probe;
    /** setflowdef inner pipe */
    struct upipe *setflowdef;
    /** probe_uref proxy probe */
    struct uprobe probe_uref_probe;
    /** probe_uref inner pipe */
    struct upipe *probe_uref;
    /** public upipe structure */
    struct upipe upipe;
    /** force progressive or interlaced */
    bool progressive;
};

/** @hidden */
static int upipe_setscan_setflowdef_probe(struct uprobe *uprobe,
                                          struct upipe *upipe, int event,
                                          va_list args);

/** @hidden */
static int upipe_setscan_probe_uref_probe(struct uprobe *uprobe,
                                          struct upipe *upipe, int event,
                                          va_list args);

UPIPE_HELPER_UPIPE(upipe_setscan, upipe, UPIPE_SETSCAN_SIGNATURE)
UPIPE_HELPER_FLOW(upipe_setscan, UREF_PIC_FLOW_DEF)
UPIPE_HELPER_UREFCOUNT(upipe_setscan, urefcount, upipe_setscan_no_ref)
UPIPE_HELPER_UREFCOUNT_REAL(upipe_setscan, urefcount_real, upipe_setscan_free)
UPIPE_HELPER_UPROBE(upipe_setscan, urefcount_real, setflowdef_probe,
                    upipe_setscan_setflowdef_probe)
UPIPE_HELPER_UPROBE(upipe_setscan, urefcount_real, probe_uref_probe,
                    upipe_setscan_probe_uref_probe)
UPIPE_HELPER_INNER(upipe_setscan, setflowdef)
UPIPE_HELPER_INNER(upipe_setscan, probe_uref)
UPIPE_HELPER_BIN_INPUT(upipe_setscan, setflowdef, input_requests)
UPIPE_HELPER_BIN_OUTPUT(upipe_setscan, probe_uref, output, output_requests)

/** @internal @This catches event from the setflowdef inner pipe.
 *
 * @param uprobe structure used to raise events
 * @param upipe description structure of the inner pipe
 * @param event event triggered by the inner pipe
 * @param args optional arguments
 * @return an error code
 */
static int upipe_setscan_setflowdef_probe(struct uprobe *uprobe,
                                          struct upipe *upipe, int event,
                                          va_list args)
{
    struct upipe_setscan *upipe_setscan =
        upipe_setscan_from_setflowdef_probe(uprobe);
    switch (event) {
        case UPROBE_NEW_FLOW_DEF:
        case UPROBE_NEED_OUTPUT:
            return UBASE_ERR_NONE;
    }
    return upipe_throw_proxy(upipe_setscan_to_upipe(upipe_setscan), upipe,
                             event, args);
}


/** @internal @This catches event from the probe_uref inner pipe.
 *
 * @param uprobe structure used to raise events
 * @param upipe description structure of the inner pipe
 * @param event event triggered by the inner pipe
 * @param args optional arguments
 * @return an error code
 */
static int upipe_setscan_probe_uref_probe(struct uprobe *uprobe,
                                          struct upipe *upipe, int event,
                                          va_list args)
{
    struct upipe_setscan *upipe_setscan =
        upipe_setscan_from_probe_uref_probe(uprobe);
    struct uref *uref = NULL;
    if (uprobe_probe_uref_check(event, args, &uref, NULL, NULL)) {
        if (uref)
            uref_pic_set_progressive(uref, upipe_setscan->progressive);
        return UBASE_ERR_NONE;
    }
    return upipe_throw_proxy(upipe_setscan_to_upipe(upipe_setscan), upipe,
                             event, args);
}

/** @internal @This allocates a setscan pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_setscan_alloc(struct upipe_mgr *mgr,
                                         struct uprobe *uprobe,
                                         uint32_t signature, va_list args)
{
    struct uref *flow_def = NULL;
    struct upipe *upipe =
        upipe_setscan_alloc_flow(mgr, uprobe, signature, args, &flow_def);

    struct upipe_setscan *upipe_setscan = upipe_setscan_from_upipe(upipe);

    upipe_setscan_init_urefcount(upipe);
    upipe_setscan_init_urefcount_real(upipe);
    upipe_setscan_init_setflowdef_probe(upipe);
    upipe_setscan_init_probe_uref_probe(upipe);
    upipe_setscan_init_bin_input(upipe);
    upipe_setscan_init_bin_output(upipe);
    upipe_setscan->progressive = uref_pic_check_progressive(flow_def);

    upipe_throw_ready(upipe);

    struct upipe_mgr *setflowdef_mgr = upipe_setflowdef_mgr_alloc();
    if (unlikely(!setflowdef_mgr)) {
        upipe_release(upipe);
        uref_free(flow_def);
        return NULL;
    }
    struct upipe *setflowdef = upipe_void_alloc(
        setflowdef_mgr,
        uprobe_pfx_alloc(uprobe_use(&upipe_setscan->setflowdef_probe),
                         UPROBE_LOG_VERBOSE, "setflowdef"));
    upipe_mgr_release(setflowdef_mgr);
    if (unlikely(!setflowdef)) {
        upipe_release(upipe);
        uref_free(flow_def);
        return NULL;
    }
    struct uref *flow_def_dup = uref_sibling_alloc_control(flow_def);
    uref_free(flow_def);
    if (unlikely(!flow_def_dup)) {
        upipe_release(setflowdef);
        upipe_release(upipe);
        return NULL;
    }
    uref_pic_set_progressive(flow_def_dup, upipe_setscan->progressive);
    upipe_setflowdef_set_dict(setflowdef, flow_def_dup);
    uref_free(flow_def_dup);
    upipe_setscan_store_bin_input(upipe, setflowdef);

    struct upipe_mgr *probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    if (unlikely(!probe_uref_mgr)) {
        upipe_release(upipe);
        return NULL;
    }
    struct upipe *probe_uref = upipe_void_alloc(
        probe_uref_mgr,
        uprobe_pfx_alloc(uprobe_use(&upipe_setscan->probe_uref_probe),
                         UPROBE_LOG_VERBOSE, "probe uref"));
    upipe_mgr_release(probe_uref_mgr);
    if (unlikely(!probe_uref)) {
        upipe_release(upipe);
        return NULL;
    }
    upipe_setscan_store_bin_output(upipe, probe_uref);

    return upipe;
}

/** @internal @This is called when there is no more references on the pipe and
 * frees it.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_setscan_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_setscan_clean_bin_output(upipe);
    upipe_setscan_clean_bin_input(upipe);
    upipe_setscan_clean_probe_uref_probe(upipe);
    upipe_setscan_clean_setflowdef_probe(upipe);
    upipe_setscan_clean_urefcount_real(upipe);
    upipe_setscan_clean_urefcount(upipe);
    upipe_setscan_free_flow(upipe);
}

/** @internal @This is called when there is no external references on the pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_setscan_no_ref(struct upipe *upipe)
{
    upipe_setscan_store_bin_output(upipe, NULL);
    upipe_setscan_store_bin_input(upipe, NULL);
    upipe_setscan_release_urefcount_real(upipe);
}

/** @internal @This handles the input buffer.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_setscan_input(struct upipe *upipe, struct uref *uref,
                                struct upump **upump_p)
{
    upipe_setscan_bin_input(upipe, uref, upump_p);
}

/** @internal @This handles control command on the pipe.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_setscan_control(struct upipe *upipe, int command, va_list args)
{
    switch (command) {
        case UPIPE_REGISTER_REQUEST:
        case UPIPE_UNREGISTER_REQUEST:
        case UPIPE_SET_FLOW_DEF:
            return upipe_setscan_control_bin_input(upipe, command, args);
    }
    return upipe_setscan_control_bin_output(upipe, command, args);
}

/** @internal @This is the static setscan pipe management structure. */
static struct upipe_mgr upipe_setscan_mgr = {
    .refcount = NULL,
    .signature = UPIPE_SETSCAN_SIGNATURE,

    .upipe_alloc = upipe_setscan_alloc,
    .upipe_input = upipe_setscan_input,
    .upipe_control = upipe_setscan_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for setscan pipes
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_setscan_mgr_alloc(void)
{
    return &upipe_setscan_mgr;
}
