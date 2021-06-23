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

#include <upipe/urefcount_helper.h>

#include <upipe/uprobe.h>

/** @internal @This is the private structure for a simple allocated probe. */
struct uprobe_alloc {
    /** refcount structure */
    struct urefcount urefcount;
    /** optional refcount on data */
    struct urefcount *data;
    /** probe structure */
    struct uprobe uprobe;
};

/** @hidden */
UREFCOUNT_HELPER(uprobe_alloc, urefcount, uprobe_alloc_free);
/** @hidden */
UBASE_FROM_TO(uprobe_alloc, uprobe, uprobe, uprobe);

/** @internal @This frees the allocated probe.
 *
 * @param uprobe_alloc private allocated structure
 */
static inline void uprobe_alloc_free(struct uprobe_alloc *uprobe_alloc)
{
    urefcount_release(uprobe_alloc->data);
    uprobe_clean(&uprobe_alloc->uprobe);
    uprobe_alloc_clean_urefcount(uprobe_alloc);
    free(uprobe_alloc);
}

/** @This allocates and initializes a probe with refcounted data.
 *
 * Please note that this function does not _use() data, so if you want to reuse
 * an existing data, you have to use it first.
 *
 * @param func function called when an event is raised
 * @param data refcount on the data
 * @param next next probe to test if this one doesn't catch the event
 * @return an allocated probe
 */
struct uprobe *uprobe_alloc_data(uprobe_throw_func func,
                                 struct urefcount *data,
                                 struct uprobe *next)
{
    struct uprobe_alloc *uprobe_alloc = malloc(sizeof (*uprobe_alloc));
    if (unlikely(!uprobe_alloc)) {
        urefcount_release(data);
        uprobe_release(next);
        return NULL;
    }
    uprobe_init(&uprobe_alloc->uprobe, func, next);
    uprobe_alloc_init_urefcount(uprobe_alloc);
    uprobe_alloc->uprobe.refcount = &uprobe_alloc->urefcount;
    uprobe_alloc->data = data;
    return &uprobe_alloc->uprobe;
}

/** @This allocates and initializes a probe.
 *
 * @param func function called when an event is raised
 * @param next next probe to test if this one doesn't catch the event
 * @return an allocated probe
 */
struct uprobe *uprobe_alloc(uprobe_throw_func func, struct uprobe *next)
{
    return uprobe_alloc_data(func, NULL, next);
}

/** @This returns a refcount on the data passed at allocation or NULL.
 *
 * @param uprobe pointer to uprobe
 * @return an urefcount or NULL
 */
struct urefcount *uprobe_alloc_get_data(struct uprobe *uprobe)
{
    if (uprobe && uprobe->refcount &&
        uprobe->refcount->cb == uprobe_alloc_dead_urefcount) {
        struct uprobe_alloc *p = uprobe_alloc_from_uprobe(uprobe);
        return p->data;
    }
    return NULL;
}
