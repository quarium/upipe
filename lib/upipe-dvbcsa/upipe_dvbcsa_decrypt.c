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

#include <upipe-dvbcsa/upipe_dvbcsa_decrypt.h>

#include <upipe/upipe.h>
#include <upipe/upump.h>
#include <upipe/uclock.h>
#include <upipe/uref_block.h>
#include <upipe/uref_clock.h>

#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_input.h>

#include <bitstream/mpeg/ts.h>
#include <dvbcsa/dvbcsa.h>

/** expected input flow format */
#define EXPECTED_FLOW_DEF "block.mpegts."

/** @hidden */
static void upipe_dvbcsa_dec_worker(struct upump *upump);

/** @This is the private structure of dvbcsa decryption pipe. */
struct upipe_dvbcsa_dec {
    /** public pipe structure */
    struct upipe upipe;
    /** urefcount structure */
    struct urefcount urefcount;
    /** output pipe */
    struct upipe *output;
    /** output flow definition */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** request list */
    struct uchain requests;
    /** uclock */
    struct uclock *uclock;
    /** uclock request */
    struct urequest uclock_request;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** upump */
    struct upump *upump;
    /** list of retained urefs */
    struct uchain urefs;
    /** number of retained urefs */
    unsigned int nb_urefs;
    /** maximum retained urefs */
    unsigned int max_urefs;
    /** blockers */
    struct uchain blockers;
    /** dvbcsa key */
    dvbcsa_bs_key_t *key;
    /** maximum number of packet per batch */
    unsigned int batch_size;
    /** number of scrambled buffer in the list */
    unsigned int scrambled;
    /** array of dvbcsa batch */
    struct dvbcsa_bs_batch_s *pcks;
    /** pipe latency */
    uint64_t latency;
};

/** @hidden */
static int upipe_dvbcsa_dec_check(struct upipe *upipe, struct uref *flow_def);

UPIPE_HELPER_UPIPE(upipe_dvbcsa_dec, upipe, UPIPE_DVBCSA_DEC_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_dvbcsa_dec, urefcount, upipe_dvbcsa_dec_free);
UPIPE_HELPER_VOID(upipe_dvbcsa_dec);
UPIPE_HELPER_OUTPUT(upipe_dvbcsa_dec, output, flow_def, output_state, requests);
UPIPE_HELPER_UCLOCK(upipe_dvbcsa_dec, uclock, uclock_request,
                    upipe_dvbcsa_dec_check,
                    upipe_dvbcsa_dec_register_output_request,
                    upipe_dvbcsa_dec_unregister_output_request);
UPIPE_HELPER_UPUMP_MGR(upipe_dvbcsa_dec, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_dvbcsa_dec, upump, upump_mgr);
UPIPE_HELPER_INPUT(upipe_dvbcsa_dec, urefs, nb_urefs, max_urefs, blockers,
                   NULL);

/** @interal @This frees a dvbcsa decription pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_dvbcsa_dec_free(struct upipe *upipe)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    upipe_throw_dead(upipe);

    struct uchain *uchain;
    while ((uchain = ulist_pop(&upipe_dvbcsa_dec->urefs)))
        uref_free(uref_from_uchain(uchain));
    free(upipe_dvbcsa_dec->pcks);
    dvbcsa_bs_key_free(upipe_dvbcsa_dec->key);
    upipe_dvbcsa_dec_clean_upump(upipe);
    upipe_dvbcsa_dec_clean_upump_mgr(upipe);
    upipe_dvbcsa_dec_clean_uclock(upipe);
    upipe_dvbcsa_dec_clean_input(upipe);
    upipe_dvbcsa_dec_clean_output(upipe);
    upipe_dvbcsa_dec_clean_urefcount(upipe);
    upipe_dvbcsa_dec_free_void(upipe);
}

/** @internal @This allocates and initializes a dvbcsa decription pipe.
 *
 * @param mgr pointer to pipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated and initialized pipe or NULL
 */
static struct upipe *upipe_dvbcsa_dec_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature,
                                            va_list args)
{
    struct upipe *upipe =
        upipe_dvbcsa_dec_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(!upipe))
        return NULL;
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    upipe_dvbcsa_dec_init_urefcount(upipe);
    upipe_dvbcsa_dec_init_output(upipe);
    upipe_dvbcsa_dec_init_input(upipe);
    upipe_dvbcsa_dec_init_uclock(upipe);
    upipe_dvbcsa_dec_init_upump_mgr(upipe);
    upipe_dvbcsa_dec_init_upump(upipe);
    upipe_dvbcsa_dec->key = dvbcsa_bs_key_alloc();
    upipe_dvbcsa_dec->batch_size = dvbcsa_bs_batch_size();
    upipe_dvbcsa_dec->pcks = calloc(upipe_dvbcsa_dec->batch_size + 1,
                                    sizeof (struct dvbcsa_bs_batch_s));
    upipe_dvbcsa_dec->scrambled = 0;

    upipe_throw_ready(upipe);

    if (unlikely(!upipe_dvbcsa_dec->key)) {
        upipe_release(upipe);
        return NULL;
    }

    return upipe;
}

/** @internal @This flushes the retained urefs.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_dvbcsa_dec_flush(struct upipe *upipe)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct uchain urefs;
    struct uref *uref;
    unsigned int scrambled = 0;
    struct dvbcsa_bs_batch_s batch[upipe_dvbcsa_dec->batch_size + 1];
    struct uref *mapped[upipe_dvbcsa_dec->batch_size + 1];
    struct uchain *uchain;
    int ret;

    ulist_init(&urefs);
    while ((uref = upipe_dvbcsa_dec_pop_input(upipe))) {
        size_t ts_header_size = TS_HEADER_SIZE;
        uint8_t buf[TS_HEADER_SIZE];

        const uint8_t *ts_header = uref_block_peek(uref, 0, sizeof (buf), buf);
        if (unlikely(ts_header == NULL)) {
            uref_free(uref);
            ret = UBASE_ERR_ALLOC;
            goto fatal;
        }
        uint8_t scrambling = ts_get_scrambling(ts_header);
        bool has_payload = ts_has_payload(ts_header);
        bool has_adaptation = ts_has_adaptation(ts_header);
        uref_block_peek_unmap(uref, 0, buf, ts_header);

        if (scrambling != 0x2 || !has_payload) {
            ulist_add(&urefs, uref_to_uchain(uref));
            continue;
        }

        if (unlikely(has_adaptation)) {
            uint8_t af_length;
            ret = uref_block_extract(uref, ts_header_size, 1, &af_length);
            if (unlikely(!ubase_check(ret))) {
                uref_free(uref);
                goto fatal;
            }
            if (unlikely(af_length >= 183)) {
                upipe_warn(upipe, "invalid adaptation field received");
                uref_free(uref);
                continue;
            }
            ts_header_size += af_length + 1;
        }

        struct ubuf *ubuf = ubuf_block_copy(uref->ubuf->mgr, uref->ubuf, 0, -1);
        if (unlikely(!ubuf)) {
            uref_free(uref);
            ret = UBASE_ERR_ALLOC;
            goto fatal;
        }
        uref_attach_ubuf(uref, ubuf);
        int size = -1;
        uint8_t *ts;
        int ret = ubuf_block_write(ubuf, 0, &size, &ts);
        if (unlikely(!ubase_check(ret))) {
            uref_free(uref);
            goto fatal;
        }
        ts_set_scrambling(ts, 0);
        batch[scrambled].data = ts + ts_header_size;
        batch[scrambled].len = size - ts_header_size;
        mapped[scrambled] = uref;
        scrambled++;
        ulist_add(&urefs, uref_to_uchain(uref));

        if (scrambled >= upipe_dvbcsa_dec->batch_size) {
            assert(upipe_dvbcsa_dec->scrambled >= scrambled);
            upipe_dvbcsa_dec->scrambled -= scrambled;
            batch[scrambled].data = NULL;
            batch[scrambled].len = 0;
            mapped[scrambled] = NULL;
            dvbcsa_bs_decrypt(upipe_dvbcsa_dec->key, batch, 184);
            for (unsigned i = 0; mapped[i]; i++)
                uref_block_unmap(mapped[i], 0);
            scrambled = 0;
        }
    }

    if (scrambled) {
        assert(upipe_dvbcsa_dec->scrambled >= scrambled);
        upipe_dvbcsa_dec->scrambled -= scrambled;
        batch[scrambled].data = NULL;
        batch[scrambled].len = 0;
        mapped[scrambled] = NULL;
        dvbcsa_bs_decrypt(upipe_dvbcsa_dec->key, batch, 184);
        for (unsigned i = 0; mapped[i]; i++)
            uref_block_unmap(mapped[i], 0);
    }

    while ((uchain = ulist_pop(&urefs)))
        upipe_dvbcsa_dec_output(upipe, uref_from_uchain(uchain), NULL);

    return;

fatal:
    if (scrambled) {
        mapped[scrambled] = NULL;
        for (unsigned i = 0; mapped[i]; i++)
            uref_block_unmap(mapped[i], 0);
    }
    while ((uchain = ulist_pop(&urefs)))
        uref_free(uref_from_uchain(uchain));
    upipe_throw_fatal(upipe, ret);
}

/** @internal @This is called when the upump triggers.
 *
 * @param upump timer
 */
static void upipe_dvbcsa_dec_worker(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    return upipe_dvbcsa_dec_flush(upipe);
}

/** @internal @This handles the input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_dvbcsa_dec_input(struct upipe *upipe,
                                   struct uref *uref,
                                   struct upump **upump_p)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    uint32_t ts_header_size = TS_HEADER_SIZE;
    uint8_t buf[TS_HEADER_SIZE];
    const uint8_t *ts_header = uref_block_peek(uref, 0, sizeof (buf), buf);
    if (unlikely(!ts_header)) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return;
    }
    uint8_t scrambling = ts_get_scrambling(ts_header);
    bool has_payload = ts_has_payload(ts_header);
    uref_block_peek_unmap(uref, 0, buf, ts_header);

    if (upipe_dvbcsa_dec_check_input(upipe) &&
        (scrambling != 0x2 || !has_payload)) {
            upipe_dvbcsa_dec_output(upipe, uref, upump_p);
            return;
    }

    if (upipe_dvbcsa_dec_check_input(upipe))
        upipe_dvbcsa_dec_wait_upump(upipe, UCLOCK_FREQ,
                                    upipe_dvbcsa_dec_worker);
    upipe_dvbcsa_dec_hold_input(upipe, uref);
    if (scrambling == 0x2 && has_payload)
        upipe_dvbcsa_dec->scrambled++;

    if (upipe_dvbcsa_dec->scrambled >= upipe_dvbcsa_dec->batch_size) {
        upipe_dvbcsa_dec_set_upump(upipe, NULL);
        upipe_dvbcsa_dec_flush(upipe);
    }
}

/** @internal @This allocates a new pump if needed.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_dvbcsa_dec_check(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    if (unlikely(!upipe_dvbcsa_dec->uclock))
        upipe_dvbcsa_dec_require_uclock(upipe);

    UBASE_RETURN(upipe_dvbcsa_dec_check_upump_mgr(upipe));
    if (unlikely(!upipe_dvbcsa_dec->upump_mgr))
        return UBASE_ERR_NONE;

    return UBASE_ERR_NONE;
}

/** @internal @This set the output flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new flow format to set
 * @return an error code
 */
static int upipe_dvbcsa_dec_set_flow_def(struct upipe *upipe,
                                         struct uref *flow_def)
{
    UBASE_RETURN(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF));
    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);
    upipe_dvbcsa_dec_store_flow_def(upipe, flow_def_dup);
    return UBASE_ERR_NONE;
}

static inline uint8_t chartohex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return (uint8_t)-1;
}

static inline uint8_t charstobyte(const char c[2])
{
    uint8_t h = chartohex(c[0]), l = chartohex(c[1]);
    return h == (uint8_t)-1 || l == (uint8_t)-1 ? (uint8_t)-1 : h * 16 + l;
}

/** @internal @This sets the decription key.
 *
 * @param upipe description structure of the pipe
 * @param key decription key
 * @return an error code
 */
static int upipe_dvbcsa_dec_set_key_real(struct upipe *upipe,
                                         const char *key)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    if (unlikely(!key) || strlen(key) != 12)
        return UBASE_ERR_INVALID;

    dvbcsa_cw_t cw;
    uint8_t checksum = 0;
    unsigned char i;
    for (i = 0; i < 3; i++, key += 2) {
        uint8_t byte = charstobyte(key);
        if (byte == (uint8_t)-1)
            return UBASE_ERR_INVALID;
        cw[i] = byte;
        checksum += byte;
    }
    cw[i++] = checksum;
    checksum = 0;
    for (; i < 7; i++, key += 2) {
        uint8_t byte = charstobyte(key);
        if (byte == (uint8_t)-1)
            return UBASE_ERR_INVALID;
        cw[i] = byte;
        checksum += byte;
    }
    cw[i] = checksum;

    upipe_notice(upipe, "key changed");
    dvbcsa_bs_key_set(cw, upipe_dvbcsa_dec->key);
    return UBASE_ERR_NONE;
}

/** @internal @This handles the pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_dvbcsa_dec_control_real(struct upipe *upipe,
                                         int command,
                                         va_list args)
{
    UBASE_HANDLED_RETURN(upipe_dvbcsa_dec_control_output(upipe, command, args));

    switch (command) {
        case UPIPE_ATTACH_UPUMP_MGR:
            return upipe_dvbcsa_dec_attach_upump_mgr(upipe);

        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_dvbcsa_dec_set_flow_def(upipe, flow_def);
        }

        case UPIPE_DVBCSA_DEC_SET_KEY: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_DVBCSA_DEC_SIGNATURE);
            const char *key = va_arg(args, const char *);
            return upipe_dvbcsa_dec_set_key_real(upipe, key);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This handles a pipe control command and checks the upump
 * manager.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_dvbcsa_dec_control(struct upipe *upipe,
                                    int command, va_list args)
{
    UBASE_RETURN(upipe_dvbcsa_dec_control_real(upipe, command, args));
    return upipe_dvbcsa_dec_check(upipe, NULL);
}

/** @internal @This is the management structure for dvbcsa decription pipes. */
static struct upipe_mgr upipe_dvbcsa_dec_mgr = {
    /** pipe signature */
    .signature = UPIPE_DVBCSA_DEC_SIGNATURE,
    /** no refcounting needed */
    .refcount = NULL,
    /** pipe allocation */
    .upipe_alloc = upipe_dvbcsa_dec_alloc,
    /** input handler */
    .upipe_input = upipe_dvbcsa_dec_input,
    /** control command handler */
    .upipe_control = upipe_dvbcsa_dec_control,
};

/** @This returns the dvbcsa decrypt pipe management structure.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_dvbcsa_dec_mgr_alloc(void)
{
    return &upipe_dvbcsa_dec_mgr;
}
