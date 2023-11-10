/*
 * Copyright (C) 2023 EasyTools
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
 * @short upipe/avutil pixeldesc conversion
 */

#ifndef _UPIPE_AV_UPIPE_AV_PIXDESC_H_
/** @hidden */
#define _UPIPE_AV_UPIPE_AV_PIXDESC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <libavutil/pixdesc.h>

#include "upipe/ubase.h"

/** @This describes a pixel description conversion between upipe and libav. */
struct upipe_av_pixdesc {
    /** Upipe color name */
    const char *upipe_name;
    /** av color value */
    int av_value;
};

/** @This lists color primaries conversions between upipe and libav. */
static const struct upipe_av_pixdesc upipe_av_color_primaries[] = {
    { "bt709", AVCOL_PRI_BT709 },
    { "bt470m", AVCOL_PRI_BT470M },
    { "bt470bg", AVCOL_PRI_BT470BG },
    { "smpte170m", AVCOL_PRI_SMPTE170M },
    { "smpte240m", AVCOL_PRI_SMPTE240M },
    { "film", AVCOL_PRI_FILM },
    { "bt2020", AVCOL_PRI_BT2020 },
    { "smpte428", AVCOL_PRI_SMPTE428 },
    { "smpte431", AVCOL_PRI_SMPTE431 },
    { "smpte432", AVCOL_PRI_SMPTE432 },
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 34, 100)
    { "ebu3213", AVCOL_PRI_EBU3213 },
#endif
};

/** @This lists color spaces conversions between upipe and libav. */
static const struct upipe_av_pixdesc upipe_av_color_space[] = {
    { "GBR", AVCOL_SPC_RGB },
    { "bt709", AVCOL_SPC_BT709 },
    { "fcc", AVCOL_SPC_FCC },
    { "bt470bg", AVCOL_SPC_BT470BG },
    { "smpte170m", AVCOL_SPC_SMPTE170M },
    { "smpte240m", AVCOL_SPC_SMPTE240M },
    { "YCgCo", AVCOL_SPC_YCGCO },
    { "bt2020nc", AVCOL_SPC_BT2020_NCL },
    { "bt2020c", AVCOL_SPC_BT2020_CL },
    { "smpte2085", AVCOL_SPC_SMPTE2085 },
    { "chroma-nc", AVCOL_SPC_CHROMA_DERIVED_NCL },
    { "chroma-c", AVCOL_SPC_CHROMA_DERIVED_CL },
    { "ictcp", AVCOL_SPC_ICTCP },
};

/** @This lists color transfer characteristics conversions between upipe and
 * libav. */
static const struct upipe_av_pixdesc
upipe_av_color_transfer_characteristic[] = {
    { "bt709", AVCOL_TRC_BT709 },
    { "bt470m", AVCOL_TRC_GAMMA22 },
    { "bt470bg", AVCOL_TRC_GAMMA28 },
    { "smpte170m", AVCOL_TRC_SMPTE170M },
    { "smpte240m", AVCOL_TRC_SMPTE240M },
    { "linear", AVCOL_TRC_LINEAR },
    { "log100", AVCOL_TRC_LOG },
    { "log316", AVCOL_TRC_LOG_SQRT },
    { "iec61966-2-4", AVCOL_TRC_IEC61966_2_4 },
    { "bt1361e", AVCOL_TRC_BT1361_ECG },
    { "iec61966-2-1", AVCOL_TRC_IEC61966_2_1 },
    { "bt2020-10", AVCOL_TRC_BT2020_10 },
    { "bt2020-12", AVCOL_TRC_BT2020_12 },
    { "smpte2084", AVCOL_TRC_SMPTE2084 },
    { "smpte428", AVCOL_TRC_SMPTE428 },
    { "arib-std-b67", AVCOL_TRC_ARIB_STD_B67 },
};

/** @This converts a Upipe color to AV color.
 *
 * @param list conversion list
 * @param upipe_name Upipe name
 * @return the av_value or -1
 */
static inline int upipe_av_pixdesc_from_upipe(
    const struct upipe_av_pixdesc *list,
    size_t size,
    const char *upipe_name)
{
    for (int i = 0; i < size; i++)
        if (!strcmp(list[i].upipe_name, upipe_name))
            return list[i].av_value;

    return -1;
}

/** @This converts a AV color to Upipe color.
 *
 * @param list conversion list
 * @param av_value libav value to convert
 * @return the Upipe name or NULL
 */
static inline const char *upipe_av_pixdesc_to_upipe(
    const struct upipe_av_pixdesc *list,
    size_t size,
    int av_value)
{
    for (int i = 0; i < size; i++)
        if (list[i].av_value == av_value)
            return list[i].upipe_name;

    return NULL;
}

#define UPIPE_AV_PIXDESC_CONVERT(Name)                                      \
static inline int upipe_av_##Name##_from_upipe(const char *upipe_name)      \
{                                                                           \
    return upipe_av_pixdesc_from_upipe(upipe_av_##Name,                     \
                                       UBASE_ARRAY_SIZE(upipe_av_##Name),   \
                                       upipe_name);                         \
}                                                                           \
                                                                            \
static inline const char *upipe_av_##Name##_to_upipe(int av_value)          \
{                                                                           \
    return upipe_av_pixdesc_to_upipe(upipe_av_##Name,                       \
                                     UBASE_ARRAY_SIZE(upipe_av_##Name),     \
                                     av_value);                             \
}

UPIPE_AV_PIXDESC_CONVERT(color_primaries);
UPIPE_AV_PIXDESC_CONVERT(color_space);
UPIPE_AV_PIXDESC_CONVERT(color_transfer_characteristic);

#ifdef __cplusplus
}
#endif
#endif
