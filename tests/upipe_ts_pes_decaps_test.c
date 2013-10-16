/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
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
 * @short unit tests for TS PES decaps module
 */

#undef NDEBUG

#include <upipe/uprobe.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_log.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/ubuf.h>
#include <upipe/ubuf_block.h>
#include <upipe/ubuf_block_mem.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_std.h>
#include <upipe/upipe.h>
#include <upipe-ts/upipe_ts_pes_decaps.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include <bitstream/mpeg/pes.h>

#define UDICT_POOL_DEPTH 0
#define UREF_POOL_DEPTH 0
#define UBUF_POOL_DEPTH 0
#define UPROBE_LOG_LEVEL UPROBE_LOG_DEBUG

static unsigned int nb_packets = 0;
static uint64_t pts = 0x112121212;
static uint64_t dts = 0x112121212 - 1080000;
static bool dataalignment = true;
static size_t payload_size = 12;
static bool expect_lost = false;
static bool expect_acquired = true;

/** definition of our uprobe */
static bool catch(struct uprobe *uprobe, struct upipe *upipe,
                  enum uprobe_event event, va_list args)
{
    switch (event) {
        default:
            assert(event & UPROBE_HANDLED_FLAG);
            break;
        case UPROBE_READY:
        case UPROBE_DEAD:
        case UPROBE_NEW_FLOW_DEF:
            break;
        case UPROBE_SYNC_ACQUIRED:
            assert(expect_acquired);
            expect_acquired = false;
            break;
        case UPROBE_SYNC_LOST:
            assert(expect_lost);
            expect_lost = false;
            break;
        case UPROBE_CLOCK_TS: {
            struct uref *uref = va_arg(args, struct uref *);
            uint64_t decaps_pts = UINT64_MAX, decaps_dts = UINT64_MAX;
            assert(uref != NULL);
            uref_clock_get_pts_orig(uref, &decaps_pts);
            uref_clock_get_dts_orig(uref, &decaps_dts);
            assert(decaps_pts == pts * 300);
            assert(decaps_dts == dts * 300);
            pts = dts = 0;
            break;
        }
    }
    return true;
}

/** helper phony pipe to test upipe_ts_pesd */
static struct upipe *ts_test_alloc(struct upipe_mgr *mgr,
                                   struct uprobe *uprobe, uint32_t signature,
                                   va_list args)
{
    struct upipe *upipe = malloc(sizeof(struct upipe));
    assert(upipe != NULL);
    upipe_init(upipe, mgr, uprobe);
    return upipe;
}

/** helper phony pipe to test upipe_ts_pesd */
static void ts_test_input(struct upipe *upipe, struct uref *uref,
                          struct upump *upump)
{
    assert(uref != NULL);
    size_t size;
    assert(uref_block_size(uref, &size));
    assert(size == payload_size);
    assert(dataalignment == uref_block_get_start(uref));
    uref_free(uref);
    nb_packets--;
}

/** helper phony pipe to test upipe_ts_pesd */
static void ts_test_free(struct upipe *upipe)
{
    upipe_clean(upipe);
    free(upipe);
}

/** helper phony pipe to test upipe_ts_pesd */
static struct upipe_mgr ts_test_mgr = {
    .upipe_alloc = ts_test_alloc,
    .upipe_input = ts_test_input,
    .upipe_control = NULL,
    .upipe_free = NULL,

    .upipe_mgr_free = NULL
};

int main(int argc, char *argv[])
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    assert(uref_mgr != NULL);
    struct ubuf_mgr *ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH,
                                                         UBUF_POOL_DEPTH,
                                                         umem_mgr, -1, -1,
                                                         -1, 0);
    assert(ubuf_mgr != NULL);
    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    struct uprobe *uprobe_stdio = uprobe_stdio_alloc(&uprobe, stdout,
                                                     UPROBE_LOG_LEVEL);
    assert(uprobe_stdio != NULL);
    struct uprobe *log = uprobe_log_alloc(uprobe_stdio, UPROBE_LOG_LEVEL);
    assert(log != NULL);

    struct upipe *upipe_sink = upipe_void_alloc(&ts_test_mgr, log);
    assert(upipe_sink != NULL);

    struct uref *uref;
    uref = uref_block_flow_alloc_def(uref_mgr, "mpegtspes.");
    assert(uref != NULL);

    struct upipe_mgr *upipe_ts_pesd_mgr = upipe_ts_pesd_mgr_alloc();
    assert(upipe_ts_pesd_mgr != NULL);
    struct upipe *upipe_ts_pesd = upipe_void_alloc(upipe_ts_pesd_mgr,
            uprobe_pfx_adhoc_alloc(log, UPROBE_LOG_LEVEL, "ts pesd"));
    assert(upipe_ts_pesd != NULL);
    assert(upipe_set_flow_def(upipe_ts_pesd, uref));
    assert(upipe_set_output(upipe_ts_pesd, upipe_sink));
    uref_free(uref);

    uint8_t *buffer;
    int size;
    uref = uref_block_alloc(uref_mgr, ubuf_mgr, PES_HEADER_SIZE_PTSDTS + 12);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PES_HEADER_SIZE_PTSDTS + 12);
    pes_init(buffer);
    pes_set_streamid(buffer, PES_STREAM_ID_VIDEO_MPEG);
    pes_set_length(buffer, PES_HEADER_SIZE_PTSDTS + 12 - PES_HEADER_SIZE);
    pes_set_headerlength(buffer, PES_HEADER_SIZE_PTSDTS - PES_HEADER_SIZE_NOPTS);
    pes_set_dataalignment(buffer);
    pes_set_pts(buffer, pts);
    pes_set_dts(buffer, dts);
    uref_block_unmap(uref, 0);
    uref_block_set_start(uref);
    nb_packets++;
    upipe_input(upipe_ts_pesd, uref, NULL);
    assert(!nb_packets);
    assert(!expect_acquired);
    assert(!pts);
    assert(!dts);

    dts = pts = 0x112121212;
    uref = uref_block_alloc(uref_mgr, ubuf_mgr, PES_HEADER_SIZE_PTS);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PES_HEADER_SIZE_PTS);
    pes_init(buffer);
    pes_set_streamid(buffer, PES_STREAM_ID_VIDEO_MPEG);
    pes_set_length(buffer, PES_HEADER_SIZE_PTS - PES_HEADER_SIZE);
    pes_set_headerlength(buffer, PES_HEADER_SIZE_PTS - PES_HEADER_SIZE_NOPTS);
    dataalignment = false;
    pes_set_pts(buffer, pts);
    uref_block_unmap(uref, 0);
    payload_size = 0;

    /* now cut it into pieces */
    nb_packets++;
    for (int i = 0; i < PES_HEADER_SIZE_PTS; i++) {
        struct uref *dup = uref_dup(uref);
        assert(dup != NULL);
        assert(uref_block_resize(dup, i, 1));
        if (!i)
            uref_block_set_start(dup);
        upipe_input(upipe_ts_pesd, dup, NULL);
    }
    assert(!nb_packets);
    uref_free(uref);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr, 42);
    assert(uref != NULL);
    payload_size = 42;
    dataalignment = false;
    pts = dts = 0;
    nb_packets++;
    upipe_input(upipe_ts_pesd, uref, NULL);
    assert(!nb_packets);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr, PES_HEADER_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PES_HEADER_SIZE);
    pes_init(buffer);
    pes_set_streamid(buffer, PES_STREAM_ID_PADDING);
    pes_set_length(buffer, 42);
    uref_block_unmap(uref, 0);
    payload_size = 0;
    expect_lost = true;
    uref_block_set_start(uref);
    /* do not increment nb_packets */
    upipe_input(upipe_ts_pesd, uref, NULL);
    assert(!nb_packets);
    assert(!expect_lost);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr, 42);
    assert(uref != NULL);
    payload_size = 42;
    dataalignment = false;
    pts = dts = 0;
    /* do not increment nb_packets */
    upipe_input(upipe_ts_pesd, uref, NULL);
    assert(!nb_packets);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr, PES_HEADER_SIZE_NOPTS + 12);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PES_HEADER_SIZE_NOPTS + 12);
    pes_init(buffer);
    pes_set_streamid(buffer, PES_STREAM_ID_VIDEO_MPEG);
    pes_set_length(buffer, PES_HEADER_SIZE_NOPTS + 12 - PES_HEADER_SIZE);
    pes_set_headerlength(buffer, 0);
    dataalignment = false;
    uref_block_unmap(uref, 0);
    uref_block_set_start(uref);
    payload_size = 12;
    expect_acquired = true;
    nb_packets++;
    upipe_input(upipe_ts_pesd, uref, NULL);
    assert(!nb_packets);
    assert(!expect_acquired);

    upipe_release(upipe_ts_pesd);
    upipe_mgr_release(upipe_ts_pesd_mgr); // nop

    ts_test_free(upipe_sink);

    uref_mgr_release(uref_mgr);
    ubuf_mgr_release(ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_log_free(log);
    uprobe_stdio_free(uprobe_stdio);

    return 0;
}
