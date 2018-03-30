#ifndef _UPIPE_FREETYPE_UPIPE_FREETYPE_H_
# define _UPIPE_FREETYPE_UPIPE_FREETYPE_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/ubase.h>
#include <upipe/upipe.h>

#define UPIPE_FREETYPE_SIGNATURE UBASE_FOURCC('f','r','t','2')

/** @This enumarates the freetype pipe commands. */
enum upipe_freetype_command {
    /** sentinel */
    UPIPE_FREETYPE_SENTINEL = UPIPE_CONTROL_LOCAL,
    /** get the string bonding box (const char *, struct upipe_freetype_bbox *)
     */
    UPIPE_FREETYPE_GET_BBOX,
    /** get the rendered string width (const char *, uint64_t *) */
    UPIPE_FREETYPE_GET_WIDTH,
    /** set the freetype pixel size (unsigned) */
    UPIPE_FREETYPE_SET_PIXEL_SIZE,
    /** set the baseline position in the buffer */
    UPIPE_FREETYPE_SET_BASELINE,
};

/** @This describes a string bonding box. */
struct upipe_freetype_bbox {
    struct {
        /** maximum x value */
        int x;
        /** maximum y value */
        int y;
    } max;
    struct {
        /** minimum x value */
        int x;
        /** minimum y value */
        int y;
    } min;
};

/** @This gets the bounding box for a string.
 *
 * @param upipe description structure of the pipe
 * @param str a string
 * @param height_p filled with the height of the renderered string
 * @param width_p filled with the width of the rendered string
 * @return an error code
 */
static int upipe_freetype_get_bbox(struct upipe *upipe,
                                   const char *str,
                                   struct upipe_freetype_bbox *bbox_p)
{
    return upipe_control(upipe, UPIPE_FREETYPE_GET_BBOX,
                         UPIPE_FREETYPE_SIGNATURE, str, bbox_p);
}

/** @This gets the width of a string when rendered.
 *
 * @param upipe description structure of the pipe
 * @param str a string to get the width from
 * @param width_p filled with the rendered width
 * @return an error code
 */
static int upipe_freetype_get_width(struct upipe *upipe,
                                    const char *str,
                                    uint64_t *width_p)
{
    return upipe_control(upipe, UPIPE_FREETYPE_GET_WIDTH,
                         UPIPE_FREETYPE_SIGNATURE, str, width_p);
}

/** @This sets the freetype pixel size.
 *
 * @param upipe description structure of the pipe
 * @param pixel_size pixel size to set
 * @return an error code
 */
static int upipe_freetype_set_pixel_size(struct upipe *upipe,
                                         unsigned pixel_size)
{
    return upipe_control(upipe, UPIPE_FREETYPE_SET_PIXEL_SIZE,
                         UPIPE_FREETYPE_SIGNATURE, pixel_size);
}

/** @This sets the baseline start position.
 *
 * @param upipe description structure of the pipe
 * @param xoff offset from the top of the buffer
 * @param yoff offset from the left of the buffer
 * @return an error code
 */
static int upipe_freetype_set_baseline(struct upipe *upipe,
                                       uint64_t xoff, uint64_t yoff)
{
    return upipe_control(upipe, UPIPE_FREETYPE_SET_BASELINE,
                         UPIPE_FREETYPE_SIGNATURE, xoff, yoff);
}

/** @This returns the freetype pipes manager.
 *
 * @return a pointer to the freetype pipes manager
 */
struct upipe_mgr *upipe_freetype_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif /* !_UPIPE_FREETYPE_UPIPE_FREETYPE_H_ */
