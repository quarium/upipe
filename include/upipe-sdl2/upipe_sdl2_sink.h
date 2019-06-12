#ifndef _UPIPE_SDL2_UPIPE_SDL2_SINK_H_
#define _UPIPE_SDL2_UPIPE_SDL2_SINK_H_

#include <upipe/ubase.h>

#define UPIPE_SDL2_SINK_SIGNATURE   UBASE_FOURCC('S','D','L','o')

struct upipe_mgr *upipe_sdl2_sink_mgr_alloc(void);

#endif
