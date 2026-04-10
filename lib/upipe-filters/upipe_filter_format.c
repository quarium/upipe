/*
 * Copyright (C) 2014-2016 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Bin pipe transforming the input to the given format
 */

#include "upipe/ubase.h"
#include "upipe/uprobe.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/uref.h"
#include "upipe/uref_pic.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uref_pic_flow_formats.h"
#include "upipe/uref_sound_flow.h"
#include "upipe/upipe.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_flow.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_urefcount_real.h"
#include "upipe/upipe_helper_flow_format.h"
#include "upipe/upipe_helper_inner.h"
#include "upipe/upipe_helper_uprobe.h"
#include "upipe/upipe_helper_bin_input.h"
#include "upipe/upipe_helper_bin_output.h"
#include "upipe/upipe_helper_input.h"
#include "upipe/upipe_helper_flow_def.h"
#include "upipe-modules/upipe_setflowdef.h"
#include "upipe-modules/upipe_interlace.h"
#include "upipe-filters/upipe_filter_format.h"
#include "upipe-filters/upipe_filter_blend.h"
#include "upipe-swscale/upipe_sws.h"
#include "upipe-av/upipe_avfilter.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

enum upipe_ffmt_surface_type {
    UNKNOWN,
    SW,
    AV_VAAPI,
    AV_QSV,
    AV_NI_QUADRA,
};

static enum upipe_ffmt_surface_type
surface_type_from_str(const char *surface_type)
{
    if (!surface_type || !strcmp(surface_type, ""))
        return SW;
    else if (!strcmp(surface_type, "av.vaapi"))
        return AV_VAAPI;
    else if (!strcmp(surface_type, "av.qsv"))
        return AV_QSV;
    else if (!strcmp(surface_type, "av.ni_quadra"))
        return AV_NI_QUADRA;
    return UNKNOWN;
}

static const char *surface_type_str(enum upipe_ffmt_surface_type type)
{
    switch (type) {
        case SW:        return "sw";
        case AV_VAAPI:     return "av.vaapi";
        case AV_QSV:       return "av.qsv";
        case AV_NI_QUADRA: return "av.ni_quadra";
        default:        break;
    }
    return NULL;
}

struct upipe_ffmt_flow {
    bool progressive;
    bool tff;
    uint64_t hsize;
    uint64_t vsize;
    struct urational fps;
    enum upipe_ffmt_surface_type surface_type;
    bool hw;
    bool fullrange;
    const struct uref_pic_flow_format *format;
    bool sdr;
    bool hdr10;
    int bit_depth;
    int matrix_coefficients;
    int colour_primaries;
    int transfer_characteristics;
};

struct upipe_ffmt_config {
    struct upipe_ffmt_flow in;
    struct upipe_ffmt_flow out;
    bool hw;
    bool need_interlace;
    bool need_deint;
    bool need_scale;
    bool need_range;
    bool need_format;
    bool need_hw_transfer;
    bool need_hw_derive;
    bool need_tonemap;
    bool need_fps;
};

/** @internal @This is the private context of a ffmt manager. */
struct upipe_ffmt_mgr {
    /** refcount management structure */
    struct urefcount urefcount;

    /** pointer to swscale manager */
    struct upipe_mgr *sws_mgr;
    /** pointer to swresample manager */
    struct upipe_mgr *swr_mgr;
    /** pointer to interlace manager */
    struct upipe_mgr *interlace_mgr;
    /** pointer to deinterlace manager */
    struct upipe_mgr *deint_mgr;
    /** pointer to avfilter manager */
    struct upipe_mgr *avfilter_mgr;

    /** public upipe_mgr structure */
    struct upipe_mgr mgr;
};

UBASE_FROM_TO(upipe_ffmt_mgr, upipe_mgr, upipe_mgr, mgr)
UBASE_FROM_TO(upipe_ffmt_mgr, urefcount, urefcount, urefcount)

/** @hidden */
static bool upipe_ffmt_handle(struct upipe *upipe, struct uref *uref,
                              struct upump **upump_p);
/** @hidden */
static int upipe_ffmt_build(struct upipe *upipe);
/** @hidden */
static int upipe_ffmt_check_flow_format(struct upipe *upipe,
                                        struct uref *flow_format);

/** @hidden */
static int upipe_ffmt_proxy_probe(struct uprobe *uprobe, struct upipe *inner,
                                  int event, va_list args);

/** @internal @This is the private context of a ffmt pipe. */
struct upipe_ffmt {
    /** real refcount management structure */
    struct urefcount urefcount_real;
    /** refcount management structure exported to the public structure */
    struct urefcount urefcount;

    /** flow format request */
    struct urequest request;

    /** proxy probe */
    struct uprobe proxy_probe;
    /** probe for the last inner pipe */
    struct uprobe last_inner_probe;

    /** flow definition on the input */
    struct uref *flow_def_input;
    /** flow definition wanted on the output */
    struct uref *flow_def_wanted;
    /** requested flow definition */
    struct uref *flow_def_requested;
    /** flow definition provided */
    struct uref *flow_def_provided;
    /** list of input bin requests */
    struct uchain input_request_list;
    /** list of output bin requests */
    struct uchain output_request_list;
    /** first inner pipe of the bin (deint or sws or swr) */
    struct upipe *first_inner;
    /** avfilter pipe or NULL */
    struct upipe *avfilter;
    /** last inner pipe of the bin (sws or swr) */
    struct upipe *last_inner;
    /** output */
    struct upipe *output;

    /** temporary uref storage (used during urequest) */
    struct uchain urefs;
    /** nb urefs in storage */
    unsigned int nb_urefs;
    /** max urefs in storage */
    unsigned int max_urefs;
    /** list of blockers (used during urequest) */
    struct uchain blockers;

    /** swscale flags */
    int sws_flags;
    /** deinterlace_vaapi mode option */
    char *deinterlace_vaapi_mode;
    /** scale_vaapi mode option */
    char *scale_vaapi_mode;
    /** vpp_qsv deinterlace option */
    char *vpp_qsv_deinterlace;
    /** vpp_qsv scale_mode option */
    char *vpp_qsv_scale_mode;
    /** ni_quadra_scale filterblit option */
    char *ni_quadra_scale_filterblit;
    /** zscale filter option */
    char *zscale_filter;
    /** tonemap tonemap option */
    char *tonemap_tonemap;
    /** tonemap param option */
    char *tonemap_param;
    /** tonemap desat option */
    char *tonemap_desat;
    /** software deinterlace backend */
    char *sw_deinterlace;
    /** software interlace backend */
    char *sw_interlace;
    /** software scale backend */
    char *sw_scale;
    /** software format backend */
    char *sw_format;
    /** force scaling in SW */
    bool force_sw_scale;

    /** avfilter hw config type */
    char *hw_type;
    /** avfilter hw config device */
    char *hw_device;

    /** allow flow format to be forwarded downstream? */
    bool forward_flow_format;

    struct upipe_ffmt_config config;
    struct upipe_ffmt_config *current_config;
    bool enforce;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_ffmt, upipe, UPIPE_FFMT_SIGNATURE)
UPIPE_HELPER_FLOW(upipe_ffmt, NULL)
UPIPE_HELPER_UREFCOUNT(upipe_ffmt, urefcount, upipe_ffmt_no_ref)
UPIPE_HELPER_UREFCOUNT_REAL(upipe_ffmt, urefcount_real, upipe_ffmt_free)
UPIPE_HELPER_FLOW_DEF(upipe_ffmt, flow_def_input, flow_def_wanted)
UPIPE_HELPER_INPUT(upipe_ffmt, urefs, nb_urefs, max_urefs, blockers,
                  upipe_ffmt_handle)
UPIPE_HELPER_INNER(upipe_ffmt, first_inner)
UPIPE_HELPER_BIN_INPUT(upipe_ffmt, first_inner, input_request_list)
UPIPE_HELPER_INNER(upipe_ffmt, avfilter)
UPIPE_HELPER_INNER(upipe_ffmt, last_inner)
UPIPE_HELPER_UPROBE(upipe_ffmt, urefcount_real, last_inner_probe, NULL)
UPIPE_HELPER_UPROBE(upipe_ffmt, urefcount_real, proxy_probe,
                    upipe_ffmt_proxy_probe)
UPIPE_HELPER_BIN_OUTPUT(upipe_ffmt, last_inner, output, output_request_list)
UPIPE_HELPER_FLOW_FORMAT(upipe_ffmt, request,
                         upipe_ffmt_check_flow_format,
                         upipe_ffmt_register_bin_output_request,
                         upipe_ffmt_unregister_bin_output_request)

/** @internal @This catches events coming from an inner pipe, and
 * attaches them to the bin pipe.
 *
 * @param uprobe pointer to the probe in upipe_ffmt_alloc
 * @param inner pointer to the inner pipe
 * @param event event triggered by the inner pipe
 * @param args arguments of the event
 * @return an error code
 */
static int upipe_ffmt_proxy_probe(struct uprobe *uprobe, struct upipe *inner,
                                  int event, va_list args)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_proxy_probe(uprobe);
    struct upipe *upipe = upipe_ffmt_to_upipe(upipe_ffmt);

    switch (event) {
        case UPROBE_NEED_OUTPUT:
        case UPROBE_NEW_FLOW_DEF:
            return UBASE_ERR_NONE;
    }
    return upipe_throw_proxy(upipe, inner, event, args);
}

/** @internal @This allocates a ffmt pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_ffmt_alloc(struct upipe_mgr *mgr,
                                      struct uprobe *uprobe,
                                      uint32_t signature, va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_ffmt_alloc_flow(mgr, uprobe, signature, args,
                                                &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;

    if (unlikely(!flow_def)) {
        upipe_ffmt_free_flow(upipe);
        return NULL;
    }

    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    upipe_ffmt_init_urefcount(upipe);
    upipe_ffmt_init_urefcount_real(upipe);
    upipe_ffmt_init_flow_def(upipe);
    upipe_ffmt_init_flow_format(upipe);
    upipe_ffmt_init_input(upipe);
    upipe_ffmt_init_proxy_probe(upipe);
    upipe_ffmt_init_last_inner_probe(upipe);
    upipe_ffmt_init_avfilter(upipe);
    upipe_ffmt_init_bin_input(upipe);
    upipe_ffmt_init_bin_output(upipe);

    upipe_ffmt_store_flow_def_attr(upipe, flow_def);

    upipe_ffmt->flow_def_requested = NULL;
    upipe_ffmt->flow_def_provided = NULL;
    upipe_ffmt->sws_flags = 0;
    upipe_ffmt->deinterlace_vaapi_mode = NULL;
    upipe_ffmt->scale_vaapi_mode = NULL;
    upipe_ffmt->vpp_qsv_deinterlace = NULL;
    upipe_ffmt->vpp_qsv_scale_mode = NULL;
    upipe_ffmt->ni_quadra_scale_filterblit = NULL;
    upipe_ffmt->zscale_filter = NULL;
    upipe_ffmt->tonemap_tonemap = NULL;
    upipe_ffmt->tonemap_param = NULL;
    upipe_ffmt->tonemap_desat = NULL;
    upipe_ffmt->sw_deinterlace = NULL;
    upipe_ffmt->sw_interlace = NULL;
    upipe_ffmt->sw_scale = NULL;
    upipe_ffmt->sw_format = NULL;
    upipe_ffmt->hw_type = NULL;
    upipe_ffmt->hw_device = NULL;
    upipe_ffmt->forward_flow_format = true;
    upipe_ffmt->current_config = NULL;
    upipe_ffmt->enforce = false;
    upipe_ffmt->force_sw_scale = false;

    upipe_throw_ready(upipe);

    struct upipe_mgr *upipe_setflowdef_mgr = upipe_setflowdef_mgr_alloc();
    if (unlikely(!upipe_setflowdef_mgr)) {
        upipe_release(upipe);
        return NULL;
    }
    struct upipe *upipe_last_inner = upipe_void_alloc(
        upipe_setflowdef_mgr,
        uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_last_inner_probe(upipe_ffmt)),
                         UPROBE_LOG_VERBOSE, "setflowdef"));
    upipe_mgr_release(upipe_setflowdef_mgr);
    if (unlikely(!upipe_last_inner)) {
        upipe_release(upipe);
        return NULL;
    }

    upipe_ffmt_store_bin_output(upipe, upipe_last_inner);

    return upipe;
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param uref flow definition packet
 * @param def flow definition name
 * @return an error code
*/
static void upipe_ffmt_set_flow_def_real(struct upipe *upipe,
                                         struct uref *flow_def, const char *def)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (upipe_ffmt_check_flow_def_input(upipe, flow_def))
        return;

    struct uref *flow_def_requested =
        upipe_ffmt_store_flow_def_input(upipe, flow_def);
    if (unlikely(flow_def_requested == NULL)) {
        uref_free(upipe_ffmt->flow_def_requested);
        upipe_ffmt->flow_def_requested = NULL;
        uref_free(upipe_ffmt->flow_def_provided);
        upipe_ffmt->flow_def_provided = NULL;
        upipe_ffmt_store_bin_input(upipe, NULL);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return;
    }

    /** It is legal to have just "sound." in flow_def_wanted to avoid
     * changing unnecessarily the sample format. */
    const char *wanted_def = NULL;
    uref_flow_get_def(upipe_ffmt->flow_def_wanted, &wanted_def);
    if (!strcmp(wanted_def, UREF_SOUND_FLOW_DEF))
        uref_flow_set_def(flow_def_requested, def);

    if (upipe_ffmt->flow_def_requested &&
        !udict_cmp(upipe_ffmt->flow_def_requested->udict,
                   flow_def_requested->udict)) {
        uref_free(flow_def_requested);
        int err = upipe_ffmt_build(upipe);
        if (unlikely(!ubase_check(err))) {
            upipe_ffmt_store_bin_input(upipe, NULL);
        }
    } else {
        uref_free(upipe_ffmt->flow_def_requested);
        upipe_ffmt->flow_def_requested = flow_def_requested;
        uref_free(upipe_ffmt->flow_def_provided);
        upipe_ffmt->flow_def_provided = NULL;
        upipe_ffmt_require_flow_format(upipe, uref_dup(flow_def_requested));
    }
}

/** @internal @This handles data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 * @return true if the packet was handled
 */
static bool upipe_ffmt_handle(struct upipe *upipe, struct uref *uref,
                              struct upump **upump_p)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    const char *def;
    if (unlikely(ubase_check(uref_flow_get_def(uref, &def)))) {
        upipe_ffmt_set_flow_def_real(upipe, uref, def);
        return true;
    }

    if (!upipe_ffmt->flow_def_provided)
        return false;

    if (upipe_ffmt->first_inner == NULL) {
        if (!upipe_ffmt->flow_def_input) {
            upipe_warn_va(upipe, "dropping...");
            uref_free(uref);
            return true;
        }
        return false;
    }

    upipe_ffmt_bin_input(upipe, uref, upump_p);
    return true;
}

/** @internal @This inputs data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_ffmt_input(struct upipe *upipe, struct uref *uref,
                             struct upump **upump_p)
{
    if (!upipe_ffmt_check_input(upipe)) {
        upipe_ffmt_hold_input(upipe, uref);
        upipe_ffmt_block_input(upipe, upump_p);
    } else if (!upipe_ffmt_handle(upipe, uref, upump_p)) {
        upipe_ffmt_hold_input(upipe, uref);
        upipe_ffmt_block_input(upipe, upump_p);
        /* Increment upipe refcount to avoid disappearing before all packets
         * have been sent. */
        upipe_use(upipe);
    }
}

/** @internal @This is an helper function to create inner pipeline.
 *
 * @param upipe description structure of the pipe
 * @param first_p pointer on the first inner pipe, may be NULL
 * @param last_p pointer on the last inner pipe, may be NULL
 * @param inner pipe to push at the end of the pipeline
 * @return an error code
 */
static int upipe_ffmt_push_pipe(struct upipe *upipe, struct upipe **first_p,
                                struct upipe **last_p, struct upipe *inner)
{
    if (!first_p || !last_p) {
        upipe_release(inner);
        return UBASE_ERR_INVALID;
    }

    struct upipe *first = *first_p;
    struct upipe *last = *last_p;
    *first_p = NULL;
    *last_p = NULL;

    if (unlikely(!inner)) {
        upipe_release(last);
        upipe_release(first);
        return UBASE_ERR_INVALID;
    }

    if (!first || !last) {
        if (unlikely(first || last)) {
            upipe_release(inner);
            upipe_release(last);
            upipe_release(first);
            return UBASE_ERR_INVALID;
        }
        first = upipe_use(inner);
        last = inner;
    } else {
        int ret = upipe_set_output(last, inner);
        upipe_release(last);
        last = inner;
        if (unlikely(!ubase_check(ret))) {
            upipe_release(last);
            upipe_release(first);
            return ret;
        }
    }

    *first_p = first;
    *last_p = last;

    return UBASE_ERR_NONE;
}

/** @internal @This store the inner pipeline.
 *
 * @param upipe description structure of the pipe
 * @param first first inner pipe
 * @param last last inner pipe
 * @param flow_def input flow def to set
 * @return an error code
 */
static int upipe_ffmt_store_bin(struct upipe *upipe, struct upipe *first,
                                struct upipe *last, struct uref *flow_def)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    int err;

    if (last) {
        err = upipe_set_output(last, upipe_ffmt->last_inner);
        upipe_release(last);
        if (unlikely(!ubase_check(err))) {
            upipe_release(first);
            return err;
        }
    }

    if (!first)
        first = upipe_use(upipe_ffmt->last_inner);

    err = upipe_set_flow_def(first, flow_def);
    if (unlikely(!ubase_check(err))) {
        upipe_release(first);
        return err;
    }

    upipe_ffmt_store_bin_input(upipe, first);

    return UBASE_ERR_NONE;
}

static int upipe_ffmt_flow_from_flow_def(struct upipe_ffmt_flow *flow,
                                         struct uref *flow_def)
{
    flow->progressive = uref_pic_check_progressive(flow_def);
    flow->tff = uref_pic_check_tff(flow_def);
    flow->fps.num = flow->fps.den = 0;
    uref_pic_flow_get_fps(flow_def, &flow->fps);
    flow->hsize = 0;
    uref_pic_flow_get_hsize(flow_def, &flow->hsize);
    flow->vsize = 0;
    uref_pic_flow_get_vsize(flow_def, &flow->vsize);
    const char *surface_type = "";
    uref_pic_flow_get_surface_type(flow_def, &surface_type);
    flow->surface_type = surface_type_from_str(surface_type);
    flow->hw = flow->surface_type == AV_VAAPI || flow->surface_type == AV_QSV ||
               flow->surface_type == AV_NI_QUADRA;
    flow->fullrange = ubase_check(uref_pic_flow_get_full_range(flow_def));
    flow->format = uref_pic_flow_get_format(flow_def);
    flow->sdr = ubase_check(uref_pic_flow_check_sdr(flow_def));
    flow->hdr10 = ubase_check(uref_pic_flow_check_hdr10(flow_def));
    flow->bit_depth = 0;
    uref_pic_flow_get_bit_depth(flow_def, &flow->bit_depth);

    flow->matrix_coefficients = -1;
    uref_pic_flow_get_matrix_coefficients_val(flow_def,
                                              &flow->matrix_coefficients);
    flow->colour_primaries = -1;
    uref_pic_flow_get_colour_primaries_val(flow_def, &flow->colour_primaries);
    flow->transfer_characteristics = -1;
    uref_pic_flow_get_transfer_characteristics_val(
        flow_def, &flow->transfer_characteristics);

    if (flow->format == NULL || flow->surface_type == UNKNOWN)
        return UBASE_ERR_INVALID;

    return UBASE_ERR_NONE;
}

static int upipe_ffmt_load_config(struct upipe *upipe, struct uref *in,
                                  struct uref *out,
                                  struct upipe_ffmt_config *config)
{
    int err;

    err = upipe_ffmt_flow_from_flow_def(&config->in, in);
    if (unlikely(!ubase_check(err))) {
        upipe_warn(upipe, "invalid input flow def");
        udict_dump_warn(in->udict, upipe->uprobe);
        return err;
    }

    err = upipe_ffmt_flow_from_flow_def(&config->out, out);
    if (unlikely(!ubase_check(err))) {
        upipe_warn(upipe, "invalid output flow def");
        udict_dump_warn(out->udict, upipe->uprobe);
        return err;
    }

    config->hw = config->in.hw || config->out.hw;

    // frame rate
    config->need_fps = config->in.fps.num && config->in.fps.den &&
                       config->out.fps.num && config->out.fps.den &&
                       urational_cmp(&config->in.fps, &config->out.fps);
    if (config->need_fps)
        upipe_notice_va(upipe,
                        "need frame rate conversion %" PRIi64 "/%" PRIu64
                        " → %" PRIi64 "/%" PRIu64,
                        config->in.fps.num, config->in.fps.den,
                        config->out.fps.num, config->out.fps.den);

    // deinterlace and interlace?
    config->need_deint = false;
    config->need_interlace = false;
    if (config->in.progressive)
        config->need_interlace = !config->out.progressive;
    else if (config->out.progressive)
        config->need_deint = true;
    else if (config->in.tff != config->out.tff) {
        config->need_deint = true;
        config->need_interlace = true;
    } else if (config->need_fps) {
        config->need_deint = true;
        config->need_interlace = true;
    }
    if (config->need_deint)
        upipe_notice(upipe, "need deinterlace");
    if (config->need_interlace)
        upipe_notice(upipe, "need interlace");

    // scaling?
    config->need_scale = config->in.hsize != config->out.hsize ||
                         config->in.vsize != config->out.vsize;
    if (config->need_scale)
        upipe_notice_va(
            upipe, "need scale %" PRIu64 "x%" PRIu64 " → %" PRIu64 "x%" PRIu64,
            config->in.hsize, config->in.vsize,
            config->out.hsize, config->out.vsize);

    // range?
    config->need_range = config->in.fullrange != config->out.fullrange;
    if (config->need_range)
        upipe_notice_va(upipe, "need range conversion %s → %s",
                        config->in.fullrange ? "full" : "limited",
                        config->out.fullrange ? "full" : "limited");

    // format?
    config->need_format = config->in.format != config->out.format;
    if (config->need_format)
        upipe_notice_va(upipe, "need format conversion %s → %s",
                        config->in.format ? config->in.format->name : "unknown",
                        config->out.format ? config->out.format->name
                                           : "unknown");

    // hardware transfer?
    config->need_hw_transfer = (config->in.hw && !config->out.hw) ||
                               (!config->in.hw && config->out.hw);
    if (config->need_hw_transfer)
        upipe_notice_va(upipe, "need transfer %s → %s",
                        config->in.hw ? "hw" : "sw",
                        config->out.hw ? "hw" : "sw");

    // need mapping from hw to hw?
    config->need_hw_derive =
        config->in.hw && config->out.hw &&
        config->in.surface_type != config->out.surface_type;
    if (config->need_hw_derive) {
        if (config->in.surface_type == AV_VAAPI ||
            config->out.surface_type == AV_QSV)
            upipe_notice_va(upipe, "need hw surface mapping %s → %s",
                            surface_type_str(config->in.surface_type),
                            surface_type_str(config->out.surface_type));
        else
            upipe_warn_va(upipe, "hw surface mapping %s → %s not supported",
                          surface_type_str(config->in.surface_type),
                          surface_type_str(config->out.surface_type));
    }

    // tonemap?
    config->need_tonemap = config->in.hdr10 && config->out.sdr;
    if (config->need_tonemap)
        upipe_notice(upipe, "need tonemap hdr10 → sdr");

    if (config->hw && config->need_interlace)
        upipe_warn(upipe, "hardware interlacing is not supported");

    return UBASE_ERR_NONE;
}

static int upipe_ffmt_cmp_flow(const struct upipe_ffmt_flow *flow1,
                               const struct upipe_ffmt_flow *flow2)
{
    if (!flow1 || !flow2)
        return !flow1 && !flow2 ? 0 : 1;

    if (flow1->progressive != flow2->progressive || flow1->tff != flow2->tff ||
        flow1->hsize != flow2->hsize || flow1->vsize != flow2->vsize ||
        urational_cmp(&flow1->fps, &flow2->fps) ||
        flow1->surface_type != flow2->surface_type ||
        flow1->hw != flow2->hw ||
        flow1->fullrange != flow2->fullrange ||
        uref_pic_flow_format_cmp(flow1->format, flow2->format) ||
        flow1->sdr != flow2->sdr ||
        flow1->hdr10 != flow2->hdr10)
        return 1;
    return 0;
}

static int upipe_ffmt_cmp_config(const struct upipe_ffmt_config *config1,
                                 const struct upipe_ffmt_config *config2)
{
    if (!config1 || !config2)
        return !config1 && !config2 ? 0 : 1;

    if (config1->hw == config2->hw &&
        config1->need_interlace == config2->need_interlace &&
        config1->need_deint == config2->need_deint &&
        config1->need_scale == config2->need_scale &&
        config1->need_range == config2->need_range &&
        config1->need_format == config2->need_format &&
        config1->need_hw_transfer == config2->need_hw_transfer &&
        config1->need_hw_derive == config2->need_hw_derive &&
        config1->need_tonemap == config2->need_tonemap &&
        config1->need_fps == config2->need_fps &&
        !upipe_ffmt_cmp_flow(&config1->out, &config2->out))
        return 0;
    return 1;
}

static struct upipe *upipe_ffmt_alloc_deint(struct upipe *upipe,
                                            struct upipe_ffmt_config *config)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (unlikely(!ffmt_mgr->deint_mgr)) {
        upipe_warn(upipe, "deinterlace manager is not set");
        return NULL;
    }

    struct upipe *deint = upipe_void_alloc(
        ffmt_mgr->deint_mgr,
        uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_proxy_probe(upipe_ffmt)),
                         UPROBE_LOG_VERBOSE, "deint"));
    if (unlikely(!deint))
        upipe_warn(upipe, "couldn't allocate deinterlace");
    else
        config->need_deint = false;
    return deint;
}

static struct upipe *
upipe_ffmt_alloc_interlace(struct upipe *upipe,
                           struct upipe_ffmt_config *config)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (unlikely(!ffmt_mgr->interlace_mgr)) {
        upipe_warn(upipe, "interlace manager is not set");
        return NULL;
    }

    struct upipe *interlace = upipe_void_alloc(
        ffmt_mgr->interlace_mgr,
        uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_proxy_probe(upipe_ffmt)),
                         UPROBE_LOG_VERBOSE, "interlace"));
    if (unlikely(!interlace)) {
        upipe_warn_va(upipe, "couldn't allocate deinterlace");
    } else {
        upipe_set_option(interlace, "tff", config->out.tff ? "true" : "false");
        config->need_interlace = false;
    }
    return interlace;
}

static struct upipe *upipe_ffmt_alloc_sws(struct upipe *upipe,
                                          struct upipe_ffmt_config *config)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (unlikely(!ffmt_mgr->sws_mgr)) {
        upipe_warn(upipe, "swscale manager is not set");
        return NULL;
    }

    struct uref *flow_def = upipe_ffmt_alloc_flow_def_attr(upipe);
    if (unlikely(!flow_def)) {
        upipe_warn(upipe, "fail to allocate flow def");
        return NULL;
    }
    if (unlikely(!ubase_check(
                     uref_pic_flow_set_format(flow_def, config->out.format)) ||
                 !ubase_check(
                     uref_pic_flow_set_hsize(flow_def, config->out.hsize)) ||
                 !ubase_check(
                     uref_pic_flow_set_vsize(flow_def, config->out.vsize)))) {
        upipe_warn(upipe, "fail to configure swscale flow format");
        uref_free(flow_def);
        return NULL;
    }
    if (config->out.fullrange &&
        unlikely(!ubase_check(uref_pic_flow_set_full_range(flow_def)))) {
        upipe_warn(upipe, "fail to configure swscale flow format");
        uref_free(flow_def);
        return NULL;
    }

    struct upipe *sws = upipe_flow_alloc(
        ffmt_mgr->sws_mgr,
        uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_proxy_probe(upipe_ffmt)),
                         UPROBE_LOG_VERBOSE, "sws"),
        flow_def);
    if (unlikely(!sws)) {
        upipe_warn_va(upipe, "couldn't allocate swscale");
        udict_dump(flow_def->udict, upipe->uprobe);
        uref_free(flow_def);
        return NULL;
    }

    uref_free(flow_def);
    if (upipe_ffmt->sws_flags)
        upipe_sws_set_flags(sws, upipe_ffmt->sws_flags);
    config->need_format = false;
    config->need_scale = false;
    config->need_range = false;
    return sws;
}

static const char *
upipe_ffmt_build_filtergraph(struct upipe *upipe,
                             struct upipe_ffmt_config *config, char *buffer,
                             size_t size)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_mgr *avfilter_mgr = ffmt_mgr->avfilter_mgr;
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    bool need_fps = config->need_fps;
    bool need_deint = config->need_deint;
    bool need_scale = config->need_scale;
    bool need_format = config->need_format;
    bool need_range = config->need_range;
    bool need_tonemap = config->need_tonemap;
    enum upipe_ffmt_surface_type surface_type = config->in.surface_type;
    const char *range_out = config->out.fullrange ? "full" : "limited";
    bool out_10bit = config->out.bit_depth == 10;
    const char *pix_fmt_planar = out_10bit ? "yuv420p10le" : "yuv420p";
    const char *pix_fmt_semiplanar = out_10bit ? "p010le" : "nv12";
    int pos = 0;
    int opt = 0;

    if (!avfilter_mgr) {
        upipe_warn(upipe, "avfilter manager is not set");
        return NULL;
    }

    const char *color_matrix = NULL;
    if (config->out.matrix_coefficients != -1 &&
        config->out.matrix_coefficients != 2)
        upipe_avfilt_mgr_get_color_space_name(avfilter_mgr,
                                              config->out.matrix_coefficients,
                                              &color_matrix);

    const char *color_primaries = NULL;
    if (config->out.colour_primaries != -1 &&
        config->out.colour_primaries != 2)
        upipe_avfilt_mgr_get_color_primaries_name(avfilter_mgr,
                                                  config->out.colour_primaries,
                                                  &color_primaries);

    const char *color_transfer = NULL;
    if (config->out.transfer_characteristics != -1 &&
        config->out.transfer_characteristics != 2)
        upipe_avfilt_mgr_get_color_transfer_name(
            ffmt_mgr->avfilter_mgr, config->out.transfer_characteristics,
            &color_transfer);

    const char *in_surface = surface_type_str(config->in.surface_type);
    const char *in_pix_fmt = NULL;
    const char *in_pix_fmt_sw = NULL;
    const char *out_surface = surface_type_str(config->out.surface_type);
    const char *out_pix_fmt = NULL;
    const char *out_pix_fmt_sw = NULL;
    struct uref *flow_def = NULL;

    flow_def = upipe_ffmt_alloc_flow_def_attr(upipe);
    uref_pic_flow_set_format(flow_def, config->in.format);
    if (in_surface && config->in.surface_type != SW)
        uref_pic_flow_set_surface_type(flow_def, in_surface);
    upipe_avfilt_mgr_get_pixfmt_name(avfilter_mgr, flow_def, &in_pix_fmt,
                                     false);
    upipe_avfilt_mgr_get_pixfmt_name(avfilter_mgr, flow_def, &in_pix_fmt_sw,
                                     true);
    uref_free(flow_def);

    flow_def = upipe_ffmt_alloc_flow_def_attr(upipe);
    uref_pic_flow_set_format(flow_def, config->out.format);
    if (out_surface && config->out.surface_type != SW)
        uref_pic_flow_set_surface_type(flow_def, out_surface);
    upipe_avfilt_mgr_get_pixfmt_name(avfilter_mgr, flow_def, &out_pix_fmt,
                                     false);
    upipe_avfilt_mgr_get_pixfmt_name(avfilter_mgr, flow_def, &out_pix_fmt_sw,
                                     true);
    uref_free(flow_def);

    size_t size_get(size_t size, int pos) {
        return pos < size ? size - pos : 0;
    }

    char *buffer_get(char *buffer, int pos, size_t size) {
        return buffer && pos < size ? buffer + pos : NULL;
    }

#define add_filter(Name)                                                       \
    pos +=                                                                     \
        (opt = 0, snprintf(buffer_get(buffer, pos, size), size_get(size, pos), \
                           "%s%s", pos ? "," : "", Name))
#define add_option(Fmt, ...)                                                \
    pos += snprintf(buffer_get(buffer, pos, size), size_get(size, pos), "%s" Fmt, \
                    opt++ ? ":" : "=", ##__VA_ARGS__)

    if (surface_type == SW) {
        bool need_sw_deint =
            (need_deint && config->out.surface_type == SW) ||
            (need_deint && config->out.surface_type == AV_NI_QUADRA) ||
            need_fps;
        bool need_sw_tonemap = (need_tonemap && config->out.surface_type ==SW);
        bool need_sw_scale = (need_scale && config->out.surface_type == SW) ||
                             (need_scale && upipe_ffmt->force_sw_scale);

        if (need_sw_deint) {
            add_filter("scale");
            add_option("interl=-1");
            add_filter("format");
            add_option("%s", pix_fmt_planar);
            add_filter("yadif");
            add_option("deint=interlaced");
            need_deint = false;
        }

        if (need_sw_tonemap) {
            add_filter("zscale");
            if (need_sw_scale) {
                add_option("width=%" PRIu64, config->out.hsize);
                add_option("height=%" PRIu64, config->out.vsize);
                add_option("filter=%s", upipe_ffmt->zscale_filter ?: "bicubic");
                need_sw_scale = false;
                need_scale = false;
            }
            add_option("npl=100");
            add_option("transfer=linear");
            add_option("primaries=%s", color_primaries);
            add_filter("tonemap");
            add_option("tonemap=%s", upipe_ffmt->tonemap_tonemap ?: "hable");
            if (upipe_ffmt->tonemap_param)
                add_option("param=%s", upipe_ffmt->tonemap_param);
            if (upipe_ffmt->tonemap_desat)
                add_option("desat=%s", upipe_ffmt->tonemap_desat);
            add_filter("zscale");
            add_option("range=%s", range_out);
            add_option("transfer=%s", color_transfer);
            add_option("matrix=%s", color_matrix);
            add_filter("format");
            add_option("%s", out_pix_fmt_sw);
            need_tonemap = false;
            need_range = false;
            need_format = false;
        }

        if (need_sw_scale) {
            add_filter("scale");
            add_option("w=%" PRIu64, config->out.hsize);
            add_option("h=%" PRIu64, config->out.vsize);
            need_scale = false;
        }

        // SW framerate conversion if needed
        if (need_fps) {
            add_filter("framerate");
            add_option("fps=%" PRIi64 "/%" PRIu64, config->out.fps.num,
                       config->out.fps.den);
            need_fps = false;
        }

        if (config->out.surface_type == SW) {
            if (need_format) {
                add_filter("scale");
                add_option("interl=-1");
                add_filter("format");
                add_option("%s", out_pix_fmt);
                need_format = false;
            }

        } else {
            // SW -> HW
            if (config->out.surface_type == AV_QSV ||
                config->out.surface_type == AV_VAAPI) {
                add_filter("scale");
                add_option("interl=-1");
                add_filter("format");
                add_option("%s", pix_fmt_semiplanar);
            }

            add_filter("hwupload");
            surface_type = config->out.surface_type;
        }
    } else if (surface_type == AV_VAAPI && config->out.surface_type == AV_QSV) {
        add_filter("hwmap");
        add_option("derive_device=qsv");
        add_filter("format");
        add_option("qsv");
        surface_type = AV_QSV;
    }

    // HW deint, scale, convert
    if (surface_type == AV_QSV) {
        const char *deinterlace = upipe_ffmt->vpp_qsv_deinterlace ?: "advanced";
        const char *scale_mode = upipe_ffmt->vpp_qsv_scale_mode ?: "hq";

        add_filter("vpp_qsv");
        if (need_deint) {
            add_option("deinterlace=%s", deinterlace);
            need_deint = false;
        }

        if (need_scale) {
            add_option("width=%" PRIu64, config->out.hsize);
            add_option("height=%" PRIu64, config->out.vsize);
            need_scale = false;
        }

        add_option("scale_mode=%s", scale_mode);

        if (need_format) {
            add_option("format=%s", out_pix_fmt_sw);
            need_format = false;
        }

        if (need_range) {
            add_option("out_range=%s", range_out);
            need_range = false;
        }

        if (color_matrix)
            add_option("out_color_matrix=%s", color_matrix);
        if (color_primaries)
            add_option("out_color_primaries=%s", color_primaries);
        if (color_transfer)
            add_option("out_color_transfer=%s", color_transfer);
        add_option("tonemap=%d", need_tonemap ? 1 : 0);
        need_tonemap = false;
        add_option("async_depth=0");

    } else if (surface_type == AV_VAAPI) {
        const char *deinterlace_mode = upipe_ffmt->deinterlace_vaapi_mode;
        const char *scale_mode = upipe_ffmt->scale_vaapi_mode ?: "hq";

        if (need_deint) {
            add_filter("deinterlace_vaapi");
            add_option("auto=1");
            if (deinterlace_mode)
                add_option("mode=%s", deinterlace_mode);
            need_deint = false;
        }

        if (need_scale || need_format || need_range) {
            add_filter("scale_vaapi");
            add_option("mode=%s", scale_mode);
            if (need_scale) {
                add_option("w=%" PRIu64, config->out.hsize);
                add_option("h=%" PRIu64, config->out.vsize);
                need_scale = false;
            }
            if (need_range) {
                add_option("out_range=%s", range_out);
                need_range = false;
            }
            if (color_primaries)
                add_option("out_color_primaries=%s", color_primaries);
            if (color_transfer)
                add_option("out_color_transfer=%s", color_transfer);
            if (color_matrix)
                add_option("out_color_matrix=%s", color_matrix);
            if (need_format) {
                add_option("format=%s", out_pix_fmt_sw);
                need_format = false;
            }
        }

        if (need_tonemap) {
            add_filter("tonemap_vaapi");
            add_option("format=%s", out_pix_fmt_sw);
            if (color_matrix)
                add_option("matrix=%s", color_matrix);
            if (color_primaries)
                add_option("primaries=%s", color_primaries);
            if (color_transfer)
                add_option("transfer=%s", color_transfer);
            need_tonemap = false;
        }

    } else if (surface_type == AV_NI_QUADRA) {
        const char *scale_filterblit = upipe_ffmt->ni_quadra_scale_filterblit;

        if (need_scale || need_format) {
            add_filter("ni_quadra_scale");
            if (need_scale) {
                add_option("size=%" PRIu64 "x%" PRIu64, config->out.hsize,
                           config->out.vsize);
                if (upipe_ffmt->enforce)
                    add_option("auto_skip=1");
                need_scale = false;
            }
            if (scale_filterblit)
                add_option("filterblit=%s", scale_filterblit);
            else
                add_option("autoselect=1");
            if (color_matrix)
                add_option("out_color_matrix=%s", color_matrix);
            if (need_format) {
                add_option("format=%s", out_pix_fmt_sw);
                need_format = false;
            }
        }
    }

    // HW -> SW
    if (surface_type != SW && config->out.surface_type == SW) {
        if (surface_type == AV_VAAPI) {
            add_filter("hwmap");
            add_option("mode=read+direct");
        } else {
            add_filter("hwdownload");
        }
        surface_type = SW;
    }

#undef add_filter
#undef add_option

    if (!pos)
        upipe_warn(upipe, "no filtergraph");
    else if (pos >= size)
        upipe_warn(upipe, "filtergraph is too long");
    else {
        config->need_fps = need_fps;
        config->need_deint = need_deint;
        config->need_format = need_format;
        config->need_scale = need_scale;
        config->need_range = need_range;
        config->need_tonemap = need_tonemap;
        config->in.surface_type = surface_type;
        return buffer;
    }
    return NULL;
}

static struct upipe *upipe_ffmt_alloc_avfilter(struct upipe *upipe,
                                               struct upipe_ffmt_config *config)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    int err;

    if (unlikely(!ffmt_mgr->avfilter_mgr)) {
        upipe_warn(upipe, "avfilt manager is not set");
        return NULL;
    }

    char buffer[512];
    const char *filters =
        upipe_ffmt_build_filtergraph(upipe, config, buffer, sizeof(buffer));

    struct upipe *avfilt = upipe_void_alloc(
        ffmt_mgr->avfilter_mgr,
        uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_proxy_probe(upipe_ffmt)),
                         UPROBE_LOG_VERBOSE, "avfilt"));
    if (unlikely(!avfilt)) {
        upipe_warn_va(upipe, "couldn't allocate deinterlace");
        return NULL;
    }

    if (upipe_ffmt->hw_type) {
        err = upipe_avfilt_set_hw_config(avfilt, upipe_ffmt->hw_type,
                                         upipe_ffmt->hw_device);
        if (unlikely(!ubase_check(err))) {
            upipe_err(upipe, "cannot set filters hw config");
            upipe_release(avfilt);
            return NULL;
        }
    }

    err = upipe_avfilt_set_filters_desc(avfilt, filters);
    if (unlikely(!ubase_check(err))) {
        upipe_err(upipe, "cannot set filters desc");
        upipe_release(avfilt);
        return NULL;
    }

    return avfilt;
}

/** @internal @This builds the inner pipeline for pic flow.
 *
 * @param upipe description structure of the pipe
 * @param flow_def_input input flow definition
 * @param flow_def_output output flow definition we want
 * @return an error code
 */
static int upipe_ffmt_build_pic(struct upipe *upipe,
                                struct uref *flow_def_input,
                                struct uref *flow_def_output)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    struct uref *flow_def_wanted = upipe_ffmt->flow_def_wanted;
    const char *sw_deinterlace =
        upipe_ffmt->sw_deinterlace ?: "upipe_filter_blend";
    const char *sw_interlace = upipe_ffmt->sw_interlace ?: "upipe_interlace";
    const char *sw_scale = upipe_ffmt->sw_scale ?: "upipe_sws";
    const char *sw_format = upipe_ffmt->sw_format ?: "upipe_sws";
    int err;

    struct upipe *first = NULL, *last = NULL;

    /* check aspect ratio */
    struct urational sar, dar;
    if (ubase_check(uref_pic_flow_get_sar(flow_def_wanted, &sar)) && sar.num) {
        struct urational input_sar;
        uint64_t hsize;
        if (!ubase_check(uref_pic_flow_get_hsize(flow_def_wanted, &hsize)) &&
            ubase_check(uref_pic_flow_get_hsize(flow_def_input, &hsize)) &&
            ubase_check(uref_pic_flow_get_sar(flow_def_input, &input_sar)) &&
            input_sar.num) {
            struct urational sar_factor = urational_divide(&input_sar, &sar);
            hsize = (hsize * sar_factor.num / sar_factor.den / 2) * 2;
            uref_pic_flow_set_hsize(flow_def_output, hsize);
            uref_pic_flow_set_hsize_visible(flow_def_output, hsize);
        }
        uref_pic_flow_set_sar(flow_def_input, sar);
    } else if (ubase_check(uref_pic_flow_get_dar(flow_def_wanted, &dar))) {
        bool overscan;
        if (ubase_check(uref_pic_flow_get_overscan(flow_def_wanted, &overscan)))
            uref_pic_flow_set_overscan(flow_def_input, overscan);
        uref_pic_flow_infer_sar(flow_def_input, dar);
    }

    /* delete sar and visible sizes to let sws set it */
    if (!ubase_check(uref_pic_flow_get_sar(flow_def_wanted, NULL)) ||
        !ubase_check(uref_pic_flow_get_hsize(flow_def_wanted, NULL)) ||
        !ubase_check(uref_pic_flow_get_vsize(flow_def_wanted, NULL)))
        uref_pic_flow_delete_sar(flow_def_output);
    uref_pic_flow_delete_hsize_visible(flow_def_output);
    uref_pic_flow_delete_vsize_visible(flow_def_output);


    struct upipe_ffmt_config config;
    upipe_ffmt_load_config(upipe, flow_def_input, flow_def_output, &config);

    if (upipe_ffmt->enforce) {
        if (uref_pic_flow_get_format(flow_def_wanted))
            config.need_format = true;
        bool progressive;
        if (ubase_check(
                uref_pic_get_progressive(flow_def_wanted, &progressive))) {
            if (progressive)
                config.need_deint = true;
            else
                config.need_interlace = true;
        }

        if (ubase_check(uref_pic_flow_get_hsize(flow_def_wanted, NULL)) ||
            ubase_check(uref_pic_flow_get_vsize(flow_def_wanted, NULL)))
            config.need_scale = true;
    }

    bool need_avfilter =
        config.hw &&
        (config.need_scale || config.need_format || config.need_range ||
         config.need_deint || config.need_hw_transfer || config.need_hw_derive);
    need_avfilter |= config.need_deint && !strcmp(sw_deinterlace, "avfilter");
    need_avfilter |= config.need_interlace && !strcmp(sw_interlace, "avfilter");
    need_avfilter |= config.need_scale && !strcmp(sw_scale, "avfilter");
    need_avfilter |= config.need_format && !strcmp(sw_format, "avfilter");
    need_avfilter |= config.need_fps;
    need_avfilter |= config.need_tonemap;

    bool need_update =
        upipe_ffmt_cmp_config(upipe_ffmt->current_config, &config) != 0;
    upipe_ffmt->config = config;
    upipe_ffmt->current_config = &upipe_ffmt->config;

    if (need_update) {
        upipe_notice(upipe, "need update");

        if (need_avfilter) {
            struct upipe *input = upipe_ffmt_alloc_avfilter(upipe, &config);
            upipe_ffmt_store_avfilter(upipe, upipe_use(input));
            UBASE_RETURN(upipe_ffmt_push_pipe(upipe, &first, &last, input));
        } else {
            upipe_ffmt_store_avfilter(upipe, NULL);
        }

        if (config.need_deint) {
            struct upipe *input = upipe_ffmt_alloc_deint(upipe, &config);
            UBASE_RETURN(upipe_ffmt_push_pipe(upipe, &first, &last, input));
        }

        if (config.need_scale || config.need_format || config.need_range) {
            struct upipe *input = upipe_ffmt_alloc_sws(upipe, &config);
            UBASE_RETURN(upipe_ffmt_push_pipe(upipe, &first, &last, input));
        }

        if (config.need_interlace) {
            struct upipe *input = upipe_ffmt_alloc_interlace(upipe, &config);
            UBASE_RETURN(upipe_ffmt_push_pipe(upipe, &first, &last, input));
            config.need_interlace = false;
        }
    }
    else {
        upipe_notice(upipe, "no need to reconfigure");
        upipe_set_flow_def(upipe_ffmt->first_inner, flow_def_input);
    }

    uref_flow_delete_def(flow_def_output);
    uref_pic_flow_clear_format(flow_def_output);
    uref_pic_flow_delete_hsize(flow_def_output);
    uref_pic_flow_delete_vsize(flow_def_output);
    uref_pic_delete_progressive(flow_def_output);
    uref_pic_delete_tff(flow_def_output);
    uref_pic_flow_delete_fps(flow_def_output);
    uref_pic_flow_delete_surface_type(flow_def_output);
    err = upipe_setflowdef_set_dict(upipe_ffmt->last_inner, flow_def_output);
    if (unlikely(!ubase_check(err))) {
        upipe_release(first);
        upipe_release(last);
        return err;
    }

    return need_update
             ? upipe_ffmt_store_bin(upipe, first, last, flow_def_input)
             : UBASE_ERR_NONE;
}

/** @internal @This builds the inner pipeline for sound flow.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_ffmt_build_sound(struct upipe *upipe,
                                  struct uref *flow_def_input,
                                  struct uref *flow_def_output)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(upipe->mgr);
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    struct upipe *first = NULL, *last = NULL;
    int err;

    if (!uref_sound_flow_compare_format(flow_def_input, flow_def_output) ||
        uref_sound_flow_cmp_rate(flow_def_input, flow_def_output)) {
        struct upipe *input = upipe_flow_alloc(
            ffmt_mgr->swr_mgr,
            uprobe_pfx_alloc(uprobe_use(upipe_ffmt_to_proxy_probe(upipe_ffmt)),
                             UPROBE_LOG_VERBOSE, "swr"),
            flow_def_output);
        if (unlikely(!input)) {
            upipe_warn_va(upipe, "couldn't allocate swresample");
            udict_dump(flow_def_output->udict, upipe->uprobe);
            upipe_release(first);
            upipe_release(last);
            return UBASE_ERR_ALLOC;
        }
        upipe_ffmt_push_pipe(upipe, &first, &last, input);
    }

    uref_flow_delete_def(flow_def_output);
    uref_sound_flow_clear_format(flow_def_output);
    uref_sound_flow_delete_rate(flow_def_output);
    err = upipe_setflowdef_set_dict(upipe_ffmt->last_inner, flow_def_output);
    if (unlikely(!ubase_check(err))) {
        upipe_release(first);
        upipe_release(last);
        return err;
    }

    return upipe_ffmt_store_bin(upipe, first, last, flow_def_input);
}

static int upipe_ffmt_build(struct upipe *upipe)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    struct uref *flow_def_wanted = upipe_ffmt->flow_def_wanted;
    struct uref *flow_def_output = uref_dup(upipe_ffmt->flow_def_provided);
    struct uref *flow_def_input = uref_dup(upipe_ffmt->flow_def_input);
    int err;

    if (unlikely(!flow_def_output || !flow_def_input)) {
        uref_free(flow_def_output);
        uref_free(flow_def_input);
        return UBASE_ERR_ALLOC;
    }

    // enforce configured attributes
    uref_attr_import(flow_def_output, flow_def_wanted);
    const char *def = NULL;
    uref_flow_get_def(upipe_ffmt->flow_def_wanted, &def);
    if (def && !strcmp(def, UREF_SOUND_FLOW_DEF))
        uref_flow_copy_def(flow_def_output, upipe_ffmt->flow_def_provided);

    if (!ubase_ncmp(def, UREF_PIC_FLOW_DEF))
        err = upipe_ffmt_build_pic(upipe, flow_def_input, flow_def_output);
    else if (!ubase_ncmp(def, UREF_SOUND_FLOW_DEF))
        err = upipe_ffmt_build_sound(upipe, flow_def_input, flow_def_output);
    else
        err = UBASE_ERR_UNHANDLED;

    uref_free(flow_def_output);
    uref_free(flow_def_input);

    return err;
}

/** @internal @This receives the result of a flow format request.
 *
 * @param upipe description structure of the pipe
 * @param flow_def_dup amended flow format
 * @return an error code
 */
static int upipe_ffmt_check_flow_format(struct upipe *upipe,
                                        struct uref *flow_def)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (flow_def == NULL)
        return UBASE_ERR_INVALID;

    if (upipe_ffmt->flow_def_provided &&
        !udict_cmp(upipe_ffmt->flow_def_provided->udict, flow_def->udict)) {
        // nothing changed
        uref_free(flow_def);
        return UBASE_ERR_NONE;
    }

    uref_free(upipe_ffmt->flow_def_provided);
    upipe_ffmt->flow_def_provided = flow_def;

    int err = upipe_ffmt_build(upipe);
    if (unlikely(!ubase_check(err)))
        upipe_ffmt_store_bin_input(upipe, NULL);

    bool was_buffered = !upipe_ffmt_check_input(upipe);
    upipe_ffmt_output_input(upipe);
    upipe_ffmt_unblock_input(upipe);
    if (was_buffered && upipe_ffmt_check_input(upipe)) {
        /* All packets have been output, release again the pipe that has been
         * used in @ref upipe_ffmt_input. */
        upipe_release(upipe);
    }
    return err;
}

/** @internal @This sets the filters options.
 *
 * @param upipe description structure of the pipe
 * @param option option name (filter name/option)
 * @param value value or NULL to use the default value
 * @return an error code
 */
static int upipe_ffmt_set_option(struct upipe *upipe,
                                 const char *option,
                                 const char *value)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

#define SET_OPTION_VALUE(Field, Value) \
    UBASE_RETURN(ubase_strdup(&upipe_ffmt->Field, Value))

#define SET_OPTION(Option, Field) \
    if (!strcmp(option, Option)) { \
        SET_OPTION_VALUE(Field, value) \
        return UBASE_ERR_NONE; \
    }

    SET_OPTION("deinterlace_vaapi/mode", deinterlace_vaapi_mode)
    SET_OPTION("scale_vaapi/mode", scale_vaapi_mode)
    SET_OPTION("vpp_qsv/deinterlace", vpp_qsv_deinterlace)
    SET_OPTION("vpp_qsv/scale_mode", vpp_qsv_scale_mode)
    SET_OPTION("ni_quadra_scale/filterblit", ni_quadra_scale_filterblit)
    SET_OPTION("zscale/filter", zscale_filter)
    SET_OPTION("tonemap/tonemap", tonemap_tonemap)
    SET_OPTION("tonemap/param", tonemap_param)
    SET_OPTION("tonemap/desat", tonemap_desat)
    SET_OPTION("sw/deinterlace", sw_deinterlace)
    SET_OPTION("sw/interlace", sw_interlace)
    SET_OPTION("sw/scale", sw_scale)
    SET_OPTION("sw/format", sw_format)

    if (!strcmp(option, "deinterlace-preset")) {
        if (!strcmp(value, "fast")) {
            SET_OPTION_VALUE(deinterlace_vaapi_mode, "bob");
            SET_OPTION_VALUE(vpp_qsv_deinterlace, "bob");
        } else if (!strcmp(value, "hq")) {
            SET_OPTION_VALUE(deinterlace_vaapi_mode, "motion_compensated");
            SET_OPTION_VALUE(vpp_qsv_deinterlace, "advanced");
        } else
            return UBASE_ERR_INVALID;
    } else if (!strcmp(option, "scale-preset")) {
        if (!strcmp(value, "fast")) {
            SET_OPTION_VALUE(scale_vaapi_mode, "fast");
            SET_OPTION_VALUE(vpp_qsv_scale_mode, "low_power");
            SET_OPTION_VALUE(ni_quadra_scale_filterblit, "0");
            SET_OPTION_VALUE(zscale_filter, "bilinear");
        } else if (!strcmp(value, "hq")) {
            SET_OPTION_VALUE(scale_vaapi_mode, "hq");
            SET_OPTION_VALUE(vpp_qsv_scale_mode, "hq");
            SET_OPTION_VALUE(ni_quadra_scale_filterblit, NULL);
            SET_OPTION_VALUE(zscale_filter, "bicubic");
        } else
            return UBASE_ERR_INVALID;
    } else if (!strcmp(option, "forward_flow_format")) {
        if (!strcmp(value, "0") || !strcasecmp(value, "false") ||
            !strcasecmp(value, "disable"))
            upipe_ffmt->forward_flow_format = false;
        else if (!strcmp(value, "1") || !strcasecmp(value, "true") ||
                 !strcasecmp(value, "enable"))
            upipe_ffmt->forward_flow_format = true;
        else
            return UBASE_ERR_INVALID;
    } else if (!strcmp(option, "enforce")) {
        if (!strcmp(value, "0") || !strcasecmp(value, "false") ||
            !strcasecmp(value, "disable"))
            upipe_ffmt->enforce = false;
        else if (!strcmp(value, "1") || !strcasecmp(value, "true") ||
                 !strcasecmp(value, "enable"))
            upipe_ffmt->enforce = true;
        else
            return UBASE_ERR_INVALID;
    } else if (!strcmp(option, "force_sw_scale")) {
        if (!strcmp(value, "0") || !strcasecmp(value, "false") ||
            !strcasecmp(value, "disable"))
            upipe_ffmt->force_sw_scale = false;
        else if (!strcmp(value, "1") || !strcasecmp(value, "true") ||
                 !strcasecmp(value, "enable"))
            upipe_ffmt->force_sw_scale = true;
        else
            return UBASE_ERR_INVALID;
    } else
        return UBASE_ERR_INVALID;

#undef SET_OPTION_VALUE
#undef SET_OPTION

    return UBASE_ERR_NONE;
}

/** @internal @This pushes the input flow definition inband.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static int upipe_ffmt_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;
    const char *def_wanted, *def;
    UBASE_RETURN(uref_flow_get_def(flow_def, &def))
    UBASE_RETURN(uref_flow_get_def(upipe_ffmt->flow_def_wanted, &def_wanted))
    if (!((!ubase_ncmp(def, UREF_PIC_FLOW_DEF) &&
           !ubase_ncmp(def_wanted, UREF_PIC_FLOW_DEF)) ||
          (!ubase_ncmp(def, UREF_SOUND_FLOW_DEF) &&
           !ubase_ncmp(def_wanted, UREF_SOUND_FLOW_DEF))))
        return UBASE_ERR_INVALID;

    struct uref *flow_def_dup;
    if (unlikely((flow_def_dup = uref_dup(flow_def)) == NULL))
        return UBASE_ERR_ALLOC;
    upipe_input(upipe, flow_def_dup, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the swscale flags.
 *
 * @param upipe description structure of the pipe
 * @param flags swscale flags
 * @return an error code
 */
static int upipe_ffmt_set_sws_flags(struct upipe *upipe, int flags)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    upipe_ffmt->sws_flags = flags;
    if (upipe_ffmt->last_inner != NULL && flags)
        /* it may not be sws but it will just return an error */
        upipe_sws_set_flags(upipe_ffmt->last_inner, flags);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the avfilter hw config.
 *
 * @param upipe description structure of the pipe
 * @param hw_type hardware device type
 * @param hw_device hardware device (use NULL for default)
 * @return an error code
 */
static int upipe_ffmt_set_hw_config(struct upipe *upipe,
                                    const char *hw_type,
                                    const char *hw_device)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    if (hw_type == NULL)
        return UBASE_ERR_INVALID;

    char *hw_type_tmp = strdup(hw_type);
    if (hw_type_tmp == NULL)
        return UBASE_ERR_ALLOC;
    char *hw_device_tmp = NULL;
    if (hw_device != NULL) {
        hw_device_tmp = strdup(hw_device);
        if (hw_device_tmp == NULL) {
            free(hw_type_tmp);
            return UBASE_ERR_ALLOC;
        }
    }

    free(upipe_ffmt->hw_type);
    upipe_ffmt->hw_type = hw_type_tmp;
    free(upipe_ffmt->hw_device);
    upipe_ffmt->hw_device = hw_device_tmp;

    if (upipe_ffmt->avfilter != NULL)
        return upipe_avfilt_set_hw_config(upipe_ffmt->avfilter,
                                          hw_type, hw_device);

    return UBASE_ERR_NONE;
}

static int upipe_ffmt_alloc_output_proxy(struct upipe *upipe,
                                         struct urequest *urequest)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    struct urequest *proxy = urequest_alloc_proxy(urequest);
    UBASE_ALLOC_RETURN(proxy);

    if (urequest->type == UREQUEST_FLOW_FORMAT && urequest->uref) {
        /** It is legal to have just "sound." in flow_def_wanted to avoid
         * changing unnecessarily the sample format. */
        const char *def = NULL;
        uref_flow_get_def(urequest->uref, &def);

        char *old_def = NULL;
        if (!ubase_ncmp(def, UREF_SOUND_FLOW_DEF))
            old_def = strdup(def);
        uref_attr_import(proxy->uref, upipe_ffmt->flow_def_wanted);
        if (old_def != NULL &&
            (!ubase_check(uref_flow_get_def(proxy->uref, &def)) ||
             !strcmp(def, UREF_SOUND_FLOW_DEF)))
            uref_flow_set_def(proxy->uref, old_def);
        free(old_def);
    }
    return upipe_ffmt_register_bin_output_request(upipe, proxy);
}

static int upipe_ffmt_free_output_proxy(struct upipe *upipe,
                                        struct urequest *urequest)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);
    struct urequest *proxy =
        urequest_find_proxy(urequest, &upipe_ffmt->output_request_list);
    if (unlikely(!proxy))
        return UBASE_ERR_INVALID;

    upipe_ffmt_unregister_bin_output_request(upipe, proxy);
    urequest_free_proxy(proxy);
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a ffmt pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_ffmt_control(struct upipe *upipe, int command, va_list args)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    switch (command) {
        case UPIPE_REGISTER_REQUEST: {
            va_list args_copy;
            va_copy(args_copy, args);
            struct urequest *request = va_arg(args_copy, struct urequest *);
            va_end(args_copy);

            if (request->type == UREQUEST_FLOW_FORMAT) {
                if (upipe_ffmt->forward_flow_format)
                    return upipe_ffmt_alloc_output_proxy(upipe, request);
                else
                    return upipe_throw_provide_request(upipe, request);
            }

            if (request->type == UREQUEST_UBUF_MGR)
                return upipe_throw_provide_request(upipe, request);

            return upipe_ffmt_register_bin_output_request(upipe, request);
        }
        case UPIPE_UNREGISTER_REQUEST: {
            va_list args_copy;
            va_copy(args_copy, args);
            struct urequest *request = va_arg(args_copy, struct urequest *);
            va_end(args_copy);

            if (request->type == UREQUEST_FLOW_FORMAT) {
                if (upipe_ffmt->forward_flow_format)
                    return upipe_ffmt_free_output_proxy(upipe, request);
                else
                    return UBASE_ERR_NONE;
            }
            if (request->type == UREQUEST_UBUF_MGR)
                return UBASE_ERR_NONE;

            return upipe_ffmt_unregister_bin_output_request(upipe, request);
        }

        case UPIPE_SET_OPTION: {
            const char *option = va_arg(args, const char *);
            const char *value = va_arg(args, const char *);
            return upipe_ffmt_set_option(upipe, option, value);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_ffmt_set_flow_def(upipe, flow_def);
        }
        case UPIPE_SWS_SET_FLAGS: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_SWS_SIGNATURE)
            int flags = va_arg(args, int);
            return upipe_ffmt_set_sws_flags(upipe, flags);
        }
        case UPIPE_AVFILT_SET_HW_CONFIG: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            const char *hw_type = va_arg(args, const char *);
            const char *hw_device = va_arg(args, const char *);
            return upipe_ffmt_set_hw_config(upipe, hw_type, hw_device);
        }
    }

    int err = upipe_ffmt_control_bin_input(upipe, command, args);
    if (err == UBASE_ERR_UNHANDLED)
        return upipe_ffmt_control_bin_output(upipe, command, args);
    return err;
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ffmt_free(struct upipe *upipe)
{
    struct upipe_ffmt *upipe_ffmt = upipe_ffmt_from_upipe(upipe);

    upipe_throw_dead(upipe);
    free(upipe_ffmt->deinterlace_vaapi_mode);
    free(upipe_ffmt->scale_vaapi_mode);
    free(upipe_ffmt->vpp_qsv_deinterlace);
    free(upipe_ffmt->vpp_qsv_scale_mode);
    free(upipe_ffmt->ni_quadra_scale_filterblit);
    free(upipe_ffmt->zscale_filter);
    free(upipe_ffmt->tonemap_tonemap);
    free(upipe_ffmt->tonemap_param);
    free(upipe_ffmt->tonemap_desat);
    free(upipe_ffmt->sw_deinterlace);
    free(upipe_ffmt->sw_interlace);
    free(upipe_ffmt->sw_scale);
    free(upipe_ffmt->sw_format);
    free(upipe_ffmt->hw_type);
    free(upipe_ffmt->hw_device);
    upipe_ffmt_clean_input(upipe);
    upipe_ffmt_clean_flow_format(upipe);
    uref_free(upipe_ffmt->flow_def_requested);
    uref_free(upipe_ffmt->flow_def_provided);
    upipe_ffmt_clean_flow_def(upipe);
    upipe_ffmt_clean_proxy_probe(upipe);
    upipe_ffmt_clean_last_inner_probe(upipe);
    upipe_ffmt_clean_urefcount_real(upipe);
    upipe_ffmt_clean_urefcount(upipe);
    upipe_ffmt_free_flow(upipe);
}

/** @This is called when there is no external reference to the pipe anymore.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_ffmt_no_ref(struct upipe *upipe)
{
    upipe_ffmt_clean_avfilter(upipe);
    upipe_ffmt_clean_bin_input(upipe);
    upipe_ffmt_clean_bin_output(upipe);
    upipe_ffmt_release_urefcount_real(upipe);
}

/** @This frees a upipe manager.
 *
 * @param urefcount pointer to urefcount structure
 */
static void upipe_ffmt_mgr_free(struct urefcount *urefcount)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_urefcount(urefcount);
    upipe_mgr_release(ffmt_mgr->swr_mgr);
    upipe_mgr_release(ffmt_mgr->sws_mgr);
    upipe_mgr_release(ffmt_mgr->interlace_mgr);
    upipe_mgr_release(ffmt_mgr->deint_mgr);
    upipe_mgr_release(ffmt_mgr->avfilter_mgr);

    urefcount_clean(urefcount);
    free(ffmt_mgr);
}

/** @This processes control commands on a ffmt manager.
 *
 * @param mgr pointer to manager
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_ffmt_mgr_control(struct upipe_mgr *mgr,
                                  int command, va_list args)
{
    struct upipe_ffmt_mgr *ffmt_mgr = upipe_ffmt_mgr_from_upipe_mgr(mgr);

    switch (command) {
#define GET_SET_MGR(name, NAME)                                             \
        case UPIPE_FFMT_MGR_GET_##NAME##_MGR: {                             \
            UBASE_SIGNATURE_CHECK(args, UPIPE_FFMT_SIGNATURE)               \
            struct upipe_mgr **p = va_arg(args, struct upipe_mgr **);       \
            *p = ffmt_mgr->name##_mgr;                                      \
            return UBASE_ERR_NONE;                                          \
        }                                                                   \
        case UPIPE_FFMT_MGR_SET_##NAME##_MGR: {                             \
            UBASE_SIGNATURE_CHECK(args, UPIPE_FFMT_SIGNATURE)               \
            if (!urefcount_single(&ffmt_mgr->urefcount))                    \
                return UBASE_ERR_BUSY;                                      \
            struct upipe_mgr *m = va_arg(args, struct upipe_mgr *);         \
            upipe_mgr_release(ffmt_mgr->name##_mgr);                        \
            ffmt_mgr->name##_mgr = upipe_mgr_use(m);                        \
            return UBASE_ERR_NONE;                                          \
        }

        GET_SET_MGR(sws, SWS)
        GET_SET_MGR(swr, SWR)
        GET_SET_MGR(deint, DEINT)
        GET_SET_MGR(avfilter, AVFILTER)
#undef GET_SET_MGR

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @This returns the management structure for all ffmt pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_ffmt_mgr_alloc(void)
{
    struct upipe_ffmt_mgr *ffmt_mgr = malloc(sizeof(struct upipe_ffmt_mgr));
    if (unlikely(ffmt_mgr == NULL))
        return NULL;

    memset(ffmt_mgr, 0, sizeof(*ffmt_mgr));
    ffmt_mgr->sws_mgr = NULL;
    ffmt_mgr->swr_mgr = NULL;
    ffmt_mgr->deint_mgr = upipe_filter_blend_mgr_alloc();
    ffmt_mgr->interlace_mgr = upipe_interlace_mgr_alloc();
    ffmt_mgr->avfilter_mgr = NULL;

    urefcount_init(upipe_ffmt_mgr_to_urefcount(ffmt_mgr),
                   upipe_ffmt_mgr_free);
    ffmt_mgr->mgr.refcount = upipe_ffmt_mgr_to_urefcount(ffmt_mgr);
    ffmt_mgr->mgr.signature = UPIPE_FFMT_SIGNATURE;
    ffmt_mgr->mgr.upipe_alloc = upipe_ffmt_alloc;
    ffmt_mgr->mgr.upipe_input = upipe_ffmt_input;
    ffmt_mgr->mgr.upipe_control = upipe_ffmt_control;
    ffmt_mgr->mgr.upipe_mgr_control = upipe_ffmt_mgr_control;
    return upipe_ffmt_mgr_to_upipe_mgr(ffmt_mgr);
}
