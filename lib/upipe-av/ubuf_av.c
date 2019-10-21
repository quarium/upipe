/*
 * Copyright (C) 2019 OpenHeadend S.A.R.L.
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

/** @file
 * @short Upipe ubuf manager for libav pictures
 */

#include <upipe/ubase.h>
#include <upipe/ulist_helper.h>
#include <upipe/ubuf.h>
#include <upipe/urefcount_helper.h>
#include <upipe/uref_pic_flow_formats.h>
#include <upipe/uref_sound_flow_formats.h>
#include <upipe-av/upipe_av_pixfmt.h>
#include <upipe-av/upipe_av_samplefmt.h>
#include <upipe-av/ubuf_av.h>

#include <libavutil/frame.h>

#define UBUF_PIC_AV_SIGNATURE UBASE_FOURCC('b','f','a','v')

/** @internal @This stores a reference on an AVFrame buffer. */
struct ref {
    /** AVFrame buffer reference */
    AVBufferRef *ref;
    /** link to the reference list */
    struct uchain uchain;
};

/** @internal @This is the private structure for picture. */
struct ubuf_pic_av {
    /** picture flow format */
    const struct uref_pic_flow_format *flow_format;
    /** picture buffer for negative linesize */
    uint8_t **buf;
};

/** @internal @This is the private structure for sound. */
struct ubuf_sound_av {
    /** sound flow format */
    const struct uref_sound_flow_format *flow_format;
    /** sound planes */
    char **channels;
};

/** @internal @This is the private structure for libav buffers. */
struct ubuf_av {
    /** public structure */
    struct ubuf ubuf;
    /** AVFrame */
    AVFrame *frame;
    /** buffer signature */
    uint32_t signature;
    /** list of mapped buffers */
    struct uchain refs;
    /** private fields */
    union {
        /** for picture */
        struct ubuf_pic_av pic;
        /** for sound */
        struct ubuf_sound_av sound;
    };
};

UBASE_FROM_TO(ubuf_av, ubuf, ubuf, ubuf);
ULIST_HELPER(ubuf_av, refs, ref, uchain);

static inline struct ubuf_pic_av *
ubuf_av_to_ubuf_pic_av(struct ubuf_av *ubuf_av)
{
    return ubuf_av && ubuf_av->signature == UBUF_AV_ALLOC_PICTURE ?
        &ubuf_av->pic : NULL;
}

static inline struct ubuf_sound_av *
ubuf_av_to_ubuf_sound_av(struct ubuf_av *ubuf_av)
{
    return (ubuf_av && ubuf_av->signature == UBUF_AV_ALLOC_SOUND) ?
        &ubuf_av->sound : NULL;
}

/** @internal @This is the libav ubuf manager structure. */
struct ubuf_av_mgr {
    /** refcount management structure */
    struct urefcount urefcount;
    /** common picture management structure */
    struct ubuf_mgr mgr;
};

UBASE_FROM_TO(ubuf_av_mgr, ubuf_mgr, ubuf_mgr, mgr);
UREFCOUNT_HELPER(ubuf_av_mgr, urefcount, ubuf_av_mgr_free);

/** @internal @This frees a libav ubuf.
 *
 * @param ubuf pointer to buffer
 */
static void ubuf_av_free(struct ubuf *ubuf)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);

    if (ubuf_sound_av) {
        char **channels = ubuf_sound_av->channels;
        while (channels && *channels)
            free(*channels++);
        free(ubuf_sound_av->channels);
    }
    if (ubuf_pic_av) {
        uint8_t **buf = ubuf_pic_av->buf;
        for (uint8_t i = 0; buf && i < ubuf_pic_av->flow_format->nb_planes; i++)
            free(buf[i]);
        free(buf);
    }
    av_frame_unref(ubuf_av->frame);
    av_frame_free(&ubuf_av->frame);
    ubuf_mgr_release(ubuf->mgr);
    ubuf_av_clean_refs(ubuf_av);
    free(ubuf_av);
}

/** @internal @This allocates a libav ubuf.
 *
 * @param mgr ubuf manager
 * @param signature signature of the ubuf allocator
 * @param args arguments
 * @return a pointer to a new ubuf or NULL in case of allocation error
 */
static struct ubuf *ubuf_av_alloc(struct ubuf_mgr *mgr,
                                  uint32_t signature,
                                  va_list args)
{
    if (signature != UBUF_AV_ALLOC_PICTURE &&
        signature != UBUF_AV_ALLOC_SOUND)
        return NULL;

    AVFrame *frame = va_arg(args, AVFrame *);

    struct ubuf_av *ubuf_av = calloc(1, sizeof (*ubuf_av));
    if (unlikely(!ubuf_av))
        return NULL;

    ubuf_av_init_refs(ubuf_av);

    struct ubuf *ubuf = ubuf_av_to_ubuf(ubuf_av);
    ubuf->mgr = ubuf_mgr_use(mgr);

    ubuf_av->frame = av_frame_alloc();
    if (unlikely(!ubuf_av->frame)) {
        free(ubuf_av);
        return NULL;
    }
    av_frame_ref(ubuf_av->frame, frame);
    ubuf_av->signature = signature;
    switch (signature) {
        case UBUF_AV_ALLOC_PICTURE:
            ubuf_av->pic.buf = NULL;
            ubuf_av->pic.flow_format = upipe_av_pixfmt_to_format(frame->format);
            if (unlikely(!ubuf_av->pic.flow_format)) {
                free(ubuf_av);
                return NULL;
            }
            break;
        case UBUF_AV_ALLOC_SOUND:
            ubuf_av->sound.flow_format =
                upipe_av_samplefmt_to_flow_format(frame->format);
            if (unlikely(!ubuf_av->sound.flow_format)) {
                free(ubuf_av);
                return NULL;
            }
            uint8_t planes = 1;
            if (ubuf_av->sound.flow_format->planar)
                planes = frame->channels;
            ubuf_av->sound.channels = calloc(planes + 1, sizeof (char *));
            if (unlikely(!ubuf_av->sound.channels)) {
                free(ubuf_av);
                return NULL;
            }
            const char *channels_desc = UPIPE_AV_SAMPLEFMT_CHANNELS;
            if (unlikely(frame->channels >= strlen(channels_desc))) {
                ubuf_av_free(ubuf);
                return NULL;
            }
            if (ubuf_av->sound.flow_format->planar) {
                for (unsigned i = 0; i < planes; i++) {
                    char tmp[2] = { channels_desc[i], '\0' };
                    ubuf_av->sound.channels[i] = strdup(tmp);
                    if (unlikely(!ubuf_av->sound.channels[i])) {
                        ubuf_av_free(ubuf);
                        return NULL;
                    }
                }
            }
            else {
                char tmp[frame->channels + 1];
                memcpy(tmp, channels_desc, frame->channels);
                ubuf_av->sound.channels[0] = strdup(tmp);
                if (unlikely(!ubuf_av->sound.channels[0])) {
                    ubuf_av_free(ubuf);
                    return NULL;
                }
            }
            ubuf_av->sound.channels[planes] = NULL;
            break;
    }

    return ubuf;
}

/** @internal @This asks for the creation of a new reference to the same buffer
 * space.
 *
 * @param ubuf pointer to buffer
 * @param new_ubuf_p reference written with a pointer to the newly allocated
 * @return an error code
 */
static int ubuf_av_dup(struct ubuf *ubuf, struct ubuf **new_ubuf_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf *new_ubuf =
        ubuf_alloc(ubuf->mgr, ubuf_av->signature, ubuf_av->frame);
    if (!new_ubuf)
        return UBASE_ERR_ALLOC;
    if (new_ubuf_p)
        *new_ubuf_p = new_ubuf;
    else
        ubuf_free(new_ubuf);
    return UBASE_ERR_NONE;
}

/** @internal @This returns the picture size.
 *
 * @param ubuf pointer to buffer
 * @param hsize_p filled with the horizontal size in pixels
 * @param vsize_p filled with the vertical size in lines
 * @param macropixel_p filled with the macropixel size
 * @return an error code
 */
static int ubuf_pic_av_size(struct ubuf *ubuf,
                            size_t *hsize_p,
                            size_t *vsize_p,
                            uint8_t *macropixel_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);
    const AVFrame *frame = ubuf_av->frame;

    if (unlikely(!ubuf_pic_av))
        return UBASE_ERR_INVALID;
    if (hsize_p)
        *hsize_p = frame->width;
    if (vsize_p)
        *vsize_p = frame->height;
    if (macropixel_p)
        *macropixel_p = ubuf_pic_av->flow_format->macropixel;
    return UBASE_ERR_NONE;
}

/** @internal @This iterates the ubuf planes.
 *
 * @param ubuf pointer to buffer
 * @param chroma_p previous planes or NULL, filled with the next plane
 * @return an error code
 */
static int ubuf_pic_av_iterate_plane(struct ubuf *ubuf,
                                     const char **chroma_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);

    if (!chroma_p || !ubuf_pic_av)
        return UBASE_ERR_INVALID;
    if (!*chroma_p) {
        if (!ubuf_pic_av->flow_format->nb_planes)
            return UBASE_ERR_NONE;
        *chroma_p = ubuf_pic_av->flow_format->planes[0].chroma;
        return UBASE_ERR_NONE;
    }

    const struct uref_pic_flow_format_plane *current = NULL;
    const struct uref_pic_flow_format_plane *plane;
    uref_pic_flow_format_foreach_plane(ubuf_pic_av->flow_format, plane) {
        if (current) {
            *chroma_p = plane->chroma;
            return UBASE_ERR_NONE;
        }
        if (!strcmp(*chroma_p, plane->chroma))
            current = plane;
    }

    *chroma_p = NULL;
    return UBASE_ERR_NONE;
}

/** @internal @This returns the picture plane size.
 *
 * @param ubuf pointer to buffer
 * @param chroma chroma plane to get the size from
 * @param stride_p filled with the stride size
 * @param hsub_p filled with the horizontal subsampling
 * @param vsub_p filled with the vertical subsampling
 * @param macropixel_size_p filled with the macropixel size
 * @return an error code
 */
static int ubuf_pic_av_plane_size(struct ubuf *ubuf,
                                  const char *chroma,
                                  size_t *stride_p,
                                  uint8_t *hsub_p,
                                  uint8_t *vsub_p,
                                  uint8_t *macropixel_size_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);

    if (unlikely(!ubuf_pic_av))
        return UBASE_ERR_INVALID;

    const struct uref_pic_flow_format_plane *plane =
        uref_pic_flow_format_get_plane(ubuf_pic_av->flow_format, chroma);
    const AVFrame *frame = ubuf_av->frame;

    if (!plane)
        return UBASE_ERR_INVALID;

    uint8_t plane_id = uref_pic_flow_format_get_plane_id(
        ubuf_pic_av->flow_format, plane);
    if (plane_id == UINT8_MAX)
        return UBASE_ERR_INVALID;

    if (stride_p) {
        if (frame->linesize[plane_id] >= 0)
            *stride_p = frame->linesize[plane_id];
        else
            *stride_p = -frame->linesize[plane_id];
    }
    if (hsub_p)
        *hsub_p = plane->hsub;
    if (vsub_p)
        *vsub_p = plane->vsub;
    if (macropixel_size_p)
        *macropixel_size_p = plane->mpixel_size;
    return UBASE_ERR_NONE;
}

/** @internal @This gets the plane_id of a plane.
 *
 * @param ubuf pointer to buffer
 * @param chroma chroma plane name
 * @param id_p filled with the plane id
 * @return an error code
 */
static int ubuf_pic_av_get_plane_id(struct ubuf *ubuf,
                                    const char *chroma,
                                    int *id_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);

    if (unlikely(!ubuf_pic_av))
        return UBASE_ERR_INVALID;

    int id = 0;
    const struct uref_pic_flow_format_plane *plane;
    uref_pic_flow_format_foreach_plane(ubuf_pic_av->flow_format, plane) {
        if (chroma && !strcmp(chroma, plane->chroma)) {
            if (id_p)
                *id_p = id;
            return UBASE_ERR_NONE;
        }
        id++;
    }
    return UBASE_ERR_INVALID;
}

static int ubuf_av_ref(struct ubuf *ubuf, AVBufferRef *av_ref)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ref *ref = malloc(sizeof (*ref));
    UBASE_ALLOC_RETURN(ref);
    ref->ref = av_buffer_ref(av_ref);
    ubuf_av_add_ref(ubuf_av, ref);
    return UBASE_ERR_NONE;
}

static int ubuf_av_unref(struct ubuf *ubuf)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ref *ref = ubuf_av_pop_ref(ubuf_av);
    if (unlikely(!ref))
        return UBASE_ERR_INVALID;

    av_buffer_unref(&ref->ref);
    free(ref);
    return UBASE_ERR_NONE;
}

/** @internal @This maps a plane for read.
 *
 * @param ubuf pointer to buffer
 * @param chroma chroma plane to map
 * @param hoffset horizontal offset in the plane
 * @param voffset vertical offset in the plane
 * @param hsize horizontal size in pixels
 * @param vsize vertical size in lines
 * @param writable true if the plane is mapped for write
 * @param buffer_p filled with the pointer for read
 * @return an error code
 */
static int ubuf_pic_av_plane_map(struct ubuf *ubuf,
                                 const char *chroma,
                                 int hoffset, int voffset,
                                 int hsize, int vsize,
                                 bool writable,
                                 uint8_t **buffer_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_pic_av *ubuf_pic_av = ubuf_av_to_ubuf_pic_av(ubuf_av);
    AVFrame *frame = ubuf_av->frame;

    if (!ubuf_pic_av)
        return UBASE_ERR_INVALID;

    if (hoffset < 0)
        hoffset = frame->width + hoffset;
    if (voffset < 0)
        voffset = frame->height + voffset;
    if (hsize < 0)
        hsize = frame->width - hoffset;
    if (vsize < 0)
        vsize = frame->height - voffset;
    if (hsize < 0 || vsize < 0 || hoffset < 0 || voffset < 0 ||
        voffset + vsize > frame->height ||
        hoffset + hsize > frame->width)
        return UBASE_ERR_INVALID;

    int plane_id;
    UBASE_RETURN(ubuf_pic_av_get_plane_id(ubuf, chroma, &plane_id));

    if (writable &&
        (!frame->buf[plane_id] || !av_buffer_is_writable(frame->buf[plane_id])))
        return UBASE_ERR_INVALID;

    if (frame->buf[plane_id])
        UBASE_RETURN(ubuf_av_ref(ubuf, frame->buf[plane_id]));

    if (frame->linesize[plane_id] < 0) {
        /* upipe does not support negative linesize, so we need to copy the
         * plane buffer to reorder the lines */
        if (writable)
            return UBASE_ERR_INVALID;
        size_t stride = -frame->linesize[plane_id];
        if (!ubuf_pic_av->buf) {
            ubuf_pic_av->buf = calloc(ubuf_pic_av->flow_format->nb_planes,
                                      sizeof (uint8_t *));
            UBASE_ALLOC_RETURN(ubuf_pic_av->buf);
        }
        if (!ubuf_pic_av->buf[plane_id]) {
            ubuf_pic_av->buf[plane_id] = malloc(vsize * stride * hsize);
            UBASE_ALLOC_RETURN(ubuf_pic_av->buf[plane_id]);
        }
        for (int i = 0; i < frame->height; i++)
            /* TODO: maybe we don't need to copy the whole plane buffer */
            memcpy(ubuf_pic_av->buf[plane_id] + i * stride,
                   frame->data[plane_id] + i * frame->linesize[plane_id],
                   stride);
        *buffer_p = ubuf_pic_av->buf[plane_id] +
            voffset * stride + hoffset;
    }
    else {
        *buffer_p = frame->data[plane_id] +
            voffset * frame->linesize[plane_id] + hoffset;
    }
    return UBASE_ERR_NONE;
}

/** @internal @This unmaps a plane.
 *
 * @param ubuf pointer to buffer
 * @param chroma chroma plane to unmap
 * @param hoffset horizontal offset in the plane
 * @param voffset vertical offset in the plane
 * @param hsize horizontal size in pixels
 * @param vsize vertical size in lines
 * @return an error code
 */
static int ubuf_pic_av_plane_unmap(struct ubuf *ubuf,
                                   const char *chroma,
                                   int hoffset,
                                   int voffset,
                                   int hsize,
                                   int vsize)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);

    int plane_id;
    UBASE_RETURN(ubuf_pic_av_get_plane_id(ubuf, chroma, &plane_id));

    AVFrame *frame = ubuf_av->frame;
    if (frame->buf[plane_id])
        return ubuf_av_unref(ubuf);
    return UBASE_ERR_NONE;
}

/** @internal @This returns the sample size of an av sound ubuf.
 *
 * @param ubuf pointer to buffer
 * @param sample_size_p filled with the sample size
 * @return an error code
 */
static int ubuf_sound_av_sample_size(struct ubuf *ubuf,
                                     uint8_t *sample_size_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);
    uint8_t sample_size = ubuf_sound_av->flow_format->sample_size;

    if (!ubuf_sound_av->flow_format->planar)
        sample_size *= ubuf_av->frame->channels;
    if (sample_size_p)
        *sample_size_p = sample_size;
    return UBASE_ERR_NONE;
}

/** @internal @This returns the sound buffer size.
 *
 * @param ubuf pointer to buffer
 * @param size_p the number of samples of the sound
 * @param sample_size_p the size of a sample in bytes
 * @return an error code
 */
static int ubuf_sound_av_size(struct ubuf *ubuf,
                              size_t *size_p,
                              uint8_t *sample_size_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);
    const AVFrame *frame = ubuf_av->frame;

    if (unlikely(!ubuf_sound_av))
        return UBASE_ERR_INVALID;

    if (size_p)
        *size_p = frame->nb_samples;
    return ubuf_sound_av_sample_size(ubuf, sample_size_p);
}

/** @internal @This iterates the sound planes.
 *
 * @param ubuf pointer to buffer
 * @param channel_p the previous channel name or NULL, filled with the next channel 
 * name
 * @return an error code
 */
static int ubuf_sound_av_iterate_plane(struct ubuf *ubuf,
                                       const char **channel_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);

    if (unlikely(!channel_p))
        return UBASE_ERR_INVALID;

    char **channels = ubuf_sound_av->channels;
    if (!*channel_p) {
        *channel_p = channels ? channels[0] : NULL;
        return UBASE_ERR_NONE;
    }

    while (channels && *channels) {
        if (!strcmp(*channels, *channel_p)) {
            *channel_p = *++channels;
            return UBASE_ERR_NONE;
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This resizes a sound buffer.
 *
 * @param ubuf pointer to buffer
 * @param offset new offset to set
 * @param new_size new size from the offset
 * @return an error code
 */
static int ubuf_sound_av_resize(struct ubuf *ubuf, int offset, int new_size)
{
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This gets the channel_id of a plane.
 *
 * @param ubuf pointer to buffer
 * @param channel channel name
 * @param id_p filled with the channel id
 * @return an error code
 */
static int ubuf_sound_av_get_channel_id(struct ubuf *ubuf,
                                        const char *channel,
                                        int *id_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);

    if (unlikely(!channel))
        return UBASE_ERR_INVALID;
    if (unlikely(!ubuf_sound_av))
        return UBASE_ERR_INVALID;

    char **channels = ubuf_sound_av->channels;
    for (int id = 0; channels && *channels; id++, channels++)
        if (!strcmp(channel, *channels)) {
            if (id_p)
                *id_p = id;
            return UBASE_ERR_NONE;
        }
    return UBASE_ERR_INVALID;
}

/** @internal @This maps a sound plane for read.
 *
 * @param ubuf pointer to buffer
 * @param channel channel to map
 * @param offset offset to map
 * @param size size from the offset to map
 * @param writable true if the plane is mapped for write
 * @param buffer_p filled with the pointer for read
 * @return an error code
 */
static int ubuf_sound_av_plane_map(struct ubuf *ubuf, const char *channel,
                                   int offset, int size, bool writable,
                                   uint8_t **buffer_p)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);
    AVFrame *frame = ubuf_av->frame;
    struct ubuf_sound_av *ubuf_sound_av = ubuf_av_to_ubuf_sound_av(ubuf_av);

    if (unlikely(!ubuf_sound_av))
        return UBASE_ERR_INVALID;

    int channel_id = -1;
    UBASE_RETURN(ubuf_sound_av_get_channel_id(ubuf, channel, &channel_id));

    size_t samples;
    uint8_t sample_size;
    UBASE_RETURN(ubuf_sound_av_size(ubuf, &samples, &sample_size));

    if (offset < 0)
        offset = samples + offset;
    if (size < 0)
        size = samples - offset;
    if (size < 0 || offset + size > samples)
        return UBASE_ERR_INVALID;

    if (writable &&
        (!frame->buf[channel_id] ||
         !av_buffer_is_writable(frame->buf[channel_id])))
        return UBASE_ERR_INVALID;

    if (frame->buf[channel_id])
        UBASE_RETURN(ubuf_av_ref(ubuf, frame->buf[channel_id]));

    if (buffer_p)
        *buffer_p = frame->data[channel_id] + offset * sample_size;
    return UBASE_ERR_NONE;
}

/** @internal @This unmaps a sound plane.
 *
 * @param channel channel to unmap
 * @param offset offset to unmap
 * @param size size from the offset to unmap
 * @return an error code
 */
static int ubuf_sound_av_plane_unmap(struct ubuf *ubuf,
                                     const char *channel,
                                     int offset,
                                     int size)
{
    struct ubuf_av *ubuf_av = ubuf_av_from_ubuf(ubuf);

    int channel_id;
    UBASE_RETURN(ubuf_sound_av_get_channel_id(ubuf, channel, &channel_id));

    AVFrame *frame = ubuf_av->frame;
    if (frame->buf[channel_id])
        return ubuf_av_unref(ubuf);
    return UBASE_ERR_NONE;
}

/** @internal @This handles buffer control commands.
 *
 * @param ubuf pointer to buffer
 * @param command command to handle
 * @param args optional arguments
 * @return an error code
 */
static int ubuf_av_control(struct ubuf *ubuf, int command, va_list args)
{
    switch (command) {
        case UBUF_DUP: {
            struct ubuf **new_ubuf_p = va_arg(args, struct ubuf **);
            return ubuf_av_dup(ubuf, new_ubuf_p);
        }

        case UBUF_SIZE_PICTURE: {
            size_t *hsize_p = va_arg(args, size_t *);
            size_t *vsize_p = va_arg(args, size_t *);
            uint8_t *macropixel_p = va_arg(args, uint8_t *);
            return ubuf_pic_av_size(ubuf, hsize_p, vsize_p, macropixel_p);
        }
        case UBUF_ITERATE_PICTURE_PLANE: {
            const char **chroma_p = va_arg(args, const char **);
            return ubuf_pic_av_iterate_plane(ubuf, chroma_p);
        }
        case UBUF_SIZE_PICTURE_PLANE: {
            const char *chroma = va_arg(args, const char *);
            size_t *stride_p = va_arg(args, size_t *);
            uint8_t *hsub_p = va_arg(args, uint8_t *);
            uint8_t *vsub_p = va_arg(args, uint8_t *);
            uint8_t *macropixel_size_p = va_arg(args, uint8_t *);
            return ubuf_pic_av_plane_size(ubuf, chroma, stride_p,
                                          hsub_p, vsub_p,
                                          macropixel_size_p);
        }
        case UBUF_READ_PICTURE_PLANE:
        case UBUF_WRITE_PICTURE_PLANE: {
            const char *chroma = va_arg(args, const char *);
            int hoffset = va_arg(args, int);
            int voffset = va_arg(args, int);
            int hsize = va_arg(args, int);
            int vsize = va_arg(args, int);
            uint8_t **buffer_p = va_arg(args, uint8_t **);
            bool writable = command == UBUF_WRITE_PICTURE_PLANE;
            return ubuf_pic_av_plane_map(ubuf, chroma, hoffset, voffset,
                                         hsize, vsize, writable, buffer_p);
        }
        case UBUF_UNMAP_PICTURE_PLANE: {
            const char *chroma = va_arg(args, const char *);
            int hoffset = va_arg(args, int);
            int voffset = va_arg(args, int);
            int hsize = va_arg(args, int);
            int vsize = va_arg(args, int);
            return ubuf_pic_av_plane_unmap(ubuf, chroma, hoffset, voffset,
                                           hsize, vsize);
        }

        case UBUF_SIZE_SOUND: {
            size_t *size_p = va_arg(args, size_t *);
            uint8_t *sample_size_p = va_arg(args, uint8_t *);
            return ubuf_sound_av_size(ubuf, size_p, sample_size_p);
        }
        case UBUF_ITERATE_SOUND_PLANE: {
            const char **channel_p = va_arg(args, const char **);
            return ubuf_sound_av_iterate_plane(ubuf, channel_p);
        }
        case UBUF_READ_SOUND_PLANE:
        case UBUF_WRITE_SOUND_PLANE: {
            const char *chroma = va_arg(args, const char *);
            int offset = va_arg(args, int);
            int size = va_arg(args, int);
            uint8_t **buffer_p = va_arg(args, uint8_t **);
            bool writable = command == UBUF_WRITE_SOUND_PLANE;
            return ubuf_sound_av_plane_map(ubuf, chroma, offset,
                                           size, writable, buffer_p);
        }
        case UBUF_UNMAP_SOUND_PLANE: {
            const char *channel = va_arg(args, const char *);
            int offset = va_arg(args, int);
            int size = va_arg(args, int);
            return ubuf_sound_av_plane_unmap(ubuf, channel, offset, size);
        }
        case UBUF_RESIZE_SOUND: {
            int offset = va_arg(args, int);
            int new_size = va_arg(args, int);
            return ubuf_sound_av_resize(ubuf, offset, new_size);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This handles the manager control commands.
 *
 * @param mgr pointer to ubuf manager
 * @param command command to handle
 * @param args optional arguments
 * @return an error code
 */
static int ubuf_av_mgr_control(struct ubuf_mgr *mgr,
                               int command, va_list args)
{
    return UBASE_ERR_UNHANDLED;
}

/** @This allocates and initializes an AVFrame buffer manager.
 *
 * @return a pointer to an ubuf manager
 */
struct ubuf_mgr *ubuf_av_mgr_alloc(void)
{
    struct ubuf_av_mgr *ubuf_av_mgr = malloc(sizeof (*ubuf_av_mgr));
    if (unlikely(!ubuf_av_mgr))
        return NULL;

    ubuf_av_mgr_init_urefcount(ubuf_av_mgr);
    ubuf_av_mgr->mgr.refcount = ubuf_av_mgr_to_urefcount(ubuf_av_mgr);
    ubuf_av_mgr->mgr.signature = UBUF_AV_SIGNATURE;
    ubuf_av_mgr->mgr.ubuf_mgr_control = ubuf_av_mgr_control;
    ubuf_av_mgr->mgr.ubuf_alloc = ubuf_av_alloc;
    ubuf_av_mgr->mgr.ubuf_free = ubuf_av_free;
    ubuf_av_mgr->mgr.ubuf_control = ubuf_av_control;

    return ubuf_av_mgr_to_ubuf_mgr(ubuf_av_mgr);
}

/** @internal @This is called when the refcount goes to zero. @This cleans and frees
 * the private AVFrame buffer manager.
 *
 * @param ubuf_av_mgr pointer to the private structure of the manager
 */
static void ubuf_av_mgr_free(struct ubuf_av_mgr *ubuf_av_mgr)
{
    ubuf_av_mgr_clean_urefcount(ubuf_av_mgr);
    free(ubuf_av_mgr);
}
