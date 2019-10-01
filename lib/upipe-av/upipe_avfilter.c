/*
 * Copyright (C) 2018 OpenHeadend S.A.R.L.
 *
 * Authors: Clément Vasseur
 *          Arnaud de Turckheim
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
 * @short Upipe avfilter module
 */

#include <upipe/upipe.h>
#include <upipe/uclock.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_clock.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_input.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe-av/upipe_avfilter.h>
#include <upipe-av/upipe_av_pixfmt.h>
#include <upipe-av/uref_avfilter_flow.h>

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

/** @internal @This enumerates the avfilter sub pipe private events. */
enum uprobe_avfilt_sub_event  {
    /** sentinel */
    UPROBE_AVFILT_SUB_SENTINEL = UPROBE_LOCAL,
    /** the filter was updated (void) */
    UPROBE_AVFILT_SUB_UPDATE,
};

/** @internal @This throws an update event for avfilter sub pipe.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static inline int upipe_avfilt_sub_throw_update(struct upipe *upipe)
{
    return upipe_throw(upipe, UPROBE_AVFILT_SUB_UPDATE,
                       UPIPE_AVFILT_SUB_SIGNATURE);
}

/** @This is the sub pipe private structure of the avfilter pipe. */
struct upipe_avfilt_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** public upipe structure */
    struct upipe upipe;
    /** chain in the super pipe list */
    struct uchain uchain;
    /** allocation flow definition */
    struct uref *flow_def_alloc;
    /** output pipe */
    struct upipe *output;
    /** output flow def */
    struct uref *flow_def;
    /** output internal state */
    enum upipe_helper_output_state output_state;
    /** registered requests on output */
    struct uchain requests;
    /** ubuf manager flow format */
    struct uref *flow_format;
    /** ubuf manager request */
    struct urequest ubuf_mgr_request;
    /** buffer manager */
    struct ubuf_mgr *ubuf_mgr;
    /** uclock request */
    struct urequest uclock_request;
    /** uclock */
    struct uclock *uclock;
    /** sub pipe name */
    const char *name;
    /** sub pipe is an input pipe */
    bool input;
    /** input width */
    size_t width;
    /** input height */
    size_t height;
    /** chroma map */
    const char *chroma_map[UPIPE_AV_MAX_PLANES];
    /** pixel format */
    enum AVPixelFormat pix_fmt;
    /** avfilter buffer source */
    AVFilterContext *buffer_ctx;
};

/** @hidden */
static inline int upipe_avfilt_sub_check_ubuf_mgr(struct upipe *upipe,
                                                  struct uref *flow_def);

UPIPE_HELPER_UPIPE(upipe_avfilt_sub, upipe, UPIPE_AVFILT_SUB_SIGNATURE)
UPIPE_HELPER_FLOW(upipe_avfilt_sub, NULL)
UPIPE_HELPER_UREFCOUNT(upipe_avfilt_sub, urefcount, upipe_avfilt_sub_free)
UPIPE_HELPER_OUTPUT(upipe_avfilt_sub, output, flow_def, output_state, requests)
UPIPE_HELPER_UBUF_MGR(upipe_avfilt_sub, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_avfilt_sub_check_ubuf_mgr,
                      upipe_avfilt_sub_register_output_request,
                      upipe_avfilt_sub_unregister_output_request)
UPIPE_HELPER_UCLOCK(upipe_avfilt_sub, uclock, uclock_request,
                    NULL,
                    upipe_avfilt_sub_register_output_request,
                    upipe_avfilt_sub_unregister_output_request)

enum upipe_avfilt_state {
    UPIPE_AVFILT_STATE_NONE,
    UPIPE_AVFILT_STATE_CONFIGURING,
    UPIPE_AVFILT_STATE_CONFIGURED,
};

/** upipe_avfilt structure */
struct upipe_avfilt {
    /** refcount management structure */
    struct urefcount urefcount;

    /** sub pipe manager */
    struct upipe_mgr sub_mgr;
    /** sub pipe list */
    struct uchain subs;

    /** filter graph description */
    char *filters_desc;
    /** avfilter filter graph */
    AVFilterGraph *filter_graph;
    /** avfilter is configured? */
    enum upipe_avfilt_state state;;

    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static int upipe_avfilt_init_filters(struct upipe *upipe);

UPIPE_HELPER_UPIPE(upipe_avfilt, upipe, UPIPE_AVFILT_SIGNATURE)
UPIPE_HELPER_VOID(upipe_avfilt)
UPIPE_HELPER_UREFCOUNT(upipe_avfilt, urefcount, upipe_avfilt_free)
UPIPE_HELPER_SUBPIPE(upipe_avfilt, upipe_avfilt_sub, sub,
                     sub_mgr, subs, uchain);

/** @internal @This is the avbuffer free callback.
 *
 * @param opaque pointer to the uref
 * @param data avbuffer data
 */
static void buffer_free_cb(void *opaque, uint8_t *data)
{
    struct uref *uref = opaque;

    uint64_t buffers;
    if (unlikely(!ubase_check(uref_attr_get_priv(uref, &buffers))))
        return;
    if (--buffers) {
        uref_attr_set_priv(uref, buffers);
        return;
    }

    const char *chroma;
    uref_pic_foreach_plane(uref, chroma) {
        uref_pic_plane_unmap(uref, chroma, 0, 0, -1, -1);
    }

    uref_free(uref);
}

/** @internal @This makes an urational from an AVRational.
 *
 * @param v AVRational to convert
 * @return an urational
 */
static inline struct urational urational(AVRational v)
{
    return (struct urational){ .num = v.num, .den = v.den };
}

/** @internal @This builds the flow definition packet.
 *
 * @param upipe description structure of the pipe
 * @param flow_def_input description structure of the pipe
 * @return an error code
 */
static int upipe_avfilt_sub_build_flow_def(struct upipe *upipe,
                                           struct uref *flow_def)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    enum AVPixelFormat pix_fmt =
        av_buffersink_get_format(upipe_avfilt_sub->buffer_ctx);
    AVRational frame_rate =
        av_buffersink_get_frame_rate(upipe_avfilt_sub->buffer_ctx);
    upipe_avfilt_sub->width =
        av_buffersink_get_w(upipe_avfilt_sub->buffer_ctx);
    upipe_avfilt_sub->height =
        av_buffersink_get_h(upipe_avfilt_sub->buffer_ctx);
    AVRational sar =
        av_buffersink_get_sample_aspect_ratio(upipe_avfilt_sub->buffer_ctx);

    UBASE_RETURN(upipe_av_pixfmt_to_flow_def(pix_fmt, flow_def))
    UBASE_RETURN(uref_pic_flow_set_hsize(flow_def, upipe_avfilt_sub->width))
    UBASE_RETURN(uref_pic_flow_set_vsize(flow_def, upipe_avfilt_sub->height))
    UBASE_RETURN(uref_pic_flow_set_fps(flow_def, urational(frame_rate)))
    UBASE_RETURN(uref_pic_flow_set_sar(flow_def, urational(sar)))

    upipe_avfilt_sub->pix_fmt =
        upipe_av_pixfmt_from_flow_def(flow_def, NULL,
                                      upipe_avfilt_sub->chroma_map);

    return UBASE_ERR_NONE;
}

/** @internal @This outputs a frame.
 *
 * @param upipe description structure of the pipe
 * @param frame AVFrame to output
 */
static void upipe_avfilt_sub_output_frame(struct upipe *upipe, AVFrame *frame)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    if (unlikely(!upipe_avfilt_sub->flow_def)) {
        struct uref *flow_def_dup = uref_dup(upipe_avfilt_sub->flow_def_alloc);
        if (unlikely(!flow_def_dup)) {
            upipe_throw_error(upipe, UBASE_ERR_ALLOC);
            return;
        }

        int ret = upipe_avfilt_sub_build_flow_def(upipe, flow_def_dup);
        if (unlikely(!ubase_check(ret))) {
            uref_free(flow_def_dup);
            upipe_throw_error(upipe, ret);
            return;
        }

        upipe_avfilt_sub_require_ubuf_mgr(upipe, flow_def_dup);
    }

    if (unlikely(!upipe_avfilt_sub->ubuf_mgr)) {
        upipe_warn(upipe, "no ubuf manager for now");
        return;
    }

    struct uref *uref = uref_pic_alloc(upipe_avfilt_sub->flow_def_alloc->mgr,
                                       upipe_avfilt_sub->ubuf_mgr,
                                       frame->width,
                                       frame->height);
    if (unlikely(uref == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    for (int i = 0; i < UPIPE_AV_MAX_PLANES &&
         upipe_avfilt_sub->chroma_map[i] != NULL; i++) {
        uint8_t *data, hsub, vsub;
        size_t stride;
        if (unlikely(!ubase_check(uref_pic_plane_size(
                        uref, upipe_avfilt_sub->chroma_map[i],
                        &stride, &hsub, &vsub, NULL)) ||
                !ubase_check(uref_pic_plane_write(
                        uref, upipe_avfilt_sub->chroma_map[i],
                        0, 0, -1, -1, &data))))
            goto err;
        for (int j = 0; j < frame->height / vsub; j++)
            memcpy(data + j * stride,
                   frame->data[i] + j * frame->linesize[i],
                   stride < frame->linesize[i] ? stride : frame->linesize[i]);
        uref_pic_plane_unmap(uref, upipe_avfilt_sub->chroma_map[i],
                             0, 0, -1, -1);
    }

    uref_clock_set_pts_prog(uref, frame->pts);
    if (upipe_avfilt_sub->uclock)
        uref_clock_set_pts_sys(uref, uclock_now(upipe_avfilt_sub->uclock));
    //uref_clock_set_dts_prog(uref, frame->pkt_dts);
    UBASE_ERROR(upipe, uref_clock_set_duration(uref, frame->pkt_duration))
    UBASE_ERROR(upipe, uref_pic_set_number(uref, frame->coded_picture_number))

    if (!frame->interlaced_frame)
        UBASE_ERROR(upipe, uref_pic_set_progressive(uref))
    else if (frame->top_field_first)
        UBASE_ERROR(upipe, uref_pic_set_tff(uref))

    if (frame->key_frame)
        UBASE_ERROR(upipe, uref_pic_set_key(uref))

    upipe_notice_va(upipe, "output frame %d(%d) pts=%f dts=%f duration=%f",
                 frame->display_picture_number,
                 frame->coded_picture_number,
                 (double) frame->pts / UCLOCK_FREQ,
                 (double) frame->pkt_dts / UCLOCK_FREQ,
                 (double) frame->pkt_duration / UCLOCK_FREQ);

    //FIXME: need an upump here?
    upipe_avfilt_sub_output(upipe, uref, NULL);
    return;

err:
    upipe_throw_error(upipe, UBASE_ERR_INVALID);
    uref_free(uref);
}

/** @internal @This checks the ubuf manager.
 *
 * @param upipe description structure of the pipe
 * @param flow_def ubuf manager flow definition
 * @return an error code
 */
static int upipe_avfilt_sub_check_ubuf_mgr(struct upipe *upipe,
                                           struct uref *flow_def)
{
    if (flow_def)
        upipe_avfilt_sub_store_flow_def(upipe, flow_def);

    return UBASE_ERR_NONE;
}

/** @internal @This checks for frame to output.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_sub_check(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    if (upipe_avfilt_sub->input)
        return;

    AVFrame *filt_frame = av_frame_alloc();
    if (unlikely(!filt_frame)) {
        upipe_err_va(upipe, "cannot allocate av frame");
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        int err = av_buffersink_get_frame(upipe_avfilt_sub->buffer_ctx,
                                          filt_frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            break;
        if (err < 0) {
            upipe_err_va(upipe, "cannot get frame from filter graph: %s",
                         av_err2str(err));
            upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
            break;
        }
        upipe_avfilt_sub_output_frame(upipe, filt_frame);
        av_frame_unref(filt_frame);
    }

    av_frame_free(&filt_frame);
}

/** @internal @This catches the internal events of the avfilter sub pipes.
 *
 * @param upipe description structure of the sub pipe
 * @param uprobe structure used to raise events
 * @param event event thrown
 * @param args optional arguments of the event
 * @return an error code
 */
static int upipe_avfilt_sub_catch(struct uprobe *uprobe,
                                  struct upipe *upipe,
                                  int event,
                                  va_list args)
{
    if (event == UPROBE_AVFILT_SUB_UPDATE &&
        ubase_get_signature(args) == UPIPE_AVFILT_SUB_SIGNATURE) {

        struct upipe_avfilt *upipe_avfilt =
            upipe_avfilt_from_sub_mgr(upipe->mgr);
        struct uchain *uchain;
        ulist_foreach(&upipe_avfilt->subs, uchain) {
            struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
            upipe_avfilt_sub_check(upipe_avfilt_sub_to_upipe(sub));
        }
        return UBASE_ERR_NONE;
    }
    else
        return uprobe_throw_next(uprobe, upipe, event, args);
}

/** @internal @This allocates and initializes a avfilter sub pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated and initialized sub pipe or NULL
 */
static struct upipe *upipe_avfilt_sub_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature,
                                            va_list args)
{
    struct uref *flow_def = NULL;
    struct upipe *upipe =
        upipe_avfilt_sub_alloc_flow(
            mgr, uprobe_alloc(upipe_avfilt_sub_catch, uprobe),
            signature, args, &flow_def);
    if (unlikely(!upipe))
        return NULL;

    upipe_avfilt_sub_init_urefcount(upipe);
    upipe_avfilt_sub_init_sub(upipe);
    upipe_avfilt_sub_init_output(upipe);
    upipe_avfilt_sub_init_ubuf_mgr(upipe);
    upipe_avfilt_sub_init_uclock(upipe);

    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    upipe_avfilt_sub->flow_def_alloc = flow_def;
    upipe_avfilt_sub->name = NULL;
    upipe_avfilt_sub->input = false;

    upipe_throw_ready(upipe);

    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(mgr);
    int ret = uref_avfilt_flow_get_name(flow_def, &upipe_avfilt_sub->name);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "no avfilter name set");
        upipe_release(upipe);
        return NULL;
    }
    upipe_avfilt_sub->input = ubase_check(uref_avfilt_flow_get_input(flow_def));
    upipe_avfilt_sub->pix_fmt =
        upipe_av_pixfmt_from_flow_def(flow_def, NULL,
                                      upipe_avfilt_sub->chroma_map);

    int err;
    if (!upipe_avfilt_sub->input) {
        const AVFilter *buffersink = avfilter_get_by_name("buffersink");

        /* buffer video sink: to terminate the filter chain. */
        err = avfilter_graph_create_filter(&upipe_avfilt_sub->buffer_ctx,
                                           buffersink, upipe_avfilt_sub->name,
                                           NULL, NULL,
                                           upipe_avfilt->filter_graph);
        if (err < 0) {
            upipe_err_va(upipe, "cannot create buffer sink: %s",
                         av_err2str(err));
            upipe_release(upipe);
            return NULL;
        }
    }
    else {
        upipe_avfilt_sub->buffer_ctx = NULL;
    }

    return upipe;
}

/** @internal @This is called when there is no more reference on the sub pipe.
 *
 * @param upipe description structure of the sub pipe
 */
static void upipe_avfilt_sub_free(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(upipe_avfilt_sub->flow_def_alloc);
    upipe_avfilt_sub_clean_uclock(upipe);
    upipe_avfilt_sub_clean_ubuf_mgr(upipe);
    upipe_avfilt_sub_clean_output(upipe);
    upipe_avfilt_sub_clean_sub(upipe);
    upipe_avfilt_sub_clean_urefcount(upipe);
    upipe_avfilt_sub_free_flow(upipe);
}

/** @internal @This converts an uref to an avframe.
 *
 * @param upipe description structure of the pipe
 * @param uref input uref to convert
 * @param frame filled with the convertion
 * @return an error code
 */
static int upipe_avfilt_sub_avframe_from_uref(struct upipe *upipe,
                                             struct uref *uref,
                                             AVFrame *frame)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    size_t hsize, vsize;
    if (unlikely(!ubase_check(uref_pic_size(uref, &hsize, &vsize, NULL)) ||
                 hsize != upipe_avfilt_sub->width ||
                 vsize != upipe_avfilt_sub->height))
        goto inval;

    for (int i = 0; i < UPIPE_AV_MAX_PLANES &&
         upipe_avfilt_sub->chroma_map[i] != NULL; i++) {
        const uint8_t *data;
        size_t stride;
        uint8_t vsub;
        if (unlikely(
                !ubase_check(
                    uref_pic_plane_read(uref, upipe_avfilt_sub->chroma_map[i],
                                        0, 0, -1, -1, &data)) ||
                !ubase_check(
                    uref_pic_plane_size(uref, upipe_avfilt_sub->chroma_map[i],
                                        &stride, NULL, &vsub, NULL))))
            goto inval;
        frame->data[i] = (uint8_t *)data;
        frame->linesize[i] = stride;
        frame->buf[i] = av_buffer_create(frame->data[i],
                                         stride * vsize / vsub,
                                         buffer_free_cb, uref,
                                         AV_BUFFER_FLAG_READONLY);
        if (frame->buf[i] == NULL) {
            uref_pic_plane_unmap(uref, upipe_avfilt_sub->chroma_map[i],
                                 0, 0, -1, -1);
            goto inval;
        }

        /* use this as an avcodec refcount */
        uref_attr_set_priv(uref, i + 1);
    }

    frame->extended_data = frame->data;
    frame->width = hsize;
    frame->height = vsize;
    frame->key_frame = ubase_check(uref_pic_get_key(uref));
    frame->format = upipe_avfilt_sub->pix_fmt;
    frame->interlaced_frame = !ubase_check(uref_pic_get_progressive(uref));
    frame->top_field_first = ubase_check(uref_pic_get_tff(uref));

    uint64_t number;
    if (ubase_check(uref_pic_get_number(uref, &number)))
        frame->coded_picture_number = number;

    uint64_t pts = UINT64_MAX;
    if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
        frame->pts = pts;

    uint64_t dts = UINT64_MAX;
    // if (ubase_check(uref_clock_get_dts_prog(uref, &dts)))
    //     frame->pkt_dts = dts;

    uint64_t duration = UINT64_MAX;
    if (ubase_check(uref_clock_get_duration(uref, &duration)))
        frame->pkt_duration = duration;

    upipe_notice_va(upipe, " input frame %d(%d) pts=%f dts=%f duration=%f",
                    frame->display_picture_number,
                    frame->coded_picture_number,
                    (double) pts / UCLOCK_FREQ,
                    (double) dts / UCLOCK_FREQ,
                    (double) duration / UCLOCK_FREQ);

    return UBASE_ERR_NONE;

inval:
    upipe_warn(upipe, "invalid buffer received");
    uref_free(uref);
    return UBASE_ERR_INVALID;
}

static void upipe_avfilt_sub_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    if (unlikely(!upipe_avfilt_sub->input)) {
        upipe_err(upipe, "receive buffer in an output sub pipe");
        uref_free(uref);
        return;
    }

    AVFrame *frame = av_frame_alloc();
    if (unlikely(!frame)) {
        upipe_err_va(upipe, "cannot allocate av frame");
        uref_free(uref);
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    int ret = upipe_avfilt_sub_avframe_from_uref(upipe, uref, frame);
    if (unlikely(!ubase_check(ret))) {
        upipe_throw_error(upipe, ret);
        av_frame_free(&frame);
        return;
    }

    int err = av_buffersrc_write_frame(upipe_avfilt_sub->buffer_ctx, frame);
    av_frame_free(&frame);
    if (unlikely(err < 0)) {
        upipe_err_va(upipe, "cannot write frame to filter graph: %s",
                     av_err2str(err));
        upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
        return;
    }

    upipe_avfilt_sub_throw_update(upipe);
}

/** @internal @This sets the input sub pipe flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow defintion
 * @return an error code
 */
static int upipe_avfilt_sub_set_flow_def(struct upipe *upipe,
                                         struct uref *flow_def)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    upipe_notice(upipe, "set input flow def");

    const char *chroma_map[UPIPE_AV_MAX_PLANES];
    enum AVPixelFormat pix_fmt;
    size_t width;
    size_t height;
    struct urational sar = { 1, 1 };
    struct urational fps = { 1, 1 };
    int err;

    pix_fmt = upipe_av_pixfmt_from_flow_def(flow_def, NULL, chroma_map);
    UBASE_RETURN(uref_pic_flow_get_hsize(flow_def, &width));
    UBASE_RETURN(uref_pic_flow_get_vsize(flow_def, &height));
    uref_pic_flow_get_sar(flow_def, &sar);
    uref_pic_flow_get_fps(flow_def, &fps);

    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);

    memcpy(upipe_avfilt_sub->chroma_map, chroma_map, sizeof (chroma_map));
    upipe_avfilt_sub->pix_fmt = pix_fmt;
    upipe_avfilt_sub->width = width;
    upipe_avfilt_sub->height = height;

    if (upipe_avfilt_sub->flow_def) {
        AVFilterContext *ctx = avfilter_graph_get_filter(
            upipe_avfilt->filter_graph, upipe_avfilt_sub->name);
        if (ctx) {
            AVBufferSrcParameters params;
            memset(&params, 0, sizeof (params));
            params.format = pix_fmt;
            params.width = width;
            params.height = height;
            params.frame_rate.num = fps.num;
            params.frame_rate.den = fps.den;
            params.sample_aspect_ratio.num = sar.num;
            params.sample_aspect_ratio.den = sar.den;
            err = av_buffersrc_parameters_set(ctx, &params);
            if (err < 0) {
                upipe_err_va(upipe, "cannot reconfigure filter: %s",
                             av_err2str(err));
                uref_free(flow_def_dup);
                return UBASE_ERR_EXTERNAL;
            }
        }
        else {
            upipe_warn_va(upipe, "filter %s not found", upipe_avfilt_sub->name);
            uref_free(flow_def_dup);
            return UBASE_ERR_INVALID;
        }
    }
    else {
        const AVFilter *buffer = avfilter_get_by_name("buffer");
        /* buffer video source: the decoded frames from the decoder will be
         * inserted here. */
        int len = snprintf(NULL, 0,
                           "video_size=%zux%zu:"
                           "pix_fmt=%d:"
                           "time_base=1/%"PRIu64":"
                           "pixel_aspect=%"PRIu64"/%"PRIu64":"
                           "frame_rate=%"PRIu64"/%"PRIu64,
                           upipe_avfilt_sub->width,
                           upipe_avfilt_sub->height,
                           upipe_avfilt_sub->pix_fmt,
                           UCLOCK_FREQ,
                           sar.num, sar.den,
                           fps.num, fps.den);
        char filter_args[len + 1];
        snprintf(filter_args, sizeof (filter_args),
                 "video_size=%zux%zu:"
                 "pix_fmt=%d:"
                 "time_base=1/%"PRIu64":"
                 "pixel_aspect=%"PRIu64"/%"PRIu64":"
                 "frame_rate=%"PRIu64"/%"PRIu64,
                 upipe_avfilt_sub->width,
                 upipe_avfilt_sub->height,
                 upipe_avfilt_sub->pix_fmt,
                 UCLOCK_FREQ,
                 sar.num, sar.den,
                 fps.num, fps.den);

        err = avfilter_graph_create_filter(&upipe_avfilt_sub->buffer_ctx,
                                           buffer, upipe_avfilt_sub->name,
                                           filter_args, NULL,
                                           upipe_avfilt->filter_graph);
        if (err < 0) {
            upipe_err_va(upipe, "cannot create filter: %s", av_err2str(err));
            uref_free(flow_def_dup);
            return UBASE_ERR_EXTERNAL;
        }
        upipe_avfilt_init_filters(upipe_avfilt_to_upipe(upipe_avfilt));
    }
    upipe_avfilt_sub_store_flow_def(upipe, flow_def_dup);
    return UBASE_ERR_NONE;
}

/** @internal @This handles the avfilter sub pipe control commands.
 *
 * @param upipe description structure of the sub pipe
 * @param command control command to handle
 * @param args optional arguments of the control command
 * @return an error code
 */
static int upipe_avfilt_sub_control(struct upipe *upipe, int cmd, va_list args)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    UBASE_HANDLED_RETURN(upipe_avfilt_sub_control_super(upipe, cmd, args));
    if (!upipe_avfilt_sub->input) {
        UBASE_HANDLED_RETURN(upipe_avfilt_sub_control_output(upipe, cmd, args));
        switch (cmd) {
            case UPIPE_ATTACH_UCLOCK:
                upipe_avfilt_sub_require_uclock(upipe);
                return UBASE_ERR_NONE;
        }
    }
    else {
        switch (cmd) {
            case UPIPE_ATTACH_UCLOCK:
                upipe_avfilt_sub_require_uclock(upipe);
                return UBASE_ERR_NONE;

            case UPIPE_SET_FLOW_DEF: {
                struct uref *flow_def = va_arg(args, struct uref *);
                return upipe_avfilt_sub_set_flow_def(upipe, flow_def);
            }
        }
    }

    return UBASE_ERR_UNHANDLED;
}

/** @internal @This initializes the avfilter graph.
 * This must be called when all the sub pipes have been created.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_avfilt_init_filters(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    struct uchain *uchain;
    int ret = UBASE_ERR_EXTERNAL, err;

    if (upipe_avfilt->state == UPIPE_AVFILT_STATE_CONFIGURED)
        return UBASE_ERR_NONE;

    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (!sub->buffer_ctx) {
            upipe_warn(upipe, "input is not ready");
            return UBASE_ERR_NONE;
        }
    }

    AVFilterInOut *outputs = NULL;
    AVFilterInOut *prev_output = NULL;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (!sub->input)
            continue;

        AVFilterInOut *inout = avfilter_inout_alloc();
        if (unlikely(!inout)) {
            upipe_err(upipe, "cannot allocate inout");
            goto end;
        }
        inout->name = av_strdup(sub->name);
        inout->filter_ctx = sub->buffer_ctx;
        inout->pad_idx = 0;
        inout->next = NULL;
        if (prev_output)
            prev_output->next = inout;
        else
            outputs = inout;
        prev_output = inout;
    }

    AVFilterInOut *inputs = NULL;
    AVFilterInOut *prev_input = NULL;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (sub->input)
            continue;

        AVFilterInOut *inout = avfilter_inout_alloc();
        if (unlikely(!inout)) {
            upipe_err(upipe, "cannot allocate inout");
            goto end;
        }
        inout->name = av_strdup(sub->name);
        inout->filter_ctx = sub->buffer_ctx;
        inout->pad_idx = 0;
        inout->next = NULL;
        if (prev_input)
            prev_input->next = inout;
        else
            inputs = inout;
        prev_input = inout;
    }

    if (outputs == NULL || inputs == NULL) {
        upipe_err(upipe, "cannot allocate filter inputs/outputs");
        goto end;
    }

    upipe_notice_va(upipe, "configuring filter %s", upipe_avfilt->filters_desc);
    if ((err = avfilter_graph_parse_ptr(upipe_avfilt->filter_graph,
                                        upipe_avfilt->filters_desc,
                                        &inputs, &outputs,
                                        NULL)) < 0) {
        upipe_err_va(upipe, "cannot parse filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    if ((err = avfilter_graph_config(upipe_avfilt->filter_graph, NULL)) < 0) {
        upipe_err_va(upipe, "cannot configure filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    ret = UBASE_ERR_NONE;
    upipe_avfilt->state = UPIPE_AVFILT_STATE_CONFIGURED;
    upipe_notice(upipe, "filter is now configured");

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (!ubase_check(ret))
        abort();
    return ret;
}

/** @internal @This sets the filter graph description.
 *
 * @param upipe description structure of the pipe
 * @param filters_desc filter graph description
 * @return an error code
 */
static int _upipe_avfilt_set_filters_desc(struct upipe *upipe,
                                          const char *filters_desc)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    char *filters_desc_dup = strdup(filters_desc);
    UBASE_ALLOC_RETURN(filters_desc_dup);
    free(upipe_avfilt->filters_desc);
    upipe_avfilt->filters_desc = filters_desc_dup;
    upipe_avfilt->state = UPIPE_AVFILT_STATE_CONFIGURING;
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on an avfilter pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_avfilt_control(struct upipe *upipe, int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_avfilt_control_subs(upipe, command, args))

    switch (command) {
        case UPIPE_AVFILT_SET_FILTERS_DESC: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            const char *filters_desc = va_arg(args, const char *);
            return _upipe_avfilt_set_filters_desc(upipe, filters_desc);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This initializes the sub pipes manager.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    memset(&upipe_avfilt->sub_mgr, 0, sizeof (upipe_avfilt->sub_mgr));
    upipe_avfilt->sub_mgr.signature = UPIPE_AVFILT_SUB_SIGNATURE;
    upipe_avfilt->sub_mgr.refcount = upipe_avfilt_to_urefcount(upipe_avfilt);
    upipe_avfilt->sub_mgr.upipe_alloc = upipe_avfilt_sub_alloc;
    upipe_avfilt->sub_mgr.upipe_input = upipe_avfilt_sub_input;
    upipe_avfilt->sub_mgr.upipe_control = upipe_avfilt_sub_control;
}

/** @internal @This allocates an avfilter pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_avfilt_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_avfilt_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_avfilt_init_urefcount(upipe);
    upipe_avfilt_init_sub_mgr(upipe);
    upipe_avfilt_init_sub_subs(upipe);

    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    upipe_avfilt->filters_desc = NULL;
    upipe_avfilt->filter_graph = NULL;
    upipe_avfilt->state = UPIPE_AVFILT_STATE_NONE;

    upipe_throw_ready(upipe);

    upipe_avfilt->filter_graph = avfilter_graph_alloc();
    if (upipe_avfilt->filter_graph == NULL) {
        upipe_err(upipe, "cannot allocate filter graph");
        upipe_release(upipe);
        return NULL;
    }

    return upipe;
}

/** @internal @This frees all resources allocated.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_free(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    upipe_throw_dead(upipe);

    free(upipe_avfilt->filters_desc);
    avfilter_graph_free(&upipe_avfilt->filter_graph);

    upipe_avfilt_clean_sub_subs(upipe);
    upipe_avfilt_clean_urefcount(upipe);
    upipe_avfilt_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_avfilt_mgr = {
    .refcount = NULL,
    .signature = UPIPE_AVFILT_SIGNATURE,

    .upipe_alloc = upipe_avfilt_alloc,
    .upipe_input = NULL,
    .upipe_control = upipe_avfilt_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for avfilter pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_avfilt_mgr_alloc(void)
{
    return &upipe_avfilt_mgr;
}
