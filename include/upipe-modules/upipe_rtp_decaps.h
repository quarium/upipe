/*
 * Copyright (C) 2014 OpenHeadend S.A.R.L.
 * Copyright (C) 2026 EasyTools
 *
 * Authors: Christophe Massiot
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module decapsulating RTP header from blocks
 */

#ifndef _UPIPE_MODULES_UPIPE_RTP_DECAPS_H_
/** @hidden */
#define _UPIPE_MODULES_UPIPE_RTP_DECAPS_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/upipe.h"

#define UPIPE_RTPD_SIGNATURE UBASE_FOURCC('r','t','p','d')

/** @This enumerates the RTP decaps control commands. */
enum upipe_rtpd_command {
    UPIPE_RTPD_SENTINEL = UPIPE_CONTROL_LOCAL,

    UPIPE_RTPD_GET_PACKETS_LOST, /* int sig, uint64_t * */
    /**  Filter RTP type (int) */
    UPIPE_RTPD_SET_INPUT_TYPE,
};

static inline int upipe_rtpd_get_packets_lost(struct upipe *upipe,
        uint64_t *lost)
{
    return upipe_control(upipe, UPIPE_RTPD_GET_PACKETS_LOST,
            UPIPE_RTPD_SIGNATURE, lost);
}

/** @This filters out rtp packet with a different type.
 *
 * @param upipe description structure of the pipe
 * @param input_type wanted input RTP type or -1 to disable filtering
 * @return an error code
 */
static inline int upipe_rtpd_set_input_type(struct upipe *upipe, int input_type)
{
    return upipe_control(upipe, UPIPE_RTPD_SET_INPUT_TYPE, UPIPE_RTPD_SIGNATURE,
                         input_type);
}

/** @This returns the management structure for rtpd pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_rtpd_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
