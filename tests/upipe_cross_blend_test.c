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
#include <upipe/upipe_helper_void.h>

#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/ubuf_mem.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/uref_std.h>
#include <upipe/uref_dump.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/uref_sound.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>

#include <upipe-modules/upipe_cross_blend.h>

#define UBUF_POOL_DEPTH     5
#define UDICT_POOL_DEPTH    5
#define UREF_POOL_DEPTH     5
#define UPROBE_LOG_LEVEL    UPROBE_LOG_DEBUG
#define N_UREFS             5
#define CROSSBLEND_PERIOD (UCLOCK_FREQ / 5)
#define PLANES              2
#define SAMPLE              1000.

struct sink {
    struct upipe upipe;
    struct urefcount urefcount;
    struct uref *flow_def;
    uint64_t count;
    float input[PLANES];
};

UPIPE_HELPER_UPIPE(sink, upipe, 0);
UPIPE_HELPER_UREFCOUNT(sink, urefcount, sink_free);
UPIPE_HELPER_VOID(sink);

static void sink_free(struct upipe *upipe)
{
    struct sink *sink = sink_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(sink->flow_def);
    sink_clean_urefcount(upipe);
    sink_free_void(upipe);
}

static struct upipe *sink_alloc(struct upipe_mgr *mgr,
                                struct uprobe *uprobe,
                                uint32_t signature, va_list args)
{
    struct upipe *upipe = sink_alloc_void(mgr, uprobe, signature, args);
    assert(upipe);

    sink_init_urefcount(upipe);
    struct sink *sink = sink_from_upipe(upipe);
    sink->count = 0;
    sink->flow_def = NULL;

    for (uint8_t plane = 0; plane < PLANES; plane++)
        sink->input[plane] = SAMPLE + plane * SAMPLE + 1;

    upipe_throw_ready(upipe);

    return upipe;
}

static void sink_input(struct upipe *upipe,
                       struct uref *uref,
                       struct upump **upump_p)
{
    struct sink *sink = sink_from_upipe(upipe);
    uint8_t planes;

    uref_dump(uref, upipe->uprobe);

    assert(sink->flow_def);
    ubase_assert(uref_sound_flow_get_planes(sink->flow_def, &planes));
    assert(planes == PLANES);

    size_t size;
    ubase_assert(uref_sound_size(uref, &size, NULL));

    const float *buf[planes];
    ubase_assert(uref_sound_read_float(uref, 0, size, buf, planes));
    for (uint8_t plane = 0; plane < planes; plane++)
        for (size_t i = 0; i < size; i++) {
            upipe_dbg_va(upipe, "buffer[%u][%zu] = %f",
                         plane, i, buf[plane][i]);
            assert(buf[plane][i] < sink->input[plane]);
            assert(buf[plane][i] > -SAMPLE - plane * SAMPLE - 1.);
            sink->input[plane] = buf[plane][i];
        }
    uref_sound_unmap(uref, 0, size, planes);

    sink->count++;
    uref_free(uref);
}

static int sink_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct sink *sink = sink_from_upipe(upipe);
    if (sink->flow_def)
        uref_free(sink->flow_def);
    sink->flow_def = uref_dup(flow_def);
    assert(sink->flow_def);
    return UBASE_ERR_NONE;
}

static int sink_control(struct upipe *upipe,
                        int command, va_list args)
{
    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return sink_set_flow_def(upipe, flow_def);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

static struct upipe_mgr sink_mgr = {
    .refcount = NULL,
    .signature = 0,
    .upipe_alloc = sink_alloc,
    .upipe_control = sink_control,
    .upipe_input = sink_input,
};

static bool xblend_in0_ended = false;
static int catch_xblend_in0(struct uprobe *uprobe,
                            struct upipe *upipe,
                            int event, va_list args)
{
    switch (event) {
        case UPROBE_SINK_END:
            xblend_in0_ended = true;
            break;
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

int main(int argc, char *argv[])
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH,
                                                   udict_mgr, 0);
    struct uprobe *logger = uprobe_stdio_alloc(NULL, stdout, UPROBE_LOG_LEVEL);
    struct upipe_mgr *upipe_xblend_mgr = upipe_xblend_mgr_alloc();

    assert(umem_mgr && udict_mgr && uref_mgr && logger && upipe_xblend_mgr);

    struct upipe *upipe_xblend =
        upipe_void_alloc(upipe_xblend_mgr,
                         uprobe_pfx_alloc(
                            uprobe_use(logger),
                            UPROBE_LOG_LEVEL, "xbld"));
    ubase_assert(upipe_xblend_set_duration(upipe_xblend, CROSSBLEND_PERIOD));

    struct upipe *upipe_sink =
        upipe_void_alloc_output(upipe_xblend,
                                &sink_mgr,
                                uprobe_pfx_alloc(
                                    uprobe_use(logger),
                                    UPROBE_LOG_LEVEL, "sink"));
    assert(upipe_sink);
    struct sink *sink = sink_from_upipe(upipe_sink);
    upipe_release(upipe_sink);

    struct upipe *upipe_xblend_in0 =
        upipe_void_alloc_sub(upipe_xblend,
                             uprobe_alloc(
                                catch_xblend_in0,
                                uprobe_pfx_alloc(
                                    uprobe_use(logger),
                                    UPROBE_LOG_LEVEL, "in 0")));
    assert(upipe_xblend_in0);

    struct uref *flow_def = uref_sound_flow_alloc_def(uref_mgr, "f32.",
                                                      2, 4 * 2);
    assert(flow_def);
    ubase_assert(uref_sound_flow_add_plane(flow_def, "l"));
    ubase_assert(uref_sound_flow_add_plane(flow_def, "r"));
    ubase_assert(uref_sound_flow_set_rate(flow_def, 80));
    struct ubuf_mgr *ubuf_sound_mgr =
        ubuf_mem_mgr_alloc_from_flow_def(
                UBUF_POOL_DEPTH, UBUF_POOL_DEPTH,
                umem_mgr, flow_def);
    assert(ubuf_sound_mgr);

    ubase_assert(upipe_set_flow_def(upipe_xblend_in0, flow_def));

    uint8_t planes;
    ubase_assert(uref_sound_flow_get_planes(flow_def, &planes));
    for (unsigned i = 0; i < N_UREFS; i++) {
        struct uref *uref = uref_sound_alloc(uref_mgr, ubuf_sound_mgr, 4);
        assert(uref);

        float *buf[planes];
        ubase_assert(uref_sound_write_float(uref, 0, 4, buf, planes));
        for (uint8_t plane = 0; plane < planes; plane++)
            for (size_t j = 0; j < 4; j++)
                buf[plane][j] = SAMPLE + SAMPLE * plane - i * 4 - j;
        uref_sound_unmap(uref, 0, 4, planes);
        upipe_input(upipe_xblend_in0, uref, NULL);
        assert(sink->count == i + 1);
    }

    for (uint8_t plane = 0; plane < planes; plane++)
        sink->input[plane] = SAMPLE + SAMPLE * plane + 1;
    sink->count = 0;

    struct upipe *upipe_xblend_in1 =
        upipe_void_alloc_sub(upipe_xblend,
                             uprobe_pfx_alloc(
                                uprobe_use(logger),
                                UPROBE_LOG_LEVEL, "in 1"));
    assert(upipe_xblend_in1);

    {
        struct uref *uref = uref_sound_alloc(uref_mgr, ubuf_sound_mgr, 4);
        assert(uref);
        upipe_input(upipe_xblend_in1, uref, NULL);
        assert(!sink->count);
    }

    ubase_assert(upipe_set_flow_def(upipe_xblend_in1, flow_def));

    for (unsigned i = 0; i < N_UREFS; i++) {
        float *buf[planes];

        struct uref *uref = uref_sound_alloc(uref_mgr, ubuf_sound_mgr, 2);
        assert(uref);
        ubase_assert(uref_sound_write_float(uref, 0, 2, buf, planes));
        for (uint8_t plane = 0; plane < planes; plane++)
            for (size_t j = 0; j < 2; j++)
                buf[plane][j] = SAMPLE + SAMPLE * plane - i * planes - j;
        ubase_assert(uref_sound_unmap(uref, 0, 2, planes));
        upipe_input(upipe_xblend_in0, uref, NULL);

        uref = uref_sound_alloc(uref_mgr, ubuf_sound_mgr, 4);
        assert(uref);
        ubase_assert(uref_sound_write_float(uref, 0, 4, buf, planes));
        for (uint8_t plane = 0; plane < planes; plane++)
            for (size_t j = 0; j < 4; j++)
                buf[plane][j] = -SAMPLE - SAMPLE * plane +
                    4 * N_UREFS - i * 4 - j;
        ubase_assert(uref_sound_unmap(uref, 0, 4, planes));
        upipe_input(upipe_xblend_in1, uref, NULL);

        uref = uref_sound_alloc(uref_mgr, ubuf_sound_mgr, 2);
        assert(uref);
        ubase_assert(uref_sound_write_float(uref, 0, 2, buf, planes));
        for (uint8_t plane = 0; plane< planes; plane++)
            for (size_t j = 0; j < 2; j++)
                buf[plane][j] = SAMPLE + SAMPLE * plane - i * planes - j - 2;
        ubase_assert(uref_sound_unmap(uref, 0, 2, planes));
        upipe_input(upipe_xblend_in0, uref, NULL);

        if (i >= 3)
            assert(xblend_in0_ended);
        else
            assert(!xblend_in0_ended);
        assert(sink->count == i + 1);
    }

    uref_free(flow_def);
    upipe_release(upipe_xblend_in1);
    upipe_release(upipe_xblend_in0);
    upipe_release(upipe_xblend);
    upipe_mgr_release(upipe_xblend_mgr);
    uprobe_release(logger);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    ubuf_mgr_release(ubuf_sound_mgr);
    umem_mgr_release(umem_mgr);
    return 0;
}
