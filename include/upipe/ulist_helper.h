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

/** @file
 * @short Ulist helper macros
 */

#ifndef _UPIPE_ULIST_HELPER_H_
/** @hidden */
#define _UPIPE_ULIST_HELPER_H_
#ifdef __cplusplus
extern "C" {
#endif

/** @This defines functions to manipulate an ulist embedded into a structure.
 *
 * Supposing you have a structure foo containing a list of bar structures:
 * @code
 *  struct bar {
 *      struct uchain uchain;
 *      ...
 *  };
 *
 *  struct foo {
 *      struct uchain bars;
 *      ...
 *  };
 * @end code
 *
 * You may use the helper like this:
 * @code
 *  ULIST_HELPER(foo, bars, bar, uchain);
 * @end code
 *
 * To define:
 * @list
 *
 * @item @code
 *  void foo_init_bars(struct foo *foo);
 * @end code
 * Initialize the embedded list. Typically used on the foo structure
 * initializer.
 *
 * @item @code
 *  void foo_add_bars(struct foo *foo, struct bar *bar);
 * @end code
 * Add a bar item into the foo list.
 *
 * @item @code
 *  void foo_delete_bars(struct bar *bar);
 * @end code
 * Remove a bar item from the foo list.
 *
 * @item @code
 *  struct bar *foo_pop_bars(struct foo *foo);
 * @end code
 * Remove the first bar item from the foo list and return it.
 *
 * @item @code
 *  struct bar *foo_iterate_bars(struct foo *foo, struct bar *bar);
 * @end code
 * Iterate over the foo list. The first iterate must be called with NULL for
 * bar. This function @b {DOES NOT} support item deletion between iteration.
 * For instance:
 * @code
 *  struct bar *bar = NULL;
 *  while ((bar = foo_iterate_bars(foo, bar))) {
 *      // DO NOT delete item from the list
 *      ...
 *  }
 * @end code
 *
 * @item @code
 *  struct bar *foo_delete_iterate_bars(struct foo *foo, struct bar *bar);
 * @end code
 * Iterate over the foo list. This function may be use to delete the current
 * bar item safely from the list.
 * For instance:
 * @code
 *  struct bar *bar = NULL, *tmp;
 *  while ((bar = foo_iterate_bars(foo, bar, &tmp))) {
 *      // DO NOT delete other item than bar
 *      foo_delete_bars(bar);
 *      ...
 *  }
 * @end code
 *
 * @item @code
 *  void foo_flush_bars(struct foo *foo, void (*cb)(struct bar *));
 * @end code
     * Remove all items from the foo list. You may pass a callback to free the
 * deleted items. For instance:
 * @code
 *  static void bar_free(struct bar *bar)
 *  {
 *      // do what you need here
 *      ...
 *  }
 *
 *  ...
 *
 *      foo_flush_bars(foo, bar_free);
 * @end list
 */
#define ULIST_HELPER(STRUCT, ULIST, ITEM, UCHAIN)                           \
                                                                            \
static inline void STRUCT##_init_##ULIST(struct STRUCT *s)                  \
{                                                                           \
    ulist_init(&s->ULIST);                                                  \
}                                                                           \
                                                                            \
static inline void STRUCT##_add_##ULIST(struct STRUCT *s, struct ITEM *i)   \
{                                                                           \
    ulist_add(&s->ULIST, &i->UCHAIN);                                       \
}                                                                           \
                                                                            \
static inline void STRUCT##_delete_##ULIST(struct ITEM *i)                  \
{                                                                           \
    ulist_delete(&i->UCHAIN);                                               \
}                                                                           \
                                                                            \
static inline struct ITEM *STRUCT##_pop_##ULIST(struct STRUCT *s)           \
{                                                                           \
    struct uchain *uchain = ulist_pop(&s->ULIST);                           \
    return uchain ? container_of(uchain, struct ITEM, UCHAIN) : NULL;       \
}                                                                           \
                                                                            \
static inline struct ITEM *STRUCT##_iterate_##ULIST(struct STRUCT *s,       \
                                                    struct ITEM *i)         \
{                                                                           \
    if (!i)                                                                 \
        return ulist_empty(&s->ULIST) ? NULL :                              \
            container_of(s->ULIST.next, struct ITEM, UCHAIN);               \
    if (i->UCHAIN.next == &s->ULIST)                                        \
        return NULL;                                                        \
    return container_of(i->UCHAIN.next, struct ITEM, UCHAIN);               \
}                                                                           \
                                                                            \
static inline struct ITEM *                                                 \
STRUCT##_delete_iterate_##ULIST(struct STRUCT *s,                           \
                                struct ITEM *i,                             \
                                struct ITEM **tmp)                          \
{                                                                           \
    if (!i) {                                                               \
        if (ulist_empty(&s->ULIST))                                         \
            return NULL;                                                    \
        i = container_of(s->ULIST.next, struct ITEM, UCHAIN);               \
        *tmp = i->UCHAIN.next == &s->ULIST ? NULL :                         \
            container_of(i->UCHAIN.next, struct ITEM, UCHAIN);              \
        return i;                                                           \
    }                                                                       \
    i = *tmp;                                                               \
    if (i)                                                                  \
        *tmp = i->UCHAIN.next == &s->ULIST ? NULL :                         \
            container_of(i->UCHAIN.next, struct ITEM, UCHAIN);              \
    else                                                                    \
        *tmp = NULL;                                                        \
    return i;                                                               \
}                                                                           \
                                                                            \
static inline void                                                          \
STRUCT##_flush_##ULIST(struct STRUCT *s, void (*cb)(struct ITEM *))         \
{                                                                           \
    for (struct ITEM *i = NULL;                                             \
         (i = STRUCT##_pop_##ULIST(s));)                                    \
        if (cb)                                                             \
            cb(i);                                                          \
}

#define ULIST_HELPER_KEY(STRUCT, ULIST, ITEM, UCHAIN, KEY)                  \
static inline struct ITEM *                                                 \
STRUCT##_find_##ULIST(struct STRUCT *s,                                     \
                      __typeof__(((struct ITEM *)0)->KEY) key)              \
{                                                                           \
    struct ITEM *i = NULL;                                                  \
    while ((i = STRUCT##_iterate_##ULIST(s, i)))                            \
        if (i->KEY == key)                                                  \
            return i;                                                       \
    return NULL;                                                            \
}

/** @This walks through an inner ulist.
 *
 * @param type name of the structure containing the list
 * @param ulist name of the @ref ulist inside the structure
 * @param item_type name of the item structure
 * @param uchain name of the @ref uchain insde the item structure
 * @param obj pointer to the structure containing the list
 * @param iterator name of the iterator
 */
#define ulist_helper_foreach(iterate, list, iterator)                   \
    for (__typeof__ (iterate(NULL, NULL)) iterator = NULL;              \
         (iterator = iterate(list, iterator));)


/** @This walks through an inner ulist. This variant allows to remove the
 * current element safely.
 *
 * @param type name of the structure containing the list
 * @param ulist name of the @ref ulist inside the structure
 * @param item_type name of the item structure
 * @param uchain name of the @ref uchain insde the item structure
 * @param obj pointer to the structure containing the list
 * @param iterator name of the iterator
 * @param tmp iterator to use for temporary storage
 */
#define ulist_helper_delete_foreach(iterate, list, iterator, tmp)       \
    for (__typeof__ (*iterate(NULL, NULL, NULL)) *iterator = NULL, *tmp;\
         (iterator = iterate(list, iterator, &tmp));)

#ifdef __cplusplus
}
#endif
#endif
