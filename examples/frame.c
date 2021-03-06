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
#include <upipe/uref_dump.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/uprobe_select_flows.h>
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
#include <upipe-framers/upipe_auto_framer.h>
#include <upipe-framers/upipe_mpga_framer.h>
#include <upipe-framers/upipe_h264_framer.h>
#include <upipe-framers/upipe_dvbsub_framer.h>
#include <upump-ev/upump_ev.h>

#include <getopt.h>

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
static bool second_framer = false;

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

static int catch_es(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    switch (event) {
        case UPROBE_NEED_OUTPUT: {
            struct uref *flow_def = va_arg(args, struct uref *);
            uref_dump_notice(flow_def, uprobe);

            upipe_use(upipe);

            if (second_framer) {
                struct upipe_mgr *upipe_autof_mgr = upipe_autof_mgr_alloc();
                upipe = upipe_void_chain_output(
                    upipe, upipe_autof_mgr,
                    uprobe_pfx_alloc(
                        uprobe_use(uprobe),
                        UPROBE_LOG_VERBOSE, "framer 2"));
                upipe_mgr_release(upipe_autof_mgr);
            }

            struct upipe_mgr *upipe_null_mgr = upipe_null_mgr_alloc();
            assert(upipe_null_mgr);
            upipe = upipe_void_chain_output(
                upipe, upipe_null_mgr,
                uprobe_pfx_alloc(
                    uprobe_use(uprobe),
                    UPROBE_LOG_VERBOSE, "null"));
            upipe_mgr_release(upipe_null_mgr);

            upipe_release(upipe);
            return UBASE_ERR_NONE;
        }
        default:
            break;
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

enum {
    OPT_VERBOSE,
    OPT_QUIET,
    OPT_TS,
    OPT_REFRAME,
};

static struct option options[] = {
    { "verbose", no_argument, NULL, OPT_VERBOSE },
    { "quiet", no_argument, NULL, OPT_QUIET },
    { "ts", no_argument, NULL, OPT_TS },
    { "reframe", no_argument, NULL, OPT_REFRAME },
    { NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
    bool ts = false;
    int opt;
    while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
        switch (opt) {
            case OPT_QUIET:
                if (uprobe_log_level < UPROBE_LOG_ERROR)
                    uprobe_log_level++;
                break;

            case OPT_VERBOSE:
                if (uprobe_log_level > 0)
                    uprobe_log_level--;
                break;

            case OPT_TS:
                ts = true;
                break;

            case OPT_REFRAME:
                second_framer = true;
                break;

            default:
                fprintf(stderr, "unknown option -%c\n", opt);
                break;
        }
    }

    assert(optind + 2 <= argc);
    const char *source = argv[optind];
    const char *framer = argv[optind + 1];
    const char *id = "auto";
    if (optind + 3 <= argc)
        id = argv[optind + 2];

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

    if (ts) {
        enum uprobe_selflow_type type = UPROBE_SELFLOW_VOID;
        if (!strcmp(framer, "sound"))
            type = UPROBE_SELFLOW_SOUND;
        else if (!strcmp(framer, "video"))
            type = UPROBE_SELFLOW_PIC;
        else if (!strcmp(framer, "sub"))
            type = UPROBE_SELFLOW_SUBPIC;

        struct upipe_mgr *upipe_ts_demux_mgr = upipe_ts_demux_mgr_alloc();
        struct upipe_mgr *upipe_autof_mgr = upipe_autof_mgr_alloc();
        upipe_ts_demux_mgr_set_autof_mgr(upipe_ts_demux_mgr, upipe_autof_mgr);
        upipe_mgr_release(upipe_autof_mgr);
        struct upipe *demux = upipe_void_alloc_output(
            upipe_src,
            upipe_ts_demux_mgr,
            uprobe_pfx_alloc(
                uprobe_selflow_alloc(
                    uprobe_use(uprobe),
                    uprobe_selflow_alloc(
                        uprobe_use(uprobe),
                        uprobe_alloc(catch_es, uprobe_use(uprobe)),
                        type, id),
                    UPROBE_SELFLOW_VOID, "auto"),
                UPROBE_LOG_VERBOSE, "ts demux"));
        assert(demux != NULL);
        upipe_mgr_release(upipe_ts_demux_mgr);
    }
    else {
        struct upipe_mgr *upipe_framer_mgr = NULL;
        if (!strcmp(framer, "mpga")) {
            upipe_framer_mgr = upipe_mpgaf_mgr_alloc();
        }
        else if (!strcmp(framer, "h264")) {
            upipe_framer_mgr = upipe_h264f_mgr_alloc();
        }
        else if (!strcmp(framer, "dvbsub")) {
            upipe_framer_mgr = upipe_dvbsubf_mgr_alloc();
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
    }

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
