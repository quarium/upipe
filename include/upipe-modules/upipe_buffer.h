/*
 * Copyright (c) 2015 Arnaud de Turckheim <quarium@gmail.com>
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
 * @short Upipe buffer module
 *
 * The buffer pipe forwards the input uref if it can. When the output
 * upump is blocked by the output pipe or by the user (see upipe_buffer_block),
 * the buffer pipe still accepts the input until max size is reached.
 */

#ifndef _UPIPE_MODULES_UPIPE_BUFFER_H_
/** @hidden */
# define _UPIPE_MODULES_UPIPE_BUFFER_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/upipe.h>

#define UPIPE_BUFFER_SIGNATURE UBASE_FOURCC('b','u','f','f')

/** @This extends @ref upipe_command with specific buffer commands. */
enum upipe_buffer_command {
    UPIPE_BUFFER_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** get the current buffer size */
    UPIPE_BUFFER_GET_SIZE,
    /** set the maximum buffer size */
    UPIPE_BUFFER_SET_MAX_SIZE,
    /** get the maximum buffer size */
    UPIPE_BUFFER_GET_MAX_SIZE,
    /** enable/disable the output of the buffer pipe */
    UPIPE_BUFFER_BLOCK,
};

/** @This converts the buffer pipe command to a string.
 *
 * @param cmd command to convert
 * @return a string or NULL if cmd is invalid
 */
static inline const char *upipe_buffer_command_str(int cmd)
{
    switch ((enum upipe_buffer_command)cmd) {
    UBASE_CASE_TO_STR(UPIPE_BUFFER_GET_SIZE);
    UBASE_CASE_TO_STR(UPIPE_BUFFER_SET_MAX_SIZE);
    UBASE_CASE_TO_STR(UPIPE_BUFFER_GET_MAX_SIZE);
    UBASE_CASE_TO_STR(UPIPE_BUFFER_BLOCK);
    case UPIPE_BUFFER_SENTINEL: break;
    }
    return NULL;
}

/** @This gets the current buffer size.
 *
 * @param upipe description structure of the pipe
 * @param size_p pointer filled with the buffer size.
 * @return an error code
 */
static inline int upipe_buffer_get_size(struct upipe *upipe, uint64_t *size_p)
{
    return upipe_control(upipe, UPIPE_BUFFER_GET_SIZE,
                         UPIPE_BUFFER_SIGNATURE, size_p);
}

/** @This sets the maximum buffer size.
 *
 * @param upipe description structure of the pipe
 * @param max_size maximum buffer size in bytes
 * @return an error code
 */
static inline int upipe_buffer_set_max_size(struct upipe *upipe,
                                            uint64_t max_size)
{
    return upipe_control(upipe, UPIPE_BUFFER_SET_MAX_SIZE,
                         UPIPE_BUFFER_SIGNATURE, max_size);
}

/** @This gets the maximum buffer size.
 *
 * @param upipe description structure of the pipe
 * @param max_size_p pointer filled with the maximum buffer size in bytes
 * @return an error code
 */
static inline int upipe_buffer_get_max_size(struct upipe *upipe,
                                            uint64_t *max_size_p)
{
    return upipe_control(upipe, UPIPE_BUFFER_GET_MAX_SIZE,
                         UPIPE_BUFFER_SIGNATURE, max_size_p);
}

/** @This blocks or unblocks the output of the buffer pipe.
 *
 * @param upipe description structure of the pipe
 * @param block block/unblock the output
 * @return an error code
 */
static inline int upipe_buffer_block(struct upipe *upipe, bool block)
{
    return upipe_control(upipe, UPIPE_BUFFER_BLOCK,
                         UPIPE_BUFFER_SIGNATURE, block);
}

/** @This return the buffer pipe manager.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_buffer_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
