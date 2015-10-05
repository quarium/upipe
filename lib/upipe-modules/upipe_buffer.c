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

#include <upipe/uclock.h>
#include <upipe/ueventfd.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_uri.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_input.h>
#include <upipe/upipe_helper_output.h>
#include <upipe-modules/upipe_buffer.h>

#define DEFAULT_MAX_SIZE        (1024 * 1024)

/** @hidden */
static void upipe_buffer_free(struct urefcount *urefcount);

/** @internal @This is the private context of a buffer pipe. */
struct upipe_buffer {
    /** upipe structure for helper */
    struct upipe upipe;
    /** urefcount structure for helper */
    struct urefcount urefcount;
    /** urefcount structure for real refcount */
    struct urefcount urefcount_real;
    /** upump_mgr pointer for helper */
    struct upump_mgr *upump_mgr;
    /** ueventfd for upump */
    struct ueventfd ueventfd;
    /** upump pointer for helper */
    struct upump *upump;
    /** upipe pointer for output helper */
    struct upipe *output;
    /** uref pointer for output helper to store flow def */
    struct uref *flow_def;
    /** output_state for output helper */
    enum upipe_helper_output_state output_state;
    /** uchain structure for output helper to store request list */
    struct uchain request_list;
    /** list of urefs for input helper */
    struct uchain urefs;
    /** number of urefs for input helper */
    unsigned int nb_urefs;
    /** max of urefs for input helper */
    unsigned int max_urefs;
    /** uchain structure for input helper to store blockers */
    struct uchain blockers;
    /** list of buffered urefs */
    struct uchain buffer;
    /** maximum buffer size before blocking the input */
    uint64_t max_size;
    /** buffer size */
    uint64_t size;
};

UBASE_FROM_TO(upipe_buffer, urefcount, urefcount_real, urefcount_real);

/** @hidden */
static bool upipe_buffer_process(struct upipe *upipe,
                                 struct uref *uref,
                                 struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_buffer, upipe, UPIPE_BUFFER_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_buffer, urefcount, upipe_buffer_no_ref);
UPIPE_HELPER_VOID(upipe_buffer);
UPIPE_HELPER_UPUMP_MGR(upipe_buffer, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_buffer, upump, upump_mgr);
UPIPE_HELPER_INPUT(upipe_buffer, urefs, nb_urefs, max_urefs, blockers,
                   upipe_buffer_process);
UPIPE_HELPER_OUTPUT(upipe_buffer, output, flow_def, output_state, request_list);

/** @internal @This allocates a buffer pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated pipe
 */
static struct upipe *upipe_buffer_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature,
                                        va_list args)
{
    struct upipe *upipe = upipe_buffer_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_buffer_init_urefcount(upipe);
    upipe_buffer_init_upump_mgr(upipe);
    upipe_buffer_init_upump(upipe);
    upipe_buffer_init_input(upipe);
    upipe_buffer_init_output(upipe);

    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);
    urefcount_init(&upipe_buffer->urefcount_real, upipe_buffer_free);
    ulist_init(&upipe_buffer->buffer);
    ueventfd_init(&upipe_buffer->ueventfd, false);
    upipe_buffer->max_size = DEFAULT_MAX_SIZE;
    upipe_buffer->size = 0;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This frees a buffer pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_buffer_free(struct urefcount *urefcount)
{
    struct upipe_buffer *upipe_buffer =
        upipe_buffer_from_urefcount_real(urefcount);
    struct upipe *upipe = upipe_buffer_to_upipe(upipe_buffer);

    upipe_throw_dead(upipe);

    struct uchain *uchain;
    while ((uchain = ulist_pop(&upipe_buffer->buffer)) != NULL)
        uref_free(uref_from_uchain(uchain));

    ueventfd_clean(&upipe_buffer->ueventfd);
    upipe_buffer_clean_output(upipe);
    upipe_buffer_clean_input(upipe);
    upipe_buffer_clean_upump(upipe);
    upipe_buffer_clean_upump_mgr(upipe);
    upipe_buffer_clean_urefcount(upipe);
    urefcount_clean(&upipe_buffer->urefcount_real);
    upipe_buffer_free_void(upipe);
}

/** @internal @This is called when there is no external reference to the pipe.
 */
static void upipe_buffer_no_ref(struct upipe *upipe)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);
    urefcount_release(&upipe_buffer->urefcount_real);
}

/** @internal @This is called when output pipe need some data.
 *
 * @param upump description structure of the output watcher
 */
static void upipe_buffer_worker(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);

    ueventfd_read(&upipe_buffer->ueventfd);

    struct uref *uref = uref_from_uchain(ulist_pop(&upipe_buffer->buffer));
    size_t size = 0;
    uref_block_size(uref, &size);
    upipe_buffer->size -= size;

    if (likely(!ulist_empty(&upipe_buffer->buffer)))
        ueventfd_write(&upipe_buffer->ueventfd);
    if (upipe_buffer_output_input(upipe))
        upipe_buffer_unblock_input(upipe);
    upipe_buffer_output(upipe, uref, &upipe_buffer->upump);
}

/** @internal @This sets the flow format for real of the buffer pipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_def the flow format to set
 * @return an error code
 */
static void upipe_buffer_set_flow_def_real(struct upipe *upipe,
                                           struct uref *flow_def)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);

    if (likely(upipe_buffer->flow_def != NULL)) {
        uref_free(flow_def);
        return;
    }
    upipe_buffer_store_flow_def(upipe, flow_def);
}

/** @internal @This processes an input uref.
 *
 * @param upipe description structure of the pipe
 * @param uref input uref
 * @param upump_p reference to pump that generated the buffer
 * @return true if the uref has been processed, false otherwise
 */
static bool upipe_buffer_process(struct upipe *upipe,
                                 struct uref *uref,
                                 struct upump **upump_p)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);

    size_t size = 0;
    uref_block_size(uref, &size);
    if (upipe_buffer->size + size > upipe_buffer->max_size)
        return false;

    upipe_buffer->size += size;
    ulist_add(&upipe_buffer->buffer, uref_to_uchain(uref));
    return true;
}

/** @internal @This tries to output an input uref, holds otherwise
 *
 * @param upipe description structure of the pipe
 * @param uref input uref
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_buffer_input(struct upipe *upipe,
                               struct uref *uref,
                               struct upump **upump_p)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);

    const char *def;
    if (unlikely(ubase_check(uref_flow_get_def(uref, &def)))) {
        upipe_buffer_set_flow_def_real(upipe, uref);
        return;
    }

    if (!upipe_buffer_output_input(upipe) ||
        !upipe_buffer_process(upipe, uref, upump_p)) {
        upipe_buffer_hold_input(upipe, uref);
        upipe_buffer_block_input(upipe, upump_p);
    }

    ueventfd_write(&upipe_buffer->ueventfd);
}

/** @internal @This sets the flow format of the buffer pipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_def the flow format to set
 * @return an error code
 */
static int upipe_buffer_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    int ret = uref_flow_match_def(flow_def, "block.");
    if (!ubase_check(ret))
        return ret;

    struct uref *flow_def_dup = uref_dup(flow_def);
    if (unlikely(flow_def_dup == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }

    upipe_input(upipe, flow_def_dup, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the maximum buffer size.
 *
 * @param upipe description structure of the pipe
 * @param max_size maximum buffer size in bytes
 * @return an error code
 */
static int _upipe_buffer_set_max_size(struct upipe *upipe, uint64_t max_size)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);
    upipe_buffer->max_size = max_size;
    return UBASE_ERR_NONE;
}

/** @internal @This gets the maximum buffer size.
 *
 * @param upipe description structure of the pipe
 * @param max_size_p pointer filled with the maximum buffer size in bytes
 * @return an error code
 */
static int _upipe_buffer_get_max_size(struct upipe *upipe,
                                      uint64_t *max_size_p)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);
    if (likely(max_size_p))
        *max_size_p = upipe_buffer->max_size;
    return UBASE_ERR_NONE;
}

/** @internal @This gets the current size of the buffer.
 *
 * @param upipe description structure of the pipe
 * @param size_p pointer filled with the current buffer size in bytes
 * @return an error code
 */
static int _upipe_buffer_get_size(struct upipe *upipe, uint64_t *size_p)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);
    if (likely(size_p))
        *size_p = upipe_buffer->size;
    return UBASE_ERR_NONE;
}

/** @internal @This dispatches the control commands.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to dispatch
 * @param args arguments of the command
 * @return an error code
 */
static int _upipe_buffer_control(struct upipe *upipe,
                                 int command,
                                 va_list args)
{
    switch (command) {
    case UPIPE_ATTACH_UPUMP_MGR:
        upipe_buffer_set_upump(upipe, NULL);
        return upipe_buffer_attach_upump_mgr(upipe);

    case UPIPE_REGISTER_REQUEST: {
        struct urequest *urequest = va_arg(args, struct urequest *);
        return upipe_buffer_alloc_output_proxy(upipe, urequest);
    }
    case UPIPE_UNREGISTER_REQUEST: {
        struct urequest *urequest = va_arg(args, struct urequest *);
        return upipe_buffer_free_output_proxy(upipe, urequest);
    }
    case UPIPE_GET_FLOW_DEF: {
        struct uref **flow_def_p = va_arg(args, struct uref **);
        return upipe_buffer_get_flow_def(upipe, flow_def_p);
    }
    case UPIPE_SET_FLOW_DEF: {
        struct uref *flow_def = va_arg(args, struct uref *);
        return upipe_buffer_set_flow_def(upipe, flow_def);
    }
    case UPIPE_GET_OUTPUT: {
        struct upipe **output_p = va_arg(args, struct upipe **);
        return upipe_buffer_get_output(upipe, output_p);
    }
    case UPIPE_SET_OUTPUT: {
        struct upipe *output = va_arg(args, struct upipe *);
        return upipe_buffer_set_output(upipe, output);
    }

    case UPIPE_BUFFER_SET_MAX_SIZE: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_BUFFER_SIGNATURE);
        uint64_t max_size = va_arg(args, uint64_t);
        return _upipe_buffer_set_max_size(upipe, max_size);
    }
    case UPIPE_BUFFER_GET_MAX_SIZE: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_BUFFER_SIGNATURE);
        uint64_t *max_size_p = va_arg(args, uint64_t *);
        return _upipe_buffer_get_max_size(upipe, max_size_p);
    }
    case UPIPE_BUFFER_GET_SIZE: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_BUFFER_SIGNATURE);
        uint64_t *size_p = va_arg(args, uint64_t *);
        return _upipe_buffer_get_size(upipe, size_p);
    }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This allocates the upump if it's not.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_buffer_check(struct upipe *upipe)
{
    struct upipe_buffer *upipe_buffer = upipe_buffer_from_upipe(upipe);

    UBASE_RETURN(upipe_buffer_check_upump_mgr(upipe));
    if (unlikely(upipe_buffer->upump_mgr == NULL)) {
        upipe_err(upipe, "no upump manager");
        return UBASE_ERR_INVALID;
    }

    if (unlikely(upipe_buffer->upump == NULL)) {
        struct upump *upump =
            ueventfd_upump_alloc(&upipe_buffer->ueventfd,
                                 upipe_buffer->upump_mgr,
                                 upipe_buffer_worker, upipe,
                                 upipe->refcount);
        if (unlikely(upump == NULL)) {
            upipe_throw_fatal(upipe, UBASE_ERR_UPUMP);
            return UBASE_ERR_UPUMP;
        }
        upipe_buffer_set_upump(upipe, upump);
        upump_start(upump);
        if (likely(!ulist_empty(&upipe_buffer->buffer)))
            ueventfd_write(&upipe_buffer->ueventfd);
    }
    return UBASE_ERR_NONE;
}

/** @internal @This dispatches the control commands and checks the pump.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to dispatch
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_buffer_control(struct upipe *upipe,
                                int command,
                                va_list args)
{
    UBASE_RETURN(_upipe_buffer_control(upipe, command, args));
    return upipe_buffer_check(upipe);
}

/** @internal @This is the static buffer pipe manager. */
static struct upipe_mgr upipe_buffer_mgr = {
    .refcount = NULL,
    .signature = UPIPE_BUFFER_SIGNATURE,
    .upipe_command_str = upipe_buffer_command_str,
    .upipe_event_str = NULL,
    .upipe_alloc = upipe_buffer_alloc,
    .upipe_input = upipe_buffer_input,
    .upipe_control = upipe_buffer_control,
};

/** @This returns the static buffer pipe manager. */
struct upipe_mgr *upipe_buffer_mgr_alloc(void)
{
    return &upipe_buffer_mgr;
}
