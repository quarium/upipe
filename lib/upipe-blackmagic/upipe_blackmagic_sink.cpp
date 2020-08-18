/*
 * Copyright (C) 2014-2016 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya
 *          Rafaël Carré
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */

/** @file
 * @short Upipe bmd_sink module
 */

#define __STDC_LIMIT_MACROS    1
#define __STDC_FORMAT_MACROS   1
#define __STDC_CONSTANT_MACROS 1

#include <upipe/config.h>
#include <upipe/ubase.h>
#include <upipe/uatomic.h>
#include <upipe/ulist.h>
#include <upipe/uqueue.h>
#include <upipe/uprobe.h>
#include <upipe/uclock.h>
#include <upipe/uref.h>
#include <upipe/uref_attr.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic_flow_formats.h>
#include <upipe/uref_sound.h>
#include <upipe/uref_sound_flow.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_dump.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_input.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe-blackmagic/upipe_blackmagic_sink.h>

#include <arpa/inet.h>
#include <assert.h>

#ifdef UPIPE_HAVE_LIBZVBI_H
#include <libzvbi.h>
#endif

#include <pthread.h>

#include <bitstream/smpte/337.h>
#include <bitstream/dvb/vbi.h>

#include "include/DeckLinkAPI.h"
#include "uclock_blackmagic_sink.h"

extern "C" {
    #include "sdi.h"
}

/** minimum number of preroll frame for pre 4k devices requirement */
#define PREROLL_FRAMES 3

#define DECKLINK_CHANNELS 16

#define PRINT_PERIODICITY (10 * UCLOCK_FREQ)

static int _upipe_bmd_sink_set_timing_adjustment(struct upipe *upipe, int64_t adj);
static int _upipe_bmd_sink_adjust_timing(struct upipe *upipe, int64_t adj);

static const unsigned max_samples = (uint64_t)48000 * 1001 / 24000;
static const size_t audio_buf_size = max_samples * DECKLINK_CHANNELS * sizeof(int32_t);

class upipe_bmd_sink_frame : public IDeckLinkVideoFrame
{
public:
    upipe_bmd_sink_frame(struct uref *_uref, void *_buffer,
                         long _width, long _height, size_t _stride) :
        uref(_uref), data(_buffer),
        width(_width), height(_height), stride(_stride) {
        uatomic_store(&refcount, 1);
    }

    ~upipe_bmd_sink_frame(void) {
        uatomic_clean(&refcount);
        uref_pic_plane_unmap(uref, "u10y10v10y10u10y10v10y10u10y10v10y10", 0, 0, -1, -1);
        uref_free(uref);
    }

    virtual long STDMETHODCALLTYPE GetWidth(void) {
        return width;
    }

    virtual long STDMETHODCALLTYPE GetHeight(void) {
        return height;
    }

    virtual long STDMETHODCALLTYPE GetRowBytes(void) {
        return stride;
    }

    virtual BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat(void) {
        return bmdFormat10BitYUV;
    }

    virtual BMDFrameFlags STDMETHODCALLTYPE GetFlags(void) {
        return bmdVideoOutputFlagDefault;
    }

    virtual HRESULT STDMETHODCALLTYPE GetBytes(void **buffer) {
        *buffer = data;
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE GetTimecode(BMDTimecodeFormat format,
                                                  IDeckLinkTimecode **timecode) {
        *timecode = NULL;
        return S_FALSE;
    }

    virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary) {
        frame_anc->AddRef();
        *ancillary = frame_anc;
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary *ancillary) {
        frame_anc = ancillary;
        return S_OK;
    }

    virtual ULONG STDMETHODCALLTYPE AddRef(void) {
        frame_anc->AddRef();
        return uatomic_fetch_add(&refcount, 1) + 1;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void) {
        frame_anc->Release();
        uint32_t new_ref = uatomic_fetch_sub(&refcount, 1) - 1;
        if (new_ref == 0)
            delete this;
        return new_ref;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {
        return E_NOINTERFACE;
    }

private:
    struct uref *uref;
    void *data;
    long width;
    long height;
    size_t stride;

    uatomic_uint32_t refcount;
    IDeckLinkVideoFrameAncillary *frame_anc;
};

/** @internal @This is the private context of an output of an bmd_sink sink
 * pipe. */
struct upipe_bmd_sink_sub {
    struct urefcount urefcount;

    struct upipe *upipe_bmd_sink;

    /** thread-safe urefs queue */
    struct uqueue uqueue;
    void *uqueue_extra;

    struct uref *uref;

    /** structure for double-linked lists */
    struct uchain uchain;

    /** delay applied to pts attribute when uclock is provided */
    uint64_t latency;

    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** watcher */
    struct upump *upump;

    /** whether this is an audio pipe */
    bool sound;

    bool dolby_e;

    bool s337;

    /** number of channels */
    uint8_t channels;

    /** position in the SDI stream */
    uint8_t channel_idx;

    /** buffered input urefs */
    struct uchain urefs;
    /** number of buffered urefs */
    unsigned nb_urefs;
    /** maximum number of buffered urefs before blocking */
    unsigned max_urefs;
    /** blockers */
    struct uchain blockers;

    /** public upipe structure */
    struct upipe upipe;
};

class callback;

/** upipe_bmd_sink structure */
struct upipe_bmd_sink {
    /** refcount management structure */
    struct urefcount urefcount;

    /** manager to create subs */
    struct upipe_mgr sub_mgr;
    /** pic subpipe */
    struct upipe_bmd_sink_sub pic_subpipe;
    /** subpic subpipe */
    struct upipe_bmd_sink_sub subpic_subpipe;

    /** list of input subpipes */
    struct uchain inputs;

    /** lock the list of subpipes, they are iterated from the
     * decklink callback */
    pthread_mutex_t lock;

    /** card index **/
    int card_idx;
    /** card topology */
    int64_t card_topo;

    /** selected output mode */
    BMDDisplayMode selectedMode;
    /** output mode **/
    BMDDisplayMode mode;
    /** support timing adjutment */
    bool timing_adjustment_support;

    /** video frame index (modulo 5) */
    uint8_t frame_idx;

    uint64_t start_pts;
    uatomic_uint32_t preroll;
    /** available frame for prerolling */
    unsigned frames;

    /** vanc/vbi temporary buffer **/

    /** closed captioning **/
    uint16_t cdp_hdr_sequence_cntr;

    /** OP47 teletext sequence counter **/
    // XXX: should counter be per-field?
    uint16_t op47_sequence_counter[2];

#ifdef UPIPE_HAVE_LIBZVBI_H
    /** vbi **/
    vbi_sampling_par sp;
#endif

    /** handle to decklink card */
    IDeckLink *deckLink;
    /** handle to decklink card output */
    IDeckLinkOutput *deckLinkOutput;

    IDeckLinkDisplayMode *displayMode;

    /** card name */
    const char *modelName;

    /** hardware uclock */
    struct uclock *uclock;
    /** system clock */
    struct uclock *uclock_std;
    /** system clock request */
    struct urequest uclock_request;

    /** genlock status */
    int genlock_status;

    /** time at which we got genlock */
    uint64_t genlock_transition_time;

    /** frame duration */
    uint64_t ticks_per_frame;

    /** public upipe structure */
    struct upipe upipe;

    /** Frame completion callback */
    callback *cb;

    /** audio buffer to merge tracks */
    int32_t *audio_buf;

    /** offset between audio sample 0 and dolby e first sample*/
    uint8_t dolbye_offset;

    /** pass through closed captions */
    uatomic_uint32_t cc;

    /** pass through teletext */
    uatomic_uint32_t ttx;

    /** last frame output */
    upipe_bmd_sink_frame *video_frame;

    /** upump manager */
    struct upump_mgr *upump_mgr;

    /** start timer */
    struct upump *timer;

    /** is opened? */
    bool opened;

    int64_t mean_diff;
    uint64_t count;
    uint64_t last_pts;
    int64_t timing_adjustment;
    uint64_t last_print;

};

/** @hidden */
static bool upipe_bmd_sink_sub_output(struct upipe *upipe,
                                      struct uref *uref,
                                      struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_bmd_sink, upipe, UPIPE_BMD_SINK_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_bmd_sink, urefcount, upipe_bmd_sink_free);
UPIPE_HELPER_UCLOCK(upipe_bmd_sink, uclock_std, uclock_request,
                    NULL, upipe_throw_provide_request, NULL);
UPIPE_HELPER_UPUMP_MGR(upipe_bmd_sink, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_bmd_sink, timer, upump_mgr);

UPIPE_HELPER_UPIPE(upipe_bmd_sink_sub, upipe, UPIPE_BMD_SINK_INPUT_SIGNATURE)
UPIPE_HELPER_UPUMP_MGR(upipe_bmd_sink_sub, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_bmd_sink_sub, upump, upump_mgr);
UPIPE_HELPER_FLOW(upipe_bmd_sink_sub, NULL);
UPIPE_HELPER_SUBPIPE(upipe_bmd_sink, upipe_bmd_sink_sub, input, sub_mgr, inputs, uchain)
UPIPE_HELPER_UREFCOUNT(upipe_bmd_sink_sub, urefcount, upipe_bmd_sink_sub_free);
UPIPE_HELPER_INPUT(upipe_bmd_sink_sub, urefs, nb_urefs, max_urefs, blockers,
                   upipe_bmd_sink_sub_output);

UBASE_FROM_TO(upipe_bmd_sink, upipe_bmd_sink_sub, pic_subpipe, pic_subpipe)
UBASE_FROM_TO(upipe_bmd_sink, upipe_bmd_sink_sub, subpic_subpipe, subpic_subpipe)

static void uqueue_uref_flush(struct uqueue *uqueue)
{
    for (;;) {
        struct uref *uref = uqueue_pop(uqueue, struct uref *);
        if (!uref)
            break;
        uref_free(uref);
    }
}

static int upipe_bmd_open_vid(struct upipe *upipe);
static void output_cb(struct upipe *upipe);

class callback : public IDeckLinkVideoOutputCallback
{
public:
    virtual HRESULT ScheduledFrameCompleted (
        IDeckLinkVideoFrame *frame,
        BMDOutputFrameCompletionResult result) {

        bool prerolling = uatomic_load(&upipe_bmd_sink->preroll);

#if 1
        static const char *result_strs[] = {
            "completed",
            "late",
            "dropped",
            "flushed",
            "?",
        };
        const char *result_str = result_strs[result > 4 ? 4 : result];

        __sync_synchronize();

        void (*print)(struct upipe *, const char *, ...);
        if (result)
            print = upipe_warn_va;
        else
            print = upipe_verbose_va;

        print(&upipe_bmd_sink->upipe,
              "%p Frame %s %s(%.2f ms, %.2f ms - %.2f ms, %.2f ms)",
              frame, result_str,
              prerolling ? "preroll " : "");
#endif

        /* next frame */
        if (!prerolling)
            output_cb(&upipe_bmd_sink->pic_subpipe.upipe);

        return S_OK;
    }

    virtual HRESULT ScheduledPlaybackHasStopped (void)
    {
        return S_OK;
    }

    callback(struct upipe_bmd_sink *upipe_bmd_sink_) {
        upipe_bmd_sink = upipe_bmd_sink_;
        uatomic_store(&refcount, 1);
    }

    ~callback(void) {
        uatomic_clean(&refcount);
    }

    virtual ULONG STDMETHODCALLTYPE AddRef(void) {
        return uatomic_fetch_add(&refcount, 1) + 1;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void) {
        uint32_t new_ref = uatomic_fetch_sub(&refcount, 1) - 1;
        if (new_ref == 0)
            delete this;
        return new_ref;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {
        return E_NOINTERFACE;
    }

private:
    uatomic_uint32_t refcount;
    struct upipe_bmd_sink *upipe_bmd_sink;
    int64_t first_timestamp;
    int64_t prev_timestamp;
};

#ifdef UPIPE_HAVE_LIBZVBI_H
/* VBI Teletext */
static void upipe_bmd_sink_extract_ttx(IDeckLinkVideoFrameAncillary *ancillary,
        const uint8_t *pic_data, size_t pic_data_size, int w, int sd,
        vbi_sampling_par *sp, uint16_t *ctr_array)
{
    const uint8_t *packet[2][5];
    int packets[2] = {0, 0};
    memset(packet, 0, sizeof(packet));

    if (pic_data[0] != DVBVBI_DATA_IDENTIFIER)
        return;

    pic_data++;
    pic_data_size--;

    static const unsigned dvb_unit_size = DVBVBI_UNIT_HEADER_SIZE + DVBVBI_LENGTH;
    for (; pic_data_size >= dvb_unit_size; pic_data += dvb_unit_size, pic_data_size -= dvb_unit_size) {
        uint8_t data_unit_id  = pic_data[0];
        uint8_t data_unit_len = pic_data[1];

        if (data_unit_id != DVBVBI_ID_TTX_SUB && data_unit_id != DVBVBI_ID_TTX_NONSUB)
            continue;

        if (data_unit_len != DVBVBI_LENGTH)
            continue;

        uint8_t line_offset = dvbvbittx_get_line(&pic_data[DVBVBI_UNIT_HEADER_SIZE]);

        uint8_t f2 = !dvbvbittx_get_field(&pic_data[DVBVBI_UNIT_HEADER_SIZE]);
        if (f2 == 0 && line_offset == 0) // line == 0
            continue;

        if (packets[f2] < (sd ? 1 : 5))
            packet[f2][packets[f2]++] = pic_data;
    }

    for (int i = 0; i < 2; i++) {
        if (packets[i] == 0)
            continue;

        if (sd) {
            uint8_t buf[720*2];
            sdi_clear_vbi(buf, 720);

            int line = sdi_encode_ttx_sd(&buf[0], packet[i][0], sp);

            void *vanc;
            ancillary->GetBufferForVerticalBlankingLine(line, &vanc);
            sdi_encode_v210_sd((uint32_t*)vanc, buf, w);
        } else {
            uint16_t buf[VANC_WIDTH*2];

            upipe_sdi_blank_c(buf, VANC_WIDTH);

            /* +1 to destination buffer to write to luma plane */
            sdi_encode_ttx(&buf[1], packets[i], &packet[i][0], &ctr_array[i]);

            void *vanc;
            int line = OP47_LINE1 + 563*i;
            ancillary->GetBufferForVerticalBlankingLine(line, &vanc);
            sdi_encode_v210((uint32_t*)vanc, buf, w);
        }
    }
}
#endif

/** @internal @This initializes an subpipe of a bmd sink pipe.
 *
 * @param upipe pointer to subpipe
 * @param sub_mgr manager of the subpipe
 * @param uprobe structure used to raise events by the subpipe
 */
static void upipe_bmd_sink_sub_init(struct upipe *upipe,
        struct upipe_mgr *sub_mgr, struct uprobe *uprobe, bool static_pipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_sub_mgr(sub_mgr);

    if (static_pipe) {
        upipe_init(upipe, sub_mgr, uprobe);
        /* increment super pipe refcount only when the static pipes are retrieved */
        upipe_mgr_release(sub_mgr);
        upipe->refcount = &upipe_bmd_sink->urefcount;
    } else
        upipe_bmd_sink_sub_init_urefcount(upipe);

    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub = upipe_bmd_sink_sub_from_upipe(upipe);
    upipe_bmd_sink_sub->upipe_bmd_sink = upipe_bmd_sink_to_upipe(upipe_bmd_sink);

    pthread_mutex_lock(&upipe_bmd_sink->lock);
    upipe_bmd_sink_sub_init_sub(upipe);
    upipe_bmd_sink_sub_init_input(upipe);

    static const uint8_t length = PREROLL_FRAMES;
    upipe_bmd_sink_sub->uqueue_extra = malloc(uqueue_sizeof(length));
    assert(upipe_bmd_sink_sub->uqueue_extra);
    uqueue_init(&upipe_bmd_sink_sub->uqueue, length, upipe_bmd_sink_sub->uqueue_extra);
    upipe_bmd_sink_sub->uref = NULL;
    upipe_bmd_sink_sub->latency = 0;
    upipe_bmd_sink_sub_init_upump_mgr(upipe);
    upipe_bmd_sink_sub_init_upump(upipe);
    upipe_bmd_sink_sub->sound = !static_pipe;
    upipe_bmd_sink_sub_set_max_length(upipe, PREROLL_FRAMES);

    upipe_throw_ready(upipe);
    pthread_mutex_unlock(&upipe_bmd_sink->lock);
}

static void upipe_bmd_sink_sub_free(struct upipe *upipe)
{
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub = upipe_bmd_sink_sub_from_upipe(upipe);
    struct upipe_bmd_sink *upipe_bmd_sink =
        upipe_bmd_sink_from_sub_mgr(upipe->mgr);

    pthread_mutex_lock(&upipe_bmd_sink->lock);
    upipe_throw_dead(upipe);

    upipe_bmd_sink_sub_clean_sub(upipe);
    pthread_mutex_unlock(&upipe_bmd_sink->lock);

    upipe_bmd_sink_sub_clean_input(upipe);
    upipe_bmd_sink_sub_clean_upump(upipe);
    upipe_bmd_sink_sub_clean_upump_mgr(upipe);
    uref_free(upipe_bmd_sink_sub->uref);
    uqueue_uref_flush(&upipe_bmd_sink_sub->uqueue);
    uqueue_clean(&upipe_bmd_sink_sub->uqueue);
    free(upipe_bmd_sink_sub->uqueue_extra);

    if (upipe_bmd_sink_sub == &upipe_bmd_sink->subpic_subpipe ||
        upipe_bmd_sink_sub == &upipe_bmd_sink->pic_subpipe) {
        upipe_clean(upipe);
        return;
    }

    upipe_bmd_sink_sub_clean_urefcount(upipe);
    upipe_bmd_sink_sub_free_flow(upipe);
}

static void copy_samples(upipe_bmd_sink_sub *upipe_bmd_sink_sub,
        struct uref *uref, uint64_t samples)
{
    struct upipe *upipe = &upipe_bmd_sink_sub->upipe;
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_sub_mgr(upipe->mgr);
    uint8_t idx = upipe_bmd_sink_sub->channel_idx;
    int32_t *out = upipe_bmd_sink->audio_buf;

    uint64_t offset = 0;
    if (upipe_bmd_sink_sub->dolby_e) {
        if (upipe_bmd_sink->dolbye_offset >= samples) {
            upipe_err_va(upipe, "offsetting for dolbye would overflow audio: "
                "dolbye %hhu, %" PRIu64" samples",
                upipe_bmd_sink->dolbye_offset, samples);
        } else {
            offset   = upipe_bmd_sink->dolbye_offset;
            samples -= upipe_bmd_sink->dolbye_offset;
        }
    }

    const uint8_t c = upipe_bmd_sink_sub->channels;
    const int32_t *in = NULL;
    UBASE_FATAL_RETURN(upipe, uref_sound_read_int32_t(uref, 0, samples, &in, 1));
    for (int i = 0; i < samples; i++)
        memcpy(&out[DECKLINK_CHANNELS * (offset + i) + idx], &in[c*i], c * sizeof(int32_t));

    uref_sound_unmap(uref, 0, samples, 1);
}

/** @internal @This fills the audio samples for one single stereo pair
 */
static unsigned upipe_bmd_sink_sub_sound_get_samples_channel(struct upipe *upipe,
        const uint64_t video_pts, struct upipe_bmd_sink_sub *upipe_bmd_sink_sub)
{
    size_t samples;
    struct uref *uref = uqueue_pop(&upipe_bmd_sink_sub->uqueue, struct uref *);
    if (!uref) {
        upipe_err(&upipe_bmd_sink_sub->upipe, "no audio");
        return 0;
    }

    if (!ubase_check(uref_sound_size(uref, &samples, NULL /* sample_size */))) {
        upipe_err(&upipe_bmd_sink_sub->upipe, "can't read sound size");
        uref_free(uref);
        return 0;
    }

    if (samples > max_samples) {
        upipe_err_va(&upipe_bmd_sink_sub->upipe, "too much samples (%zu)", samples);
        samples = max_samples;
    }

    /* read the samples into our final buffer */
    copy_samples(upipe_bmd_sink_sub, uref, samples);

    uref_free(uref);

    return samples;
}

/** @internal @This fills one video frame worth of audio samples
 */
static unsigned upipe_bmd_sink_sub_sound_get_samples(struct upipe *upipe,
        const uint64_t video_pts)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);

    /* Clear buffer */
    memset(upipe_bmd_sink->audio_buf, 0, audio_buf_size);

    unsigned samples = 0;

    /* interate through input subpipes */
    pthread_mutex_lock(&upipe_bmd_sink->lock);
    struct uchain *uchain = NULL;
    ulist_foreach(&upipe_bmd_sink->inputs, uchain) {
        struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
            upipe_bmd_sink_sub_from_uchain(uchain);
        if (!upipe_bmd_sink_sub->sound)
            continue;

        unsigned s = upipe_bmd_sink_sub_sound_get_samples_channel(upipe, video_pts, upipe_bmd_sink_sub);
        if (samples < s)
            samples = s;
    }
    pthread_mutex_unlock(&upipe_bmd_sink->lock);

    return samples;
}

static upipe_bmd_sink_frame *get_video_frame(struct upipe *upipe,
                                             struct uref *uref)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    int w = upipe_bmd_sink->displayMode->GetWidth();
    int h = upipe_bmd_sink->displayMode->GetHeight();
    int sd = upipe_bmd_sink->mode == bmdModePAL || upipe_bmd_sink->mode == bmdModeNTSC;
#ifdef UPIPE_HAVE_LIBZVBI_H
    int ttx = upipe_bmd_sink->mode == bmdModePAL || upipe_bmd_sink->mode == bmdModeHD1080i50;
#endif

    if (!uref) {
        if (!upipe_bmd_sink->video_frame)
            return NULL;

        /* increase refcount before outputting this frame */
        ULONG ref = upipe_bmd_sink->video_frame->AddRef();
        upipe_warn_va(upipe, "REUSING FRAME %p : %lu", upipe_bmd_sink->video_frame, ref);
        return upipe_bmd_sink->video_frame;
    }

    const char *v210 = "u10y10v10y10u10y10v10y10u10y10v10y10";
    size_t stride;
    const uint8_t *plane;
    if (unlikely(!ubase_check(uref_pic_plane_size(uref, v210, &stride,
                        NULL, NULL, NULL)) ||
                !ubase_check(uref_pic_plane_read(uref, v210, 0, 0, -1, -1,
                        &plane)))) {
        upipe_err_va(upipe, "Could not read v210 plane");
        return NULL;
    }
    upipe_bmd_sink_frame *video_frame = new upipe_bmd_sink_frame(uref,
            (void*)plane, w, h, stride);
    if (!video_frame) {
        uref_free(uref);
        return NULL;
    }

    if (upipe_bmd_sink->video_frame)
        upipe_bmd_sink->video_frame->Release();
    upipe_bmd_sink->video_frame = NULL;

    IDeckLinkVideoFrameAncillary *ancillary = NULL;
    HRESULT res = upipe_bmd_sink->deckLinkOutput->CreateAncillaryData(video_frame->GetPixelFormat(), &ancillary);
    if (res != S_OK) {
        upipe_err(upipe, "Could not create ancillary data");
        delete video_frame;
        return NULL;
    }

    if (uatomic_load(&upipe_bmd_sink->cc)) {
        const uint8_t *pic_data = NULL;
        size_t pic_data_size = 0;
        uref_pic_get_cea_708(uref, &pic_data, &pic_data_size);
        int ntsc = upipe_bmd_sink->mode == bmdModeNTSC ||
            upipe_bmd_sink->mode == bmdModeHD1080i5994 ||
            upipe_bmd_sink->mode == bmdModeHD720p5994;

        if (ntsc && pic_data_size > 0) {
            /** XXX: Support crazy 25fps captions? **/
            const uint8_t fps = upipe_bmd_sink->mode == bmdModeNTSC ||
                upipe_bmd_sink->mode == bmdModeHD1080i5994 ? 0x4 : 0x7;
            void *vanc;
            ancillary->GetBufferForVerticalBlankingLine(CC_LINE, &vanc);
            uint16_t buf[VANC_WIDTH*2];
            upipe_sdi_blank_c(buf, VANC_WIDTH);
            /* +1 to write into the Y plane */
            sdi_write_cdp(pic_data, pic_data_size, &buf[1], upipe_bmd_sink->mode == bmdModeNTSC ? 1 : 2,
                    &upipe_bmd_sink->cdp_hdr_sequence_cntr, fps);
            sdi_calc_parity_checksum(&buf[1]);

            if (!sd)
                sdi_encode_v210((uint32_t*)vanc, buf, w);
        }
    }

    /* Loop through subpic data */
    struct upipe_bmd_sink_sub *subpic_sub = &upipe_bmd_sink->subpic_subpipe;

    uint64_t vid_pts = 0;
    uref_clock_get_cr_sys(uref, &vid_pts);

    for (;;) {
        /* buffered uref if any */
        struct uref *subpic = subpic_sub->uref;
        if (subpic)
            subpic_sub->uref = NULL;
        else { /* thread-safe queue */
            subpic = uqueue_pop(&subpic_sub->uqueue, struct uref *);
            if (!subpic)
                break;
        }

#ifdef UPIPE_HAVE_LIBZVBI_H
        if (!ttx) {
            uref_free(subpic);
            continue;
        }

        uint64_t subpic_pts = 0;
        uref_clock_get_cr_sys(subpic, &subpic_pts);
        //printf("\n SUBPIC PTS %" PRIu64" \n", subpic_pts );

        /* Delete old urefs */
        if (subpic_pts + (UCLOCK_FREQ/25) < vid_pts) {
            uref_free(subpic);
            continue;
        }

        /* Buffer if needed */
        if (subpic_pts - (UCLOCK_FREQ/25) > vid_pts) {
            subpic_sub->uref = subpic;
            break;
        }

        if (!uatomic_load(&upipe_bmd_sink->ttx)) {
            uref_free(subpic);
            break;
        }

        /* Choose the closest subpic in the past */
        //printf("\n CHOSEN SUBPIC %" PRIu64" \n", subpic_pts);
        const uint8_t *buf;
        int size = -1;
        if (ubase_check(uref_block_read(subpic, 0, &size, &buf))) {
            upipe_bmd_sink_extract_ttx(ancillary, buf, size, w, sd,
                    &upipe_bmd_sink->sp,
                    &upipe_bmd_sink->op47_sequence_counter[0]);
            uref_block_unmap(subpic, 0);
        }
        uref_free(subpic);
#else
        uref_free(subpic);
        continue;
#endif
    }

    video_frame->SetAncillaryData(ancillary);

    video_frame->AddRef(); // we're gonna buffer this frame
    upipe_bmd_sink->video_frame = video_frame;

    return video_frame;
}

static void schedule_frame(struct upipe *upipe, bool prerolling)
{
    HRESULT result;
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_sub_mgr(upipe->mgr);
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
        upipe_bmd_sink_sub_from_upipe(upipe);
    struct uref *uref = uqueue_pop(&upipe_bmd_sink_sub->uqueue, struct uref *);

    upipe_bmd_sink_frame *video_frame =
        get_video_frame(&upipe_bmd_sink->upipe, uref);
    if (!video_frame) {
        abort();
        return;
    }

    uint64_t pts_sys = UINT64_MAX;
    if (uref)
        uref_clock_get_pts_sys(uref, &pts_sys);

    uint64_t ticks_per_frame = upipe_bmd_sink->ticks_per_frame;
    uint64_t pts;
    if (upipe_bmd_sink->last_pts == UINT64_MAX)
        pts = upipe_bmd_sink->start_pts;
    else
        pts = upipe_bmd_sink->last_pts + ticks_per_frame;
    upipe_bmd_sink->last_pts = pts;

    uint64_t now = UINT64_MAX;
    if (upipe_bmd_sink->uclock_std)
        now = uclock_now(upipe_bmd_sink->uclock_std);

    if (!prerolling) {
        int64_t current_diff = (int64_t)now - (int64_t)pts;
        int64_t diff = (int64_t)ticks_per_frame - current_diff;
        static const int64_t divider = 500;

        int64_t mean_diff =
            ((upipe_bmd_sink->mean_diff * divider) + diff) / (divider + 1);

//        int64_t error = 2 * mean_diff - upipe_bmd_sink->mean_diff;
//        int64_t adj = 0;
//        if (abs(error) > abs(mean_diff)) {
//            if (upipe_bmd_sink->count++ > divider) {
//                /* ad +/- 5ppm */
//                adj = 5 * (error >= 0 ? -1 : 1);
//                upipe_bmd_sink->count = 0;
//            }
//        }
        upipe_bmd_sink->mean_diff = mean_diff;

        int64_t ppm = 0;
        if (uref) {
            uref_clock_set_cr_sys(uref, now);
            upipe_throw_clock_ref(upipe, uref, pts, 0);
            upipe_throw_clock_ts(upipe, uref);
            struct urational drift;
            if (ubase_check(uref_clock_get_rate(uref, &drift))) {
                static int64_t count = 0;
                if (count > 100) {
                    ppm = 1000000 - 1000000 * drift.num / drift.den;
                    count = 0;
                }
                else
                    count++;
            }
        }
        if (now > upipe_bmd_sink->last_print + PRINT_PERIODICITY) {
            upipe_notice_va(upipe, "target %.3f ms,"
                            " current %.3f ms,"
                            " diff %.3f ms,"
                            " mean_diff %.3f ms,"
                            " pts_diff %.3f ms,"
                            " ppm %" PRIi64 ","
                            "",
                            ticks_per_frame * 1000. / UCLOCK_FREQ,
                            current_diff * 1000. / UCLOCK_FREQ,
                            diff * 1000. / UCLOCK_FREQ,
                            upipe_bmd_sink->mean_diff * 1000. / UCLOCK_FREQ,
                            ((int64_t)now - (int64_t)pts_sys) * 1000. / UCLOCK_FREQ,
                            ppm);
        }

        if (ppm != INT64_MAX)
            _upipe_bmd_sink_adjust_timing(&upipe_bmd_sink->upipe, -ppm);
    }
    else {
        upipe_bmd_sink->mean_diff = 0;
        upipe_bmd_sink->count = 0;
        upipe_bmd_sink->last_print = 0;
    }

    if (now > upipe_bmd_sink->last_print + PRINT_PERIODICITY)
        upipe_bmd_sink->last_print = now;

    result = upipe_bmd_sink->deckLinkOutput->ScheduleVideoFrame(
        video_frame, pts, ticks_per_frame, UCLOCK_FREQ);
    video_frame->Release();

    if( result != S_OK )
        upipe_err_va(upipe, "DROPPED FRAME %x", result);

    /* audio */
    unsigned samples = upipe_bmd_sink_sub_sound_get_samples(&upipe_bmd_sink->upipe, pts);

    uint32_t written;
    result = upipe_bmd_sink->deckLinkOutput->ScheduleAudioSamples(
            upipe_bmd_sink->audio_buf, samples, pts,
            UCLOCK_FREQ, &written);
    if( result != S_OK ) {
        upipe_err_va(upipe, "DROPPED AUDIO: %x", result);
        written = 0;
    }
    if (written != samples)
        upipe_dbg_va(upipe, "written %u/%u", written, samples);

    /* debug */

    uint32_t buffered;
    upipe_bmd_sink->deckLinkOutput->GetBufferedAudioSampleFrameCount(&buffered);

#if 0
    if (buffered == 0) {
        /* TODO: get notified as soon as audio buffers empty */
        upipe_bmd_sink->deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    }
#endif

    uint32_t vbuffered;
    upipe_bmd_sink->deckLinkOutput->GetBufferedVideoFrameCount(&vbuffered);
    if (0) upipe_dbg_va(upipe, "A %.2f | V %.2f",
            (float)(1000 * buffered) / 48000, (float) 1000 * vbuffered / 25);
}

static void output_cb(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink =
        upipe_bmd_sink_from_sub_mgr(upipe->mgr);

    uint64_t now = uclock_now(upipe_bmd_sink->uclock);
    schedule_frame(upipe, false);

    /* Restart playback 4s after genlock transition */
    if (upipe_bmd_sink->genlock_transition_time) {
        if (now > upipe_bmd_sink->genlock_transition_time + 4 * UCLOCK_FREQ) {
            upipe_warn(upipe, "restarting playback after genlock synchronization");
            upipe_bmd_sink->genlock_transition_time = 0;
            upipe_bmd_sink->deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
            upipe_bmd_sink->deckLinkOutput->StartScheduledPlayback(
                upipe_bmd_sink->last_pts + upipe_bmd_sink->ticks_per_frame,
                UCLOCK_FREQ, 1.0);
        }
    }

    int genlock_status = upipe_bmd_sink->genlock_status;
    upipe_bmd_sink_get_genlock_status(&upipe_bmd_sink->upipe, &upipe_bmd_sink->genlock_status);
    if (genlock_status == UPIPE_BMD_SINK_GENLOCK_UNLOCKED) {
        if (upipe_bmd_sink->genlock_status == UPIPE_BMD_SINK_GENLOCK_LOCKED) {
            upipe_warn(upipe, "genlock synchronized");
            upipe_bmd_sink->genlock_transition_time =
                uclock_now(upipe_bmd_sink->uclock);
        }
    }
}

/** @internal @This starts the playback.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_bmd_sink_start(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);

    upipe_notice(upipe, "Starting playback");
    if (upipe_bmd_sink->deckLinkOutput->EndAudioPreroll() != S_OK)
        upipe_err_va(upipe, "End preroll failed");
    upipe_bmd_sink->deckLinkOutput->StartScheduledPlayback(
        upipe_bmd_sink->start_pts, UCLOCK_FREQ, 1.0);
}

/** @internal @This is called when we need to start.
 *
 * @param upump description structure of the timer
 */
static void upipe_bmd_sink_schedule_start_cb(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    upipe_bmd_sink_start(upipe);
}

/** @internal @This schedules a restart if needed.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_bmd_sink_schedule_start(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    bool active;
    if (upipe_bmd_sink->start_pts == UINT64_MAX ||
        !upipe_bmd_sink->deckLinkOutput ||
        upipe_bmd_sink->deckLinkOutput->IsScheduledPlaybackRunning(&active)
        != S_OK || active ||
        uatomic_load(&upipe_bmd_sink->preroll))
        return;

    upipe_bmd_sink_check_upump_mgr(upipe);
    if (upipe_bmd_sink->upump_mgr && upipe_bmd_sink->uclock) {
        uint64_t now = uclock_now(upipe_bmd_sink->uclock);
        if (now < upipe_bmd_sink->start_pts) {
            upipe_bmd_sink_wait_timer(upipe,
                                      upipe_bmd_sink->start_pts - now,
                                      upipe_bmd_sink_schedule_start_cb);
            return;
        }
    }
    upipe_bmd_sink_start(upipe);
}

/** @internal @This handles input uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to source pump
 * @return true is the uref was outputted
 */
static bool upipe_bmd_sink_sub_output(struct upipe *upipe,
                                      struct uref *uref,
                                      struct upump **upump_p)
{
    struct upipe_bmd_sink *upipe_bmd_sink =
        upipe_bmd_sink_from_sub_mgr(upipe->mgr);
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
        upipe_bmd_sink_sub_from_upipe(upipe);

    const char *def;
    if (unlikely(ubase_check(uref_flow_get_def(uref, &def)))) {
        upipe_bmd_sink_sub->latency = 0;

        uref_clock_get_latency(uref, &upipe_bmd_sink_sub->latency);
        upipe_dbg_va(upipe, "latency %" PRIu64, upipe_bmd_sink_sub->latency);

        upipe_bmd_sink_sub->s337 = !ubase_ncmp(def, "sound.s32.s337.");
        upipe_bmd_sink_sub->dolby_e = upipe_bmd_sink_sub->s337 &&
            !ubase_ncmp(def, "sound.s32.s337.dolbye.");

        upipe_bmd_sink_sub_check_upump_mgr(upipe);

        uref_free(uref);
        return true;
    }

    if (!upipe_bmd_sink->deckLink ||
        !upipe_bmd_sink->uclock_std || !upipe_bmd_sink->uclock) {
        upipe_warn(upipe, "sink is not ready");
        return false;
    }

    uint64_t pts;
    if (unlikely(!ubase_check(uref_clock_get_pts_sys(uref, &pts)))) {
        upipe_err(upipe, "Could not read pts");
        uref_free(uref);
        return true;
    }

    if (unlikely(!uqueue_push(&upipe_bmd_sink_sub->uqueue, uref)))
        return false;

    /* output is controlled by the pic subpipe */
    if (upipe_bmd_sink_sub != &upipe_bmd_sink->pic_subpipe)
        return true;

    if (!uatomic_load(&upipe_bmd_sink->preroll))
        return true;

    if (upipe_bmd_sink->start_pts == UINT64_MAX) {
        upipe_bmd_sink->start_pts = pts;
        upipe_bmd_sink->last_pts = UINT64_MAX;
    }

    if (++upipe_bmd_sink->frames < PREROLL_FRAMES)
        return true;

    /* We're done buffering and now prerolling,
     * get the first one we buffered */
    for (unsigned i = 0; i < PREROLL_FRAMES; i++) {
        schedule_frame(upipe, true);
        uatomic_fetch_sub(&upipe_bmd_sink->preroll, 1);
    }
    upipe_bmd_sink_schedule_start(upipe_bmd_sink_to_upipe(upipe_bmd_sink));
    return true;
}

/** @internal @This handles output data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to upump structure
 */
static void upipe_bmd_sink_sub_input(struct upipe *upipe, struct uref *uref,
                                     struct upump **upump_p)
{
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
        upipe_bmd_sink_sub_from_upipe(upipe);

    if (!upipe_bmd_sink_sub_check_input(upipe)) {
        upipe_bmd_sink_sub_hold_input(upipe, uref);
        upipe_bmd_sink_sub_block_input(upipe, upump_p);
    }
    else if (!upipe_bmd_sink_sub_output(upipe, uref, upump_p)) {
        //fprintf(stderr, "block input!\n");
        upipe_bmd_sink_sub_hold_input(upipe, uref);
        upipe_bmd_sink_sub_block_input(upipe, upump_p);
        if (upipe_bmd_sink_sub->upump)
            upump_start(upipe_bmd_sink_sub->upump);
        upipe_use(upipe);
    }
}

uint32_t upipe_bmd_mode_from_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkOutput *deckLinkOutput = upipe_bmd_sink->deckLinkOutput;
    char *displayModeName = NULL;
    uint32_t bmdMode = bmdModeUnknown;

    if (!deckLinkOutput) {
        upipe_err(upipe, "Card not opened yet");
        return bmdModeUnknown;
    }

    uint64_t hsize, vsize;
    struct urational fps;
    if (unlikely(!ubase_check(uref_pic_flow_get_hsize(flow_def, &hsize)) ||
                !ubase_check(uref_pic_flow_get_vsize(flow_def, &vsize)) ||
                !ubase_check(uref_pic_flow_get_fps(flow_def, &fps)))) {
        upipe_err(upipe, "cannot read size and frame rate");
        uref_dump(flow_def, upipe->uprobe);
        return bmdModeUnknown;
    }

    bool interlaced = !ubase_check(uref_pic_get_progressive(flow_def));

    upipe_notice_va(upipe, "%" PRIu64"x%" PRIu64" %" PRId64"/%" PRIu64" interlaced %d",
            hsize, vsize, fps.num, fps.den, interlaced);

    IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
    HRESULT result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK){
        upipe_err(upipe, "decklink card has no display modes");
        return bmdModeUnknown;
    }

    IDeckLinkDisplayMode *mode = NULL;
    while ((result = displayModeIterator->Next(&mode)) == S_OK) {
        BMDFieldDominance field;
        BMDTimeValue timeValue;
        BMDTimeScale timeScale;
        struct urational bmd_fps;

        if (mode->GetWidth() != hsize)
            goto next;

        if (mode->GetHeight() != vsize)
            goto next;

        mode->GetFrameRate(&timeValue, &timeScale);
        bmd_fps.num = timeScale;
        bmd_fps.den = timeValue;

        if (urational_cmp(&fps, &bmd_fps))
            goto next;

        field = mode->GetFieldDominance();
        if (field == bmdUnknownFieldDominance) {
            upipe_err(upipe, "unknown field dominance");
        } else if (field == bmdLowerFieldFirst || field == bmdUpperFieldFirst) {
            if (!interlaced) {
                goto next;
            }
        } else {
            if (interlaced) {
                goto next;
            }
        }
        break;
next:
        mode->Release();
    }

    if (result != S_OK || !mode)
        goto end;

    if (mode->GetName((const char**)&displayModeName) == S_OK) {
        upipe_dbg_va(upipe, "Flow def is mode %s", displayModeName);
        free(displayModeName);
    }
    bmdMode = mode->GetDisplayMode();

    _upipe_bmd_sink_set_timing_adjustment(&upipe_bmd_sink->upipe, 127);
end:
    if (mode)
        mode->Release();

    displayModeIterator->Release();

    return bmdMode;
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static int upipe_bmd_sink_sub_set_flow_def(struct upipe *upipe,
                                          struct uref *flow_def)
{
    struct upipe_bmd_sink *upipe_bmd_sink =
        upipe_bmd_sink_from_sub_mgr(upipe->mgr);
    struct upipe *super = upipe_bmd_sink_to_upipe(upipe_bmd_sink);
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
        upipe_bmd_sink_sub_from_upipe(upipe);

    if (flow_def == NULL)
        return UBASE_ERR_INVALID;

    uint64_t latency;
    if (ubase_check(uref_clock_get_latency(flow_def, &latency))) {
        if (/*upipe_bmd_sink_sub->latency && */latency != upipe_bmd_sink_sub->latency) {
            upipe_dbg_va(upipe, "latency %" PRIu64" -> %" PRIu64,
                    upipe_bmd_sink_sub->latency, latency);
            upipe_bmd_sink_sub->latency = latency;
        }
    }

    if (upipe_bmd_sink_sub == &upipe_bmd_sink->pic_subpipe) {

        if (unlikely(!ubase_check(uref_pic_flow_check_v210(flow_def)))) {
            upipe_err(upipe, "incompatible input flow def");
            uref_dump(flow_def, upipe->uprobe);
            return UBASE_ERR_EXTERNAL;
        }

        BMDDisplayMode bmdMode =
            upipe_bmd_mode_from_flow_def(&upipe_bmd_sink->upipe, flow_def);
        if (bmdMode == bmdModeUnknown) {
            upipe_err(upipe, "input flow def is not supported");
            return UBASE_ERR_INVALID;
        }
        if (upipe_bmd_sink->selectedMode != bmdModeUnknown &&
            bmdMode != upipe_bmd_sink->selectedMode) {
            upipe_warn(upipe, "incompatible input flow def for selected mode");
            return UBASE_ERR_INVALID;
        }
        if (bmdMode != upipe_bmd_sink->mode) {
            upipe_notice(upipe, "Changing output configuration");
            upipe_bmd_sink->mode = bmdMode;
            UBASE_RETURN(upipe_bmd_open_vid(super));
        }

        struct dolbye_offset {
            BMDDisplayMode mode;
            uint8_t offset;
        };

        static const struct dolbye_offset table[2][2] = {
            /* All others */
            {
                {
                    bmdModeHD1080i50, 33,
                },
                {
                    bmdModeHD1080i5994, 31,
                },
            },

            /* SDI (including Duo) */
            {
                {
                    bmdModeHD1080i50, 54,
                },
                {
                    bmdModeHD1080i5994, 48,
                },
            },

        };

        const struct dolbye_offset *t = &table[0][0];
        if (upipe_bmd_sink->modelName && !strcmp(upipe_bmd_sink->modelName, "DeckLink SDI")) {
            t = &table[1][0];
        }

        const size_t n = sizeof(table) / 2 / sizeof(**table);

        for (size_t i = 0; i < n; i++) {
            const struct dolbye_offset *e = &t[i];
            if (e->mode == bmdMode) {
                upipe_bmd_sink->dolbye_offset = e->offset;
                break;
            }
        }

        upipe_bmd_sink->frame_idx = 0;
    } else if (upipe_bmd_sink_sub != &upipe_bmd_sink->subpic_subpipe) {
        if (!ubase_check(uref_sound_flow_get_channels(flow_def, &upipe_bmd_sink_sub->channels))) {
            upipe_err(upipe, "Could not read number of channels");
            return UBASE_ERR_INVALID;
        }

        if (upipe_bmd_sink_sub->channels > 2) {
            upipe_err_va(upipe, "Too many audio channels %u", upipe_bmd_sink_sub->channels);
            return UBASE_ERR_INVALID;
        }
    }

    flow_def = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def)
    upipe_input(upipe, flow_def, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on an output subpipe of an
 * bmd_sink pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_bmd_sink_sub_control_real(struct upipe *upipe,
                                           int command, va_list args)
{
    UBASE_HANDLED_RETURN(
        upipe_bmd_sink_sub_control_super(upipe, command, args));
    UBASE_HANDLED_RETURN(upipe_control_provide_request(upipe, command, args));
    switch (command) {
        case UPIPE_ATTACH_UPUMP_MGR: {
            upipe_bmd_sink_sub_set_upump(upipe, NULL);
            UBASE_RETURN(upipe_bmd_sink_sub_attach_upump_mgr(upipe))
            return UBASE_ERR_NONE;
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_bmd_sink_sub_set_flow_def(upipe, flow_def);
        }

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

static void upipe_bmd_sink_sub_push_cb(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    if (upipe_bmd_sink_sub_check_input(upipe)) {
        upump_stop(upump);
        return;
    }

    upipe_bmd_sink_sub_output_input(upipe);
    upipe_bmd_sink_sub_unblock_input(upipe);
    if (upipe_bmd_sink_sub_check_input(upipe)) {
        upump_stop(upump);
        upipe_release(upipe);
    }
}

static int upipe_bmd_sink_sub_check(struct upipe *upipe)
{
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
        upipe_bmd_sink_sub_from_upipe(upipe);

    UBASE_RETURN(upipe_bmd_sink_sub_check_upump_mgr(upipe));
    if (upipe_bmd_sink_sub->upump_mgr) {
        if (!upipe_bmd_sink_sub->upump) {
            struct upump *upump = uqueue_upump_alloc_push(
                &upipe_bmd_sink_sub->uqueue,
                upipe_bmd_sink_sub->upump_mgr,
                upipe_bmd_sink_sub_push_cb, upipe,
                upipe->refcount);
            if (unlikely(!upump))
                return UBASE_ERR_ALLOC;

            upipe_bmd_sink_sub_set_upump(upipe, upump);
            if (!upipe_bmd_sink_sub_check_input(upipe))
                upump_start(upump);
        }
    }

    return UBASE_ERR_NONE;
}

static int upipe_bmd_sink_sub_control(struct upipe *upipe,
                                      int command, va_list args)
{
    UBASE_RETURN(upipe_bmd_sink_sub_control_real(upipe, command, args));
    return upipe_bmd_sink_sub_check(upipe);
}

static struct upipe *upipe_bmd_sink_sub_alloc(struct upipe_mgr *mgr,
                                                 struct uprobe *uprobe,
                                                 uint32_t signature, va_list args)
{
    struct uref *flow_def = NULL;
    struct upipe *upipe = upipe_bmd_sink_sub_alloc_flow(mgr,
            uprobe, signature, args, &flow_def);
    struct upipe_bmd_sink_sub *upipe_bmd_sink_sub = upipe_bmd_sink_sub_from_upipe(upipe);

    if (unlikely(upipe == NULL || flow_def == NULL))
        goto error;

    const char *def;
    if (!ubase_check(uref_flow_get_def(flow_def, &def)))
        goto error;

    if (ubase_ncmp(def, "sound."))
        goto error;

    uint8_t channel_idx;
    if (!ubase_check(uref_bmd_sink_get_channel(flow_def, &channel_idx))) {
        upipe_err(upipe, "Could not read channel_idx");
        uref_dump(flow_def, uprobe);
        goto error;
    }

    if (channel_idx >= DECKLINK_CHANNELS) {
        upipe_err_va(upipe, "channel_idx %hhu not in range", channel_idx);
        goto error;
    }

    upipe_bmd_sink_sub_init(upipe, mgr, uprobe, false);

    upipe_bmd_sink_sub->channel_idx = channel_idx;

    /* different subpipe type */
    uref_dump(flow_def, uprobe);
    uref_free(flow_def);

    return upipe;

error:
    uref_free(flow_def);
    if (upipe) {
        upipe_clean(upipe);
        free(upipe_bmd_sink_sub);
    }
    return NULL;
}

/** @internal @This initializes the output manager for an bmd_sink pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_bmd_sink_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_bmd_sink->sub_mgr;
    sub_mgr->refcount = upipe_bmd_sink_to_urefcount(upipe_bmd_sink);
    sub_mgr->signature = UPIPE_BMD_SINK_INPUT_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_bmd_sink_sub_alloc;
    sub_mgr->upipe_input = upipe_bmd_sink_sub_input;
    sub_mgr->upipe_control = upipe_bmd_sink_sub_control;
    sub_mgr->upipe_mgr_control = NULL;
}

/** @internal @This allocates a bmd_sink pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_bmd_sink_alloc(struct upipe_mgr *mgr,
                                          struct uprobe *uprobe,
                                          uint32_t signature, va_list args)
{
    if (signature != UPIPE_BMD_SINK_SIGNATURE)
        return NULL;
    struct uprobe *uprobe_pic = va_arg(args, struct uprobe *);
    struct uprobe *uprobe_subpic = va_arg(args, struct uprobe *);

    struct upipe_bmd_sink *upipe_bmd_sink =
        (struct upipe_bmd_sink *)calloc(1, sizeof(struct upipe_bmd_sink));
    if (unlikely(upipe_bmd_sink == NULL)) {
        uprobe_release(uprobe_pic);
        uprobe_release(uprobe_subpic);
        return NULL;
    }

    struct upipe *upipe = upipe_bmd_sink_to_upipe(upipe_bmd_sink);
    upipe_init(upipe, mgr, uprobe);

    upipe_bmd_sink_init_sub_inputs(upipe);
    upipe_bmd_sink_init_sub_mgr(upipe);
    upipe_bmd_sink_init_urefcount(upipe);
    upipe_bmd_sink_init_uclock(upipe);
    upipe_bmd_sink_init_upump_mgr(upipe);
    upipe_bmd_sink_init_timer(upipe);

    pthread_mutex_init(&upipe_bmd_sink->lock, NULL);

    /* Initalise subpipes */
    upipe_bmd_sink_sub_init(upipe_bmd_sink_sub_to_upipe(upipe_bmd_sink_to_pic_subpipe(upipe_bmd_sink)),
                            &upipe_bmd_sink->sub_mgr, uprobe_pic, true);
    upipe_bmd_sink_sub_init(upipe_bmd_sink_sub_to_upipe(upipe_bmd_sink_to_subpic_subpipe(upipe_bmd_sink)),
                            &upipe_bmd_sink->sub_mgr, uprobe_subpic, true);

    upipe_bmd_sink->audio_buf = (int32_t*)malloc(audio_buf_size);
    upipe_bmd_sink->uclock = NULL;
    upipe_bmd_sink->card_idx = -1;
    upipe_bmd_sink->card_topo = -1;
    upipe_bmd_sink->opened = false;
    upipe_bmd_sink->mode = bmdModeUnknown;
    upipe_bmd_sink->selectedMode = bmdModeUnknown;
    upipe_bmd_sink->timing_adjustment_support = false;
    upipe_bmd_sink->start_pts = UINT64_MAX;
    upipe_bmd_sink->timing_adjustment = INT64_MAX;

    upipe_throw_ready(upipe);
    return upipe;
}

static void upipe_bmd_stop(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkOutput *deckLinkOutput = upipe_bmd_sink->deckLinkOutput;

    upipe_bmd_sink->start_pts = UINT64_MAX;
    uatomic_store(&upipe_bmd_sink->preroll, PREROLL_FRAMES);
    upipe_bmd_sink->frames = 0;
    __sync_synchronize();

    uclock_release(upipe_bmd_sink->uclock);
    upipe_bmd_sink->uclock = NULL;
    deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    deckLinkOutput->DisableAudioOutput();
    /* bump clock upwards before it's made unavailable by DisableVideoOutput */
    deckLinkOutput->DisableVideoOutput();

    struct uchain *uchain = NULL;
    ulist_foreach(&upipe_bmd_sink->inputs, uchain) {
        struct upipe_bmd_sink_sub *upipe_bmd_sink_sub =
            upipe_bmd_sink_sub_from_uchain(uchain);
        uqueue_uref_flush(&upipe_bmd_sink_sub->uqueue);
    }

    if (upipe_bmd_sink->displayMode) {
        upipe_bmd_sink->displayMode->Release();
        upipe_bmd_sink->displayMode = NULL;
    }

    if (upipe_bmd_sink->video_frame) {
        upipe_bmd_sink->video_frame->Release();
        upipe_bmd_sink->video_frame = NULL;
    }

    upipe_bmd_sink->opened = false;
}

static int upipe_bmd_open_vid(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkOutput *deckLinkOutput = upipe_bmd_sink->deckLinkOutput;
    IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
    char* displayModeName = NULL;
    IDeckLinkDisplayMode* displayMode = NULL;
    int err = UBASE_ERR_NONE;
    HRESULT result = E_NOINTERFACE;

    upipe_bmd_stop(upipe);

    if (upipe_bmd_sink->uclock_std)
        upipe_bmd_sink->uclock = uclock_bmd_sink_alloc(
            upipe_bmd_sink->deckLink, upipe_bmd_sink->uclock_std);

    result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK){
        upipe_err_va(upipe, "decklink card has no display modes");
        err = UBASE_ERR_EXTERNAL;
        goto end;
    }

    while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
    {
        if (displayMode->GetDisplayMode() == upipe_bmd_sink->mode)
            break;

        displayMode->Release();
    }

    if (result != S_OK || displayMode == NULL)
    {
        uint32_t mode = htonl(upipe_bmd_sink->mode);
        upipe_err_va(upipe, "Unable to get display mode %4.4s\n", (char*)&mode);
        err = UBASE_ERR_EXTERNAL;
        goto end;
    }

    result = displayMode->GetName((const char**)&displayModeName);
    if (result == S_OK)
    {
        upipe_dbg_va(upipe, "Using mode %s", displayModeName);
        free(displayModeName);
    }

    upipe_bmd_sink->displayMode = displayMode;

    BMDTimeValue timeValue;
    BMDTimeScale timeScale;
    displayMode->GetFrameRate(&timeValue, &timeScale);
    upipe_bmd_sink->ticks_per_frame = UCLOCK_FREQ * timeValue / timeScale;

    result = deckLinkOutput->EnableVideoOutput(displayMode->GetDisplayMode(),
                                               bmdVideoOutputVANC);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to enable video output. Is another application using the card?\n");
        err = UBASE_ERR_EXTERNAL;
        goto end;
    }

    result = deckLinkOutput->EnableAudioOutput(48000, bmdAudioSampleType32bitInteger,
            DECKLINK_CHANNELS, bmdAudioOutputStreamTimestamped);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to enable audio output. Is another application using the card?\n");
        err = UBASE_ERR_EXTERNAL;
        goto end;
    }

    if (deckLinkOutput->BeginAudioPreroll() != S_OK)
        upipe_err(upipe, "Could not begin audio preroll");

    upipe_bmd_sink->genlock_status = -1;
    upipe_bmd_sink->genlock_transition_time = 0;

#ifdef UPIPE_HAVE_LIBZVBI_H
    if (upipe_bmd_sink->mode == bmdModePAL) {
        upipe_bmd_sink->sp.scanning         = 625; /* PAL */
        upipe_bmd_sink->sp.sampling_format  = VBI_PIXFMT_YUV420;
        upipe_bmd_sink->sp.sampling_rate    = 13.5e6;
        upipe_bmd_sink->sp.bytes_per_line   = 720;
        upipe_bmd_sink->sp.start[0]     = 6;
        upipe_bmd_sink->sp.count[0]     = 17;
        upipe_bmd_sink->sp.start[1]     = 319;
        upipe_bmd_sink->sp.count[1]     = 17;
        upipe_bmd_sink->sp.interlaced   = FALSE;
        upipe_bmd_sink->sp.synchronous  = FALSE;
        upipe_bmd_sink->sp.offset       = 128;
    } else if (upipe_bmd_sink->mode == bmdModeNTSC) {
        upipe_bmd_sink->sp.scanning         = 525; /* NTSC */
        upipe_bmd_sink->sp.sampling_format  = VBI_PIXFMT_YUV420;
        upipe_bmd_sink->sp.sampling_rate    = 13.5e6;
        upipe_bmd_sink->sp.bytes_per_line   = 720;
        upipe_bmd_sink->sp.interlaced   = FALSE;
        upipe_bmd_sink->sp.synchronous  = TRUE;
    }
#endif

    upipe_bmd_sink->opened = true;

end:
    if (displayModeIterator != NULL)
        displayModeIterator->Release();

    return err;
}

/** @internal @This asks to open the given device.
 *
 * @param upipe description structure of the pipe
 * @param uri URI
 * @return an error code
 */
static int upipe_bmd_sink_open_card(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkAttributes *deckLinkAttributes = NULL;

    int err = UBASE_ERR_NONE;
    HRESULT result = E_NOINTERFACE;

    assert(!upipe_bmd_sink->deckLink);

    /* decklink interface interator */
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator) {
        upipe_err_va(upipe, "decklink drivers not found");
        return UBASE_ERR_EXTERNAL;
    }

    /* get decklink interface handler */
    IDeckLink *deckLink = NULL;

    if (upipe_bmd_sink->card_topo >= 0) {
        for ( ; ; ) {
            if (deckLink)
                deckLink->Release();
            result = deckLinkIterator->Next(&deckLink);
            if (result != S_OK)
                break;

            if (deckLink->QueryInterface(IID_IDeckLinkAttributes,
                                         (void**)&deckLinkAttributes) == S_OK) {
                int64_t deckLinkTopologicalId = 0;
                HRESULT result =
                    deckLinkAttributes->GetInt(BMDDeckLinkTopologicalID,
                            &deckLinkTopologicalId);
                deckLinkAttributes->Release();
                if (result == S_OK &&
                    (uint64_t)deckLinkTopologicalId == upipe_bmd_sink->card_topo)
                    break;
            }
        }
    }
    else if (upipe_bmd_sink->card_idx >= 0) {
        for (int i = 0; i <= upipe_bmd_sink->card_idx; i++) {
            if (deckLink)
                deckLink->Release();
            result = deckLinkIterator->Next(&deckLink);
            if (result != S_OK)
                break;
        }
    }

    if (result != S_OK) {
        upipe_err_va(upipe, "decklink card %d not found", upipe_bmd_sink->card_idx);
        err = UBASE_ERR_EXTERNAL;
        if (deckLink)
            deckLink->Release();
        goto end;
    }

    if (deckLink->GetModelName(&upipe_bmd_sink->modelName) != S_OK) {
        upipe_err(upipe, "Could not read card model name");
    }

    if (deckLink->QueryInterface(IID_IDeckLinkAttributes,
                                 (void**)&deckLinkAttributes) == S_OK) {
        HRESULT result =
            deckLinkAttributes->GetFlag(BMDDeckLinkSupportsClockTimingAdjustment,
                                        &upipe_bmd_sink->timing_adjustment_support);
        deckLinkAttributes->Release();
        if (result == S_OK)
            upipe_notice_va(upipe, "clock timing adjustement supported: %s",
                            upipe_bmd_sink->timing_adjustment_support ? "true" : "false");
        else
            upipe_warn_va(upipe, "cannot get clock timing adjustement supported flag");
    }

    if (deckLink->QueryInterface(IID_IDeckLinkOutput,
                                 (void**)&upipe_bmd_sink->deckLinkOutput) != S_OK) {
        upipe_err_va(upipe, "decklink card has no output");
        err = UBASE_ERR_EXTERNAL;
        deckLink->Release();
        goto end;
    }

    upipe_bmd_sink->cb = new callback(upipe_bmd_sink);
    if (upipe_bmd_sink->deckLinkOutput->SetScheduledFrameCompletionCallback(
                upipe_bmd_sink->cb) != S_OK)
        upipe_err(upipe, "Could not set callback");

    upipe_bmd_sink->deckLink = deckLink;
end:

    deckLinkIterator->Release();

    return err;
}

/** @internal @This sets the content of a bmd_sink option.
 *
 * @param upipe description structure of the pipe
 * @param option name of the option
 * @param content content of the option, or NULL to delete it
 * @return an error code
 */
static int upipe_bmd_sink_set_option(struct upipe *upipe,
                                   const char *k, const char *v)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    assert(k != NULL);

    if (!strcmp(k, "card-index"))
        upipe_bmd_sink->card_idx = atoi(v);
    else if (!strcmp(k, "card-topology"))
        upipe_bmd_sink->card_topo = strtoll(v, NULL, 10);
    else if (!strcmp(k, "mode")) {
        if (!v || strlen(v) != 4)
            return UBASE_ERR_INVALID;
        union {
            BMDDisplayMode mode_id;
            char mode_s[4];
        } u;
        memcpy(u.mode_s, v, sizeof(u.mode_s));
        upipe_bmd_sink->selectedMode = htonl(u.mode_id);
    } else if (!strcmp(k, "cc")) {
        uatomic_store(&upipe_bmd_sink->cc, strcmp(v, "0"));
    } else if (!strcmp(k, "teletext")) {
        uatomic_store(&upipe_bmd_sink->ttx, strcmp(v, "0"));
    } else
        return UBASE_ERR_INVALID;

    return UBASE_ERR_NONE;
}

/** @internal @This returns the bmd_sink genlock status.
 *
 * @param upipe description structure of the pipe
 * @param pointer to integer for genlock status
 * @return an error code
 */
static int _upipe_bmd_sink_get_genlock_status(struct upipe *upipe, int *status)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    BMDReferenceStatus reference_status;

    if (!upipe_bmd_sink->deckLinkOutput) {
        upipe_err_va(upipe, "No output configured");
        return UBASE_ERR_INVALID;
    }

    HRESULT result = upipe_bmd_sink->deckLinkOutput->GetReferenceStatus(&reference_status);
    if (result != S_OK)
        return UBASE_ERR_EXTERNAL;

    if (reference_status & bmdReferenceNotSupportedByHardware) {
        *status = UPIPE_BMD_SINK_GENLOCK_UNSUPPORTED;
        return UBASE_ERR_NONE;
    }

    if (reference_status & bmdReferenceLocked) {
        *status = UPIPE_BMD_SINK_GENLOCK_LOCKED;
        return UBASE_ERR_NONE;
    }

    *status = UPIPE_BMD_SINK_GENLOCK_UNLOCKED;
    return UBASE_ERR_NONE;
}

/** @internal @This returns the bmd_sink genlock offset.
 *
 * @param upipe description structure of the pipe
 * @param pointer to int64_t for genlock offset
 * @return an error code
 */
static int _upipe_bmd_sink_get_genlock_offset(struct upipe *upipe, int64_t *offset)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    BMDReferenceStatus reference_status;
    IDeckLinkConfiguration *decklink_configuration;
    HRESULT result;

    if (!upipe_bmd_sink->deckLinkOutput) {
        upipe_err_va(upipe, "No output configured");
        return UBASE_ERR_INVALID;
    }

    result = upipe_bmd_sink->deckLinkOutput->GetReferenceStatus(&reference_status);
    if (result != S_OK)
        return UBASE_ERR_EXTERNAL;
    if ((reference_status & bmdReferenceNotSupportedByHardware) ||
        !(reference_status & bmdReferenceLocked)) {
        *offset = 0;
        return UBASE_ERR_EXTERNAL;
    }

    result = upipe_bmd_sink->deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&decklink_configuration);
    if (result != S_OK) {
        *offset = 0;
        return UBASE_ERR_EXTERNAL;
    }

    result = decklink_configuration->GetInt(bmdDeckLinkConfigReferenceInputTimingOffset, offset);
    if (result != S_OK) {
        *offset = 0;
        decklink_configuration->Release();
        return UBASE_ERR_EXTERNAL;
    }
    decklink_configuration->Release();

    return UBASE_ERR_NONE;
}

/** @internal @This sets the bmd_sink genlock offset.
 *
 * @param upipe description structure of the pipe
 * @param int64_t requested genlock offset
 * @return an error code
 */
static int _upipe_bmd_sink_set_genlock_offset(struct upipe *upipe, int64_t offset)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    BMDReferenceStatus reference_status;
    IDeckLinkConfiguration *decklink_configuration;
    HRESULT result;

    if (!upipe_bmd_sink->deckLinkOutput) {
        upipe_err_va(upipe, "No output configured");
        return UBASE_ERR_INVALID;
    }

    result = upipe_bmd_sink->deckLinkOutput->GetReferenceStatus(&reference_status);
    if (result != S_OK)
        return UBASE_ERR_EXTERNAL;

    if ((reference_status & bmdReferenceNotSupportedByHardware)) {
        return UBASE_ERR_EXTERNAL;
    }

    result = upipe_bmd_sink->deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&decklink_configuration);
    if (result != S_OK) {
        return UBASE_ERR_EXTERNAL;
    }

    result = decklink_configuration->SetInt(bmdDeckLinkConfigReferenceInputTimingOffset, offset);
    if (result != S_OK) {
        decklink_configuration->Release();
        return UBASE_ERR_EXTERNAL;
    }

    decklink_configuration->WriteConfigurationToPreferences();
    decklink_configuration->Release();

    return UBASE_ERR_NONE;
}

/** @internal @This sets the bmd_sink timing adjustment.
 *
 * @param upipe description structure of the pipe
 * @param int64_t requested timing adjustment
 * @return an error code
 */
static int _upipe_bmd_sink_set_timing_adjustment(struct upipe *upipe, int64_t adj)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkConfiguration *decklink_configuration;
    HRESULT result;

    if (!upipe_bmd_sink->timing_adjustment_support)
        return UBASE_ERR_INVALID;

    result = upipe_bmd_sink->deckLink->QueryInterface(
        IID_IDeckLinkConfiguration, (void**)&decklink_configuration);
    if (result != S_OK)
        return UBASE_ERR_EXTERNAL;

    if (upipe_bmd_sink->timing_adjustment == INT64_MAX) {
        result = decklink_configuration->GetInt(
            bmdDeckLinkConfigClockTimingAdjustment,
            &upipe_bmd_sink->timing_adjustment);
        if (result != S_OK) {
            decklink_configuration->Release();
            return UBASE_ERR_EXTERNAL;
        }
        upipe_notice_va(upipe, "current timing adjustment %" PRIi64,
                        upipe_bmd_sink->timing_adjustment);
    }

    if (adj > 127)
        adj = 127;
    else if (adj < -127)
        adj = -127;

    if (upipe_bmd_sink->timing_adjustment == adj)
        return UBASE_ERR_NONE;

    upipe_bmd_sink->timing_adjustment = adj;

    result = decklink_configuration->SetInt(
        bmdDeckLinkConfigClockTimingAdjustment,
        upipe_bmd_sink->timing_adjustment);
    if (result != S_OK) {
        decklink_configuration->Release();
        return UBASE_ERR_EXTERNAL;
    }

    decklink_configuration->WriteConfigurationToPreferences();
    decklink_configuration->Release();

    upipe_notice_va(upipe, "adjust timing to %" PRIi64" ppm", adj);

    return UBASE_ERR_NONE;
}

static int _upipe_bmd_sink_adjust_timing(struct upipe *upipe, int64_t adj)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);
    IDeckLinkConfiguration *decklink_configuration;
    HRESULT result;

    if (!upipe_bmd_sink->timing_adjustment_support)
        return UBASE_ERR_INVALID;

    result = upipe_bmd_sink->deckLink->QueryInterface(
        IID_IDeckLinkConfiguration, (void**)&decklink_configuration);
    if (result != S_OK)
        return UBASE_ERR_EXTERNAL;

    if (upipe_bmd_sink->timing_adjustment == INT64_MAX) {
        result = decklink_configuration->GetInt(
            bmdDeckLinkConfigClockTimingAdjustment,
            &upipe_bmd_sink->timing_adjustment);
        if (result != S_OK) {
            decklink_configuration->Release();
            return UBASE_ERR_EXTERNAL;
        }
        upipe_notice_va(upipe, "current timing adjustment %" PRIi64,
                        upipe_bmd_sink->timing_adjustment);
    }

    adj += upipe_bmd_sink->timing_adjustment;
    if (adj > 127)
        adj = 127;
    else if (adj < -127)
        adj = -127;

    if (upipe_bmd_sink->timing_adjustment == adj)
        return UBASE_ERR_NONE;

    upipe_bmd_sink->timing_adjustment = adj;

    result = decklink_configuration->SetInt(
        bmdDeckLinkConfigClockTimingAdjustment,
        upipe_bmd_sink->timing_adjustment);
    if (result != S_OK) {
        decklink_configuration->Release();
        return UBASE_ERR_EXTERNAL;
    }

    decklink_configuration->WriteConfigurationToPreferences();
    decklink_configuration->Release();

    upipe_notice_va(upipe, "adjust timing to %" PRIi64" ppm", adj);

    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on an bmd_sink source pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_bmd_sink_control_real(struct upipe *upipe,
                                       int command, va_list args)
{
    struct upipe_bmd_sink *bmd_sink = upipe_bmd_sink_from_upipe(upipe);

    UBASE_HANDLED_RETURN(upipe_bmd_sink_control_inputs(upipe, command, args));
    switch (command) {
        case UPIPE_SET_URI:
            if (!bmd_sink->deckLink) {
                UBASE_RETURN(upipe_bmd_sink_open_card(upipe));
            }
            return UBASE_ERR_NONE;

        case UPIPE_ATTACH_UCLOCK:
            upipe_bmd_sink_require_uclock(upipe);
            return UBASE_ERR_NONE;

        case UPIPE_ATTACH_UPUMP_MGR:
            upipe_bmd_sink_set_timer(upipe, NULL);
            return upipe_bmd_sink_attach_upump_mgr(upipe);

        case UPIPE_BMD_SINK_GET_PIC_SUB: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            struct upipe **upipe_p = va_arg(args, struct upipe **);
            *upipe_p =  upipe_bmd_sink_sub_to_upipe(
                            upipe_bmd_sink_to_pic_subpipe(
                                upipe_bmd_sink_from_upipe(upipe)));
            return UBASE_ERR_NONE;
        }
        case UPIPE_BMD_SINK_GET_SUBPIC_SUB: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            struct upipe **upipe_p = va_arg(args, struct upipe **);
            *upipe_p =  upipe_bmd_sink_sub_to_upipe(
                            upipe_bmd_sink_to_subpic_subpipe(
                                upipe_bmd_sink_from_upipe(upipe)));
            return UBASE_ERR_NONE;
        }
        case UPIPE_BMD_SINK_GET_UCLOCK: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            struct uclock **pp_uclock = va_arg(args, struct uclock **);
            *pp_uclock = bmd_sink->uclock;
            return UBASE_ERR_NONE;
        }
        case UPIPE_BMD_SINK_GET_GENLOCK_STATUS: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            int *status = va_arg(args, int *);
            return _upipe_bmd_sink_get_genlock_status(upipe, status);
        }
        case UPIPE_BMD_SINK_GET_GENLOCK_OFFSET: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            int64_t *offset = va_arg(args, int64_t *);
            return _upipe_bmd_sink_get_genlock_offset(upipe, offset);
        }
        case UPIPE_BMD_SINK_SET_GENLOCK_OFFSET: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_BMD_SINK_SIGNATURE)
            int64_t offset = va_arg(args, int64_t);
            return _upipe_bmd_sink_set_genlock_offset(upipe, offset);
        }
        case UPIPE_SET_OPTION: {
            const char *k = va_arg(args, const char *);
            const char *v = va_arg(args, const char *);
            return upipe_bmd_sink_set_option(upipe, k, v);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

static int upipe_bmd_sink_check(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);

    if (flow_def)
        uref_free(flow_def);

    if (!upipe_bmd_sink->uclock_std)
        upipe_bmd_sink_require_uclock(upipe);

    upipe_bmd_sink_schedule_start(upipe);

    return UBASE_ERR_NONE;
}

static int upipe_bmd_sink_control(struct upipe *upipe,
                                  int command, va_list args)
{
    UBASE_RETURN(upipe_bmd_sink_control_real(upipe, command, args));
    return upipe_bmd_sink_check(upipe, NULL);
}

/** @internal @This frees all resources allocated.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_bmd_sink_free(struct upipe *upipe)
{
    struct upipe_bmd_sink *upipe_bmd_sink = upipe_bmd_sink_from_upipe(upipe);

    if (upipe_bmd_sink->deckLink)
        upipe_bmd_stop(upipe);

    upipe_bmd_sink_sub_free(upipe_bmd_sink_sub_to_upipe(&upipe_bmd_sink->pic_subpipe));
    upipe_bmd_sink_sub_free(upipe_bmd_sink_sub_to_upipe(&upipe_bmd_sink->subpic_subpipe));
    upipe_dbg_va(upipe, "releasing blackmagic sink pipe %p", upipe);

    upipe_throw_dead(upipe);

    free(upipe_bmd_sink->audio_buf);

    if (upipe_bmd_sink->deckLink) {
        free((void*)upipe_bmd_sink->modelName);
        upipe_bmd_sink->deckLinkOutput->Release();
        upipe_bmd_sink->deckLink->Release();
    }

    pthread_mutex_destroy(&upipe_bmd_sink->lock);

    if (upipe_bmd_sink->cb)
        upipe_bmd_sink->cb->Release();

    upipe_bmd_sink_clean_timer(upipe);
    upipe_bmd_sink_clean_upump_mgr(upipe);
    upipe_bmd_sink_clean_uclock(upipe);
    upipe_bmd_sink_clean_sub_inputs(upipe);
    upipe_bmd_sink_clean_urefcount(upipe);
    upipe_clean(upipe);
    free(upipe_bmd_sink);
}

/** upipe_bmd_sink (/dev/bmd_sink) */
static struct upipe_mgr upipe_bmd_sink_mgr = {
    /* .refcount = */ NULL,
    /* .signature = */ UPIPE_BMD_SINK_SIGNATURE,

    /* .upipe_err_str = */ NULL,
    /* .upipe_command_str = */ NULL,
    /* .upipe_event_str = */ NULL,

    /* .upipe_alloc = */ upipe_bmd_sink_alloc,
    /* .upipe_input = */ NULL,
    /* .upipe_control = */ upipe_bmd_sink_control,

    /* .upipe_mgr_control = */ NULL
};

/** @This returns the management structure for bmd_sink pipes
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_bmd_sink_mgr_alloc(void)
{
    return &upipe_bmd_sink_mgr;
}
