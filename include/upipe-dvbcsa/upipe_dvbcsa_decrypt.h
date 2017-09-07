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

#ifndef _UPIPE_DVBCSA_UPIPE_DVBCSA_DEC_H_
#define _UPIPE_DVBCSA_UPIPE_DVBCSA_DEC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/upipe.h>

#define UPIPE_DVBCSA_DEC_SIGNATURE  UBASE_FOURCC('d','v','b','d')

/** @This enumerates the custom control commands. */
enum upipe_dvbcsa_dec_command {
    /** sentinel */
    UPIPE_DVBCSA_DEC_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** set the key (const char *) */
    UPIPE_DVBCSA_DEC_SET_KEY,
};

/** @This sets the key
 *
 * @param upipe description structure of the pipe
 * @param key decription key
 * @return an error code
 */
static inline int upipe_dvbcsa_dec_set_key(struct upipe *upipe,
                                           const char *key)
{
    return upipe_control(upipe, UPIPE_DVBCSA_DEC_SET_KEY,
                         UPIPE_DVBCSA_DEC_SIGNATURE, key);
}

/** @This returns the dvbcsa decrypt pipe management structure.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_dvbcsa_dec_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
