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

#include <upipe/urefcount.h>
#include <upipe/uclock.h>
#include <upipe/uclock_std.h>
#include <upipe/urefcount_helper.h>

#include "uclock_blackmagic_sink.h"

#include "include/DeckLinkAPI.h"

#define PRINT_PERIODICITY   (UCLOCK_FREQ * 1)

struct uclock_bmd_sink {
    struct urefcount urefcount;
    struct uclock uclock;
    struct uclock *uclock_std;
    IDeckLinkOutput *deckLinkOutput;
    uint64_t offset;
    uint64_t start_time;
    bool initializing;
};

UREFCOUNT_HELPER(uclock_bmd_sink, urefcount, uclock_bmd_sink_free);
UBASE_FROM_TO(uclock_bmd_sink, uclock, uclock, uclock);

static uint64_t uclock_bmd_sink_now(struct uclock *uclock)
{
    struct uclock_bmd_sink *uclock_bmd_sink =
        uclock_bmd_sink_from_uclock(uclock);
    BMDTimeValue hardware_time = UINT64_MAX, time_in_frame, ticks_per_frame;
    uint64_t std_clock = uclock_now(uclock_bmd_sink->uclock_std);

    if (!uclock_bmd_sink->deckLinkOutput)
        return std_clock;

    HRESULT res = uclock_bmd_sink->deckLinkOutput->GetHardwareReferenceClock(
            UCLOCK_FREQ, &hardware_time, &time_in_frame, &ticks_per_frame);
    if (res != S_OK)
        return std_clock;

    if (uclock_bmd_sink->offset == UINT64_MAX) {
        uclock_bmd_sink->offset = hardware_time;
        uclock_bmd_sink->start_time = std_clock;
        uclock_bmd_sink->initializing = true;
        return std_clock;
    }

    uint64_t hw_clock =
        (uint64_t)(hardware_time - uclock_bmd_sink->offset) +
        uclock_bmd_sink->start_time;
    uint64_t diff =
        hw_clock > std_clock ? hw_clock - std_clock : std_clock - hw_clock;

    if (uclock_bmd_sink->initializing) {
        if (std_clock - uclock_bmd_sink->start_time < UCLOCK_FREQ / 100)
            return std_clock;

        if (diff > UCLOCK_FREQ / 1000) {
            uclock_bmd_sink->offset = hardware_time;
            uclock_bmd_sink->start_time = std_clock;
            return std_clock;
        }
        uclock_bmd_sink->initializing = false;
    }
    return hw_clock;
}

static void uclock_bmd_sink_free(struct uclock_bmd_sink *uclock_bmd_sink)
{
    uclock_bmd_sink->deckLinkOutput->Release();
    uclock_bmd_sink->deckLinkOutput = NULL;
    uclock_bmd_sink_clean_urefcount(uclock_bmd_sink);
    uclock_release(uclock_bmd_sink->uclock_std);
    free(uclock_bmd_sink);
}

struct uclock *uclock_bmd_sink_alloc(IDeckLink *deckLink,
                                     struct uclock *uclock_std)
{
    struct uclock_bmd_sink *uclock_bmd_sink =
        (struct uclock_bmd_sink *)malloc(sizeof (*uclock_bmd_sink));
    if (unlikely(!uclock_bmd_sink))
        return NULL;

    if (!deckLink || !uclock_std ||
        deckLink->QueryInterface(
            IID_IDeckLinkOutput,
            (void **)&uclock_bmd_sink->deckLinkOutput) != S_OK) {
        free(uclock_bmd_sink);
        return NULL;
    }
    uclock_bmd_sink->uclock_std = uclock_use(uclock_std);
    uclock_bmd_sink->offset = UINT64_MAX;
    uclock_bmd_sink->start_time = UINT64_MAX;
    uclock_bmd_sink->initializing = true;

    struct uclock *uclock = uclock_bmd_sink_to_uclock(uclock_bmd_sink);
    uclock_bmd_sink_init_urefcount(uclock_bmd_sink);
    uclock->refcount = uclock_bmd_sink_to_urefcount(uclock_bmd_sink);
    uclock->uclock_now = uclock_bmd_sink_now;
    return uclock_bmd_sink_to_uclock(uclock_bmd_sink);
}
