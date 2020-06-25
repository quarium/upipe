/*
 * Copyright (C) 2020 EasyTools
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
#include <upipe-framers/upipe_dvbsub_framer.h>
#include <upump-ev/upump_ev.h>

#include <bitstream/mpeg/ts.h>

#include <limits.h>
#include <getopt.h>

#define MAX_PIDS        8192
#define PID_ALL         UINT16_MAX
#define STRINGIFY_(X)   # X
#define STRINGIFY(X)    STRINGIFY_(X)

struct uprobe_uref {
    struct uprobe uprobe;
    struct upipe *pids[MAX_PIDS];
    struct upipe *upipe_ts_split;
};

UBASE_FROM_TO(uprobe_uref, uprobe, uprobe, uprobe);

struct es {
    struct uchain uchain;
    uint16_t pid;
    bool enabled;
    bool ts_decaps;
    bool es_decaps;
    char *file_sink;
    char *framer;
};

UBASE_FROM_TO(es, uchain, uchain, uchain);

enum opt {
    OPT_HELP,
    OPT_VERBOSE,
    OPT_QUIET,
    OPT_NONE,
    OPT_ADD,
    OPT_DEL,
    OPT_FILE_SINK,
    OPT_TS_DECAPS,
    OPT_ES_DECAPS,
    OPT_FRAMER,
};

static const struct option options[] = {
    { "help", no_argument, NULL, OPT_HELP },
    { "verbose", no_argument, NULL, OPT_VERBOSE },
    { "quiet", no_argument, NULL, OPT_QUIET },
    { "none", no_argument, NULL, OPT_NONE },
    { "add", required_argument, NULL, OPT_ADD },
    { "del", required_argument, NULL, OPT_DEL },
    { "file-sink", required_argument, NULL, OPT_FILE_SINK },
    { "ts-decaps", no_argument, NULL, OPT_TS_DECAPS },
    { "es-decaps", no_argument, NULL, OPT_ES_DECAPS },
    { "framer", required_argument, NULL, OPT_FRAMER },
    { 0, 0, 0, 0 },
};

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [options] source.ts\n", name);
    fprintf(stderr, "   --help: print this help\n");
    fprintf(stderr, "   --verbose: be more verbose\n");
    fprintf(stderr, "   --quiet: be more quiet\n");
    fprintf(stderr, "   --none: unselect all pids\n");
    fprintf(stderr, "   --add pid: select pid\n");
    fprintf(stderr, "   --del pid: unselect pid\n");
    fprintf(stderr, "   --file-sink: use file sink\n");
    fprintf(stderr, "   --ts-decaps: decaps TS\n");
    fprintf(stderr, "   --es-decaps: decaps PES\n");
    fprintf(stderr, "   --framer framer: use framer\n");
}

static enum uprobe_log_level uprobe_log_level = UPROBE_LOG_NOTICE;
static bool new_pids[MAX_PIDS];
static struct uchain es_list;
static struct uref_mgr *uref_mgr = NULL;

static struct es *es_find_by_pid(uint16_t pid)
{
    struct uchain *uchain;
    ulist_foreach(&es_list, uchain) {
        struct es *es = es_from_uchain(uchain);
        if (es->pid == pid)
            return es;
    }
    return NULL;
}

static struct es *es_current(void)
{
    struct uchain *uchain = ulist_last(&es_list);
    return uchain ? es_from_uchain(uchain) : NULL;
}

static struct es *es_add(uint16_t pid)
{
    struct es *es = es_find_by_pid(pid);
    if (es)
        return es;

    es = malloc(sizeof (*es));
    es->file_sink = NULL;
    es->ts_decaps = false;
    es->es_decaps = false;
    es->enabled = true;
    es->framer = NULL;
    es->pid = pid;
    ulist_add(&es_list, es_to_uchain(es));
    return es;
}

static struct es *es_del(uint16_t pid)
{
    struct es *es = es_add(pid);
    es->enabled = false;
    return es;
}

static void es_clean(struct es *es)
{
    ulist_delete(es_to_uchain(es));
    free(es->file_sink);
    free(es->framer);
    free(es);
}

static void es_list_init(void)
{
    ulist_init(&es_list);
}

static void es_list_clean(void)
{
    struct uchain *uchain;
    while ((uchain = ulist_pop(&es_list))) {
        struct es *es = es_from_uchain(uchain);
        ulist_init(&es->uchain);
        es_clean(es);
    }
}

static int catch_uref(struct uprobe *uprobe, struct upipe *upipe,
                      int event, va_list args)
{
    if (event != UPROBE_PROBE_UREF ||
        ubase_get_signature(args) != UPIPE_PROBE_UREF_SIGNATURE)
        return uprobe_throw_next(uprobe, upipe, event, args);

    UBASE_SIGNATURE_CHECK(args, UPIPE_PROBE_UREF_SIGNATURE);
    struct uref *uref = va_arg(args, struct uref *);

    uint8_t buffer[TS_HEADER_SIZE];
    const uint8_t *ts_header =
        uref_block_peek(uref, 0, TS_HEADER_SIZE, buffer);
    if (unlikely(ts_header == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    uint16_t pid = ts_get_pid(ts_header);
    UBASE_FATAL(upipe, uref_block_peek_unmap(uref, 0, buffer, ts_header));

    if (pid >= MAX_PIDS) {
        upipe_warn_va(upipe, "invalid PID %u", pid);
        return UBASE_ERR_INVALID;
    }

    if (new_pids[pid])
        upipe_notice_va(upipe, "new pid %u", pid);
    new_pids[pid] = false;

    struct uprobe_uref *uprobe_uref = uprobe_uref_from_uprobe(uprobe);
    if (uprobe_uref->pids[pid])
        return UBASE_ERR_NONE;

    struct es *es = es_find_by_pid(pid);
    struct es *all = es_find_by_pid(PID_ALL);

    if (!es) {
        if (!all || !all->enabled)
            return UBASE_ERR_NONE;

        es = es_add(pid);
        if (!es)
            return UBASE_ERR_NONE;
    }
    else if (!es->enabled)
        return UBASE_ERR_NONE;

    if (!es->ts_decaps && all->ts_decaps)
        es->ts_decaps = all->ts_decaps;
    if (!es->es_decaps && all->es_decaps)
        es->es_decaps = all->es_decaps;
    if (!es->file_sink && all->file_sink) {
        char buffer[strlen(all->file_sink) + strlen(STRINGIFY(MAX_PIDS)) + 1];
        char *ext = strrchr(all->file_sink, '.');
        if (!ext)
            ext = all->file_sink + strlen(all->file_sink);
        int ret = snprintf(buffer, sizeof (buffer), "%.*s%u%s",
                           (int)(ext - all->file_sink), all->file_sink,
                           pid, ext);
        assert(ret > 0 && ret < sizeof (buffer));
        es->file_sink = strdup(buffer);
    }
    if (!es->framer && all->framer)
        es->framer = strdup(all->framer);

    struct uref *flow_def = uref_sibling_alloc_control(uref);
    assert(flow_def);
    uref_flow_set_def(flow_def, "block.mpegts.");
    uref_ts_flow_set_pid(flow_def, pid);

    uprobe_uref->pids[pid] = upipe_flow_alloc_sub(
        uprobe_uref->upipe_ts_split,
        uprobe_pfx_alloc_va(
            uprobe_use(uprobe->next),
            UPROBE_LOG_VERBOSE, "split %u", pid),
        flow_def);
    assert(uprobe_uref->pids[pid]);

    struct upipe *chain = upipe_use(uprobe_uref->pids[pid]);

    if (es->ts_decaps || es->es_decaps || es->framer) {
        struct upipe_mgr *upipe_ts_decaps_mgr = upipe_ts_decaps_mgr_alloc();
        assert(upipe_ts_decaps_mgr);
        chain = upipe_void_chain_output(
            chain, upipe_ts_decaps_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "tsd %u", pid));
        assert(chain);
        upipe_mgr_release(upipe_ts_decaps_mgr);
    }

    if (es->es_decaps || es->framer) {
        struct upipe_mgr *upipe_setflowdef_mgr = upipe_setflowdef_mgr_alloc();
        assert(upipe_setflowdef_mgr);
        chain = upipe_void_chain_output(
            chain, upipe_setflowdef_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "setflowdef %u", pid));
        assert(chain);
        upipe_mgr_release(upipe_setflowdef_mgr);
        uref_flow_set_def(flow_def, "block.mpegtspes.");
        ubase_assert(upipe_setflowdef_set_dict(chain, flow_def));

        struct upipe_mgr *upipe_ts_pesd_mgr = upipe_ts_pesd_mgr_alloc();
        assert(upipe_ts_pesd_mgr);
        chain = upipe_void_chain_output(
            chain, upipe_ts_pesd_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "pesd %u", pid));
        assert(chain);
        upipe_mgr_release(upipe_ts_pesd_mgr);
    }

    if (es->framer) {
        struct upipe_mgr *upipe_framer_mgr = NULL;
        if (!strcmp(es->framer, "dvbsub")) {
            upipe_framer_mgr  = upipe_dvbsubf_mgr_alloc();
        }

        if (!upipe_framer_mgr) {
            upipe_warn_va(upipe, "no such framer %s", es->framer);
            return UBASE_ERR_INVALID;
        }

        chain = upipe_void_chain_output(
            chain, upipe_framer_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "%s %u", es->framer, pid));
        assert(chain);
        upipe_mgr_release(upipe_framer_mgr);
    }

    if (es->file_sink) {
        struct upipe_mgr *upipe_fsink_mgr = upipe_fsink_mgr_alloc();
        assert(upipe_fsink_mgr);
        chain = upipe_void_chain_output(
            chain, upipe_fsink_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "fsink %u", pid));
        assert(chain);
        upipe_mgr_release(upipe_fsink_mgr);
        ubase_assert(upipe_fsink_set_path(chain, es->file_sink, UPIPE_FSINK_CREATE));
    }
    else {
        struct upipe_mgr *upipe_null_mgr = upipe_null_mgr_alloc();
        assert(upipe_null_mgr);
        chain = upipe_void_chain_output(
            chain, upipe_null_mgr,
            uprobe_pfx_alloc_va(
                uprobe_use(uprobe->next),
                UPROBE_LOG_VERBOSE, "null %u", pid));
        assert(chain);;
        upipe_mgr_release(upipe_null_mgr);
    }

    upipe_release(chain);
    uref_free(flow_def);

    return UBASE_ERR_NONE;
}

static void uprobe_uref_init(struct uprobe_uref *uprobe_uref,
                             struct uprobe *next)
{
    for (unsigned i = 0; i < MAX_PIDS; i++)
        uprobe_uref->pids[i] = NULL;
    uprobe_uref->upipe_ts_split = NULL;
    uprobe_init(&uprobe_uref->uprobe, catch_uref, next);
}

static void uprobe_uref_clean(struct uprobe_uref *uprobe_uref)
{
    for (unsigned i = 0; i < MAX_PIDS; i++)
        upipe_release(uprobe_uref->pids[i]);
    upipe_release(uprobe_uref->upipe_ts_split);
    uprobe_clean(&uprobe_uref->uprobe);
}

static int catch_pid(struct uprobe *uprobe, struct upipe *upipe,
                     int event, va_list args)
{
    if ((event != UPROBE_TS_SPLIT_ADD_PID &&
         event != UPROBE_TS_SPLIT_DEL_PID) ||
        ubase_get_signature(args) != UPIPE_TS_SPLIT_SIGNATURE)
        return uprobe_throw_next(uprobe, upipe, event, args);

    UBASE_SIGNATURE_CHECK(args, UPIPE_TS_SPLIT_SIGNATURE);
    unsigned int pid = va_arg(args, unsigned int);

    switch (event) {
        case UPROBE_TS_SPLIT_ADD_PID:
            upipe_notice_va(upipe, "add pid %u", pid);
            break;
        case UPROBE_TS_SPLIT_DEL_PID:
            upipe_notice_va(upipe, "del pid %u", pid);
            break;
    }

    return UBASE_ERR_NONE;
}

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
    const char *name = argv[0];
    for (unsigned i = 0; i < MAX_PIDS; i++)
        new_pids[i] = true;

    es_list_init();
    es_add(PID_ALL);

    int c;
    while ((c = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (c) {
            case OPT_HELP:
                usage(name);
                exit(0);
                break;
            case OPT_VERBOSE:
                switch (uprobe_log_level) {
                    case UPROBE_LOG_ERROR:
                        uprobe_log_level = UPROBE_LOG_WARNING;
                        break;
                    case UPROBE_LOG_WARNING:
                        uprobe_log_level = UPROBE_LOG_NOTICE;
                        break;
                    case UPROBE_LOG_NOTICE:
                        uprobe_log_level = UPROBE_LOG_DEBUG;
                        break;
                    default:
                        uprobe_log_level = UPROBE_LOG_VERBOSE;
                }
                break;
            case OPT_QUIET:
                switch (uprobe_log_level) {
                    case UPROBE_LOG_NOTICE:
                        uprobe_log_level = UPROBE_LOG_WARNING;
                        break;
                    case UPROBE_LOG_DEBUG:
                        uprobe_log_level = UPROBE_LOG_NOTICE;
                        break;
                    case UPROBE_LOG_VERBOSE:
                        uprobe_log_level = UPROBE_LOG_DEBUG;
                        break;
                    default:
                        uprobe_log_level = UPROBE_LOG_ERROR;
                }
                break;
            case OPT_NONE:
                es_del(PID_ALL);
                break;
            case OPT_ADD: {
                unsigned long pid = strtoul(optarg, NULL, 0);
                if (pid >= MAX_PIDS)
                    fprintf(stderr, "invalid pid %s\n", optarg);
                else
                    es_add(pid);
                break;
            }
            case OPT_DEL: {
                unsigned long pid = strtoul(optarg, NULL, 0);
                if (pid >= MAX_PIDS)
                    fprintf(stderr, "invalid pid %s\n", optarg);
                else
                    es_del(pid);
                break;
            }
            case OPT_FILE_SINK: {
                struct es *es = es_current();
                if (es) {
                    free(es->file_sink);
                    es->file_sink = strdup(optarg);
                }
                break;
            }
            case OPT_TS_DECAPS: {
                struct es *es = es_current();
                if (es)
                    es->ts_decaps = true;
                break;
            }
            case OPT_ES_DECAPS: {
                struct es *es = es_current();
                if (es)
                    es->es_decaps = true;
                break;
            }

            case OPT_FRAMER: {
                struct es *es = es_current();
                if (es)
                    es->framer = strdup(optarg);
                break;
            }

            default:
                usage(name);
                exit(-1);
        }
    }

    if (optind >= argc) {
        usage(name);
        exit(-1);
    }

    /* get options and arguments */
    const char *source = NULL;
    source = argv[optind];

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

    struct uprobe_uref uprobe_uref;
    uprobe_uref_init(&uprobe_uref, uprobe_use(uprobe));
    struct uprobe uprobe_pid;
    uprobe_init(&uprobe_pid, catch_pid, uprobe_use(uprobe));

    /* create file source */
    struct upipe *upipe_src = upipe_source_alloc(source, uprobe);

    /* create ts sync */
    struct upipe_mgr *upipe_ts_sync_mgr = upipe_ts_sync_mgr_alloc();
    assert(upipe_ts_sync_mgr);
    struct upipe *upipe_ts_sync = upipe_void_alloc(
        upipe_ts_sync_mgr,
        uprobe_pfx_alloc(
            uprobe_use(uprobe),
            UPROBE_LOG_VERBOSE, "ts_sync"));
    assert(upipe_ts_sync);
    upipe_mgr_release(upipe_ts_sync_mgr);

    /* create uref probe */
    struct upipe_mgr *upipe_probe_uref_mgr = upipe_probe_uref_mgr_alloc();
    assert(upipe_probe_uref_mgr);
    struct upipe *upipe_probe_uref = upipe_void_alloc(
        upipe_probe_uref_mgr,
        uprobe_pfx_alloc(
            uprobe_use(&uprobe_uref.uprobe),
            UPROBE_LOG_VERBOSE, "probe"));
    assert(upipe_probe_uref_mgr);
    upipe_mgr_release(upipe_probe_uref_mgr);

    /* create ts PID split */
    struct upipe_mgr *upipe_ts_split_mgr = upipe_ts_split_mgr_alloc();
    assert(upipe_ts_split_mgr);
    struct upipe *upipe_ts_split = upipe_void_alloc(
        upipe_ts_split_mgr,
        uprobe_pfx_alloc(
            uprobe_use(&uprobe_pid),
            UPROBE_LOG_VERBOSE, "ts_split"));
    assert(upipe_ts_split);
    upipe_mgr_release(upipe_ts_split_mgr);

    uprobe_uref.upipe_ts_split = upipe_use(upipe_ts_split);
    upipe_set_output(upipe_src, upipe_ts_sync);
    upipe_set_output(upipe_ts_sync, upipe_probe_uref);
    upipe_set_output(upipe_probe_uref, upipe_ts_split);

    /* main loop */
    upump_mgr_run(upump_mgr, NULL);

    /* release probes, pipes and managers */
    uprobe_clean(&uprobe_pid);
    uprobe_uref_clean(&uprobe_uref);
    uprobe_release(uprobe);
    upipe_release(upipe_ts_split);
    upipe_release(upipe_probe_uref);
    upipe_release(upipe_ts_sync);
    upipe_release(upipe_src);
    uref_mgr_release(uref_mgr);
    umem_mgr_release(umem_mgr);
    upump_mgr_release(upump_mgr);

    es_list_clean();

    return 0;
}
