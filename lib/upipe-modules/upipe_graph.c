/*
 * Copyright (C) 2019 OpenHeadend S.A.R.L.
 *
 * Authors: Arnaud de Turckheim
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation https (the
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
 * @short Upipe module to graph
 */

#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_urefcount_real.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_uref_mgr.h>
#include <upipe/upipe_helper_flow_format.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_input.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/upipe.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic_flow_formats.h>
#include <upipe/uref_pic.h>
#include <upipe/ubuf_pic.h>
#include <upipe/uclock.h>
#include <upipe/upump.h>

#include <upipe-modules/uref_graph_flow.h>
#include <upipe-modules/uref_graph.h>
#include <upipe-modules/upipe_graph.h>

#include <math.h>

#define DEFAULT_HISTORY_SIZE        (60 * 2)
#define DEFAULT_REFRESH_TIMEOUT     UCLOCK_FREQ / 2 /* 1s */
#define DEFAULT_MINIMUM             -2
#define DEFAULT_MAXIMUM             2

/** @internal @This describes a planes for writing. */
struct plane {
    /** chroma name */
    const char *chroma;
    /** horizontal sub sampling */
    uint8_t hsub;
    /** vertical sub sampling */
    uint8_t vsub;
    /** stride */
    size_t stride;
    /** mapped data */
    uint8_t *data;
    /** color bytes */
    const uint8_t *color;
    /** color size in bytes */
    size_t color_size;
};

/** @internal @This describes coordinate in a picture buffer. */
struct rect_coord {
    /** offset from the left */
    uint64_t off;
    /** size from the left offset */
    uint64_t size;
};

/** @internal @This describes the part of a picture buffer. */
struct rect {
    /** horizontal coordinates */
    struct rect_coord h;
    /** vertical coordinates */
    struct rect_coord v;
};

/** @internal @This is the private structure of a graph pipe. */
struct upipe_graph {
    /** public pipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** internal refcount structure */
    struct urefcount urefcount_real;
    /** subpipe manager */
    struct upipe_mgr mgr;
    /** list of sub input pipes */
    struct uchain inputs;
    /** output pipe */
    struct upipe *output;
    /** output flow def */
    struct uref *flow_def;
    /** output internal helper state */
    enum upipe_helper_output_state output_state;
    /** registered output requests */
    struct uchain requests;
    /** uref manager */
    struct uref_mgr *uref_mgr;
    /** uref manager request */
    struct urequest uref_mgr_request;
    /** flow format request */
    struct urequest flow_format_request;
    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** ubuf manager flow format */
    struct uref *flow_format;
    /** ubuf manager request */
    struct urequest ubuf_mgr_request;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** timer */
    struct upump *timer;
    /** uclock */
    struct uclock *uclock;
    /** uclock request */
    struct urequest uclock_request;
    /** output horizontal offset */
    uint64_t hoffset;
    /** output vertical offset */
    uint64_t voffset;
    /** output horizontal size */
    uint64_t hsize;
    /** output vertical size */
    uint64_t vsize;
    /** fullrange? */
    bool fullrange;
    /** background color */
    uint8_t color[3];
    /** history size */
    uint64_t history_size;
    /** refresh timeout */
    uint64_t refresh_timeout;
    /** maximum of the graph */
    int64_t max;
    /** minimum of the graph */
    int64_t min;
    /** index in the history ring buffer */
    size_t index;
};

/** @hidden */
static int upipe_graph_check_flow_format(struct upipe *upipe,
                                         struct uref *flow_format);

/** @hidden */
static int upipe_graph_check_ubuf_mgr(struct upipe *upipe,
                                      struct uref *flow_format);

/** @hidden */
static int upipe_graph_check(struct upipe *upipe, struct uref *flow_format);

UPIPE_HELPER_UPIPE(upipe_graph, upipe, UPIPE_GRAPH_SIGNATURE);
UPIPE_HELPER_VOID(upipe_graph);
UPIPE_HELPER_UREFCOUNT(upipe_graph, urefcount, upipe_graph_no_ref);
UPIPE_HELPER_UREFCOUNT_REAL(upipe_graph, urefcount_real, upipe_graph_free);
UPIPE_HELPER_OUTPUT(upipe_graph, output, flow_def, output_state, requests);
UPIPE_HELPER_UREF_MGR(upipe_graph, uref_mgr, uref_mgr_request,
                      upipe_graph_check,
                      upipe_graph_register_output_request,
                      upipe_graph_unregister_output_request);
UPIPE_HELPER_FLOW_FORMAT(upipe_graph, flow_format_request,
                         upipe_graph_check_flow_format,
                         upipe_graph_register_output_request,
                         upipe_graph_unregister_output_request);
UPIPE_HELPER_UBUF_MGR(upipe_graph, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_graph_check_ubuf_mgr,
                      upipe_graph_register_output_request,
                      upipe_graph_unregister_output_request);
UPIPE_HELPER_UPUMP_MGR(upipe_graph, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_graph, timer, upump_mgr);
UPIPE_HELPER_UCLOCK(upipe_graph, uclock, uclock_request,
                    upipe_graph_check,
                    upipe_graph_register_output_request,
                    upipe_graph_unregister_output_request);

/** @internal @This is the private structure of a graph input subpipe. */
struct upipe_graph_input {
    /** public pipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** link for the super pipe list */
    struct uchain uchain;
    /** allocation flow definition */
    struct uref *flow_def;
    /** filled between value and zero */
    bool filled;
    /** stacked */
    bool stacked;
    /** graph name */
    const char *name;
    /** graph color */
    uint8_t yuva[4];
    /** values */
    int64_t *values;
};

UPIPE_HELPER_UPIPE(upipe_graph_input, upipe, UPIPE_GRAPH_SUB_SIGNATURE);
UPIPE_HELPER_FLOW(upipe_graph_input, UREF_GRAPH_FLOW_DEF);
UPIPE_HELPER_UREFCOUNT(upipe_graph_input, urefcount, upipe_graph_input_free);

UPIPE_HELPER_SUBPIPE(upipe_graph, upipe_graph_input, input, mgr, inputs, uchain);

/** @internal @This allocates a sub graph input pipe.
 *
 * @param mgr management structure for this pipe
 * @param uprobe structure used to raise events
 * @param sig signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe of NULL in case of error
 */
static struct upipe *upipe_graph_input_alloc(struct upipe_mgr *mgr,
                                             struct uprobe *uprobe,
                                             uint32_t sig,
                                             va_list args)
{
    struct uref *flow_def = NULL;
    struct upipe *upipe =
        upipe_graph_input_alloc_flow(mgr, uprobe, sig, args, &flow_def);
    if (unlikely(!upipe))
        return NULL;

    upipe_graph_input_init_urefcount(upipe);
    upipe_graph_input_init_sub(upipe);

    struct upipe_graph *graph = upipe_graph_from_mgr(mgr);
    struct upipe_graph_input *input = upipe_graph_input_from_upipe(upipe);
    input->values = calloc(graph->history_size, sizeof (int64_t));
    input->flow_def = flow_def;
    input->name = NULL;
    input->filled = false;
    uint8_t rgba[4] = { 0xff, 0xff, 0xff, 0xff };
    ubuf_pic_rgba_to_yuva(rgba, false, input->yuva);

    upipe_throw_ready(upipe);

    if (unlikely(!input->values)) {
        upipe_err(upipe, "fail to allocate array value");
        upipe_release(upipe);
        return NULL;
    }

    uref_graph_flow_get_name(flow_def, &input->name);
    const char *color = NULL;
    uref_graph_flow_get_color(flow_def, &color);
    if (color) {
        if (unlikely(!ubase_check(ubuf_pic_parse_rgb(color, rgba)) &&
                     !ubase_check(ubuf_pic_parse_rgba(color, rgba))))
            upipe_warn(upipe, "invalid color");
        else
            ubuf_pic_rgba_to_yuva(rgba, false, input->yuva);
    }
    input->filled = ubase_check(uref_graph_flow_get_filled(flow_def));
    input->stacked = ubase_check(uref_graph_flow_get_stacked(flow_def));

    return upipe;
}

/** @internal @This frees a sub graph input pipe.
 *
 * @param upipe description structure of the subpipe
 */
static void upipe_graph_input_free(struct upipe *upipe)
{
    struct upipe_graph_input *input = upipe_graph_input_from_upipe(upipe);

    upipe_throw_dead(upipe);

    free(input->values);
    uref_free(input->flow_def);
    upipe_graph_input_clean_sub(upipe);
    upipe_graph_input_clean_urefcount(upipe);

    upipe_graph_input_free_flow(upipe);
}

/** @internal @This sets the color of the input graph.
 *
 * @param upipe description structure of the subpipe
 * @param color RGB color to set
 * @return an error code
 */
static int upipe_graph_input_set_color(struct upipe *upipe, const char *color)
{
    struct upipe_graph_input *input = upipe_graph_input_from_upipe(upipe);
    uint8_t rgba[4] = { 255, 255, 255, 255 };
    if (unlikely(!ubase_check(ubuf_pic_parse_rgb(color, rgba)) &&
                 !ubase_check(ubuf_pic_parse_rgba(color, rgba))))
        return UBASE_ERR_INVALID;
    ubuf_pic_rgba_to_yuva(rgba, false, input->yuva);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the new value for the input graph.
 *
 * @param upipe description structure of the pipe
 * @param value new value to set
 * @return an error code
 */
static int upipe_graph_input_set_value(struct upipe *upipe, int64_t value)
{
    struct upipe_graph_input *input = upipe_graph_input_from_upipe(upipe);
    struct upipe_graph *graph = upipe_graph_from_mgr(upipe->mgr);
    if (!input->values)
        return UBASE_ERR_ALLOC;

    input->values[graph->index] = value;
    return UBASE_ERR_NONE;
}

/** @internal @This handles the input values.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing the value
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_graph_input_input(struct upipe *upipe, struct uref *uref,
                                    struct upump **upump_p)
{
    int64_t value;
    if (unlikely(!ubase_check(uref_graph_get_value(uref, &value))))
        upipe_warn(upipe, "uref with no graph value");
    else
        upipe_graph_input_set_value(upipe, value);
    uref_free(uref);
}

/** @internal @This sets the input flow def.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow def
 * @return an error code
 */
static int upipe_graph_input_set_flow_def(struct upipe *upipe,
                                          struct uref *flow_def)
{
    return uref_flow_match_def(flow_def, UREF_GRAPH_FLOW_DEF);
}

/** @internal @This handles the sub pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param cmd control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_graph_input_control(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_graph_input_control_super(upipe, cmd, args));
    switch (cmd) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_graph_input_set_flow_def(upipe, flow_def);
        }
    }

    if (cmd > UPIPE_CONTROL_LOCAL &&
        ubase_get_signature(args) == UPIPE_GRAPH_SUB_SIGNATURE) {
        switch (cmd) {
            case UPIPE_GRAPH_SUB_SET_COLOR:
                UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SUB_SIGNATURE);
                const char *color = va_arg(args, const char *);
                return upipe_graph_input_set_color(upipe, color);
            case UPIPE_GRAPH_SUB_SET_VALUE:
                UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SUB_SIGNATURE);
                int64_t value = va_arg(args, int64_t);
                return upipe_graph_input_set_value(upipe, value);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This initializes the sub pipe manager.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_graph_init_mgr(struct upipe *upipe)
{
    struct upipe_graph *upipe_graph = upipe_graph_from_upipe(upipe);
    struct upipe_mgr *mgr = upipe_graph_to_mgr(upipe_graph);
    mgr->refcount = upipe_graph_to_urefcount_real(upipe_graph);
    mgr->signature = UPIPE_GRAPH_SUB_SIGNATURE;
    mgr->upipe_err_str = NULL;
    mgr->upipe_command_str = NULL;
    mgr->upipe_alloc = upipe_graph_input_alloc;
    mgr->upipe_input = upipe_graph_input_input;
    mgr->upipe_control = upipe_graph_input_control;
}

/** @internal @This cleans the sub pipe manager.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_graph_clean_mgr(struct upipe *upipe)
{
}

/** @internal @This allocates a graph pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param sig signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe of NULL in case of error
 */
static struct upipe *upipe_graph_alloc(struct upipe_mgr *mgr,
                                         struct uprobe *uprobe,
                                         uint32_t sig,
                                         va_list args)
{
    struct upipe *upipe = upipe_graph_alloc_void(mgr, uprobe, sig, args);
    if (unlikely(!upipe))
        return NULL;

    upipe_graph_init_urefcount(upipe);
    upipe_graph_init_urefcount_real(upipe);
    upipe_graph_init_sub_inputs(upipe);
    upipe_graph_init_mgr(upipe);
    upipe_graph_init_output(upipe);
    upipe_graph_init_uref_mgr(upipe);
    upipe_graph_init_flow_format(upipe);
    upipe_graph_init_ubuf_mgr(upipe);
    upipe_graph_init_upump_mgr(upipe);
    upipe_graph_init_timer(upipe);
    upipe_graph_init_uclock(upipe);
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    graph->history_size = DEFAULT_HISTORY_SIZE;
    graph->refresh_timeout = DEFAULT_REFRESH_TIMEOUT;
    graph->index = 0;
    graph->hsize = 0;
    graph->vsize = 0;
    graph->hoffset = 0;
    graph->voffset = 0;
    graph->fullrange = false;
    graph->min = DEFAULT_MINIMUM;
    graph->max = DEFAULT_MAXIMUM;
    /* black background */
    uint8_t rgb[3] = { 0, 0, 0 };
    ubuf_pic_rgb_to_yuv(rgb, graph->fullrange, graph->color);

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This frees a graph pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_graph_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_graph_clean_uclock(upipe);
    upipe_graph_clean_timer(upipe);
    upipe_graph_clean_upump_mgr(upipe);
    upipe_graph_clean_ubuf_mgr(upipe);
    upipe_graph_clean_flow_format(upipe);
    upipe_graph_clean_uref_mgr(upipe);
    upipe_graph_clean_output(upipe);
    upipe_graph_clean_mgr(upipe);
    upipe_graph_clean_sub_inputs(upipe);
    upipe_graph_clean_urefcount_real(upipe);
    upipe_graph_clean_urefcount(upipe);

    upipe_graph_free_void(upipe);
}

/** @internal @This is called when there is no external reference to the pipe
 *
 * @param upipe description structure of the pipe
 */
static void upipe_graph_no_ref(struct upipe *upipe)
{
    upipe_graph_release_urefcount_real(upipe);
}

/** @internal @This tries to output a graph.
 *
 * @param upipe description structure of the pipe
 * @param uref uref control packet
 * @param upump_p reference to the pump that generated the buffer
 * @return true if the packet was handled
 */
static void upipe_graph_handle(struct upipe *upipe)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);

    if (!graph->ubuf_mgr || !graph->uref_mgr)
        return;

    struct uref *uref =
        uref_pic_alloc(graph->uref_mgr, graph->ubuf_mgr,
                       graph->hsize, graph->vsize);
    if (unlikely(!uref)) {
        upipe_warn_va(upipe, "fail to allocate %"PRIu64"x%"PRIu64" picture",
                      graph->hsize, graph->vsize);
        uref_free(uref);
        return;
    }

    uref_pic_clear(uref, 0, 0, -1, -1, true);

    struct {
        const char *name;
        uint8_t hsub;
        uint8_t vsub;
        size_t stride;
        uint8_t *data;
    } planes[] = {
        { "y8", 0, 0, 0, NULL },
        { "u8", 0, 0, 0, NULL },
        { "v8", 0, 0, 0, NULL },
    };
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(planes); i++) {
        int err = uref_pic_plane_write(uref, planes[i].name, 0, 0, -1, -1,
                                       &planes[i].data);
        if (unlikely(!ubase_check(err))) {
            for (; i; i--)
                uref_pic_plane_unmap(uref, planes[i - 1].name, 0, 0, -1, -1);
            upipe_err(upipe, "fail to write on picture");
            return;
        }
        uref_pic_plane_size(uref, planes[i].name, &planes[i].stride,
                            &planes[i].hsub, &planes[i].vsub, NULL);
    }

    size_t hsize = 0, vsize = 0;
    uint8_t macropixel;
    uref_pic_size(uref, &hsize, &vsize, &macropixel);
    uint64_t history_size = graph->history_size;
    int64_t min = graph->min;
    int64_t max = graph->max;
    if (min >= max)
        return upipe_graph_output(upipe, uref, &graph->timer);

    int64_t vmin[hsize];
    int64_t vmax[hsize];
    memset(vmin, 0, hsize * sizeof (int64_t));
    memset(vmax, 0, hsize * sizeof (int64_t));

    upipe_foreach_sub(upipe, sub) {
        struct upipe_graph_input *input = upipe_graph_input_from_upipe(sub);

        if (!input->values)
            continue;

        for (uint64_t h = 0; h < hsize; h++) {
            uint64_t index = h * history_size / hsize;
            int64_t value =
                input->values[(graph->index + (index + 1)) % history_size];

            int64_t tmp = value;
            if (input->stacked) {
                if (value >= 0) {
                    value += vmax[h];
                    vmax[h] += tmp;
                } else {
                    value += vmin[h];
                    vmin[h] += tmp;
                }
            }

            for (uint64_t v = 0; v < vsize; v++) {
                float current = ((float)(vsize - v) * (max - min) / vsize) + min;

                if ((value >= current && value >= 0 && current >= 0 &&
                     (!input->stacked || current > (vmax[h] - tmp))) ||
                    (value <= current && value <= 0 && current <= 0 &&
                     (!input->stacked || current < (vmin[h] - tmp)))) {
                    for (unsigned p = 0; p < 3; p++) {
                        planes[p].data[(h / planes[p].hsub
                                        + (v / planes[p].vsub)
                                        * planes[p].stride)] = input->yuva[p];
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(planes); i++)
        uref_pic_plane_unmap(uref, planes[i].name, 0, 0, -1, -1);

    upipe_graph_output(upipe, uref, &graph->timer);
}

/** @internal @This is called by a timer to output a new updated graph picture.
 *
 * @param upump description structure of the timer
 */
static void upipe_graph_refresh(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    upipe_graph_handle(upipe);
    graph->index = (graph->index + 1) % graph->history_size;
}

/** @internal @This checks the requested flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_format amended flow format
 * @return an error code
 */
static int upipe_graph_check_flow_format(struct upipe *upipe,
                                         struct uref *flow_format)
{
    if (flow_format)
        upipe_graph_require_ubuf_mgr(upipe, flow_format);
    return UBASE_ERR_NONE;
}

/** @internal @This checks the ubuf flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_format amended flow format
 * @return an error code
 */
static int upipe_graph_check_ubuf_mgr(struct upipe *upipe,
                                      struct uref *flow_format)
{
    if (!flow_format)
        return UBASE_ERR_NONE;

    struct upipe_graph *upipe_graph = upipe_graph_from_upipe(upipe);
    upipe_graph_store_flow_def(upipe, flow_format);
    UBASE_RETURN(uref_pic_flow_get_hsize(flow_format, &upipe_graph->hsize));
    UBASE_RETURN(uref_pic_flow_get_vsize(flow_format, &upipe_graph->vsize));
    return UBASE_ERR_NONE;
}

/** @internal @This checks the internal state of the pipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_format is ignored
 * @return an error code
 */
static int upipe_graph_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_graph *upipe_graph = upipe_graph_from_upipe(upipe);

    if (unlikely(flow_format))
        uref_free(flow_format);

    if (unlikely(!upipe_graph->uref_mgr)) {
        upipe_graph_require_uref_mgr(upipe);
        return UBASE_ERR_NONE;
    }

    if (!upipe_graph->ubuf_mgr &&
        urequest_get_opaque(&upipe_graph->flow_format_request,
                            struct upipe *) != upipe) {
        struct uref *flow_format =
            uref_pic_flow_alloc_yuv420p(upipe_graph->uref_mgr);
        UBASE_ALLOC_RETURN(flow_format);
        int err = uref_pic_set_progressive(flow_format);
        if (unlikely(!ubase_check(err))) {
            uref_free(flow_format);
            return err;
        }
        upipe_graph_require_flow_format(upipe, flow_format);
    }

    if (!upipe_graph->uclock) {
        upipe_graph_require_uclock(upipe);
        return UBASE_ERR_NONE;
    }

    UBASE_RETURN(upipe_graph_check_upump_mgr(upipe));
    if (!upipe_graph->upump_mgr)
        return UBASE_ERR_NONE;

    if (unlikely(!upipe_graph->timer)) {
        struct upump *timer = upump_alloc_timer(upipe_graph->upump_mgr,
                                                upipe_graph_refresh, upipe,
                                                upipe->refcount, 0,
                                                upipe_graph->refresh_timeout);
        if (unlikely(!timer))
            return UBASE_ERR_UPUMP;

        upipe_graph_set_timer(upipe, timer);
        //upump_start(timer);
    }

    return UBASE_ERR_NONE;
}

/** @internal @This maps the planes for writing.
 *
 * @param upipe description structure of the pipe
 * @param uref owner of the ubuf plane
 * @param r part of the picture to map for writing
 * @param planes planes description filled with the planes parameters
 * @return an error code
 */
static int upipe_graph_map_planes(struct upipe *upipe,
                                  struct uref *uref,
                                  const struct rect *r,
                                  struct plane *planes)
{
    int err;

    if (!planes || !planes->chroma)
        /* no more plane to map */
        return UBASE_ERR_NONE;

    /* get plane size */
    err = uref_pic_plane_size(uref, planes->chroma, &planes->stride,
                              &planes->hsub, &planes->vsub, NULL);
    if (unlikely(!ubase_check(err)))
        return err;

    err = ubuf_pic_plane_set_color(uref->ubuf, planes->chroma,
                                   r->h.off, r->v.off, r->h.size, r->v.size,
                                   planes->color, planes->color_size);
    if (unlikely(!ubase_check(err)))
        return err;

    /* try to map the plane */
    err = uref_pic_plane_write(uref, planes->chroma, r->h.off, r->v.off,
                               r->h.size, r->v.size, &planes->data);
    if (unlikely(!ubase_check(err)))
        return err;

    /* try to map the next plane */
    err = upipe_graph_map_planes(upipe, uref, r, planes + 1);
    if (unlikely(!ubase_check(err))) {
        uref_pic_plane_unmap(uref, planes->chroma, r->h.off, r->v.off,
                             r->h.size, r->v.size);
        return err;
    }
    return UBASE_ERR_NONE;
}

/** @internal @This maps the buffer for writing and copies it if it's not
 * writable.
 *
 * @param upipe description structure of the pipe
 * @param uref owner of the ubuf
 * @param r part of the picture to map for writing
 * @param planes planes description filled with the planes parameters
 * @return an error code
 */
static int upipe_graph_map(struct upipe *upipe, struct uref *uref,
                           const struct rect *r, struct plane *planes)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    int err;

    err = uref_pic_clear(uref, r->h.off, r->v.off, r->h.size, r->v.size,
                         graph->fullrange);
    if (!ubase_check(err)) {
        struct ubuf *ubuf = ubuf_pic_copy(
            uref->ubuf->mgr, uref->ubuf, 0, 0, -1, -1);
        uref_attach_ubuf(uref, ubuf);
        err = uref_pic_clear(uref, r->h.off, r->v.off, r->h.size, r->v.size,
                             graph->fullrange);
        if (unlikely(!ubase_check(err)))
            return err;
    }

    return upipe_graph_map_planes(upipe, uref, r, planes);
}

/** @internal @This unmaps the buffer.
 *
 * @param upipe description structure of the pipe
 * @param uref owner of the ubuf
 * @param r part of the picture to unmap
 * @param planes planes description filled with the planes parameters
 */
static void upipe_graph_unmap(struct upipe *upipe, struct uref *uref,
                              const struct rect *r, struct plane *planes)
{
    if (!planes || !planes->chroma)
        return;
    uref_pic_plane_unmap(uref, planes->chroma,
                         r->h.off, r->v.off, r->h.size, r->v.size);
    return upipe_graph_unmap(upipe, uref, r, planes + 1);
}

/** @internal @This handles input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input picture buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_graph_handle2(struct upipe *upipe, struct uref *uref,
                                struct upump **upump_p)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);

    if (unlikely(!graph->ubuf_mgr)) {
        upipe_warn(upipe, "no ubuf manager, dropping...");
        uref_free(uref);
        return;
    }

    size_t pic_hsize = 0;
    size_t pic_vsize = 0;
    uref_pic_size(uref, &pic_hsize, &pic_vsize, NULL);

    uint64_t hoffset = graph->hoffset;
    uint64_t voffset = graph->voffset;
    uint64_t hsize = graph->hsize;
    uint64_t vsize = graph->vsize;

    if (hsize > pic_hsize || vsize > pic_vsize)
        /* out of the screen */
        return upipe_graph_output(upipe, uref, upump_p);

    if (hoffset + hsize > pic_hsize)
        hsize = pic_hsize - hoffset;
    if (voffset + vsize > pic_vsize)
        vsize = pic_vsize - voffset;

    if (!hsize || !vsize)
        /* nothing to render */
        return upipe_graph_output(upipe, uref, upump_p);

    uint64_t history_size = graph->history_size;
    int64_t min = graph->min;
    int64_t max = graph->max;
    int64_t height = max - min;
    if (min >= max) {
        upipe_warn(upipe, "invalid min and max value");
        return upipe_graph_output(upipe, uref, &graph->timer);
    }

    struct plane planes[] = {
        { "y8", 0, 0, 0, NULL, graph->color + 0, 1 },
        { "u8", 0, 0, 0, NULL, graph->color + 1, 1 },
        { "v8", 0, 0, 0, NULL, graph->color + 2, 1 },
        { NULL, 0, 0, 0, NULL, NULL,    0 }
    };
    struct rect rect = {
        .h = { .off = hoffset, .size = hsize },
        .v = { .off = voffset, .size = vsize },
    };
    int err = upipe_graph_map(upipe, uref, &rect, planes);
    if (unlikely(!ubase_check(err))) {
        upipe_warn(upipe, "fail to map buffer");
        uref_free(uref);
        return;
    }

    unsigned vsubmax = 1;
    for (struct plane *p = planes; p->chroma; p++)
        if (p->vsub > vsubmax)
            vsubmax = p->vsub;
    float hres = (float)(height * vsubmax) / vsize;

    int64_t vmin[hsize];
    int64_t vmax[hsize];
    memset(vmin, 0, hsize * sizeof (int64_t));
    memset(vmax, 0, hsize * sizeof (int64_t));

    upipe_foreach_sub(upipe, sub) {
        struct upipe_graph_input *input = upipe_graph_input_from_upipe(sub);

        for (uint64_t h = 0; h < hsize; h++) {
            uint64_t index = h * history_size / hsize;
            int64_t value =
                input->values[(graph->index + (index + 1)) % history_size];

            int64_t tmp = value;
            int64_t lmin = 0;
            int64_t lmax = 0;
            if (input->stacked) {
                if (value >= 0) {
                    value += vmax[h];
                    lmax = vmax[h];
                    vmax[h] += tmp;
                } else {
                    value += vmin[h];
                    lmin = vmin[h];
                    vmin[h] += tmp;
                }
            }

            for (uint64_t v = 0; v < vsize; v++) {
                float current = ((float)(vsize - v) * height / vsize) + min;

                if ((fabs(current - value) <= hres) ||
                    (input->filled &&
                     ((value >= current && value >= 0 && current > lmax) ||
                     (value <= current && value <= 0 && current < lmin)))) {

                    for (unsigned p = 0; p < 3; p++) {
                        planes[p].data[(h / planes[p].hsub
                                        + (v / planes[p].vsub)
                                        * planes[p].stride)] = input->yuva[p];
                    }
                }
            }
        }
    }

    upipe_graph_unmap(upipe, uref, &rect, planes);
    upipe_graph_output(upipe, uref, &graph->timer);
}

/** @internal @This updates the index in the values ring buffer.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_graph_update_index(struct upipe *upipe)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    size_t next_index = (graph->index + 1) % graph->history_size;

    struct uchain *uchain;
    ulist_foreach(&graph->inputs, uchain) {
        struct upipe_graph_input *input = upipe_graph_input_from_uchain(uchain);
        if (!input->values)
            continue;

        input->values[next_index] = input->values[graph->index];
    }
    graph->index = next_index;
}

/** @internal @This handles input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input picture buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_graph_input(struct upipe *upipe, struct uref *uref,
                              struct upump **upump_p)
{
    upipe_graph_handle2(upipe, uref, upump_p);
    upipe_graph_update_index(upipe);
}

/** @internal @This sets the minimum value of the graph.
 *
 * @param upipe description structure of the pipe
 * @param min minimum value shown on the graph
 * @return an error code
 */
static int upipe_graph_set_minimum_real(struct upipe *upipe, int64_t min)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    graph->min = min;
    return UBASE_ERR_NONE;
}

/** @internal @This sets the maximum value of the graph.
 *
 * @param upipe description structure of the pipe
 * @param max maximum value shown on the graph
 * @return an error code
 */
static int upipe_graph_set_maximum_real(struct upipe *upipe, int64_t max)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);
    graph->max = max;
    return UBASE_ERR_NONE;
}

/** @internal @This sets the history size of the graph pipe.
 *
 * @param upipe description structure of the pipe
 * @param size history size in number of elements
 * @return an error code
 */
static int upipe_graph_set_history_real(struct upipe *upipe, uint64_t size)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);

    struct uchain *uchain;
    ulist_foreach(&graph->inputs, uchain) {
        struct upipe_graph_input *input = upipe_graph_input_from_uchain(uchain);
        int64_t *values = NULL;
        if (size) {
            uint64_t *values = calloc(size, sizeof (uint64_t));
            if (unlikely(!values)) {
                upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
                free(input->values);
                input->values = NULL;
                continue;
            }
        }

        uint64_t from_index = graph->index;
        uint64_t to_index = 0;
        for (uint64_t i = 0; i < graph->history_size && i < size; i++) {
            values[to_index] = input->values[from_index];
            from_index = from_index ? from_index - 1 : graph->history_size - 1;
            to_index = to_index ? to_index - 1 : size - 1;
        }

        free(input->values);
        input->values = values;
    }
    graph->history_size = size;
    graph->index = 0;
    return UBASE_ERR_NONE;
}

/** @internal @This sets the background color of the graph.
 *
 * @param upipe description structure of the pipe
 * @param color string representing rgb color (rgb(R, G, B))
 * @return an error code
 */
static int upipe_graph_set_color_real(struct upipe *upipe, const char *color)
{
    struct upipe_graph *graph = upipe_graph_from_upipe(upipe);

    uint8_t rgb[3];
    if (unlikely(!ubase_check(ubuf_pic_parse_rgb(color, rgb))))
        return UBASE_ERR_INVALID;
    ubuf_pic_rgb_to_yuv(rgb, graph->fullrange, graph->color);
    return UBASE_ERR_NONE;
}

/** @internal @This checks and sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new flow def to set
 * @return an error code
 */
static int upipe_graph_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    UBASE_RETURN(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF));
    if (unlikely(!ubase_check(uref_pic_flow_check_yuv420p(flow_def)) &&
                 !ubase_check(uref_pic_flow_check_yuv422p(flow_def)) &&
                 !ubase_check(uref_pic_flow_check_yuv444p(flow_def)) &&
                 !ubase_check(uref_pic_flow_check_yuva420p(flow_def)) &&
                 !ubase_check(uref_pic_flow_check_yuva422p(flow_def)) &&
                 !ubase_check(uref_pic_flow_check_yuva444p(flow_def))))
        return UBASE_ERR_INVALID;
    return UBASE_ERR_NONE;
}

/** @internal @This handles the pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param cmd control command to handle
 * @param args optional arguments of the control command
 * @return an error code
 */
static int upipe_graph_control_real(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_graph_control_inputs(upipe, cmd, args));
    UBASE_HANDLED_RETURN(upipe_graph_control_output(upipe, cmd, args));
    switch (cmd) {
        case UPIPE_ATTACH_UPUMP_MGR:
            upipe_graph_set_timer(upipe, NULL);
            upipe_graph_attach_upump_mgr(upipe);
            return UBASE_ERR_NONE;
        case UPIPE_ATTACH_UCLOCK:
            upipe_graph_set_timer(upipe, NULL);
            upipe_graph_require_uclock(upipe);
            return UBASE_ERR_NONE;
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_graph_set_flow_def(upipe, flow_def);
        }
    }

    if (cmd < UPIPE_CONTROL_LOCAL ||
        ubase_get_signature(args) != UPIPE_GRAPH_SIGNATURE)
        return UBASE_ERR_UNHANDLED;

    switch (cmd) {
        case UPIPE_GRAPH_SET_MINIMUM: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SIGNATURE);
            int64_t min = va_arg(args, int64_t);
            return upipe_graph_set_minimum_real(upipe, min);
        }
        case UPIPE_GRAPH_SET_MAXIMUM: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SIGNATURE);
            int64_t max = va_arg(args, int64_t);
            return upipe_graph_set_maximum_real(upipe, max);
        }
        case UPIPE_GRAPH_SET_HISTORY: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SIGNATURE);
            uint64_t size = va_arg(args, uint64_t);
            return upipe_graph_set_history_real(upipe, size);
        }
        case UPIPE_GRAPH_SET_COLOR: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_GRAPH_SIGNATURE);
            const char *color = va_arg(args, const char *);
            return upipe_graph_set_color_real(upipe, color);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This handles control commands and check the internal pipe state.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_graph_control(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_RETURN(upipe_graph_control_real(upipe, cmd, args));
    return upipe_graph_check(upipe, NULL);
}

/** @internal @This is the static graph pipe manager. */
static struct upipe_mgr upipe_graph_mgr = {
    .refcount = NULL,
    .signature = UPIPE_GRAPH_SIGNATURE,
    .upipe_err_str = NULL,
    .upipe_command_str = NULL,
    .upipe_event_str = NULL,
    .upipe_alloc = upipe_graph_alloc,
    .upipe_input = upipe_graph_input,
    .upipe_control = upipe_graph_control,
};

/** @This returns the static graph pipe manager.
 *
 * @return a pointer to the graph pipe manager.
 */
struct upipe_mgr *upipe_graph_mgr_alloc(void)
{
    return &upipe_graph_mgr;
}
