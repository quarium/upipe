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

#include <upipe/ubase.h>
#include <upipe/upipe.h>

/** @This is the cross blend pipe signature. */
#define UPIPE_XBLEND_SIGNATURE  UBASE_FOURCC('x','b','l','d')

/** @This enumatates the private cross blend pipe control commands. */
enum upipe_xblend_control {
    /** sentinel */
    UPIPE_XBLEND_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** set the cross blend duration (uint64_t) */
    UPIPE_XBLEND_SET_DURATION,
    /** get the cross blend duration (uint64_t *) */
    UPIPE_XBLEND_GET_DURATION,
};

/** @This sets the cross blend duration.
 *
 * @param upipe description structure of the pipe
 * @param duration cross blend duration
 * @return an error code
 */
static inline int upipe_xblend_set_duration(struct upipe *upipe,
                                            uint64_t duration)
{
    return upipe_control(upipe, UPIPE_XBLEND_SET_DURATION,
                         UPIPE_XBLEND_SIGNATURE, duration);
}

/** @This gets the cross blend duration.
 *
 * @param upipe description structure of the pipe
 * @param duration_p filled with cross blend duration
 * @return an error code
 */
static inline int upipe_xblend_get_duration(struct upipe *upipe,
                                            uint64_t *duration_p)
{
    return upipe_control(upipe, UPIPE_XBLEND_GET_DURATION,
                         UPIPE_XBLEND_SIGNATURE, duration_p);
}

/** @This returns the cross blend pipes management structure.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_xblend_mgr_alloc(void);
