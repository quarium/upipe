/*
 * Copyright (C) 2019 OpenHeadend S.A.R.L.
 *
 * Authors: Arnaud de Turckheim
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe sound flow format definitions and helpers.
 */

#ifndef _UPIPE_UREF_SOUND_FLOW_FORMATS_H_
/** @hidden */
#define _UPIPE_UREF_SOUND_FLOW_FORMATS_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "upipe/uref_sound_flow.h"

/** @This describes a sound format. */
struct uref_sound_flow_format {
    /** name */
    const char *name;
    /** sample size */
    uint8_t sample_size;
    /** format is planar? */
    bool planar;
};

/** @This checks a flow format.
 *
 * @param uref uref control packet
 * @param format sound format to check
 * @return an error code
 */
static inline int
uref_sound_flow_check_format(struct uref *uref,
                             const struct uref_sound_flow_format *format)
{
    const char *def;
    UBASE_RETURN(uref_flow_get_def(uref, &def));
    if (ubase_ncmp(def, format->name))
        return UBASE_ERR_INVALID;
    return UBASE_ERR_NONE;
}

/** @This sets a flow format.
 *
 * @param uref uref control packet
 * @param format sound format to set
 * @return an error code
 */
static inline int
uref_sound_flow_set_format(struct uref *uref,
                           const struct uref_sound_flow_format *format,
                           uint8_t channels)
{
    static const char *names = "lrcLRS12345689";
    if (channels > strlen(names))
        return UBASE_ERR_INVALID;

    uref_sound_flow_clear_format(uref);
    UBASE_RETURN(uref_flow_set_def(uref, format->name));
    UBASE_RETURN(uref_sound_flow_set_channels(uref, channels));
    UBASE_RETURN(uref_sound_flow_set_planes(uref, 0));
    if (format->planar) {
        UBASE_RETURN(
            uref_sound_flow_set_sample_size(uref, format->sample_size));
        for (uint8_t c = 0; c < channels; c++) {
            char name[2] = { names[c], '\0' };
            UBASE_RETURN(uref_sound_flow_add_plane(uref, name));
        }
    } else {
        char name[channels + 1];
        memcpy(name, names, channels);
        name[channels] = '\0';
        UBASE_RETURN(uref_sound_flow_set_sample_size(
            uref, channels * format->sample_size));
        UBASE_RETURN(uref_sound_flow_add_plane(uref, name));
    }
    return UBASE_ERR_NONE;
}


/** @This allocates a control packet to define a new sound flow
 * @see uref_sound_flow_alloc_def, and registers the planes according to the
 * format.
 *
 * @param mgr uref management structure
 * @param format desired format to configure
 * @return pointer to uref control packet, or NULL in case of error
 */
static inline struct uref *
uref_sound_flow_alloc_format(struct uref_mgr *mgr,
                             const struct uref_sound_flow_format *format,
                             uint8_t channels)
{
    struct uref *uref = uref_alloc_control(mgr);

    if (unlikely(!uref))
        return NULL;

    if (unlikely(
            !ubase_check(uref_sound_flow_set_format(uref, format, channels)))) {
        uref_free(uref);
        return NULL;
    }

    return uref;
}


#define UREF_SOUND_FLOW_FORMAT_DEFINE(NAME, SAMPLE_SIZE)    \
static const struct uref_sound_flow_format                  \
uref_sound_flow_format_##NAME = {                           \
    .name = "sound." #NAME ".",                             \
    .sample_size = SAMPLE_SIZE,                             \
    .planar = false                                         \
};                                                          \
static const struct uref_sound_flow_format                  \
uref_sound_flow_format_##NAME##_planar = {                  \
    .name = "sound." #NAME ".",                             \
    .sample_size = SAMPLE_SIZE,                             \
    .planar = true                                          \
}

UREF_SOUND_FLOW_FORMAT_DEFINE(u8, 1);
UREF_SOUND_FLOW_FORMAT_DEFINE(s16, 2);
UREF_SOUND_FLOW_FORMAT_DEFINE(s32, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(s64, 8);
UREF_SOUND_FLOW_FORMAT_DEFINE(f32, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(f64, 8);

UREF_SOUND_FLOW_FORMAT_DEFINE(u8be, 1);
UREF_SOUND_FLOW_FORMAT_DEFINE(s16be, 2);
UREF_SOUND_FLOW_FORMAT_DEFINE(s32be, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(s64be, 8);
UREF_SOUND_FLOW_FORMAT_DEFINE(f32be, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(f64be, 8);

UREF_SOUND_FLOW_FORMAT_DEFINE(u8le, 1);
UREF_SOUND_FLOW_FORMAT_DEFINE(s16le, 2);
UREF_SOUND_FLOW_FORMAT_DEFINE(s32le, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(s64le, 8);
UREF_SOUND_FLOW_FORMAT_DEFINE(f32le, 4);
UREF_SOUND_FLOW_FORMAT_DEFINE(f64le, 8);

#define UREF_SOUND_FLOW_FORMAT_FOREACH(Do, ...) \
	Do(u8, ## __VA_ARGS__) \
	Do(s16, ## __VA_ARGS__) \
	Do(s32, ## __VA_ARGS__) \
	Do(s64, ## __VA_ARGS__) \
	Do(f32, ## __VA_ARGS__) \
	Do(f64, ## __VA_ARGS__) \
	Do(u8be, ## __VA_ARGS__) \
	Do(s16be, ## __VA_ARGS__) \
	Do(s32be, ## __VA_ARGS__) \
	Do(s64be, ## __VA_ARGS__) \
	Do(f32be, ## __VA_ARGS__) \
	Do(f64be, ## __VA_ARGS__) \
	Do(u8le, ## __VA_ARGS__) \
	Do(s16le, ## __VA_ARGS__) \
	Do(s32le, ## __VA_ARGS__) \
	Do(s64le, ## __VA_ARGS__) \
	Do(f32le, ## __VA_ARGS__) \
	Do(f64le, ## __VA_ARGS__)

static const struct uref_sound_flow_format *uref_sound_flow_formats[] = {
#define Do(Type)    &uref_sound_flow_format_##Type,
UREF_SOUND_FLOW_FORMAT_FOREACH(Do)
#undef Do
};

static inline const struct uref_sound_flow_format *
uref_sound_flow_get_format(struct uref *uref)
{
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(uref_sound_flow_formats); i++) {
        const struct uref_sound_flow_format *format = uref_sound_flow_formats[i];
        if (ubase_check(uref_sound_flow_check_format(uref, format)))
            return format;
    }
    return NULL;
}

static inline const struct uref_sound_flow_format *
uref_sound_flow_get_format_by_name(const char *name)
{
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(uref_sound_flow_formats); i++)
        if (!strcmp(uref_sound_flow_formats[i]->name, name))
            return uref_sound_flow_formats[i];

    return NULL;
}


#ifdef __cplusplus
}
#endif

#endif
