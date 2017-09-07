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

#include <upump-ev/upump_ev.h>

#include <upipe/umem.h>
#include <upipe/umem_alloc.h>

#include <upipe/udict.h>
#include <upipe/udict_inline.h>

#include <upipe/uref_std.h>

#include <upipe/uclock_std.h>

#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem_pool.h>
#include <upipe/uprobe_uclock.h>

#include <upipe-modules/upipe_file_source.h>
#include <upipe-modules/upipe_file_sink.h>
#include <upipe-modules/upipe_rtp_source.h>
#include <upipe-modules/upipe_rtp_prepend.h>
#include <upipe-modules/upipe_udp_sink.h>

#include <upipe-ts/upipe_ts_align.h>
#include <upipe-ts/upipe_ts_check.h>

#include <upipe-dvbcsa/upipe_dvbcsa_decrypt.h>

#include <assert.h>

#define UPUMP_POOL                      5
#define UPUMP_BLOCKER_POOL              5
#define UDICT_POOL_DEPTH                500
#define UREF_POOL_DEPTH                 500
#define UBUF_POOL_DEPTH                 3000
#define UBUF_SHARED_POOL_DEPTH          50
#define UPROBE_LEVEL                    UPROBE_LOG_DEBUG

int main(int argc, char *argv[])
{
    assert(argc == 3);
    const char *input = argv[1];
    const char *output = argv[2];

    struct uclock *uclock = uclock_std_alloc(0);
    assert(uclock);

    struct upump_mgr *upump_mgr =
        upump_ev_mgr_alloc_default(UPUMP_POOL, UPUMP_BLOCKER_POOL);
    assert(upump_mgr);

    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr);

    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr);

    struct uref_mgr *uref_mgr =
        uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(uref_mgr);

    struct uprobe *uprobe_main = uprobe_stdio_alloc(NULL, stderr, UPROBE_LEVEL);
    assert(uprobe_main);
    uprobe_main = uprobe_ubuf_mem_pool_alloc(uprobe_main, umem_mgr,
                                             UBUF_POOL_DEPTH,
                                             UBUF_SHARED_POOL_DEPTH);
    assert(uprobe_main);
    uprobe_main = uprobe_uref_mgr_alloc(uprobe_main, uref_mgr);
    assert(uprobe_main);
    uprobe_main = uprobe_upump_mgr_alloc(uprobe_main, upump_mgr);
    assert(uprobe_main);
    uprobe_main = uprobe_uclock_alloc(uprobe_main, uclock);

    struct upipe_mgr *upipe_fsrc_mgr = upipe_fsrc_mgr_alloc();
    assert(upipe_fsrc_mgr);
    struct upipe *source =
        upipe_void_alloc(upipe_fsrc_mgr,
                         uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                          UPROBE_LEVEL, "src"));
    assert(source);
    upipe_mgr_release(upipe_fsrc_mgr);
    if (!ubase_check(upipe_set_uri(source, input))) {
        upipe_release(source);

        struct upipe_mgr *upipe_rtpsrc_mgr = upipe_rtpsrc_mgr_alloc();
        assert(upipe_rtpsrc_mgr);
        source = upipe_void_alloc(upipe_rtpsrc_mgr,
                                  uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                                   UPROBE_LEVEL, "src"));
        assert(source);
        upipe_mgr_release(upipe_rtpsrc_mgr);
        ubase_assert(upipe_set_uri(source, input));
    }

    struct upipe_mgr *upipe_ts_align_mgr = upipe_ts_align_mgr_alloc();
    assert(upipe_ts_align_mgr);
    struct upipe *ts_align =
        upipe_void_alloc(upipe_ts_align_mgr,
                         uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                          UPROBE_LEVEL, "align"));
    assert(ts_align);
    upipe_mgr_release(upipe_ts_align_mgr);

    ubase_assert(upipe_set_output(source, ts_align));

    struct upipe_mgr *upipe_ts_check_mgr = upipe_ts_check_mgr_alloc();
    assert(upipe_ts_check_mgr);
    struct upipe *ts_check =
        upipe_void_alloc(upipe_ts_check_mgr,
                         uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                          UPROBE_LEVEL, "check"));
    assert(ts_check);
    upipe_mgr_release(upipe_ts_check_mgr);

    ubase_assert(upipe_set_output(ts_align, ts_check));
    upipe_release(ts_align);

    struct upipe_mgr *upipe_dvbcsa_dec_mgr = upipe_dvbcsa_dec_mgr_alloc();
    assert(upipe_dvbcsa_dec_mgr);
    struct upipe *ts_decrypt =
        upipe_void_alloc(upipe_dvbcsa_dec_mgr,
                         uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                          UPROBE_LEVEL, "decrypt"));
    assert(ts_decrypt);
    upipe_mgr_release(upipe_dvbcsa_dec_mgr);

    ubase_assert(upipe_dvbcsa_dec_set_key(ts_decrypt, "124578875421"));
    ubase_assert(upipe_set_output(ts_check, ts_decrypt));
    upipe_release(ts_check);

    struct upipe *sink;
    if (!strncmp(output, "rtp://", strlen("rtp://"))) {
        output += strlen("rtp://");

        struct upipe_mgr *upipe_rtp_prepend_mgr = upipe_rtp_prepend_mgr_alloc();
        assert(upipe_rtp_prepend_mgr);
        sink =
            upipe_void_chain_output(
                ts_decrypt, upipe_rtp_prepend_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                 UPROBE_LEVEL, "rtpp"));
        assert(sink);
        upipe_mgr_release(upipe_rtp_prepend_mgr);

        struct upipe_mgr *upipe_udpsink_mgr = upipe_udpsink_mgr_alloc();
        assert(upipe_udpsink_mgr);
        sink = upipe_void_chain_output(sink, upipe_udpsink_mgr,
                                       uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                                        UPROBE_LEVEL, "udp"));
        assert(sink);
        upipe_mgr_release(upipe_udpsink_mgr);
        ubase_assert(upipe_set_uri(sink, output));
    }
    else {
        struct upipe_mgr *upipe_fsink_mgr = upipe_fsink_mgr_alloc();
        assert(upipe_fsink_mgr);
        sink =
            upipe_void_chain_output(
                ts_decrypt, upipe_fsink_mgr,
                uprobe_pfx_alloc(uprobe_use(uprobe_main),
                                 UPROBE_LEVEL, "file sink"));
        assert(sink);
        upipe_mgr_release(upipe_fsink_mgr);

        ubase_assert(upipe_fsink_set_path(sink, output,
                                          UPIPE_FSINK_OVERWRITE));
    }

    upipe_release(sink);

    upump_mgr_run(upump_mgr, NULL);

    upipe_release(source);
    uprobe_release(uprobe_main);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    upump_mgr_release(upump_mgr);
    uclock_release(uclock);
    return 0;
}
