/*
 * Copyright (C) 2020 EasyTools
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

#include <upipe/ubase.h>
#include <upipe/uuri.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict_inline.h>
#include <upipe/uref_std.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/uref_block.h>
#include <upipe-modules/upipe_file_source.h>
#include <upipe-modules/upipe_rtp_source.h>
#include <upipe-modules/upipe_udp_source.h>
#include <upipe-modules/upipe_file_sink.h>
#include <upipe-modules/upipe_probe_uref.h>
#include <upipe-modules/upipe_null.h>
#include <upipe-modules/upipe_setflowdef.h>
#include <upipe-ts/upipe_ts_sync.h>
#include <upipe-ts/upipe_ts_split.h>
#include <upipe-ts/upipe_ts_decaps.h>
#include <upipe-ts/upipe_ts_pes_decaps.h>
#include <upipe-framers/upipe_mpga_framer.h>
#include <upipe-framers/upipe_h264_framer.h>
#include <upump-ev/upump_ev.h>

#define USTRING_STR(USTRING, STRING, ...)                           \
    do {                                                            \
        char STRING[USTRING.len + 1];                               \
        ustring_cpy(USTRING, STRING, sizeof(STRING));               \
        __VA_ARGS__;                                                \
    } while (0)

#define UURI_AUTHORITY_STR(UURI, STRING, ...)                               \
    do {                                                                    \
        size_t len = 0;                                                     \
        uuri_authority_len(&UURI.authority, &len);                          \
        char STRING[len + 1];                                               \
        uuri_authority_to_buffer(&UURI.authority, STRING, sizeof (STRING)); \
        __VA_ARGS__;                                                        \
    } while (0)

static enum uprobe_log_level uprobe_log_level = UPROBE_LOG_DEBUG;
static struct uref_mgr *uref_mgr = NULL;

static struct upipe *upipe_source_alloc(const char *uri, struct uprobe *uprobe)
{
    struct uuri uuri;
    if (unlikely(!ubase_check(uuri_from_str(&uuri, uri)))) {
        /* URI must be a file path */
        uuri = uuri_null();
        uuri.scheme = ustring_from_str("file");
        uuri.path = ustring_from_str(uri);
    }

    struct upipe *upipe_src = NULL;

    if (ustring_match_str(uuri.scheme, "file")) {
        struct upipe_mgr *upipe_fsrc_mgr = upipe_fsrc_mgr_alloc();
        assert(upipe_fsrc_mgr);
        upipe_src = upipe_void_alloc(
            upipe_fsrc_mgr,
            uprobe_pfx_alloc(
                uprobe_use(uprobe),
                UPROBE_LOG_VERBOSE, "fsrc"));
        assert(upipe_src);
        upipe_mgr_release(upipe_fsrc_mgr);
        USTRING_STR(uuri.path, path, upipe_set_uri(upipe_src, path));
    }
    else if (ustring_match_str(uuri.scheme, "rtp")) {
        struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, "rtp.");
        assert(flow_def);
        struct upipe_mgr *upipe_rtpsrc_mgr = upipe_rtpsrc_mgr_alloc();
        upipe_src = upipe_flow_alloc(
            upipe_rtpsrc_mgr,
            uprobe_pfx_alloc(
                uprobe_use(uprobe),
                UPROBE_LOG_VERBOSE, "rtp"),
            flow_def);
        upipe_mgr_release(upipe_rtpsrc_mgr);
        uref_free(flow_def);
        assert(upipe_src);
        UURI_AUTHORITY_STR(uuri, path, upipe_set_uri(upipe_src, path));

    }
    else if (ustring_match_str(uuri.scheme, "udp")) {
        struct upipe_mgr *upipe_udpsrc_mgr =upipe_udpsrc_mgr_alloc();
        upipe_src = upipe_void_alloc(
            upipe_udpsrc_mgr,
            uprobe_pfx_alloc(
                uprobe_use(uprobe),
                UPROBE_LOG_VERBOSE, "udp"));
        upipe_mgr_release(upipe_udpsrc_mgr);
        assert(upipe_src);
        UURI_AUTHORITY_STR(uuri, path, upipe_set_uri(upipe_src, path));

    }
    return upipe_src;
}

int main(int argc, char *argv[])
{
    assert(argc > 2);
    const char *source = argv[1];
    const char *framer = argv[2];

    /* create managers */
    struct upump_mgr *upump_mgr = upump_ev_mgr_alloc_default(0, 0);
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(0, umem_mgr, -1, -1);
    uref_mgr = uref_std_mgr_alloc(0, udict_mgr, 0);
    udict_mgr_release(udict_mgr);

    /* create root probe */
    struct uprobe *uprobe = NULL;
    uprobe = uprobe_stdio_alloc(NULL, stderr, uprobe_log_level);
    assert(uprobe != NULL);
    uprobe = uprobe_uref_mgr_alloc(uprobe, uref_mgr);
    assert(uprobe != NULL);
    uprobe = uprobe_upump_mgr_alloc(uprobe, upump_mgr);
    assert(uprobe != NULL);
    uprobe = uprobe_ubuf_mem_alloc(uprobe, umem_mgr, 0, 0);
    assert(uprobe != NULL);

    /* create file source */
    struct upipe *upipe_src = upipe_source_alloc(source, uprobe);

    struct upipe_mgr *upipe_framer_mgr = NULL;
    if (!strcmp(framer, "mpga")) {
        upipe_framer_mgr = upipe_mpgaf_mgr_alloc();
    }
    else if (!strcmp(framer, "h264")) {
        upipe_framer_mgr = upipe_h264f_mgr_alloc();
    }
    assert(upipe_framer_mgr);

    struct upipe *upipe_framer = upipe_void_alloc(
        upipe_framer_mgr,
        uprobe_pfx_alloc(
            uprobe_use(uprobe),
            UPROBE_LOG_VERBOSE, framer));
    assert(upipe_framer);
    upipe_mgr_release(upipe_framer_mgr);
    upipe_set_output(upipe_src, upipe_framer);

    struct upipe_mgr *upipe_null_mgr = upipe_null_mgr_alloc();
    assert(upipe_null_mgr);
    struct upipe *upipe_null = upipe_void_alloc(
        upipe_null_mgr,
        uprobe_pfx_alloc(
            uprobe_use(uprobe),
            UPROBE_LOG_VERBOSE, "null"));
    upipe_mgr_release(upipe_null_mgr);
    upipe_set_output(upipe_framer, upipe_null);
    upipe_release(upipe_framer);
    upipe_release(upipe_null);

    /* main loop */
    upump_mgr_run(upump_mgr, NULL);

    /* release probes, pipes and managers */
    uprobe_release(uprobe);
    upipe_release(upipe_src);
    uref_mgr_release(uref_mgr);
    umem_mgr_release(umem_mgr);
    upump_mgr_release(upump_mgr);

    return 0;
}
