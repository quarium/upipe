/*
 * Copyright (C) 2019 OpenHeadend S.A.R.L.
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
 * @short Upipe helper macros for embedded ulist.
 */

#ifndef _UPIPE_ULIST_HELPER_H_
/** @hidden */
#define _UPIPE_ULIST_HELPER_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/ulist.h>

#define ULIST_HELPER(STRUCTURE, ULIST, SUBSTRUCTURE, UCHAIN)                \
UBASE_FROM_TO(STRUCTURE, uchain, ULIST, ULIST);                             \
UBASE_FROM_TO(SUBSTRUCTURE, uchain, UCHAIN, UCHAIN);                        \
                                                                            \
static void STRUCTURE##_init_##ULIST(struct STRUCTURE *s)                   \
{                                                                           \
    struct uchain *list = STRUCTURE##_to_##ULIST(s);                        \
    ulist_init(list);                                                       \
}                                                                           \
                                                                            \
static void STRUCTURE##_clean_##ULIST(struct STRUCTURE *s)                  \
{                                                                           \
}                                                                           \
                                                                            \
static inline void STRUCTURE##_add_##SUBSTRUCTURE(struct STRUCTURE *s,      \
                                                  struct SUBSTRUCTURE *i)   \
{                                                                           \
    ulist_add(STRUCTURE##_to_##ULIST(s), SUBSTRUCTURE##_to_##UCHAIN(i));    \
}                                                                           \
                                                                            \
static inline struct SUBSTRUCTURE *                                         \
STRUCTURE##_peek_##SUBSTRUCTURE(struct STRUCTURE *s)                        \
{                                                                           \
    struct uchain *list = STRUCTURE##_to_##ULIST(s);                        \
    struct uchain *elt = ulist_peek(list);                                  \
    return elt ? SUBSTRUCTURE##_from_##UCHAIN(elt) : NULL;                  \
}                                                                           \
                                                                            \
static inline struct SUBSTRUCTURE *                                         \
STRUCTURE##_pop_##SUBSTRUCTURE(struct STRUCTURE *s)                         \
{                                                                           \
    struct uchain *list = STRUCTURE##_to_##ULIST(s);                        \
    struct uchain *elt = ulist_pop(list);                                   \
    return elt ? SUBSTRUCTURE##_from_##UCHAIN(elt) : NULL;                  \
}                                                                           \
                                                                            \
static inline struct SUBSTRUCTURE *                                         \
STRUCTURE##_iterator_##SUBSTRUCTURE(struct STRUCTURE *s,                    \
                                    struct SUBSTRUCTURE *i)                 \
{                                                                           \
    if (!i)                                                                 \
        return STRUCTURE##_peek_##SUBSTRUCTURE(s);                          \
    struct uchain *list = STRUCTURE##_to_##ULIST(s);                        \
    struct uchain *elt = SUBSTRUCTURE##_to_##UCHAIN(i);                     \
    if (list == elt->next)                                                  \
        return NULL;                                                        \
    return SUBSTRUCTURE##_from_##UCHAIN(elt->next);                         \
}                                                                           \
                                                                            \
static inline struct SUBSTRUCTURE *                                         \
STRUCTURE##_delete_iterator_##SUBSTRUCTURE(struct STRUCTURE *s,             \
                                           struct SUBSTRUCTURE *i,          \
                                           struct uchain **tmp)             \
{                                                                           \
    struct uchain *list = STRUCTURE##_to_##ULIST(s);                        \
    struct uchain *next = *tmp;                                             \
    if (!next)                                                              \
        next = ulist_peek(list);                                            \
    else if (next == list) {                                                \
        *tmp = NULL;                                                        \
        return NULL;                                                        \
    }                                                                       \
    *tmp = next->next;                                                      \
    return SUBSTRUCTURE##_from_##UCHAIN(next);                              \
}

#define ulist_helper_iterator(iterator, ulist, uchain)                      \
    for (uchain = NULL; uchain = iterator(ulist, uchain); )

#define ulist_helper_delete_iterator(iterator, ulist, uchain, uchain_tmp)   \
    for (uchain_tmp = NULL; uchain = iterator(ulist, uchain, &uchain_tmp); )

#ifdef __cplusplus
}
#endif
#endif
