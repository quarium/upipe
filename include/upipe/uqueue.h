/*
 * Copyright (C) 2012-2015 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
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
 * @short Upipe thread-safe queue of elements
 */

#ifndef _UPIPE_UQUEUE_H_
/** @hidden */
#define _UPIPE_UQUEUE_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/config.h>
#include <upipe/ubase.h>
#include <upipe/uatomic.h>
#include <upipe/ufifo.h>
#include <upipe/ueventfd.h>
#include <upipe/upump.h>

#include <stdint.h>
#include <assert.h>
#include <pthread.h>

/** @This enumerates the uqueue implementation type. */
enum uqueue_type {
    /** use atomic */
    UQUEUE_ATOMIC,
    /** use mutex */
    UQUEUE_MUTEX,
    /** use mutex second implementation */
    UQUEUE_MUTEX2,
    /** use mutex with buffer list */
    UQUEUE_MUTEX_LIST,
    /** use only event fd */
    UQUEUE_EVENTFD,
    /** uqueue abort */
    UQUEUE_ABORT,
};

/** @This is am uqueue element. */
struct uqueue_elt {
    /** link in the current list */
    struct uchain uchain;
    /** opaque object */
    void *opaque;
};

UBASE_FROM_TO(uqueue_elt, uchain, uchain, uchain);

/** @This is the implementation of a queue. */
struct uqueue {
    /** uqueue type */
    enum uqueue_type type;
    union {
        struct {
            /** FIFO */
            struct ufifo fifo;
        };
        struct {
            /** mutex */
            pthread_mutex_t mutex;
            /** extra data */
            struct uqueue_elt *elts;
            /** empty slots */
            struct uchain empty;
            /** carrier slots */
            struct uchain carrier;
            /** pop list */
            struct uchain ready;
        };
        struct {
            unsigned in;
            unsigned in_count;
            int in_fd;
            unsigned out;
            unsigned out_count;
            int out_fd;
            void **extra;
        };
    };
    /** number of elements in the queue */
    uatomic_uint32_t counter;
    /** maximum number of elements in the queue */
    uint32_t length;
    /** ueventfd triggered when data can be pushed */
    struct ueventfd event_push;
    /** ueventfd triggered when data can be popped */
    struct ueventfd event_pop;
};

/** @This gets the uqueue implemenation */
static inline enum uqueue_type uqueue_type_get(void)
{
    const char *e = getenv("UPIPE_UQUEUE_TYPE");
    if (e == NULL)
        return UQUEUE_ATOMIC;
    if (!strcmp(e, "mutex"))
        return UQUEUE_MUTEX;
    if (!strcmp(e, "mutex2"))
        return UQUEUE_MUTEX2;
    if (!strcmp(e, "mutex_list"))
        return UQUEUE_MUTEX_LIST;
    if (!strcmp(e, "eventfd"))
        return UQUEUE_EVENTFD;
    if (!strcmp(e, "abort"))
        return UQUEUE_ABORT;
    return UQUEUE_ATOMIC;
}

/** @This returns the required size of extra data space for uqueue.
 *
 * @param length maximum number of elements in the queue
 * @return size in octets to allocate
 */
static inline unsigned uqueue_sizeof(unsigned length)
{
    switch (uqueue_type_get()) {
        case UQUEUE_ATOMIC:
            return ufifo_sizeof(length);
        case UQUEUE_MUTEX:
        case UQUEUE_MUTEX2:
        case UQUEUE_MUTEX_LIST:
            return length * sizeof (struct uqueue_elt);
        case UQUEUE_EVENTFD:
            return length * sizeof (void *);
        case UQUEUE_ABORT:
            return 0;
    }
    return 0;
}

/** @This initializes a uqueue.
 *
 * @param uqueue pointer to a uqueue structure
 * @param length maximum number of elements in the queue
 * @param extra mandatory extra space allocated by the caller, with the size
 * returned by @ref #ufifo_sizeof
 * @return false in case of failure
 */
static inline bool uqueue_init(struct uqueue *uqueue, uint8_t length,
                               void *extra)
{
    if (unlikely(!ueventfd_init(&uqueue->event_push, true)))
        return false;
    if (unlikely(!ueventfd_init(&uqueue->event_pop, false))) {
        ueventfd_clean(&uqueue->event_push);
        return false;
    }

    switch (uqueue_type_get()) {
        case UQUEUE_ATOMIC:
            uqueue->type = UQUEUE_ATOMIC;
            ufifo_init(&uqueue->fifo, length, extra);
            break;

        case UQUEUE_MUTEX:
        case UQUEUE_MUTEX2:
        case UQUEUE_MUTEX_LIST:
            uqueue->type = UQUEUE_MUTEX;
            pthread_mutex_init(&uqueue->mutex, 0);
            uqueue->elts = (struct uqueue_elt *)extra;
            ulist_init(&uqueue->empty);
            ulist_init(&uqueue->carrier);
            ulist_init(&uqueue->ready);
            for (uint8_t i = 0; i < length; i++)
                ulist_add(&uqueue->empty, &uqueue->elts[i].uchain);
            break;
        case UQUEUE_EVENTFD:
            uqueue->type = UQUEUE_EVENTFD;
            uqueue->extra = (void **)extra;
            uqueue->in = 0;
            uqueue->in_count = length;
            uqueue->in_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            uqueue->out = 0;
            uqueue->out_count = 0;
            uqueue->out_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (unlikely(uqueue->in_fd < 0 || uqueue->out_fd < 0))
                return false;
            break;
        case UQUEUE_ABORT:
            break;
    }
    uatomic_init(&uqueue->counter, 0);
    uqueue->length = length;

    return true;
}

/** @This allocates a watcher triggering when data is ready to be pushed.
 *
 * @param uqueue pointer to a uqueue structure
 * @param upump_mgr management structure for this event loop
 * @param cb function to call when the watcher triggers
 * @param opaque pointer to the module's internal structure
 * @param refcount pointer to urefcount structure to increment during callback,
 * or NULL
 * @return pointer to allocated watcher, or NULL in case of failure
 */
static inline struct upump *uqueue_upump_alloc_push(struct uqueue *uqueue,
                                                    struct upump_mgr *upump_mgr,
                                                    upump_cb cb, void *opaque,
                                                    struct urefcount *refcount)
{
    return ueventfd_upump_alloc(&uqueue->event_push, upump_mgr, cb, opaque,
                                refcount);
}

/** @This allocates a watcher triggering when data is ready to be popped.
 *
 * @param uqueue pointer to a uqueue structure
 * @param upump_mgr management structure for this event loop
 * @param cb function to call when the watcher triggers
 * @param opaque pointer to the module's internal structure
 * @param refcount pointer to urefcount structure to increment during callback,
 * or NULL
 * @return pointer to allocated watcher, or NULL in case of failure
 */
static inline struct upump *uqueue_upump_alloc_pop(struct uqueue *uqueue,
                                                   struct upump_mgr *upump_mgr,
                                                   upump_cb cb, void *opaque,
                                                   struct urefcount *refcount)
{
    return ueventfd_upump_alloc(&uqueue->event_pop, upump_mgr, cb, opaque,
                                refcount);
}

/** @This pushes an element into the queue.
 *
 * @param uqueue pointer to a uqueue structure
 * @param element pointer to element to push
 * @return false if the queue is full and the element couldn't be queued
 */
static inline bool uqueue_push(struct uqueue *uqueue, void *element)
{
    switch (uqueue->type) {
        case UQUEUE_ATOMIC:
            if (unlikely(!ufifo_push(&uqueue->fifo, element))) {
                /* signal that we are full */
                ueventfd_read(&uqueue->event_push);

                /* double-check */
                if (likely(!ufifo_push(&uqueue->fifo, element)))
                    return false;

                /* signal that we're alright again */
                ueventfd_write(&uqueue->event_push);
            }

            if (unlikely(uatomic_fetch_add(&uqueue->counter, 1) == 0))
                ueventfd_write(&uqueue->event_pop);
            return true;

        case UQUEUE_MUTEX: {
            bool pushed = false;

            ueventfd_read(&uqueue->event_push);

            assert(!pthread_mutex_lock(&uqueue->mutex));
            struct uchain *uchain = ulist_pop(&uqueue->empty);
            if (likely(uchain)) {
                uqueue_elt_from_uchain(uchain)->opaque = element;
                ulist_add(&uqueue->carrier, uchain);
                pushed = true;
            }
            pthread_mutex_unlock(&uqueue->mutex);

            if (!pushed)
                return false;
            ueventfd_write(&uqueue->event_push);
            if (unlikely(uatomic_fetch_add(&uqueue->counter, 1) == 0))
                ueventfd_write(&uqueue->event_pop);
            return true;
        }

        case UQUEUE_MUTEX2:
        case UQUEUE_MUTEX_LIST: {
            bool pushed = false;

            assert(!pthread_mutex_lock(&uqueue->mutex));

            struct uchain *uchain = ulist_pop(&uqueue->empty);
            if (likely(uchain)) {
                uqueue_elt_from_uchain(uchain)->opaque = element;
                ulist_add(&uqueue->carrier, uchain);
                pushed = true;
            }
            if (!pushed)
                ueventfd_read(&uqueue->event_push);
            else {
                ueventfd_write(&uqueue->event_push);
                if (unlikely(uatomic_fetch_add(&uqueue->counter, 1) == 0))
                    ueventfd_write(&uqueue->event_pop);
            }
            pthread_mutex_unlock(&uqueue->mutex);

            return pushed;
        }
        case UQUEUE_EVENTFD: {
            int ret;

            if (!uqueue->in_count) {
                eventfd_t efd;
                ret = eventfd_read(uqueue->in_fd, &efd);
                assert(ret == 0 || errno == EWOULDBLOCK);
                uqueue->in_count += !ret ? efd : 0;
            }
            if (!uqueue->in_count) {
                ueventfd_read(&uqueue->event_push);
                return false;
            }
            uqueue->extra[uqueue->in] = element;
            uqueue->in = (uqueue->in + 1) % uqueue->length;
            uqueue->in_count--;
            ret = eventfd_write(uqueue->out_fd, 1);
            assert(ret == 0);
            ueventfd_write(&uqueue->event_pop);
            return true;
        }
        case UQUEUE_ABORT:
            abort();
            break;
    }
    return false;
}

/** @internal @This pops an element from the queue.
 *
 * @param uqueue pointer to a uqueue structure
 * @return pointer to element, or NULL if the LIFO is empty
 */
static inline void *uqueue_pop_internal(struct uqueue *uqueue)
{
    void *element = NULL;

    switch (uqueue->type) {
        case UQUEUE_ATOMIC: {
            element = ufifo_pop(&uqueue->fifo, void *);
            if (unlikely(element == NULL)) {
                /* signal that we starve */
                ueventfd_read(&uqueue->event_pop);

                /* double-check */
                element = ufifo_pop(&uqueue->fifo, void *);
                if (likely(element == NULL))
                    return NULL;

                /* signal that we're alright again */
                ueventfd_write(&uqueue->event_pop);
            }

            if (unlikely(uatomic_fetch_sub(&uqueue->counter, 1) ==
                         uqueue->length))
                ueventfd_write(&uqueue->event_push);
            break;
        }

        case UQUEUE_MUTEX: {
            ueventfd_read(&uqueue->event_pop);

            assert(!pthread_mutex_lock(&uqueue->mutex));
            struct uchain *uchain = ulist_pop(&uqueue->carrier);
            if (likely(uchain)) {
                element = uqueue_elt_from_uchain(uchain)->opaque;
                ulist_add(&uqueue->empty, uchain);
            }
            pthread_mutex_unlock(&uqueue->mutex);

            if (!element)
                break;

            ueventfd_write(&uqueue->event_pop);
            if (unlikely(uatomic_fetch_sub(&uqueue->counter, 1) ==
                         uqueue->length))
                ueventfd_write(&uqueue->event_push);
            break;
        }

        case UQUEUE_MUTEX2: {
            assert(!pthread_mutex_lock(&uqueue->mutex));
            struct uchain *uchain = ulist_pop(&uqueue->carrier);
            if (likely(uchain)) {
                element = uqueue_elt_from_uchain(uchain)->opaque;
                ulist_add(&uqueue->empty, uchain);
            }
            if (!element)
                ueventfd_read(&uqueue->event_pop);
            else {
                ueventfd_write(&uqueue->event_pop);
                if (unlikely(uatomic_fetch_sub(&uqueue->counter, 1) ==
                             uqueue->length))
                    ueventfd_write(&uqueue->event_push);
            }
            pthread_mutex_unlock(&uqueue->mutex);
            break;
        }

        case UQUEUE_MUTEX_LIST: {
            struct uchain *uchain = ulist_pop(&uqueue->ready);
            if (uchain) {
                element = uqueue_elt_from_uchain(uchain)->opaque;
                break;
            }
            assert(!pthread_mutex_lock(&uqueue->mutex));
            while ((uchain = ulist_pop(&uqueue->carrier))) {
                ulist_add(&uqueue->ready, uchain);
            }
            uchain = ulist_pop(&uqueue->carrier);
            if (likely(uchain)) {
                element = uqueue_elt_from_uchain(uchain)->opaque;
                ulist_add(&uqueue->empty, uchain);
            }
            if (!element)
                ueventfd_read(&uqueue->event_pop);
            else {
                ueventfd_write(&uqueue->event_pop);
                if (unlikely(uatomic_fetch_sub(&uqueue->counter, 1) ==
                             uqueue->length))
                    ueventfd_write(&uqueue->event_push);
            }
            pthread_mutex_unlock(&uqueue->mutex);
            break;
        }

        case UQUEUE_EVENTFD: {
            int ret;

            if (!uqueue->out_count) {
                eventfd_t efd;
                ret = eventfd_read(uqueue->out_fd, &efd);
                assert(ret == 0 || errno == EWOULDBLOCK);
                uqueue->out_count += !ret ? efd : 0;
            }
            if (!uqueue->out_count) {
                ueventfd_read(&uqueue->event_pop);
                return NULL;
            }

            element = uqueue->extra[uqueue->out];
            uqueue->out = (uqueue->out + 1) % uqueue->length;
            uqueue->out_count--;
            ret = eventfd_write(uqueue->in_fd, 1);
            assert(ret == 0);
            ueventfd_write(&uqueue->event_push);
            break;
        }

        case UQUEUE_ABORT:
            abort();
            break;
    }
    return element;
}

/** @This pops an element from the queue with type checking.
 *
 * @param uqueue pointer to a uqueue structure
 * @param type type of the opaque pointer
 * @return pointer to element, or NULL if the LIFO is empty
 */
#define uqueue_pop(uqueue, type) (type)uqueue_pop_internal(uqueue)

/** @This returns the number of elements in the queue.
 *
 * @param uqueue pointer to a uqueue structure
 */
static inline unsigned int uqueue_length(struct uqueue *uqueue)
{
    return uatomic_load(&uqueue->counter);
}

/** @This cleans up the queue data structure. Please note that it is the
 * caller's responsibility to empty the queue first.
 *
 * @param uqueue pointer to a uqueue structure
 */
static inline void uqueue_clean(struct uqueue *uqueue)
{
    uatomic_clean(&uqueue->counter);
    switch (uqueue->type) {
        case UQUEUE_ATOMIC:
            ufifo_clean(&uqueue->fifo);
            break;
        case UQUEUE_MUTEX:
        case UQUEUE_MUTEX2:
        case UQUEUE_MUTEX_LIST:
            pthread_mutex_destroy(&uqueue->mutex);
            break;
        case UQUEUE_EVENTFD:
            break;
        case UQUEUE_ABORT:
            break;
    }
    ueventfd_clean(&uqueue->event_push);
    ueventfd_clean(&uqueue->event_pop);
}

#ifdef __cplusplus
}
#endif
#endif
