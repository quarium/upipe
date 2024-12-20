/*
 * Copyright (C) 2024 EasyTools S.A.S.
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

#include "upipe/upipe.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_void.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_flow_format.h"
#include "upipe/upipe_helper_ubuf_mgr.h"
#include "upipe/upipe_helper_input.h"
#include "upipe/ubuf_block.h"
#include "upipe/uref_sound_flow.h"
#include "upipe/uref_sound.h"
#include "upipe-modules/upipe_s302m_encode.h"

/** @internal @This is the expected input flow definition */
#define EXPECTED_FLOW_DEF   "sound."

/** @internal @This is the size of an AES3 header in octets */
#define AES3_HEADER_SIZE 4

/** @internal @This is a lookup table to reverse bits order of a byte */
const uint8_t reverse[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0,
    0x30, 0xB0, 0x70, 0xF0, 0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8, 0x04, 0x84, 0x44, 0xC4,
    0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC,
    0x3C, 0xBC, 0x7C, 0xFC, 0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2, 0x0A, 0x8A, 0x4A, 0xCA,
    0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6,
    0x36, 0xB6, 0x76, 0xF6, 0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE, 0x01, 0x81, 0x41, 0xC1,
    0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9,
    0x39, 0xB9, 0x79, 0xF9, 0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
    0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5, 0x0D, 0x8D, 0x4D, 0xCD,
    0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3,
    0x33, 0xB3, 0x73, 0xF3, 0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
    0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB, 0x07, 0x87, 0x47, 0xC7,
    0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF,
    0x3F, 0xBF, 0x7F, 0xFF,
};

/** @internal @This is the private structure of a s302m encoder */
struct upipe_s302m_enc {
    /** refcount on the structure */
    struct urefcount urefcount;
    /** output pipe */
    struct upipe *output;
    /** input flow def */
    struct uref *input_flow_def;
    /** provided flow format */
    struct uref *flow_format;
    /** output flow def */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** output requests */
    struct uchain requests;
    /** flow format request */
    struct urequest flow_format_request;
    /** ubuf manager request */
    struct urequest ubuf_mgr_request;
    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** retained urefs */
    struct uchain urefs;
    /** number of retained urefs */
    unsigned nb_urefs;
    /** maximum of retained urefs before blocking input */
    unsigned max_urefs;
    /** input blockers */
    struct uchain blockers;
    /** number of input channels */
    uint8_t channels;
    /** number of input planes */
    uint8_t planes;
    /** input sample size */
    uint8_t sample_size;
    /** output sample size in bits */
    uint8_t sample_bits;
    /** number of input samples */
    uint64_t samples;
    /** s302m framing bit counter */
    uint8_t framing_index;
    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static int upipe_s302m_enc_check_flow_format(struct upipe *upipe,
                                             struct uref *flow_format);
/** @hidden */
static int upipe_s302m_enc_check_ubuf_mgr(struct upipe *upipe,
                                          struct uref *flow_format);
/** @hidden */
static bool upipe_s302m_enc_handle(struct upipe *upipe, struct uref *uref,
                                   struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_s302m_enc, upipe, UPIPE_S302M_ENC_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_s302m_enc, urefcount, upipe_s302m_enc_free);
UPIPE_HELPER_VOID(upipe_s302m_enc);
UPIPE_HELPER_OUTPUT(upipe_s302m_enc, output, flow_def, output_state, requests);
UPIPE_HELPER_FLOW_FORMAT(upipe_s302m_enc, flow_format_request,
                         upipe_s302m_enc_check_flow_format,
                         upipe_s302m_enc_register_output_request,
                         upipe_s302m_enc_unregister_output_request);
UPIPE_HELPER_UBUF_MGR(upipe_s302m_enc, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_s302m_enc_check_ubuf_mgr,
                      upipe_s302m_enc_register_output_request,
                      upipe_s302m_enc_unregister_output_request);
UPIPE_HELPER_INPUT(upipe_s302m_enc, urefs, nb_urefs, max_urefs, blockers,
                   upipe_s302m_enc_handle);

/** @internal @This allocates a new S302M encoder.
 *
 * @param mgr upipe manager for S302M pipes
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe
 * @param args optional arguments
 * @return pointer to allocated pipe, or NULL in case of failure
 */
static struct upipe *upipe_s302m_enc_alloc(struct upipe_mgr *mgr,
                                           struct uprobe *uprobe,
                                           uint32_t signature, va_list args)
{
    struct upipe *upipe =
        upipe_s302m_enc_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(!upipe))
        return NULL;

    upipe_s302m_enc_init_urefcount(upipe);
    upipe_s302m_enc_init_output(upipe);
    upipe_s302m_enc_init_flow_format(upipe);
    upipe_s302m_enc_init_ubuf_mgr(upipe);

    struct upipe_s302m_enc *upipe_s302m_enc = upipe_s302m_enc_from_upipe(upipe);
    upipe_s302m_enc->input_flow_def = NULL;
    upipe_s302m_enc->framing_index = 0;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This is called when there is not more reference on the pipe to
 * free it
 *
 * @param upipe description structure of the pipe
 */
static void upipe_s302m_enc_free(struct upipe *upipe)
{
    struct upipe_s302m_enc *upipe_s302m_enc = upipe_s302m_enc_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(upipe_s302m_enc->input_flow_def);
    upipe_s302m_enc_clean_ubuf_mgr(upipe);
    upipe_s302m_enc_clean_flow_format(upipe);
    upipe_s302m_enc_clean_output(upipe);
    upipe_s302m_enc_clean_urefcount(upipe);
    upipe_s302m_enc_free_void(upipe);
}

/** @internal @This sets the input flow format of the pipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow format to set
 */
static void upipe_s302m_enc_set_flow_def_real(struct upipe *upipe,
                                              struct uref *flow_def)
{
    struct upipe_s302m_enc *upipe_s302m_enc = upipe_s302m_enc_from_upipe(upipe);
    uint8_t sample_bits = 0;

    uref_sound_flow_get_channels(flow_def, &upipe_s302m_enc->channels);
    uref_sound_flow_get_planes(flow_def, &upipe_s302m_enc->planes);
    uref_sound_flow_get_sample_size(flow_def, &upipe_s302m_enc->sample_size);
    uref_sound_flow_get_samples(flow_def, &upipe_s302m_enc->samples);
    uref_sound_flow_get_raw_sample_size(flow_def, &sample_bits);

    if (upipe_s302m_enc->sample_size == 2)
        upipe_s302m_enc->sample_bits = 16;
    else {
        if (sample_bits == 24 || sample_bits == 20 || sample_bits == 16)
            upipe_s302m_enc->sample_bits = sample_bits;
        else
            upipe_s302m_enc->sample_bits = 24;
    }

    uref_free(upipe_s302m_enc->input_flow_def);
    upipe_s302m_enc->input_flow_def = flow_def;

    struct uref *flow_format = uref_sibling_alloc_control(flow_def);
    uref_flow_set_def(flow_format, "block.s302m.");
    upipe_s302m_enc_require_flow_format(upipe, flow_format);
}

/** @internal @This encodes buffer into S302M.
 *
 * @param upipe description structure of the pipe
 * @param uref buffer to encode
 * @param upump_p reference to pump that generated the buffer
 * @return false if the buffer must be buffered, true if it has been sent or
 * dropped
 */
static bool upipe_s302m_enc_handle(struct upipe *upipe, struct uref *uref,
                                   struct upump **upump_p)
{
    struct upipe_s302m_enc *upipe_s302m_enc = upipe_s302m_enc_from_upipe(upipe);
    int ret;

    if (unlikely(ubase_check(uref_flow_get_def(uref, NULL)))) {
        upipe_s302m_enc_set_flow_def_real(upipe, uref);
        return true;
    }

    if (!upipe_s302m_enc->input_flow_def) {
        upipe_warn(upipe, "no input flow def set, dropping");
        uref_free(uref);
        return true;
    }

    if (!upipe_s302m_enc->ubuf_mgr)
        return false;

    const uint8_t *in[upipe_s302m_enc->planes];
    ret = uref_sound_read_uint8_t(uref, 0, -1, in, upipe_s302m_enc->planes);
    if (unlikely(!ubase_check(ret))) {
        upipe_err(upipe, "fail to read sound buffer");
        uref_free(uref);
        return true;
    }
    uint64_t samples = upipe_s302m_enc->samples;
    uint8_t channels = upipe_s302m_enc->channels;
    uint8_t sample_bits = upipe_s302m_enc->sample_bits;
    int out_size = AES3_HEADER_SIZE;
    out_size += (samples * channels * (sample_bits + 4)) / 8;
    struct ubuf *ubuf = ubuf_block_alloc(upipe_s302m_enc->ubuf_mgr, out_size);
    if (unlikely(!ubuf)) {
        upipe_err(upipe, "fail to allocate block");
        uref_sound_unmap(uref, 0, -1, upipe_s302m_enc->planes);
        uref_free(uref);
        return true;
    }
    int size = -1;
    uint8_t *out = NULL;
    ret = ubuf_block_write(ubuf, 0, &size, &out);
    if (unlikely(!ubase_check(ret))) {
        upipe_err(upipe, "fail to write block");
        uref_sound_unmap(uref, 0, -1, upipe_s302m_enc->planes);
        uref_free(uref);
        return true;
    }

    /* AES Header */
    out[0] = (uint16_t)(out_size - AES3_HEADER_SIZE) >> 8;
    out[1] = (uint16_t)(out_size - AES3_HEADER_SIZE) & 0xff;
    out[2] = ((channels / 2) - 1) << 6;
    out[3] = ((sample_bits - 16) / 4) << 4;

    uint8_t *o = out + AES3_HEADER_SIZE;
    for (uint64_t i = 0; i < samples; ) {
        for (uint8_t c = 0; c < channels; c += 2) {
            uint32_t s1, s2;
            uint8_t vucf = 0;

            if (upipe_s302m_enc->sample_size == 2) {
                if (upipe_s302m_enc->planes == 1) {
                    s1 = ((uint16_t *)in[0])[i * channels + c];
                    s2 = ((uint16_t *)in[0])[i * channels + c + 1];
                } else {
                    s1 = ((uint16_t *)in[c])[i];
                    s2 = ((uint16_t *)in[c + 1])[i];
                }
            } else {
                if (upipe_s302m_enc->planes == 1) {
                    s1 = ((uint32_t *)in[0])[i * channels + c];
                    s2 = ((uint32_t *)in[0])[i * channels + c + 1];
                } else {
                    s1 = ((uint32_t *)in[c])[i];
                    s2 = ((uint32_t *)in[c + 1])[i];
                }
            }

            if (sample_bits == 24) {
                vucf = upipe_s302m_enc->framing_index == 0 ? 0x10 : 0;

                o[0] = reverse[(s1 & 0x0000FF00) >>  8];
                o[1] = reverse[(s1 & 0x00FF0000) >> 16];
                o[2] = reverse[(s1 & 0xFF000000) >> 24];
                o[3] = reverse[(s2 & 0x00000F00) >>  4] | vucf;
                o[4] = reverse[(s2 & 0x000FF000) >> 12];
                o[5] = reverse[(s2 & 0x0FF00000) >> 20];
                o[6] = reverse[(s2 & 0xF0000000) >> 28];

            } else if (sample_bits == 20) {
                vucf = upipe_s302m_enc->framing_index == 0 ? 0x80 : 0;

                o[0] = reverse[ (s1 & 0x000FF000) >> 12];
                o[1] = reverse[ (s1 & 0x0FF00000) >> 20];
                o[2] = reverse[((s1 & 0xF0000000) >> 28) | vucf];
                o[3] = reverse[ (s2 & 0x000FF000) >> 12];
                o[4] = reverse[ (s2 & 0x0FF00000) >> 20];
                o[5] = reverse[ (s2 & 0xF0000000) >> 28];

            } else {
                vucf = upipe_s302m_enc->framing_index == 0 ? 0x10 : 0;

                o[0] = reverse[(s1 & 0x00FF) >>  0];
                o[1] = reverse[(s1 & 0xFF00) >>  8];
                o[2] = reverse[(s2 & 0x000F) <<  4] | vucf;
                o[3] = reverse[(s2 & 0x0FF0) >>  4];
                o[4] = reverse[(s2 & 0xF000) >> 12];
            }

            o += 5 + (sample_bits - 16) / 4;
        }

        upipe_s302m_enc->framing_index++;
        if (upipe_s302m_enc->framing_index >= 192)
            upipe_s302m_enc->framing_index = 0;
    }

    uref_sound_unmap(uref, 0, -1, upipe_s302m_enc->planes);
    ubuf_block_unmap(ubuf, 0);
    uref_attach_ubuf(uref, ubuf);
    upipe_s302m_enc_output(upipe, uref, upump_p);
    return true;
}

/** @internal @This handle input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref buffer to handle
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_s302m_enc_input(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p)
{
    if (!upipe_s302m_enc_check_input(upipe)) {
        upipe_s302m_enc_hold_input(upipe, uref);
        upipe_s302m_enc_block_input(upipe, upump_p);
    } else if (!upipe_s302m_enc_handle(upipe, uref, upump_p)) {
        upipe_use(upipe);
        upipe_s302m_enc_hold_input(upipe, uref);
        upipe_s302m_enc_block_input(upipe, upump_p);
    }
}

/** @internal @This is called when the downstream ubuf manager changes.
 *
 * @param upipe description structure of the pipe
 * @param flow_format new flow format of the ubuf manager
 * @return an error code
 */
static int upipe_s302m_enc_check_ubuf_mgr(struct upipe *upipe,
                                          struct uref *flow_format)
{
    if (unlikely(!flow_format))
        return UBASE_ERR_INVALID;

    upipe_s302m_enc_store_flow_def(upipe, flow_format);

    if (!upipe_s302m_enc_check_input(upipe)) {
        bool emptied = upipe_s302m_enc_output_input(upipe);
        upipe_s302m_enc_unblock_input(upipe);

        if (emptied)
            upipe_release(upipe);
    }
    return UBASE_ERR_NONE;
}

/** @internal @This is called for downstream flow format negotiations.
 *
 * @param upipe description structure of the pipe
 * @param flow_format new flow format
 * @return an error code
 */
static int upipe_s302m_enc_check_flow_format(struct upipe *upipe,
                                             struct uref *flow_format)
{
    upipe_s302m_enc_require_ubuf_mgr(upipe, flow_format);
    return UBASE_ERR_NONE;
}

/** @internal @This checks a new input flow definition and pushes inband.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new flow definition
 * @return an error code
 */
static int upipe_s302m_enc_set_flow_def(struct upipe *upipe,
                                        struct uref *flow_def)
{
    if (unlikely(!flow_def))
        return UBASE_ERR_INVALID;

    if (unlikely(
            !ubase_check(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF))))
        return UBASE_ERR_INVALID;

    uint8_t channels = 0;
    uint8_t planes = 0;
    uint8_t sample_size = 0;
    uint64_t samples = 0;
    UBASE_RETURN(uref_sound_flow_get_channels(flow_def, &channels));
    UBASE_RETURN(uref_sound_flow_get_planes(flow_def, &planes));
    UBASE_RETURN(uref_sound_flow_get_sample_size(flow_def, &sample_size));
    UBASE_RETURN(uref_sound_flow_get_samples(flow_def, &samples));

    if (!channels || channels > 8 || channels % 2)
        return UBASE_ERR_INVALID;
    if (!planes || (planes != 1 && planes != channels))
        return UBASE_ERR_INVALID;
    if (sample_size != 4 && sample_size != 2)
        return UBASE_ERR_INVALID;

    struct uref *flow_def_dup = uref_dup(flow_def);
    if (unlikely(!flow_def_dup))
        return UBASE_ERR_ALLOC;

    upipe_input(upipe, flow_def, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This handle control command for the pipe.
 *
 * @param upipe description structure of the pipe
 * @param cmd control command to handle
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_s302m_enc_control(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_s302m_enc_control_output(upipe, cmd, args));
    switch (cmd) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_s302m_enc_set_flow_def(upipe, flow_def);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This is the static pipe manager for S302M encoders. */
static struct upipe_mgr upipe_s302m_enc_mgr = {
    .refcount = NULL,
    .signature = UPIPE_S302M_ENC_SIGNATURE,
    .upipe_alloc = upipe_s302m_enc_alloc,
    .upipe_control = upipe_s302m_enc_control,
    .upipe_input = upipe_s302m_enc_input,
};

/** @This returns the management structure for all s302m encoders.
 *
 * @return pointer to the manager
 */
struct upipe_mgr *upipe_s302m_enc_mgr_alloc(void)
{
    return &upipe_s302m_enc_mgr;
}
