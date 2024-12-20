
/*
 * Copyright (C) 2024 EasyTools S.A.S.
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

#ifndef _UPIPE_MODULES_UPIPE_S302M_ENCODE_H_
#define _UPIPE_MODULES_UPIPE_S302M_ENCODE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/upipe.h"

/** @This is the s302m encoder pipe signature */
#define UPIPE_S302M_ENC_SIGNATURE UBASE_FOURCC('S','3','0','2')

/** @This returns the management structure for all s302m encoders.
 *
 * @return pointer to the manager
 */
struct upipe_mgr *upipe_s302m_enc_mgr_alloc(void);

#ifdef __cplusplus
}
#endif

#endif
