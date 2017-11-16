#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_select_flows.h>
#include <upipe/uprobe_ubuf_mem_pool.h>
#include <upipe/uprobe_uclock.h>

#include <upipe/umem_pool.h>
#include <upipe/udict_inline.h>
#include <upipe/uref_std.h>
#include <upipe/uclock.h>
#include <upipe/uclock_std.h>

#include <upump-ev/upump_ev.h>

#include <upipe-modules/upipe_file_source.h>
#include <upipe-modules/upipe_file_sink.h>
#include <upipe-modules/upipe_setflowdef.h>

#include <upipe-ts/upipe_ts_demux.h>
#include <upipe-ts/upipe_ts_mux.h>

#include <upipe-framers/upipe_auto_framer.h>

#include <assert.h>

#define UMEM_POOL               512
#define UDICT_POOL_DEPTH        500
#define UREF_POOL_DEPTH         500
#define UBUF_POOL_DEPTH         3000
#define UBUF_SHARED_POOL_DEPTH  50
#define UPUMP_POOL              10
#define UPUMP_BLOCKER_POOL      10
#define XFER_QUEUE              255
#define XFER_POOL               20
#define FSRC_OUT_QUEUE_LENGTH   5
#define SRC_OUT_QUEUE_LENGTH    10000
#define DEC_IN_QUEUE_LENGTH     25
#define DEC_OUT_QUEUE_LENGTH    5
#define SOUND_QUEUE_LENGTH      10
#define PADDING_OCTETRATE               128000

int loglevel = UPROBE_LOG_DEBUG;
static struct uref_mgr *uref_mgr = NULL;
static struct uprobe *uprobe = NULL;
static struct upipe *upipe_fsrc = NULL;
static struct upipe *upipe_ts_mux = NULL;

static int catch_fsrc(struct uprobe *uprobe, struct upipe *upipe,
                      int event, va_list args)
{
    switch (event) {
        case UPROBE_SOURCE_END:
            upipe_release(upipe_fsrc);
            upipe_fsrc = NULL;
            upipe_release(upipe_ts_mux);
            upipe_ts_mux = NULL;
            return UBASE_ERR_NONE;
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

static int catch_video(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    switch (event) {
        case UPROBE_NEED_OUTPUT: {
            struct upipe_mgr *upipe_setflowdef_mgr =
                upipe_setflowdef_mgr_alloc();
            assert(upipe_setflowdef_mgr);
            struct upipe *upipe_setflowdef =
                upipe_void_alloc_output(
                    upipe, upipe_setflowdef_mgr,
                    uprobe_pfx_alloc(uprobe_use(uprobe),
                                     loglevel, "setflowdef"));
            assert(upipe_setflowdef);
            upipe_mgr_release(upipe_setflowdef_mgr);

            struct uref *flow_def = uref_alloc_control(uref_mgr);
            assert(flow_def != NULL);
            uref_ts_flow_set_pid(flow_def, 257);
            upipe_setflowdef_set_dict(upipe_setflowdef, flow_def);
            uref_free(flow_def);

            struct upipe *mux_input =
                upipe_void_chain_output_sub(
                    upipe_setflowdef, upipe_ts_mux,
                    uprobe_pfx_alloc(uprobe_use(uprobe),
                                     loglevel, "mux pic"));
            assert(mux_input);
            upipe_release(mux_input);
            return UBASE_ERR_NONE;
        }
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

static int catch_audio(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    switch (event) {
        case UPROBE_NEED_OUTPUT: {
            struct upipe_mgr *upipe_setflowdef_mgr =
                upipe_setflowdef_mgr_alloc();
            assert(upipe_setflowdef_mgr);
            struct upipe *upipe_setflowdef =
                upipe_void_alloc_output(
                    upipe, upipe_setflowdef_mgr,
                    uprobe_pfx_alloc(uprobe_use(uprobe),
                                     loglevel, "setflowdef"));
            assert(upipe_setflowdef);
            upipe_mgr_release(upipe_setflowdef_mgr);

            struct uref *flow_def = uref_alloc_control(uref_mgr);
            assert(flow_def != NULL);
            uref_ts_flow_set_pid(flow_def, 258);
            upipe_setflowdef_set_dict(upipe_setflowdef, flow_def);
            uref_free(flow_def);

            struct upipe *mux_input =
                upipe_void_chain_output_sub(
                    upipe_setflowdef, upipe_ts_mux,
                    uprobe_pfx_alloc(uprobe_use(uprobe),
                                     loglevel, "mux pic"));
            assert(mux_input);
            upipe_release(mux_input);
            return UBASE_ERR_NONE;
        }
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

static int catch_sub(struct uprobe *uprobe, struct upipe *upipe,
                     int event, va_list args)
{
    return uprobe_throw_next(uprobe, upipe, event, args);
}

int main(int argc, char *argv[])
{
    assert(argc > 2);
    const char *in = argv[1];
    const char *out = argv[2];

    struct upump_mgr *upump_mgr =
        upump_ev_mgr_alloc_default(UPUMP_POOL, UPUMP_BLOCKER_POOL);
    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    struct udict_mgr *udict_mgr =
        udict_inline_mgr_alloc(UDICT_POOL_DEPTH, umem_mgr, -1, -1);
    uref_mgr =
        uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);

    uprobe = uprobe_stdio_alloc(NULL, stderr, loglevel);
    assert(uprobe);
    uprobe = uprobe_uref_mgr_alloc(uprobe, uref_mgr);
    assert(uprobe);
    uprobe = uprobe_upump_mgr_alloc(uprobe, upump_mgr);
    assert(uprobe);
    uprobe = uprobe_ubuf_mem_pool_alloc(uprobe, umem_mgr,
            UBUF_POOL_DEPTH, UBUF_SHARED_POOL_DEPTH);
    assert(uprobe);
    struct uclock *uclock = uclock_std_alloc(0);
    assert(uclock);
    uprobe = uprobe_uclock_alloc(uprobe, uclock);
    assert(uprobe);
    uclock_release(uclock);

    struct uprobe uprobe_fsrc;
    struct uprobe uprobe_video;
    struct uprobe uprobe_audio;
    struct uprobe uprobe_sub;

    uprobe_init(&uprobe_fsrc, catch_fsrc, uprobe_use(uprobe));
    uprobe_init(&uprobe_video, catch_video, uprobe_use(uprobe));
    uprobe_init(&uprobe_audio, catch_audio, uprobe_use(uprobe));
    uprobe_init(&uprobe_sub, catch_sub, uprobe_use(uprobe));

    struct upipe_mgr *upipe_fsrc_mgr = upipe_fsrc_mgr_alloc();
    assert(upipe_fsrc_mgr);
    upipe_fsrc =
        upipe_void_alloc(upipe_fsrc_mgr,
                         uprobe_pfx_alloc(uprobe_use(&uprobe_fsrc),
                                          loglevel, "fsrc"));
    assert(upipe_fsrc);
    upipe_mgr_release(upipe_fsrc_mgr);
    ubase_assert(upipe_set_uri(upipe_fsrc, in));

    /* ts demux */
    struct upipe_mgr *upipe_ts_demux_mgr = upipe_ts_demux_mgr_alloc();
    struct upipe_mgr *upipe_autof_mgr = upipe_autof_mgr_alloc();
    upipe_ts_demux_mgr_set_autof_mgr(upipe_ts_demux_mgr, upipe_autof_mgr);
    upipe_mgr_release(upipe_autof_mgr);
    struct upipe *upipe_ts_demux = upipe_void_alloc_output(upipe_fsrc,
            upipe_ts_demux_mgr,
            uprobe_pfx_alloc(
                uprobe_selflow_alloc(uprobe_use(uprobe),
                    uprobe_selflow_alloc(
                        uprobe_selflow_alloc(
                            uprobe_selflow_alloc(uprobe_use(uprobe),
                                uprobe_use(&uprobe_video),
                                UPROBE_SELFLOW_PIC, "auto"),
                            uprobe_use(&uprobe_sub),
                            UPROBE_SELFLOW_SUBPIC, "auto"),
                        uprobe_use(&uprobe_audio),
                        UPROBE_SELFLOW_SOUND, "auto"),
                    UPROBE_SELFLOW_VOID, "auto"),
                UPROBE_LOG_VERBOSE, "demux"));
    upipe_release(upipe_ts_demux);
    upipe_mgr_release(upipe_ts_demux_mgr);

    struct upipe_mgr *upipe_ts_mux_mgr = upipe_ts_mux_mgr_alloc();
    assert(upipe_ts_mux_mgr);
    upipe_ts_mux =
        upipe_void_alloc(upipe_ts_mux_mgr,
                         uprobe_pfx_alloc(uprobe_use(uprobe),
                                          loglevel, "mux"));
    assert(upipe_ts_mux);
    upipe_ts_mux_set_mode(upipe_ts_mux, UPIPE_TS_MUX_MODE_CAPPED);
    upipe_ts_mux_set_padding_octetrate(upipe_ts_mux, PADDING_OCTETRATE);
    upipe_attach_uclock(upipe_ts_mux);
    upipe_mgr_release(upipe_ts_mux_mgr);

    struct upipe_mgr *upipe_fsink_mgr = upipe_fsink_mgr_alloc();
    assert(upipe_fsink_mgr);
    struct upipe *upipe_fsink =
        upipe_void_alloc_output(upipe_ts_mux,
                                upipe_fsink_mgr,
                                uprobe_pfx_alloc(uprobe_use(uprobe),
                                                 loglevel, "fsink"));
    assert(upipe_fsink);
    upipe_mgr_release(upipe_fsink_mgr);
    ubase_assert(upipe_fsink_set_path(upipe_fsink, out,
                                      UPIPE_FSINK_OVERWRITE));
    upipe_release(upipe_fsink);

    struct uref *flow_def = uref_alloc_control(uref_mgr);
    uref_flow_set_def(flow_def, "void.");
    upipe_set_flow_def(upipe_ts_mux, flow_def);
    uref_free(flow_def);

    flow_def = uref_alloc_control(uref_mgr);
    uref_flow_set_def(flow_def, "void.");
    upipe_ts_mux = upipe_void_chain_sub(
        upipe_ts_mux,
        uprobe_pfx_alloc(uprobe_use(uprobe),
                         UPROBE_LOG_VERBOSE, "prog"));
    assert(upipe_ts_mux);
    uref_flow_set_id(flow_def, 1);
    uref_ts_flow_set_pid(flow_def, 256);
    upipe_set_flow_def(upipe_ts_mux, flow_def);
    uref_free(flow_def);

    /* main loop */
    upump_mgr_run(upump_mgr, NULL);

    upipe_release(upipe_ts_mux);
    upipe_release(upipe_fsrc);
    uprobe_release(uprobe);
    upump_mgr_release(upump_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);

    return 0;
}
