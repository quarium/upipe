/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */

/** @file
 * @short Upipe module splitting tables of the PSI of a transport stream
 */

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uprobe.h>
#include <upipe/uref.h>
#include <upipe/uref_block.h>
#include <upipe/ubuf.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe-ts/uref_ts_flow.h>
#include <upipe-ts/upipe_ts_psi_split.h>
#include <upipe-modules/upipe_proxy.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include <bitstream/mpeg/ts.h>

/** we only accept blocks containing exactly one PSI section */
#define EXPECTED_FLOW_DEF "block.mpegtspsi."

/** @internal @This is the private context of a ts_psi_split pipe. */
struct upipe_ts_psi_split {
    /** list of subs */
    struct uchain subs;

    /** manager to create subs */
    struct upipe_mgr sub_mgr;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_ts_psi_split, upipe, UPIPE_TS_PSI_SPLIT_SIGNATURE)
UPIPE_HELPER_VOID(upipe_ts_psi_split)

/** @internal @This is the private context of an output of a ts_psi_split pipe. */
struct upipe_ts_psi_split_sub {
    /** structure for double-linked lists */
    struct uchain uchain;

    /** pipe acting as output */
    struct upipe *output;
    /** flow definition packet on this output */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_ts_psi_split_sub, upipe, UPIPE_TS_PSI_SPLIT_OUTPUT_SIGNATURE)
UPIPE_HELPER_FLOW(upipe_ts_psi_split_sub, NULL)
UPIPE_HELPER_OUTPUT(upipe_ts_psi_split_sub, output, flow_def, flow_def_sent)

UPIPE_HELPER_SUBPIPE(upipe_ts_psi_split, upipe_ts_psi_split_sub, sub,
                     sub_mgr, subs, uchain)

/** @internal @This allocates an output subpipe of a ts_psi_split pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_ts_psi_split_sub_alloc(struct upipe_mgr *mgr,
                                                  struct uprobe *uprobe,
                                                  uint32_t signature,
                                                  va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_ts_psi_split_sub_alloc_flow(mgr, uprobe,
                                                            signature,
                                                            args, &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_ts_psi_split_sub_init_output(upipe);
    upipe_ts_psi_split_sub_init_sub(upipe);
    upipe_ts_psi_split_sub_store_flow_def(upipe, flow_def);

    struct upipe_ts_psi_split *upipe_ts_psi_split =
        upipe_ts_psi_split_from_sub_mgr(mgr);
    upipe_use(upipe_ts_psi_split_to_upipe(upipe_ts_psi_split));

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This processes control commands on an output subpipe of a
 * ts_psi_split pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_ts_psi_split_sub_control(struct upipe *upipe,
                                              enum upipe_command command,
                                              va_list args)
{
    switch (command) {
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_ts_psi_split_sub_get_flow_def(upipe, p);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_ts_psi_split_sub_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_ts_psi_split_sub_set_output(upipe, output);
        }
        case UPIPE_SUB_GET_SUPER: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_ts_psi_split_sub_get_super(upipe, p);
        }

        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ts_psi_split_sub_free(struct upipe *upipe)
{
    struct upipe_ts_psi_split *upipe_ts_psi_split =
        upipe_ts_psi_split_from_sub_mgr(upipe->mgr);
    upipe_throw_dead(upipe);

    upipe_ts_psi_split_sub_clean_output(upipe);
    upipe_ts_psi_split_sub_clean_sub(upipe);
    upipe_ts_psi_split_sub_free_flow(upipe);

    upipe_release(upipe_ts_psi_split_to_upipe(upipe_ts_psi_split));
}

/** @internal @This initializes the output manager for a ts_psi_split pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ts_psi_split_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_ts_psi_split *upipe_ts_psi_split =
        upipe_ts_psi_split_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_ts_psi_split->sub_mgr;
    sub_mgr->signature = UPIPE_TS_PSI_SPLIT_OUTPUT_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_ts_psi_split_sub_alloc;
    sub_mgr->upipe_input = NULL;
    sub_mgr->upipe_control = upipe_ts_psi_split_sub_control;
    sub_mgr->upipe_free = upipe_ts_psi_split_sub_free;
    sub_mgr->upipe_mgr_free = NULL;
}

/** @internal @This allocates a ts_psi_split pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_ts_psi_split_alloc(struct upipe_mgr *mgr,
                                              struct uprobe *uprobe,
                                              uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_ts_psi_split_alloc_void(mgr, uprobe, signature,
                                                        args);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_ts_psi_split_init_sub_mgr(upipe);
    upipe_ts_psi_split_init_sub_subs(upipe);
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This demuxes a PSI section to the appropriate output(s).
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump pump that generated the buffer
 */
static void upipe_ts_psi_split_input(struct upipe *upipe, struct uref *uref,
                                     struct upump *upump)
{
    struct upipe_ts_psi_split *upipe_ts_psi_split =
        upipe_ts_psi_split_from_upipe(upipe);
    struct uchain *uchain;
    ulist_foreach (&upipe_ts_psi_split->subs, uchain) {
        struct upipe_ts_psi_split_sub *output =
                upipe_ts_psi_split_sub_from_uchain(uchain);
        const uint8_t *filter, *mask;
        size_t size;
        if (uref_ts_flow_get_psi_filter(output->flow_def, &filter, &mask,
                                        &size) &&
            uref_block_match(uref, filter, mask, size)) {
            if (likely(uchain->next == NULL)) {
                upipe_ts_psi_split_sub_output(
                        upipe_ts_psi_split_sub_to_upipe(output), uref,
                        upump);
                uref = NULL;
            } else {
                struct uref *new_uref = uref_dup(uref);
                if (likely(new_uref != NULL))
                    upipe_ts_psi_split_sub_output(
                            upipe_ts_psi_split_sub_to_upipe(output), new_uref,
                            upump);
                else {
                    uref_free(uref);
                    upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
                    return;
                }
            }
        }
    }
    if (uref != NULL)
        uref_free(uref);
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return false if the flow definition is not handled
 */
static bool upipe_ts_psi_split_set_flow_def(struct upipe *upipe,
                                            struct uref *flow_def)
{
    if (flow_def == NULL)
        return false;
    return uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF);
}

/** @internal @This processes control commands.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_ts_psi_split_control(struct upipe *upipe,
                                       enum upipe_command command, va_list args)
{
    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_ts_psi_split_set_flow_def(upipe, flow_def);
        }
        case UPIPE_GET_SUB_MGR: {
            struct upipe_mgr **p = va_arg(args, struct upipe_mgr **);
            return upipe_ts_psi_split_get_sub_mgr(upipe, p);
        }
        case UPIPE_ITERATE_SUB: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_ts_psi_split_iterate_sub(upipe, p);
        }

        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ts_psi_split_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);
    upipe_ts_psi_split_clean_sub_subs(upipe);
    upipe_ts_psi_split_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_ts_psi_split_mgr = {
    .signature = UPIPE_TS_PSI_SPLIT_SIGNATURE,

    .upipe_alloc = upipe_ts_psi_split_alloc,
    .upipe_input = upipe_ts_psi_split_input,
    .upipe_control = upipe_ts_psi_split_control,
    .upipe_free = upipe_ts_psi_split_free,

    .upipe_mgr_free = NULL
};

/** @This is called when the proxy is released.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ts_psi_split_proxy_released(struct upipe *upipe)
{
    upipe_ts_psi_split_throw_sub_subs(upipe, UPROBE_SOURCE_END);
}

/** @This returns the management structure for all ts_psi_split pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_ts_psi_split_mgr_alloc(void)
{
    return upipe_proxy_mgr_alloc(&upipe_ts_psi_split_mgr,
                                 upipe_ts_psi_split_proxy_released);
}
