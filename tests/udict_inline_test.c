/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
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
 * @short unit tests for the inline manager of dictionary attributes
 */

#undef NDEBUG

#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/udict_dump.h>
#include <upipe/uprobe.h>
#include <upipe/uprobe_stdio.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#define UDICT_POOL_DEPTH 1

#define SALUTATION "Hello everyone, this is just some padding to make the structure bigger, if you don't mind."

int main(int argc, char **argv)
{
    struct uprobe *uprobe = uprobe_stdio_alloc(NULL, stdout, UPROBE_LOG_DEBUG);
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH, umem_mgr,
                                                   -1, -1);
    assert(mgr != NULL);

    struct udict *udict1 = udict_alloc(mgr, 0);
    assert(udict1 != NULL);

    uint8_t opaque[27];
    memset(opaque, 0xaa, sizeof(opaque));
    assert(udict_set_opaque(udict1, opaque, sizeof(opaque), UDICT_TYPE_OPAQUE,
                            "x.opaque"));
    assert(udict_set_string(udict1, "pouet", UDICT_TYPE_FLOW_DEF, NULL));
    assert(udict_set_void(udict1, NULL, UDICT_TYPE_FLOW_ERROR, NULL));
    assert(udict_set_bool(udict1, true, UDICT_TYPE_BOOL, "x.truc"));
    assert(udict_set_unsigned(udict1, UINT64_MAX, UDICT_TYPE_CLOCK_PTS, NULL));
    assert(udict_set_int_va(udict1, INT64_MAX, UDICT_TYPE_INT,
                            "x.date[%d]", 400));
    assert(udict_set_float(udict1, 1.0, UDICT_TYPE_FLOAT, "x.version"));
    assert(udict_set_string(udict1, SALUTATION, UDICT_TYPE_STRING,
                            "x.salutation"));
    struct urational rational = { num: 64, den: 45 };
    assert(udict_set_rational_va(udict1, rational, UDICT_TYPE_RATIONAL,
                                 "x.ar[%d]", 0));

    const uint8_t *opaque2;
    size_t size;
    assert(udict_get_opaque(udict1, &opaque2, &size, UDICT_TYPE_OPAQUE,
                            "x.opaque"));
    assert(size == sizeof(opaque));
    assert(!memcmp(opaque, opaque2, sizeof(opaque)));
    const char *string;
    assert(udict_get_string(udict1, &string, UDICT_TYPE_FLOW_DEF, NULL));
    assert(!strcmp(string, "pouet"));
    assert(!udict_get_void(udict1, NULL, UDICT_TYPE_VOID, "f.eof"));
    assert(udict_get_void(udict1, NULL, UDICT_TYPE_FLOW_ERROR, NULL));

    assert(udict_delete(udict1, UDICT_TYPE_FLOW_ERROR, NULL));
    assert(udict_delete(udict1, UDICT_TYPE_FLOW_DEF, NULL));
    assert(!udict_delete(udict1, UDICT_TYPE_VOID, "x.truc"));
    assert(!udict_delete(udict1, UDICT_TYPE_BOOL, "k.pts"));

    bool b;
    assert(udict_get_bool(udict1, &b, UDICT_TYPE_BOOL, "x.truc"));
    assert(b);
    uint64_t u;
    assert(udict_get_unsigned(udict1, &u, UDICT_TYPE_CLOCK_PTS, NULL));
    assert(u == UINT64_MAX);
    int64_t d;
    assert(udict_get_int_va(udict1, &d, UDICT_TYPE_INT, "x.date[%d]", 400));
    assert(d == INT64_MAX);
    double f;
    assert(udict_get_float(udict1, &f, UDICT_TYPE_FLOAT, "x.version"));
    assert(f == 1.0);
    assert(udict_get_string(udict1, &string, UDICT_TYPE_STRING,
                            "x.salutation"));
    assert(!strcmp(string, SALUTATION));
    struct urational r;
    assert(udict_get_rational_va(udict1, &r, UDICT_TYPE_RATIONAL,
                                 "x.ar[%d]", 0));
    assert(r.num == 64 && r.den == 45);

    udict_dump(udict1, uprobe);

    struct udict *udict2 = udict_dup(udict1);
    assert(udict2 != NULL);
    udict_dump(udict2, uprobe);
    udict_free(udict2);

    udict2 = udict_copy(mgr, udict1);
    assert(udict2 != NULL);
    udict_dump(udict2, uprobe);
    udict_free(udict2);

    udict_free(udict1);
    udict_mgr_release(mgr);

    umem_mgr_release(umem_mgr);
    uprobe_stdio_free(uprobe);
    return 0;
}
