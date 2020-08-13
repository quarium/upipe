/*
 * Copyright (C) 2020 EasyTools
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

#include <upipe/ubase.h>
#include <upipe/upipe.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict_inline.h>
#include <upipe/uref_std.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/uref_clock.h>
#include <upipe/uprobe_dejitter.h>
#include <upipe/uclock.h>
#include <upipe/uclock_std.h>
#include <upump-ev/upump_ev.h>

#include <limits.h>
#include <getopt.h>

#define MIN_DEVIATION   0
#define FREQ            (UCLOCK_FREQ / 100)
#define MAX_DEVIATION   (FREQ / 10)

enum opt {
    OPT_HELP,
    OPT_VERBOSE = 'v',
    OPT_QUIET = 'q',
};

static const struct option options[] = {
    { "help", no_argument, NULL, OPT_HELP },
    { "verbose", no_argument, NULL, OPT_VERBOSE },
    { "quiet", no_argument, NULL, OPT_QUIET },
    { 0, 0, 0, 0 },
};

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [options] source.ts\n", name);
    fprintf(stderr, "   --help: print this help\n");
    fprintf(stderr, "   --verbose: be more verbose\n");
    fprintf(stderr, "   --quiet: be more quiet\n");
}

static enum uprobe_log_level uprobe_log_level = UPROBE_LOG_NOTICE;
static struct uprobe *uprobe = NULL;
static struct uref_mgr *uref_mgr = NULL;
static struct uclock *uclock = NULL;
static struct upipe upipe;
static int64_t ppm = 0;

static void timer_cb(struct upump *upump)
{
#define PPM(Value)  (Value * (int64_t)UCLOCK_FREQ / 1000000)
    struct urational drift = {
        .num = UCLOCK_FREQ + PPM(ppm),
        .den = UCLOCK_FREQ
    };
    struct uref *uref = uref_alloc_control(uref_mgr);
    uint64_t now = uclock_now(uclock);
    static uint64_t first_now = UINT64_MAX;
    if (first_now == UINT64_MAX)
        first_now = now;

    uint64_t cr_prog = (now - first_now) * drift.num / drift.den;
    cr_prog += random() % MAX_DEVIATION;
    uref_clock_set_cr_sys(uref, now);
    upipe_throw_clock_ref(&upipe, uref, cr_prog, 0);
    uref_free(uref);
}

int main(int argc, char *argv[])
{
    assert(argc > 1);
    const char *name = argv[0];

    int c;
    while ((c = getopt_long(argc, argv, "vq", options, NULL)) != -1) {
        switch (c) {
            case OPT_HELP:
                usage(name);
                exit(0);
                break;
            case OPT_VERBOSE:
                switch (uprobe_log_level) {
                    case UPROBE_LOG_ERROR:
                        uprobe_log_level = UPROBE_LOG_WARNING;
                        break;
                    case UPROBE_LOG_WARNING:
                        uprobe_log_level = UPROBE_LOG_NOTICE;
                        break;
                    case UPROBE_LOG_NOTICE:
                        uprobe_log_level = UPROBE_LOG_INFO;
                        break;
                    case UPROBE_LOG_INFO:
                        uprobe_log_level = UPROBE_LOG_DEBUG;
                        break;
                    default:
                        uprobe_log_level = UPROBE_LOG_VERBOSE;
                }
                break;
            case OPT_QUIET:
                switch (uprobe_log_level) {
                    case UPROBE_LOG_NOTICE:
                        uprobe_log_level = UPROBE_LOG_WARNING;
                        break;
                    case UPROBE_LOG_INFO:
                        uprobe_log_level = UPROBE_LOG_NOTICE;
                        break;
                    case UPROBE_LOG_DEBUG:
                        uprobe_log_level = UPROBE_LOG_INFO;
                        break;
                    case UPROBE_LOG_VERBOSE:
                        uprobe_log_level = UPROBE_LOG_DEBUG;
                        break;
                    default:
                        uprobe_log_level = UPROBE_LOG_ERROR;
                }
                break;

            default:
                usage(name);
                exit(-1);
        }
    }

    assert(optind < argc);
    ppm = strtoll(argv[optind], NULL, 10);

    /* create managers */
    struct upump_mgr *upump_mgr = upump_ev_mgr_alloc_default(0, 0);
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(0, umem_mgr, -1, -1);
    uref_mgr = uref_std_mgr_alloc(0, udict_mgr, 0);
    udict_mgr_release(udict_mgr);
    uclock = uclock_std_alloc(0);

    /* create root probe */
    uprobe = uprobe_stdio_alloc(NULL, stderr, uprobe_log_level);
    assert(uprobe != NULL);
    uprobe_stdio_set_time_format(uprobe, "%H:%M:%S");
    uprobe = uprobe_uref_mgr_alloc(uprobe, uref_mgr);
    assert(uprobe != NULL);
    uprobe = uprobe_upump_mgr_alloc(uprobe, upump_mgr);
    assert(uprobe != NULL);
    uprobe = uprobe_dejitter_alloc(uprobe, true, 1);
    uprobe_dejitter_set_minimum_deviation(uprobe, MIN_DEVIATION);

    upipe_init(&upipe, NULL, uprobe_use(uprobe));

    struct upump *timer = upump_alloc_timer(upump_mgr,
                                            timer_cb, NULL, NULL,
                                            0, FREQ);
    assert(timer);
    upump_start(timer);

    /* main loop */
    upump_mgr_run(upump_mgr, NULL);

    /* release probes, pipes and managers */
    upump_free(timer);
    uclock_release(uclock);
    upipe_clean(&upipe);
    uprobe_release(uprobe);
    uref_mgr_release(uref_mgr);
    umem_mgr_release(umem_mgr);
    upump_mgr_release(upump_mgr);

    return 0;
}
