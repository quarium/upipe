/*
 * Copyright (C) 2020 EasyTools
 *
 * Authors: Arnaud de Turckheim
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation https (the
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
 * @short HTTPS hooks for SSL data read/write.
 */

#ifndef _UPIPE_MODULES_HTTPS_SOURCE_HOOK_H_
#define _UPIPE_MODULES_HTTPS_SOURCE_HOOK_H_

#include <upipe/uref.h>
#include <upipe-modules/upipe_http_source.h>
#include <bearssl.h>

/** This describes a x509 no anchor context to allow not trusted certificate. */
struct x509_noanchor_context {
    const br_x509_class *vtable;
    const br_x509_class **inner;
};

/** @This describes a SSL context for HTTPS. */
struct https_src_hook {
    /** public hook structure */
    struct upipe_http_src_hook hook;
    /** client structure */
    br_ssl_client_context client;
    /** x509 context */
    br_x509_minimal_context x509;
    /** io buffer */
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    /** no anchor context */
    struct x509_noanchor_context x509_noanchor;
};

/** @This initializes the ssl context.
 *
 * @param https private SSL HTTPS context to initialize
 * @param flow_def connection attributes
 * @return the public hook description
 */
struct upipe_http_src_hook *https_src_hook_init(struct https_src_hook *https,
                                                struct uref *flow_def);

#endif
