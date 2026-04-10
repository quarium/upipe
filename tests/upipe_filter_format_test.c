/*
 * Copyright (C) 2025 EasyTools
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

#undef NDEBUG

#include "upipe/uprobe.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/uprobe_stdio.h"
#include "upipe/uprobe_ubuf_mem.h"
#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_pic_mem.h"
#include "upipe/ubuf_mem.h"
#include "upipe/uref.h"
#include "upipe/uref_attr.h"
#include "upipe/uref_sound_flow.h"
#include "upipe/uref_sound_flow_formats.h"
#include "upipe/uref_sound.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uref_pic_flow_formats.h"
#include "upipe/uref_pic.h"
#include "upipe/uref_std.h"
#include "upipe/upipe.h"
#include "upipe/uref_dump.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_flow_format.h"
#include "upipe/upipe_helper_ubuf_mgr.h"
#include "upipe/upipe_helper_inner.h"
#include "upipe/upipe_helper_bin_output.h"
#include "upipe-filters/upipe_filter_format.h"
#include "upipe-swresample/upipe_swr.h"
#include "upipe-swscale/upipe_sws.h"
#include "upipe-av/upipe_avfilter.h"

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#define UDICT_POOL_DEPTH    5
#define UREF_POOL_DEPTH     5
#define UBUF_POOL_DEPTH     5
#define UPROBE_LOG_LEVEL    UPROBE_LOG_INFO

#define WIDTH               (16 * 4)
#define HEIGHT              (9 * 4)
#define FRAMES              5

UREF_ATTR_STRING(test, input, "test.input", test attribute on input flow def);
UREF_ATTR_STRING(test, wanted, "test.wanted", test attibute on wanted flow def);
UREF_ATTR_STRING(test, output, "test.output",
                 test attribute requested by output);
UREF_ATTR_STRING(test, name, "test.name", frame name);
UREF_ATTR_UNSIGNED(test, counter, "counter", frame counter);

struct input {
    struct upipe upipe;
    struct urefcount urefcount;
    struct uref *flow_def;
    struct uref *flow_def_input;
    struct uref *flow_def_provided;
    struct uref *flow_format;
    struct upipe *output;
    enum upipe_helper_output_state output_state;
    struct uchain requests;
    struct urequest request_flow_format;
    struct urequest request_ubuf_mgr;
    struct ubuf_mgr *ubuf_mgr;
};

static int input_check_flow_format(struct upipe *upipe,
                                   struct uref *flow_format);
static int input_check_ubuf_mgr(struct upipe *upipe, struct uref *flow_format);

UBASE_FROM_TO(input, upipe, upipe, upipe);
UPIPE_HELPER_UREFCOUNT(input, urefcount, input_free);
UPIPE_HELPER_OUTPUT(input, output, flow_def, output_state, requests);
UPIPE_HELPER_FLOW_FORMAT(input, request_flow_format, input_check_flow_format,
                         input_register_output_request,
                         input_unregister_output_request);
UPIPE_HELPER_UBUF_MGR(input, ubuf_mgr, flow_format, request_ubuf_mgr,
                      input_check_ubuf_mgr, input_register_output_request,
                      input_unregister_output_request)

struct output;
typedef void (*output_check_uref)(struct output *, struct uref *);
typedef void (*output_check_flow_def)(struct output *, struct uref *);

struct output {
    struct upipe upipe;
    struct urefcount urefcount;
    struct urequest *urequest;
    struct uref *flow_def;
    struct uref *attributes;
    output_check_uref check_uref;
    output_check_flow_def check_flow_def;
    unsigned counter;
    const struct uref_pic_flow_format *format;
};

UBASE_FROM_TO(output, upipe, upipe, upipe);
UPIPE_HELPER_UREFCOUNT(output, urefcount, output_free);

struct test_pipe {
    struct upipe upipe;
    struct urefcount urefcount;
    struct upipe *input;
    struct upipe *ffmt;
    struct upipe *output;
};

UBASE_FROM_TO(test_pipe, upipe, upipe, upipe);
UPIPE_HELPER_UREFCOUNT(test_pipe, urefcount, test_pipe_free);
UPIPE_HELPER_INNER(test_pipe, input);
UPIPE_HELPER_INNER(test_pipe, ffmt);
UPIPE_HELPER_INNER(test_pipe, output);

typedef void (*test_cb)(void);

#define MAX_CUSTOM_FORMATS 200

static struct urational fps = { .num = 25, .den = 1 };
static struct umem_mgr *umem_mgr = NULL;
static struct uref_mgr *uref_mgr;
static struct uprobe *logger = NULL;
static enum uprobe_log_level log_level = UPROBE_LOG_LEVEL;
static const char *input_hw_type = NULL;
static const char *input_hw_device = NULL;
static const char *output_hw_type = NULL;
static const char *output_hw_device = NULL;
static unsigned nb_formats = 0;
static const struct uref_pic_flow_format *custom_formats[MAX_CUSTOM_FORMATS] = {
    0
};
static const struct uref_pic_flow_format **formats = custom_formats;


static void test_start(const char *name, test_cb test)
{
    test = test;

    uprobe_notice_va(logger, NULL, "-----------------------------------------");
    uprobe_notice_va(logger, NULL, "    start %s", name);
}

#define TEST_START(test)   test_start(#test, test)

static void test_end(const char *name, test_cb test)
{
    uprobe_notice_va(logger, NULL, "-----------------------------------------");
}

#define TEST_END(test)   test_end(#test, test)

/** definition of our uprobe */
static int catch(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    switch (event) {
        default:
            assert(0);
            break;
        case UPROBE_SYNC_ACQUIRED:
        case UPROBE_SYNC_LOST:
        case UPROBE_READY:
        case UPROBE_DEAD:
        case UPROBE_NEW_FLOW_DEF:
            break;
    }
    return UBASE_ERR_NONE;
}

static struct uref *pic_alloc(struct ubuf_mgr *ubuf_mgr, unsigned counter,
                              int width, int height)
{
    struct uref *uref = uref_pic_alloc(uref_mgr, ubuf_mgr, width, height);
    assert(uref);
    ubase_assert(uref_pic_set_progressive(uref, true));

    uint64_t hsize;
    uint64_t vsize;
    uint8_t macro;
    ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macro));
    assert(width && height && hsize == width && vsize == height);

    const char *chroma;
    uref_pic_foreach_plane(uref, chroma) {
        uint8_t hsub;
        uint8_t vsub;
        uint8_t macropixel;
        uint64_t stride;
        uint8_t *buf;

        ubase_assert(uref_pic_plane_write(uref, chroma, 0, 0, -1, -1, &buf));
        ubase_assert(uref_pic_plane_size(uref, chroma, &stride, &hsub, &vsub,
                                         &macropixel));
        assert(stride && hsub && vsub);

        unsigned lines = height / vsub;
        unsigned size = (width * macropixel) / (hsub * macro);
        for (uint64_t y = 0; y < lines; y++) {
            for (uint64_t x = 0; x < size; x++)
                buf[x] = counter;
            buf += stride;
        }
        uref_pic_plane_unmap(uref, chroma, 0, 0, -1, -1);
    }
    return uref;
}

static struct uref *sound_alloc(struct ubuf_mgr *ubuf_mgr, unsigned counter)
{
    struct uref *uref = uref_sound_alloc(uref_mgr, ubuf_mgr, 10);
    assert(uref);
    return uref;
}

static void pic_dump(struct uref *flow_def, struct uref *uref)
{
    uint64_t width = 0;
    uint64_t height = 0;
    ubase_assert(uref_pic_flow_get_hsize(flow_def, &width));
    ubase_assert(uref_pic_flow_get_vsize(flow_def, &height));

    uint64_t hsize = 0;
    uint64_t vsize = 0;
    uint8_t macro = 0;
    ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macro));

    uprobe_dbg_va(logger, NULL, "pic size %" PRIu64 "x%" PRIu64, hsize, vsize);
    assert(width == hsize && height == vsize);

    const char *chroma;
    uref_pic_foreach_plane(uref, chroma)
    {
        uint8_t hsub;
        uint8_t vsub;
        uint64_t stride;
        uint8_t macropixel;
        const uint8_t *buf;

        ubase_assert(uref_pic_plane_size(uref, chroma, &stride, &hsub, &vsub,
                                         &macropixel));

        assert(hsub && vsub && width && height && macro && stride &&
               macropixel);

        uprobe_dbg_va(logger, NULL,
                      "  %s: hsub=%u, vsub=%u, stride=%" PRIu64
                      ", macropixel=%u\n",
                      chroma, hsub, vsub, stride, macropixel);

        ubase_assert(uref_pic_plane_read(uref, chroma, 0, 0, -1, -1, &buf));
        uref_pic_plane_unmap(uref, chroma, 0, 0, -1, -1);
    }
}

static void sound_dump(struct uref *flow_def, struct uref *uref)
{
    size_t size = 0;
    uint8_t sample_size = 0;
    ubase_assert(uref_sound_size(uref, &size, &sample_size));
    uprobe_dbg_va(logger, NULL, "sound size %zu (%u)", size, sample_size);

    const char *channel;
    uref_sound_foreach_plane(uref, channel)
    {
        const uint8_t *buf;
        uprobe_dbg_va(logger, NULL, "    channel %s", channel);
        ubase_assert(uref_sound_plane_read_uint8_t(uref, channel, 0, -1, &buf));
        uref_sound_plane_unmap(uref, channel, 0, -1);
    }
}

static void frame_dump(struct uref *flow_def, struct uref *uref)
{
    const char *name;
    ubase_assert(uref_test_get_name(uref, &name));
    uprobe_dbg_va(logger, NULL, "%s:", name);
    uref_dump(uref, logger);

    bool pic = ubase_check(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF));
    bool snd = ubase_check(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF));

    if (pic)
        pic_dump(flow_def, uref);
    else if (snd)
        sound_dump(flow_def, uref);
}

static struct upipe *ffmt_alloc(struct uref *flow_def_wanted)
{
    static unsigned wanted = 0;
    assert(flow_def_wanted);
    ubase_assert(
        uref_test_set_wanted_va(flow_def_wanted, "wanted %u", wanted++));

    struct upipe_mgr *upipe_ffmt_mgr = upipe_ffmt_mgr_alloc();
    struct upipe_mgr *upipe_sws_mgr = upipe_sws_mgr_alloc();
    struct upipe_mgr *upipe_swr_mgr = upipe_swr_mgr_alloc();
    struct upipe_mgr *upipe_avfilt_mgr = upipe_avfilt_mgr_alloc();
    upipe_ffmt_mgr_set_sws_mgr(upipe_ffmt_mgr, upipe_sws_mgr);
    upipe_ffmt_mgr_set_swr_mgr(upipe_ffmt_mgr, upipe_swr_mgr);
    upipe_ffmt_mgr_set_avfilter_mgr(upipe_ffmt_mgr, upipe_avfilt_mgr);
    upipe_mgr_release(upipe_sws_mgr);
    upipe_mgr_release(upipe_swr_mgr);
    upipe_mgr_release(upipe_avfilt_mgr);

    struct upipe *upipe = upipe_flow_alloc(
        upipe_ffmt_mgr,
        uprobe_pfx_alloc(uprobe_use(logger), log_level, "ffmt"),
        flow_def_wanted);
    assert(upipe);

    upipe_mgr_release(upipe_ffmt_mgr);

    return upipe;
}

static int output_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct output *output = output_from_upipe(upipe);

    ubase_assert(uref_test_get_input(flow_def, NULL));
    ubase_assert(uref_test_get_wanted(flow_def, NULL));
    ubase_assert(uref_test_get_output(flow_def, NULL));

    const struct uref_pic_flow_format *format =
        uref_pic_flow_get_format(flow_def);
    assert(format);

    struct uref *attributes = uref_dup(flow_def);
    uref_pic_flow_clear_format(attributes);
    if (!output->flow_def) {
        upipe_info_va(upipe, "output set flow def %s", format->name);
        udict_dump_info(attributes->udict, upipe->uprobe);
    } else {
        upipe_info_va(upipe, "output update flow def %s → %s",
                      output->format->name, format->name);
        if (udict_cmp(output->attributes->udict, attributes->udict))
            udict_diff_info(output->attributes->udict, attributes->udict,
                            upipe->uprobe);
    }
    assert(output->urequest);
    uref_free(output->flow_def);
    uref_free(output->attributes);
    output->flow_def = uref_dup(flow_def);
    output->attributes = attributes;
    output->format = format;
    assert(output->flow_def);
    if (output->check_flow_def)
        output->check_flow_def(output, flow_def);
    return UBASE_ERR_NONE;
}

static int output_register_request(struct upipe *upipe,
                                   struct urequest *urequest)
{
    if (urequest->type == UREQUEST_FLOW_FORMAT) {
        struct output *output = output_from_upipe(upipe);
        upipe_dbg(upipe, "request flow format");
        uref_dump(urequest->uref, upipe->uprobe);
        uref_test_set_output(urequest->uref, "output");
        urequest_free_proxy(output->urequest);
        output->urequest = urequest_alloc_proxy(urequest);
    }
    return upipe_throw_provide_request(upipe, urequest);
}

static int output_unregister_request(struct upipe *upipe,
                                   struct urequest *urequest)
{
    if (urequest->type == UREQUEST_FLOW_FORMAT) {
        struct output *output = output_from_upipe(upipe);
        urequest_free_proxy(output->urequest);
        output->urequest = NULL;
    }
    return UBASE_ERR_NONE;
}

static int output_control(struct upipe *upipe, int command, va_list args)
{
    switch (command) {
        case UPIPE_REGISTER_REQUEST: {
            struct urequest *urequest = va_arg(args, struct urequest *);
            return output_register_request(upipe, urequest);
        }

        case UPIPE_UNREGISTER_REQUEST: {
            struct urequest *urequest = va_arg(args, struct urequest *);
            return output_unregister_request(upipe, urequest);
        }

        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return output_set_flow_def(upipe, flow_def);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

static void output_input(struct upipe *upipe, struct uref *uref,
                         struct upump **upump_p)
{
    struct output *output = output_from_upipe(upipe);
    uref_test_set_output_va(uref, "output %u", output->counter);
    uref_test_set_name_va(uref, "output %u", output->counter);
    frame_dump(output->flow_def, uref);

    if (output->check_uref)
        output->check_uref(output, uref);

    uref_free(uref);

    output->counter++;
}

static struct upipe *output_alloc(output_check_flow_def check_flow_def,
                                  output_check_uref check_uref)
{
    static struct upipe_mgr output_mgr = {
        .upipe_input = output_input,
        .upipe_control = output_control,
    };

    struct output *output = malloc(sizeof(*output));
    assert(output);
    struct upipe *upipe = output_to_upipe(output);
    upipe_init(upipe, upipe_mgr_use(&output_mgr),
               uprobe_pfx_alloc(uprobe_use(logger), log_level, "output"));
    output_init_urefcount(upipe);
    output->check_flow_def = check_flow_def;
    output->check_uref = check_uref;
    output->urequest = NULL;
    output->flow_def = NULL;
    output->counter = 0;
    output->format = NULL;
    output->attributes = NULL;

    upipe_throw_ready(upipe);
    return upipe;
}

static void output_free(struct upipe *upipe)
{
    struct output *output = output_from_upipe(upipe);

    upipe_throw_dead(upipe);

    assert(output->urequest == NULL);
    uref_free(output->attributes);
    uref_free(output->flow_def);
    output_clean_urefcount(upipe);
    upipe_clean(upipe);
    free(output);
}

static int input_check_ubuf_mgr(struct upipe *upipe, struct uref *flow_format)
{
    assert(flow_format);

    input_store_flow_def(upipe, flow_format);

    return UBASE_ERR_NONE;
}

static int input_check_flow_format(struct upipe *upipe,
                                   struct uref *flow_format)
{
    struct input *input = input_from_upipe(upipe);
    assert(flow_format);
    assert(input->flow_def_input);

    uref_free(input->flow_def_provided);
    input->flow_def_provided = flow_format;

    return UBASE_ERR_NONE;
}

static int input_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    struct input *input = input_from_upipe(upipe);
    struct urational fps = { .num = 0, .den = 0 };
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t duration = UINT64_MAX;

    assert(flow_def);
    ubase_assert(uref_pic_flow_set_align(flow_def, 64));
    ubase_assert(uref_pic_flow_set_hmappend(flow_def, 32));
    ubase_assert(uref_test_set_input(flow_def, "input"));

    uref_free(input->flow_def_input);
    input->flow_def_input = uref_dup(flow_def);
    assert(input->flow_def_input);

    input_require_flow_format(upipe, uref_dup(flow_def));
    assert(input->flow_def_provided);

    flow_def = uref_dup(flow_def);
    assert(flow_def);

    bool pic = ubase_check(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF));
    bool snd = ubase_check(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF));

    if (pic) {
        uref_pic_flow_get_fps(flow_def, &fps);
        uref_pic_flow_get_hsize(flow_def, &width);
        uref_pic_flow_get_vsize(flow_def, &height);

        if (!width)
            uref_pic_flow_get_hsize(input->flow_def_provided, &width);
        if (!width) {
            upipe_dbg_va(upipe, "use default width %u", WIDTH);
            width = WIDTH;
        }

        if (!height)
            uref_pic_flow_get_vsize(input->flow_def_provided, &height);
        if (!height) {
            upipe_dbg_va(upipe, "use default height %u", HEIGHT);
            height = HEIGHT;
        }

        if (!fps.num || !fps.den) {
            uref_pic_flow_get_fps(input->flow_def_provided, &fps);
            uref_pic_flow_copy_fps(flow_def, input->flow_def_provided);
        }
        if (fps.num && fps.den)
            duration = fps.den * UCLOCK_FREQ / fps.num;

        ubase_assert(uref_pic_flow_set_hsize(flow_def, width));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, height));
    } else if (snd) {

    } else {
        abort();
    }

    input_require_ubuf_mgr(upipe, flow_def);
    assert(input->ubuf_mgr);

    if (input->output_state == UPIPE_HELPER_OUTPUT_NONE) {
        UBASE_RETURN(upipe_set_flow_def(input->output, input->flow_def));
        input->output_state = UPIPE_HELPER_OUTPUT_VALID;
    }

    for (unsigned counter = 0; counter < FRAMES; counter++) {
        struct uref *uref = NULL;
        if (pic)
            uref = pic_alloc(input->ubuf_mgr, counter, width, height);
        else if (snd)
            uref = sound_alloc(input->ubuf_mgr, counter);

        if (uref_pic_check_progressive(input->flow_def))
            uref_pic_set_progressive(uref, true);
        else
            uref_pic_set_progressive(uref, false);

        uref_test_set_counter(uref, counter);
        if (duration != UINT64_MAX) {
            uref_clock_set_duration(uref, duration);
            uref_clock_set_pts_prog(uref, counter * duration);
        }
        uref_test_set_input_va(uref, "input %u", counter);
        uref_test_set_name_va(uref, "input %u", counter);
        frame_dump(flow_def, uref);
        input_output(upipe, uref, NULL);
    }

    return UBASE_ERR_NONE;
}

static int input_control(struct upipe *upipe, int command, va_list args)
{
    struct input *input = input_from_upipe(upipe);

    switch (command) {
        case UPIPE_SET_OUTPUT:
        case UPIPE_GET_OUTPUT:
        case UPIPE_GET_FLOW_DEF:
            return input_control_output(upipe, command, args);

        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            assert(flow_def);
            return input_set_flow_def(upipe, flow_def);
        }

        case UPIPE_SET_OPTION: {
            const char *option = va_arg(args, const char *);
            const char *value = va_arg(args, const char *);
            assert(input->output);
            ubase_assert(upipe_set_option(input->output, option, value));
            return UBASE_ERR_NONE;
        }
    }
    return UBASE_ERR_INVALID;
}

static struct upipe *input_alloc(void)
{
    static struct upipe_mgr input_mgr = {
        .upipe_control = input_control,
    };

    struct input *input = malloc(sizeof(*input));
    assert(input);
    struct upipe *upipe = input_to_upipe(input);
    upipe_init(upipe, upipe_mgr_use(&input_mgr),
               uprobe_pfx_alloc(uprobe_use(logger), log_level, "input"));
    input_init_urefcount(upipe);
    input_init_output(upipe);
    input_init_flow_format(upipe);
    input_init_ubuf_mgr(upipe);
    input->flow_def_input = NULL;
    input->flow_def_provided = NULL;

    upipe_throw_ready(upipe);

    return upipe;
}

static void input_free(struct upipe *upipe)
{
    struct input *input = input_from_upipe(upipe);

    upipe_throw_dead(upipe);

    uref_free(input->flow_def_input);
    uref_free(input->flow_def_provided);
    input_clean_ubuf_mgr(upipe);
    input_clean_flow_format(upipe);
    input_clean_output(upipe);
    input_clean_urefcount(upipe);
    upipe_clean(upipe);
    free(input);
}

static int test_pipe_control(struct upipe *upipe, int command, va_list args)
{
    switch (command) {
        case UPIPE_SET_OUTPUT:
        case UPIPE_GET_OUTPUT:
        case UPIPE_GET_FLOW_DEF:
            return test_pipe_control_output(upipe, command, args);

        case UPIPE_SET_FLOW_DEF:
            return test_pipe_control_input(upipe, command, args);

        case UPIPE_SET_OPTION:
            return test_pipe_control_ffmt(upipe, command, args);
    }
    abort();
    return UBASE_ERR_UNHANDLED;
}

static struct upipe *test_pipe_alloc(struct uref *flow_def_wanted,
                                     output_check_flow_def check_flow_def,
                                     output_check_uref check_uref)
{
    static struct upipe_mgr test_pipe_mgr = {
        .upipe_control = test_pipe_control,
    };

    struct test_pipe *test_pipe = malloc(sizeof(*test_pipe));
    assert(test_pipe);
    struct upipe *upipe = test_pipe_to_upipe(test_pipe);
    upipe_init(upipe, upipe_mgr_use(&test_pipe_mgr),
               uprobe_pfx_alloc(uprobe_use(logger), log_level, "test"));
    test_pipe_init_urefcount(upipe);
    test_pipe_init_input(upipe);
    test_pipe_init_ffmt(upipe);
    test_pipe_init_output(upipe);

    if (output_hw_type)
        uref_pic_flow_set_surface_type_va(flow_def_wanted, "av.%s",
                                          output_hw_type);

    struct upipe *input = input_alloc();
    struct upipe *ffmt = ffmt_alloc(flow_def_wanted);
    struct upipe *output = output_alloc(check_flow_def, check_uref);
    assert(input);
    assert(ffmt);
    assert(output);
    test_pipe_store_input(upipe, input);
    test_pipe_store_ffmt(upipe, ffmt);
    test_pipe_store_output(upipe, output);

    if (input_hw_type != NULL && input_hw_device != NULL) {
        struct upipe_mgr *upipe_ffmt_mgr = upipe_ffmt_mgr_alloc();
        assert(upipe_ffmt_mgr);
        struct upipe_mgr *upipe_avfilt_mgr = upipe_avfilt_mgr_alloc();
        assert(upipe_avfilt_mgr);
        ubase_assert(
            upipe_ffmt_mgr_set_avfilter_mgr(upipe_ffmt_mgr, upipe_avfilt_mgr));
        upipe_mgr_release(upipe_avfilt_mgr);
        struct uref *flow_def_upload = uref_alloc_control(uref_mgr);
        ubase_assert(uref_flow_set_def(flow_def_upload, "pic."));
        ubase_assert(uref_pic_flow_set_surface_type_va(flow_def_upload, "av.%s",
                                                       input_hw_type));
        assert(flow_def_upload);
        struct upipe *ffmt_upload = upipe_flow_alloc(
            upipe_ffmt_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), log_level, "upload"),
            flow_def_upload);
        uref_free(flow_def_upload);
        upipe_mgr_release(upipe_ffmt_mgr);
        assert(ffmt_upload);
        ubase_assert(upipe_set_option(ffmt, "forward_flow_format", "false"));
        ubase_assert(upipe_avfilt_set_hw_config(ffmt_upload, input_hw_type,
                                                input_hw_device));

        ubase_assert(upipe_set_output(ffmt, output));
        ubase_assert(upipe_set_output(input, ffmt_upload));
        ubase_assert(upipe_set_output(ffmt_upload, ffmt));
        upipe_release(ffmt_upload);
    } else {
        ubase_assert(upipe_set_output(ffmt, output));
        ubase_assert(upipe_set_output(input, ffmt));
    }

    if (output_hw_device)
        ubase_assert(
            upipe_avfilt_set_hw_config(ffmt, output_hw_type, output_hw_device));

    upipe_throw_ready(upipe);

    return upipe;
}

static void test_pipe_free(struct upipe *upipe)
{
    struct test_pipe *test_pipe = test_pipe_from_upipe(upipe);

    upipe_throw_dead(upipe);

    test_pipe_clean_output(upipe);
    test_pipe_clean_ffmt(upipe);
    test_pipe_clean_input(upipe);
    test_pipe_clean_urefcount(upipe);
    upipe_clean(upipe);
    free(test_pipe);
}

static void test_input_pic(void)
{
    struct upipe *input = NULL;

    TEST_START(test_input_pic);

    {
        struct uref *flow_def_wanted = uref_alloc_control(uref_mgr);
        ubase_assert(uref_flow_set_def(flow_def_wanted, UREF_PIC_FLOW_DEF));
        input = test_pipe_alloc(flow_def_wanted, NULL, NULL);
        uref_free(flow_def_wanted);
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        struct uref *flow_def =
            uref_pic_flow_alloc_format(uref_mgr, formats[i]);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        ubase_assert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(uref_sound_flow_formats); i++) {
        struct uref *flow_def = uref_sound_flow_alloc_format(
            uref_mgr, uref_sound_flow_formats[i], 2);
        assert(flow_def);
        ubase_nassert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_input_pic);
}

static void test_input_sound(void)
{
    struct upipe *input = NULL;

    TEST_START(test_input_sound);

    {
        struct uref *flow_def_wanted = uref_alloc_control(uref_mgr);
        ubase_assert(uref_flow_set_def(flow_def_wanted, UREF_SOUND_FLOW_DEF));
        input = ffmt_alloc(flow_def_wanted);
        uref_free(flow_def_wanted);
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        struct uref *flow_def =
            uref_pic_flow_alloc_format(uref_mgr, formats[i]);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        ubase_nassert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(uref_sound_flow_formats); i++) {
        struct uref *flow_def = uref_sound_flow_alloc_format(
            uref_mgr, uref_sound_flow_formats[i], 2);
        assert(flow_def);
        ubase_assert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_input_sound);
}

static void test_scale(void)
{
    struct upipe *input = NULL;

    void check_flow_def(struct output *output, struct uref *flow_def) {
        uint64_t hsize = 0, vsize = 0;
        ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
        ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
        assert(hsize == WIDTH / 2);
        assert(vsize == HEIGHT / 2);
    }

    void check_uref(struct output *output, struct uref *uref) {
        uint64_t hsize = 0, vsize = 0;
        uint8_t macropixel = 0;
        ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
        assert(hsize == WIDTH / 2);
        assert(vsize == HEIGHT / 2);
    }

    TEST_START(test_scale);

    {
        struct uref *flow_def_wanted = uref_alloc_control(uref_mgr);
        ubase_assert(uref_flow_set_def(flow_def_wanted, UREF_PIC_FLOW_DEF));
        ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH / 2));
        ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT / 2));
        ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
        input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
        uref_free(flow_def_wanted);
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        const struct uref_pic_flow_format *f = formats[i];
        uprobe_notice_va(logger, NULL, "format: %s", f->name);
        struct uref *flow_def = uref_pic_flow_alloc_format(uref_mgr, f);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
        ubase_assert(uref_pic_set_progressive(flow_def, true));
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        ubase_assert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_scale);
}

static void test_format(void)
{
    for (unsigned j = 0; j < nb_formats; j++) {
        const struct uref_pic_flow_format *format = formats[j];
        char *name;
        assert(asprintf(&name, "test_format_%s", format->name) > 0);

        void check_flow_def(struct output *output, struct uref *flow_def) {
            uint64_t hsize = 0, vsize = 0;
            ubase_assert(uref_pic_flow_check_format(flow_def, format));
            ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
            ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        }

        void check_uref(struct output *output, struct uref *uref) {
            uint64_t hsize = 0, vsize = 0;
            uint8_t macropixel = 0;
            ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        }

        test_start(name, test_format);

        struct upipe *input = NULL;
        {
            struct uref *flow_def_wanted =
                uref_pic_flow_alloc_format(uref_mgr, format);
            ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
        }

        for (unsigned i = 0; i < nb_formats; i++) {
            struct uref *flow_def =
                uref_pic_flow_alloc_format(uref_mgr, formats[i]);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);

        test_end(name, test_format);
        free(name);
    }
}

static void test_format_avfilter(void)
{
    for (unsigned j = 0; j < nb_formats; j++) {
        const struct uref_pic_flow_format *format = formats[j];
        char *name;
        assert(asprintf(&name, "test_format_avfilter_%s", format->name) > 0);

        void check_flow_def(struct output *output, struct uref *flow_def) {
            uint64_t hsize = 0, vsize = 0;
            ubase_assert(uref_pic_flow_check_format(flow_def, format));
            ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
            ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        }

        void check_uref(struct output *output, struct uref *uref) {
            uint64_t hsize = 0, vsize = 0;
            uint8_t macropixel = 0;
            ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        }

        test_start(name, test_format);

        struct upipe *input = NULL;
        {
            struct uref *flow_def_wanted =
                uref_pic_flow_alloc_format(uref_mgr, format);
            ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
            ubase_assert(upipe_set_option(input, "sw/format", "avfilter"));
        }

        for (unsigned i = 0; i < nb_formats; i++) {
            struct uref *flow_def =
                uref_pic_flow_alloc_format(uref_mgr, formats[i]);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);

        test_end(name, test_format);
        free(name);
    }
}

static void test_deint(void)
{
    void check_flow_def(struct output *output, struct uref *flow_def) {
        uint64_t hsize = 0, vsize = 0;
        assert(uref_pic_check_progressive(flow_def));
        assert(!uref_pic_check_tff(flow_def));
        ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
        ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
        assert(hsize == WIDTH / 2);
        assert(vsize == HEIGHT / 2);
    }

    void check_uref(struct output *output, struct uref *uref) {
        assert(uref_pic_check_progressive(uref));
        assert(!uref_pic_check_tff(uref));
    }

    TEST_START(test_deint);

    for (unsigned j = 0; j < nb_formats; j++) {
        struct upipe *input = NULL;
        {
            struct uref *flow_def_wanted =
                uref_pic_flow_alloc_format(uref_mgr, formats[j]);
            ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH / 2));
            ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT / 2));
            ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
            ubase_assert(uref_pic_set_tff(flow_def_wanted, false));
            input =
                test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
        }

        for (unsigned i = 0; i < nb_formats; i++) {
            struct uref *flow_def =
                uref_pic_flow_alloc_format(uref_mgr, formats[i]);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            {
                ubase_assert(uref_pic_set_progressive(flow_def, true));
                ubase_assert(upipe_set_flow_def(input, flow_def));
            }
            {
                ubase_assert(uref_pic_set_tff(flow_def, true));
                ubase_assert(uref_pic_set_progressive(flow_def, false));
                ubase_assert(upipe_set_flow_def(input, flow_def));
            }
            uref_free(flow_def);
        }

        upipe_release(input);
    }

    TEST_END(test_deint);
}

static void test_deint_avfilter(void)
{
    void check_flow_def(struct output *output, struct uref *flow_def) {
        uint64_t hsize = 0, vsize = 0;
        if (!output_hw_type)
            ubase_assert(uref_pic_flow_check_yuv420p(flow_def));
        assert(uref_pic_check_progressive(flow_def));
        assert(!uref_pic_check_tff(flow_def));
        ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
        ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
        assert(hsize == WIDTH / 2);
        assert(vsize == HEIGHT / 2);
    }

    void check_uref(struct output *output, struct uref *uref) {
        assert(uref_pic_check_progressive(uref));
        assert(!uref_pic_check_tff(uref));
    }

    TEST_START(test_deint_avfilter);

    struct upipe *input = NULL;
    {
        struct uref *flow_def_wanted = uref_pic_flow_alloc_yuv420p(uref_mgr);
        ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH / 2));
        ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT / 2));
        ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
        ubase_assert(uref_pic_set_tff(flow_def_wanted, false));
        input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
        uref_free(flow_def_wanted);
        ubase_assert(upipe_set_option(input, "sw/deinterlace", "avfilter"));
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        struct uref *flow_def =
            uref_pic_flow_alloc_format(uref_mgr, formats[i]);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        {
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
        }
        {
            ubase_assert(uref_pic_set_tff(flow_def, true));
            ubase_assert(uref_pic_set_progressive(flow_def, false));
            ubase_assert(upipe_set_flow_def(input, flow_def));
        }
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_deint_avfilter);
}

static void test_interlace(void)
{
    void check_flow_def(struct output *output, struct uref *flow_def) {
        assert(uref_pic_check_tff(flow_def));
        assert(!uref_pic_check_progressive(flow_def));
    }

    void check_uref(struct output *output, struct uref *uref) {
        uint64_t hsize = 0, vsize = 0;
        uint8_t macropixel = 0;
        assert(!uref_pic_check_progressive(uref));
        ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
        assert(hsize == WIDTH);
        assert(vsize == HEIGHT);
    }

    TEST_START(test_interlace);

    struct upipe *input = NULL;
    {

        struct uref *flow_def_wanted = uref_pic_flow_alloc_nv12(uref_mgr);
        ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH));
        ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT));
        ubase_assert(uref_pic_set_progressive(flow_def_wanted, false));
        ubase_assert(uref_pic_set_tff(flow_def_wanted, true));
        input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
        uref_free(flow_def_wanted);
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        struct uref *flow_def =
            uref_pic_flow_alloc_format(uref_mgr, formats[i]);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH / 2));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT / 2));
        ubase_assert(uref_pic_set_progressive(flow_def, true));
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        ubase_assert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_interlace);
}

static void test_deint_interlace(void)
{
    void check_flow_def(struct output *output, struct uref *flow_def) {
        assert(uref_pic_check_tff(flow_def));
        assert(!uref_pic_check_progressive(flow_def));
    }

    void check_uref(struct output *output, struct uref *uref) {
        uint64_t hsize = 0, vsize = 0;
        uint8_t macropixel = 0;
        assert(!uref_pic_check_progressive(uref));
        ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
        assert(hsize == WIDTH);
        assert(vsize == HEIGHT);
    };

    TEST_START(test_deint_interlace);

    struct upipe *input = NULL;
    {
        struct uref *flow_def_wanted = uref_pic_flow_alloc_nv12(uref_mgr);
        ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH));
        ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT));
        ubase_assert(uref_pic_set_progressive(flow_def_wanted, false));
        ubase_assert(uref_pic_set_tff(flow_def_wanted, true));
        input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
        uref_free(flow_def_wanted);
    }

    for (unsigned i = 0; i < nb_formats; i++) {
        struct uref *flow_def =
            uref_pic_flow_alloc_format(uref_mgr, formats[i]);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH / 2));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT / 2));
        ubase_assert(uref_pic_set_progressive(flow_def, false));
        ubase_assert(uref_pic_set_tff(flow_def, false));
        ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
        ubase_assert(upipe_set_flow_def(input, flow_def));
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_deint_interlace);
}

static void test_auto(void)
{
    struct uref *provide = NULL;
    void check_flow_def(struct output *output, struct uref *flow_def) {
        ubase_assert(uref_test_get_output(flow_def, NULL));

        if (output->counter <= 2) {
            if (!ubase_check(uref_pic_flow_get_surface_type(flow_def, NULL)))
                ubase_assert(uref_pic_flow_check_yuv420p(flow_def));
        }
        else
            ubase_assert(uref_pic_flow_check_nv12(flow_def));

        if (output->counter < 6) {
            assert(uref_pic_check_progressive(flow_def));
            assert(!uref_pic_check_tff(flow_def));
        } else {
            assert(!uref_pic_check_progressive(flow_def));
            assert(uref_pic_check_tff(flow_def));
        }

        uint64_t hsize = 0, vsize = 0;
        ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
        ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
        if (output->counter < 2) {
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        } else {
            assert(hsize == WIDTH / 2);
            assert(vsize == HEIGHT / 2);
        }

        assert(output->counter != 4 && output->counter != 5);
    }

    void check_uref(struct output *output, struct uref *uref) {
        uint64_t hsize = 0, vsize = 0;
        uint8_t macropixel = 0;
        ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));

        if (output->counter > 1) {
            assert(hsize == WIDTH / 2);
            assert(vsize == HEIGHT / 2);
        } else {
            assert(hsize == WIDTH);
            assert(vsize == HEIGHT);
        }

        bool progressive = false;
        ubase_assert(uref_pic_get_progressive(uref, &progressive));
        if (output->counter < 6) {
            assert(progressive);
        } else {
            assert(!progressive);
        }

        if (!provide)
            provide = uref_dup(output->urequest->uref);

        switch (output->counter) {
            case 1: {
                ubase_assert(uref_pic_flow_set_hsize(provide, WIDTH / 2));
                ubase_assert(uref_pic_flow_set_vsize(provide, HEIGHT / 2));
                break;
            }

            case 2: {
                ubase_assert(uref_pic_flow_set_nv12(provide));
            }

            case 3:
            case 4:
                break;

            case 5:
                ubase_assert(uref_pic_set_progressive(provide, false));
                ubase_assert(uref_pic_set_tff(provide, true));
                break;
        }
        struct uref *flow_format = uref_dup(provide);
        assert(flow_format);
        urequest_provide_flow_format(output->urequest, flow_format);
    }

    TEST_START(test_auto);

    {
        struct upipe *input = NULL;
        {
            struct uref *flow_def_wanted = uref_alloc_control(uref_mgr);
            ubase_assert(uref_flow_set_def(flow_def_wanted, UREF_PIC_FLOW_DEF));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));

            uref_free(flow_def);
        }

        upipe_release(input);
    }
    uref_free(provide);
    provide = NULL;

    {
        struct upipe *input = NULL;
        {

            struct uref *flow_def_wanted = uref_alloc_control(uref_mgr);
            ubase_assert(uref_flow_set_def(flow_def_wanted, UREF_PIC_FLOW_DEF));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            ubase_assert(upipe_set_option(input, "sw/deinterlace", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/interlace", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/scale", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/format", "avfilter"));
            uref_free(flow_def_wanted);
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);
    }
    uref_free(provide);
    provide = NULL;

    TEST_END(test_auto);
}

static void test_force(void)
{
    struct uref *provide = NULL;

    void check_flow_def(struct output *output, struct uref *flow_def) {
        uint64_t hsize = 0, vsize = 0;
        ubase_assert(uref_pic_flow_check_yuv420p(flow_def));
        assert(uref_pic_check_progressive(flow_def));
        assert(!uref_pic_check_tff(flow_def));
        ubase_assert(uref_pic_flow_get_hsize(flow_def, &hsize));
        ubase_assert(uref_pic_flow_get_vsize(flow_def, &vsize));
        assert(hsize == WIDTH);
        assert(vsize == HEIGHT);

        ubase_assert(uref_test_get_output(flow_def, NULL));
    }

    void check_uref(struct output *output, struct uref *uref) {
        uint64_t hsize = 0, vsize = 0;
        uint8_t macropixel = 0;
        ubase_assert(uref_pic_size(uref, &hsize, &vsize, &macropixel));
        assert(hsize == WIDTH);
        assert(vsize == HEIGHT);

        bool progressive = false;
        ubase_assert(uref_pic_get_progressive(uref, &progressive));
        assert(progressive);

        if (!provide)
            provide = uref_dup(output->urequest->uref);

        switch (output->counter) {
            case 1: {
                ubase_assert(uref_pic_flow_set_hsize(provide, WIDTH));
                ubase_assert(uref_pic_flow_set_vsize(provide, HEIGHT));
                break;
            }

            case 2: {
                ubase_assert(uref_pic_flow_set_nv12(provide));
            }

            case 3:
            case 4:
                break;

            case 5:
                ubase_assert(uref_pic_set_progressive(provide, true));
                ubase_assert(uref_pic_set_tff(provide, false));
                break;
        }
        struct uref *flow_format = uref_dup(provide);
        assert(flow_format);
        urequest_provide_flow_format(output->urequest, flow_format);
    }

    TEST_START(test_force);

    {
        struct upipe *input = NULL;

        {

            struct uref *flow_def_wanted = uref_pic_flow_alloc_yuv420p(uref_mgr);
            ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT));
            ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            ubase_assert(
                upipe_set_option(input, "forward_flow_format", "false"));
            uref_free(flow_def_wanted);
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH / 2));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT / 2));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(uref_pic_set_progressive(flow_def, false));
            ubase_assert(uref_pic_set_tff(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_nv12(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH * 2));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT * 2));
            ubase_assert(uref_pic_flow_set_fps(flow_def, fps));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);

        uref_free(provide);
        provide = NULL;
    }

    {
        struct upipe *input = NULL;
        {

            /* filter format */
            struct uref *flow_def_wanted = uref_pic_flow_alloc_yuv420p(uref_mgr);
            ubase_assert(uref_pic_flow_set_hsize(flow_def_wanted, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def_wanted, HEIGHT));
            ubase_assert(uref_pic_set_progressive(flow_def_wanted, true));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
            ubase_assert(upipe_set_option(input, "sw/deinterlace", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/interlace", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/scale", "avfilter"));
            ubase_assert(upipe_set_option(input, "sw/format", "avfilter"));
            ubase_assert(
                upipe_set_option(input, "forward_flow_format", "false"));
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH / 2));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT / 2));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        {
            struct uref *flow_def = uref_pic_flow_alloc_nv12(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH * 2));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT * 2));
            ubase_assert(uref_pic_set_progressive(flow_def, true));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);
        uref_free(provide);
        provide = NULL;
    }

    TEST_END(test_force);
}

static void test_fps(void)
{
    struct uref *provide = NULL;
    void check_flow_def(struct output *output, struct uref *flow_def) {
    }

    void check_uref(struct output *output, struct uref *uref) {
    }

    TEST_START(test_fps);

    {
        struct upipe *input = NULL;
        {

            struct urational output_fps = { .num = fps.num * 2, .den = fps.den };
            struct uref *flow_def_wanted = uref_pic_flow_alloc_yuv420p(uref_mgr);
            ubase_assert(uref_pic_flow_set_fps(flow_def_wanted, output_fps));
            input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
            uref_free(flow_def_wanted);
        }

        {
            struct urational input_fps = { .num = fps.num, .den = fps.den };
            struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
            assert(flow_def);
            ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
            ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
            ubase_assert(uref_pic_flow_set_fps(flow_def, input_fps));
            ubase_assert(uref_pic_set_progressive(flow_def, false));
            ubase_assert(upipe_set_flow_def(input, flow_def));
            uref_free(flow_def);
        }

        upipe_release(input);
        uref_free(provide);
        provide = NULL;
    }

    TEST_END(test_fps);

}

static void test_test(void)
{
    void check_flow_def(struct output *output, struct uref *flow_def) {
    }

    void check_uref(struct output *output, struct uref *uref) {
    }

    TEST_START(test_test);

    struct upipe *input = NULL;
    {
        struct uref *flow_def_wanted = uref_pic_flow_alloc_yuv420p(uref_mgr);
        input = test_pipe_alloc(flow_def_wanted, check_flow_def, check_uref);
        uref_free(flow_def_wanted);
    }

    {
        struct uref *flow_def = uref_pic_flow_alloc_yuv420p(uref_mgr);
        assert(flow_def);
        ubase_assert(uref_pic_flow_set_hsize(flow_def, WIDTH));
        ubase_assert(uref_pic_flow_set_vsize(flow_def, HEIGHT));
        upipe_set_flow_def(input, flow_def);
        uref_free(flow_def);
    }

    upipe_release(input);

    TEST_END(test_test);
}

static struct {
    const char *name;
    test_cb func;
} tests[] = {
#define TEST(test)  { .name = #test, .func = test }
    TEST(test_input_pic),
    TEST(test_input_sound),
    TEST(test_scale),
    TEST(test_format),
    TEST(test_format_avfilter),
    TEST(test_deint),
    TEST(test_deint_avfilter),
    TEST(test_interlace),
    TEST(test_deint_interlace),
    TEST(test_auto),
    TEST(test_force),
    TEST(test_fps),
    TEST(test_test),
#undef TEST
};

static void add_format(const char *name)
{
    const struct uref_pic_flow_format *format =
        uref_pic_flow_get_format_by_name(name);
    assert(format);
    assert(nb_formats < MAX_CUSTOM_FORMATS);
    formats[nb_formats++] = format;
}

int main(int argc, char **argv)
{
    int c;

    printf("Compiled %s %s (%s)\n", __DATE__, __TIME__, __FILE__);

    while ((c = getopt(argc, argv, "vqt:T:d:D:f:")) != -1) {
        switch (c) {
            case 'v':
                log_level--;
                break;
            case 'q':
                log_level++;
                break;
            case 't':
                input_hw_type = optarg;
                break;
            case 'T':
                output_hw_type = optarg;
                break;
            case 'd':
                input_hw_device = optarg;
                break;
            case 'D':
                output_hw_device = optarg;
                break;
            case 'f':
                add_format(optarg);
                break;
        }
    }

    if (nb_formats == 0) {
        nb_formats = UBASE_ARRAY_SIZE(uref_pic_flow_formats);
        formats = uref_pic_flow_formats;
    }

    /* upipe env */
    umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH, umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(uref_mgr != NULL);

    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    logger = uprobe_stdio_alloc(&uprobe, stdout, log_level);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH,
                                   UBUF_POOL_DEPTH);
    assert(logger != NULL);

    /* output pipe */
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(tests); i++) {
        bool do_test = false;
        if (optind >= argc)
            do_test = true;
        else {
            for (int j = optind; !do_test && j < argc; j++)
                if (!strcmp(argv[j], tests[i].name))
                    do_test = true;
        }
        if (do_test)
            tests[i].func();
    }

    uref_mgr_release(uref_mgr);
    uprobe_release(logger);
    uprobe_clean(&uprobe);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    return 0;
}
