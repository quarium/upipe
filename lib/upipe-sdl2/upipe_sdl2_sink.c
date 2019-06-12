#include <upipe/upipe.h>
#include <upipe/urefcount_helper.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow_formats.h>
#include <upipe/uref_dump.h>
#include <upipe/uclock.h>
#include <upipe-sdl2/upipe_sdl2_sink.h>

#define GL_GLEXT_PROTOTYPES

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <limits.h>

#define APP_SHADER_SOURCE(...) #__VA_ARGS__;

static const bool test = false;

const char * const APP_VERTEX_SHADER = APP_SHADER_SOURCE(
    attribute vec2 vertex;
    varying vec2 tex_coord;

    void main() {
        tex_coord = vertex;
        gl_Position = vec4((vertex * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
    }
);

const char * const APP_FRAGMENT_SHADER_YCRCB = APP_SHADER_SOURCE(
    uniform sampler2D texture_y;
    uniform sampler2D texture_cb;
    uniform sampler2D texture_cr;
    varying vec2 tex_coord;

    mat4 rec601 = mat4(
        1.16438,  0.00000,  1.59603, -0.87079,
        1.16438, -0.39176, -0.81297,  0.52959,
        1.16438,  2.01723,  0.00000, -1.08139,
        0, 0, 0, 1
    );

    void main() {
        float y = texture2D(texture_y, tex_coord).r;
        float cb = texture2D(texture_cb, tex_coord).r;
        float cr = texture2D(texture_cr, tex_coord).r;

        gl_FragColor = vec4(y, cb, cr, 1.0) * rec601;
    }
);

const char * const APP_FRAGMENT_SHADER_RGB = APP_SHADER_SOURCE(
    uniform sampler2D texture_rgb;
    varying vec2 tex_coord;

    void main() {
        gl_FragColor = vec4(texture2D(texture_rgb, tex_coord).rgb, 1.0);
    }
);

#undef APP_SHADER_SOURCE

struct upipe_sdl2_sink_mgr {
    struct upipe_mgr mgr;
    struct urefcount urefcount;
};

UBASE_FROM_TO(upipe_sdl2_sink_mgr, upipe_mgr, mgr, mgr);
UREFCOUNT_HELPER(upipe_sdl2_sink_mgr, urefcount, upipe_sdl2_sink_mgr_free);

struct upipe_sdl2_sink {
    struct upipe upipe;
    struct urefcount urefcount;
    struct upump_mgr *upump_mgr;
    struct upump *idler;

    SDL_Window *window;
    SDL_GLContext gl;
    GLuint shader_program;
    GLuint vertex_shader;
    GLuint fragment_shader;

    enum texture_mode {
        TEXTURE_MODE_NONE,
        TEXTURE_MODE_YUV,
        TEXTURE_MODE_RGB565,
        TEXTURE_MODE_RGB24,
    } texture_mode;
    GLuint y, u, v;
    GLuint rgb;

    int width;
    int height;

    struct uref *last;
    struct uref *current;
};

UPIPE_HELPER_UPIPE(upipe_sdl2_sink, upipe, UPIPE_SDL2_SINK_SIGNATURE);
UPIPE_HELPER_VOID(upipe_sdl2_sink);
UPIPE_HELPER_UREFCOUNT(upipe_sdl2_sink, urefcount, upipe_sdl2_sink_free);
UPIPE_HELPER_UPUMP_MGR(upipe_sdl2_sink, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_sdl2_sink, idler, upump_mgr);

static GLuint upipe_sdl2_sink_create_texture(struct upipe *upipe,
                                             GLuint index,
                                             const char *name)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);
    GLuint texture;
    glGenTextures(1, &texture);

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!test)
        glUniform1i(glGetUniformLocation(sdl2_sink->shader_program, name), index);
    return texture;
}

static GLuint upipe_sdl2_sink_compile_shader(struct upipe *upipe,
                                             GLenum type,
                                             const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        int log_written;
        char log[256];
        glGetShaderInfoLog(shader, 256, &log_written, log);
        upipe_err_va(upipe, "Error compiling shader: %s", log);
    }
    return shader;
}

static struct upipe *upipe_sdl2_sink_alloc(struct upipe_mgr *mgr,
                                           struct uprobe *uprobe,
                                           uint32_t sig,
                                           va_list args)
{
    struct upipe *upipe = upipe_sdl2_sink_alloc_void(mgr, uprobe, sig, args);
    if (unlikely(!upipe))
        return NULL;
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    upipe_sdl2_sink_init_urefcount(upipe);
    upipe_sdl2_sink_init_upump_mgr(upipe);
    upipe_sdl2_sink_init_idler(upipe);
    sdl2_sink->texture_mode = TEXTURE_MODE_NONE;
    sdl2_sink->last = NULL;
    sdl2_sink->current = NULL;

    upipe_throw_ready(upipe);

    sdl2_sink->window = SDL_CreateWindow(
        "Window",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
        );

    if (unlikely(!sdl2_sink->window)) {
        upipe_release(upipe);
        return NULL;
    }
    sdl2_sink->gl = SDL_GL_CreateContext(sdl2_sink->window);

    SDL_GL_SetSwapInterval(1);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    const char * fsh = sdl2_sink->texture_mode == TEXTURE_MODE_YUV
        ? APP_FRAGMENT_SHADER_YCRCB
        : APP_FRAGMENT_SHADER_RGB;

    sdl2_sink->fragment_shader =
        upipe_sdl2_sink_compile_shader(upipe, GL_FRAGMENT_SHADER, fsh);
    sdl2_sink->vertex_shader =
        upipe_sdl2_sink_compile_shader(upipe, GL_VERTEX_SHADER,
                                       APP_VERTEX_SHADER);

    sdl2_sink->shader_program = glCreateProgram();
    glAttachShader(sdl2_sink->shader_program, sdl2_sink->vertex_shader);
    glAttachShader(sdl2_sink->shader_program, sdl2_sink->fragment_shader);
    glLinkProgram(sdl2_sink->shader_program);
    glUseProgram(sdl2_sink->shader_program);

    if (sdl2_sink->texture_mode == TEXTURE_MODE_YUV) {
        sdl2_sink->y = upipe_sdl2_sink_create_texture(upipe, 0, "texture_y");
        sdl2_sink->u = upipe_sdl2_sink_create_texture(upipe, 1, "texture_u");
        sdl2_sink->v = upipe_sdl2_sink_create_texture(upipe, 2, "texture_v");
    }
    else {
        sdl2_sink->rgb = upipe_sdl2_sink_create_texture(upipe, 0, "texture_rgb");
    }

    glBindFramebuffer(0, GL_FRAMEBUFFER);
    return upipe;
}

static void upipe_sdl2_sink_free(struct upipe *upipe)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    upipe_throw_dead(upipe);

    if (sdl2_sink->last)
        uref_free(sdl2_sink->last);
    if (sdl2_sink->current)
        uref_free(sdl2_sink->current);
    upipe_sdl2_sink_clean_idler(upipe);
    upipe_sdl2_sink_clean_upump_mgr(upipe);
    upipe_sdl2_sink_clean_urefcount(upipe);

    upipe_sdl2_sink_free_void(upipe);
}

static void upipe_sdl2_sink_update_texture(struct upipe *upipe,
                                           GLuint unit,
                                           GLuint texture,
                                           const uint8_t *plane)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_LUMINANCE,
        sdl2_sink->width, sdl2_sink->height, 0,
        GL_LUMINANCE, GL_UNSIGNED_BYTE, plane);
}

/** @This loads a uref picture into the specified texture
 * @param uref uref structure describing the picture
 * @param texture GL texture
 * @return false in case of error
 */
static bool upipe_gl_texture_load_uref(struct uref *uref, GLuint texture)
{
    const uint8_t *data = NULL;
    bool rgb565 = false;
    size_t width, height, stride;
    uint8_t msize;
    uref_pic_size(uref, &width, &height, NULL);
    if (!ubase_check(uref_pic_plane_read(uref, "r8g8b8", 0, 0, -1, -1,
                                         &data)) ||
        !ubase_check(uref_pic_plane_size(uref, "r8g8b8", &stride,
                                         NULL, NULL, &msize))) {
        if (!ubase_check(uref_pic_plane_read(uref, "r5g6b5", 0, 0, -1, -1,
                                             &data)) ||
            !ubase_check(uref_pic_plane_size(uref, "r5g6b5", &stride,
                                             NULL, NULL, &msize))) {
            return false;
        }
        rgb565 = true;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / msize);
    glPixelStorei(GL_UNPACK_ALIGNMENT, rgb565 ? 2 : 4);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
            rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, data);
    uref_pic_plane_unmap(uref, rgb565 ? "r5g6b5" : "r8g8b8", 0, 0, -1, -1);

    return true;
}
static void upipe_sdl2_sink_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    switch (sdl2_sink->texture_mode) {
        case TEXTURE_MODE_NONE:
            upipe_warn(upipe, "no texture mode set, dropping...");
            uref_free(uref);
            return;

        case TEXTURE_MODE_YUV: {
            const uint8_t *y;
            uref_pic_plane_read(uref, "y8", 0, 0, -1, -1, &y);
            upipe_sdl2_sink_update_texture(upipe, GL_TEXTURE0,
                                           sdl2_sink->y, y);
            const uint8_t *u;
            uref_pic_plane_read(uref, "u8", 0, 0, -1, -1, &u);
            upipe_sdl2_sink_update_texture(upipe, GL_TEXTURE1,
                                           sdl2_sink->u, u);
            const uint8_t *v;
            uref_pic_plane_read(uref, "v8", 0, 0, -1, -1, &v);
            upipe_sdl2_sink_update_texture(upipe, GL_TEXTURE2,
                                           sdl2_sink->v, v);
            break;
        }

        case TEXTURE_MODE_RGB565: {
            const uint8_t *rgb;
            size_t width, height, stride;
            uint8_t msize;
            uref_pic_size(uref, &width, &height, NULL);
            ubase_assert(uref_pic_plane_read(uref, "r5g6b5",
                                             0, 0, -1, -1, &rgb));
            ubase_assert(uref_pic_plane_size(uref, "r5g6b5", &stride,
                                             NULL, NULL, &msize));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / msize);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            glBindTexture(GL_TEXTURE_2D, sdl2_sink->rgb);
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb);
            uref_pic_plane_unmap(uref, "r5g6b5", 0, 0, -1, -1);
            break;
        }

        case TEXTURE_MODE_RGB24: {
            const uint8_t *rgb;
            size_t width, height, stride;
            uint8_t msize;
            uref_pic_size(uref, &width, &height, NULL);
            ubase_assert(uref_pic_plane_read(uref, "r8g8b8",
                                             0, 0, -1, -1, &rgb));
            ubase_assert(uref_pic_plane_size(uref, "r8g8b8", &stride,
                                             NULL, NULL, &msize));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / msize);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glBindTexture(GL_TEXTURE_2D, sdl2_sink->rgb);
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                GL_RGB, GL_UNSIGNED_BYTE, rgb);
            uref_pic_plane_unmap(uref, "r8g8b8", 0, 0, -1, -1);
            break;
        }
    }

    float scale = 1;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    glBindTexture(GL_TEXTURE_2D, sdl2_sink->rgb);
    glLoadIdentity();
    glTranslatef(0, 0, -10);

    // Main movie "display"
    glPushMatrix();
    glScalef(scale, 1, 1);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0, 1); glVertex3f(-2, -2,  -4);
        glTexCoord2f(0, 0); glVertex3f(-2,  2,  -4);
        glTexCoord2f(1, 0); glVertex3f( 2,  2,  -4);
        glTexCoord2f(1, 1); glVertex3f( 2, -2,  -4);
    }
    glEnd();
    glPopMatrix();
    glDisable(GL_TEXTURE_2D);
    SDL_GL_SwapWindow(sdl2_sink->window);
}

static void upipe_sdl2_sink_idle(struct upump *idler)
{
    struct upipe *upipe = upump_get_opaque(idler, struct upipe *);
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    fprintf(stderr, "IDLE...\n");

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT || (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE)
		) {
			//self->wants_to_quit = true;
                        exit(1);
		}

        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            //glViewport(0, 0, ev.window.data1, ev.window.data2);
        }
    }

}

static int upipe_sdl2_sink_set_flow_def(struct upipe *upipe,
                                        struct uref *flow_def)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

#define CHECK_FORMAT(FLOW_DEF, FORMAT) \
    ubase_check(uref_pic_flow_check_format(FLOW_DEF, &FORMAT))

    if (CHECK_FORMAT(flow_def, uref_pic_flow_format_rgb565)) {
        sdl2_sink->texture_mode = TEXTURE_MODE_RGB565;
        
    }
    else if (CHECK_FORMAT(flow_def, uref_pic_flow_format_rgb24)) {
        sdl2_sink->texture_mode = TEXTURE_MODE_RGB24;
    }
    else if (CHECK_FORMAT(flow_def, uref_pic_flow_format_yuv420p)) {
        sdl2_sink->texture_mode = TEXTURE_MODE_YUV;
    }
    else {
        upipe_warn(upipe, "unsupported flow def");
        return UBASE_ERR_INVALID;
    }

#undef CHECK_FORMAT

    uint64_t vsize, hsize;
    UBASE_RETURN(uref_pic_flow_get_hsize(flow_def, &hsize));
    UBASE_RETURN(uref_pic_flow_get_vsize(flow_def, &vsize));
    if (hsize > INT_MAX || vsize > INT_MAX)
        return UBASE_ERR_INVALID;
    sdl2_sink->width = hsize;
    sdl2_sink->height = vsize;

    glViewport(0, 0, hsize, vsize);
    return UBASE_ERR_NONE;
}

static int upipe_sdl2_sink_control_real(struct upipe *upipe,
                                        int cmd, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_control_provide_request(upipe, cmd, args));
    switch (cmd) {
        case UPIPE_ATTACH_UPUMP_MGR:
            upipe_sdl2_sink_set_idler(upipe, NULL);
            return upipe_sdl2_sink_attach_upump_mgr(upipe);

        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_sdl2_sink_set_flow_def(upipe, flow_def);
        }
    }
    return UBASE_ERR_UNHANDLED;
}

static int upipe_sdl2_sink_check(struct upipe *upipe)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    UBASE_RETURN(upipe_sdl2_sink_check_upump_mgr(upipe));
    if (unlikely(!sdl2_sink->upump_mgr))
        return UBASE_ERR_NONE;

    if (unlikely(!sdl2_sink->idler)) {
//        struct upump *idler = upump_alloc_idler(sdl2_sink->upump_mgr,
//                                                upipe_sdl2_sink_idle, upipe,
//                                                upipe->refcount);
        struct upump *idler = upump_alloc_timer(sdl2_sink->upump_mgr,
                                                upipe_sdl2_sink_idle,
                                                upipe, upipe->refcount,
                                                UCLOCK_FREQ, UCLOCK_FREQ / 10);
        if (unlikely(!idler))
            return UBASE_ERR_UPUMP;

        upump_start(idler);
        sdl2_sink->idler = idler;
        fprintf(stderr, "set idler\n");
    }

    return UBASE_ERR_NONE;
}

static int upipe_sdl2_sink_control(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_RETURN(upipe_sdl2_sink_control_real(upipe, cmd, args));
    return upipe_sdl2_sink_check(upipe);
}

struct upipe_mgr *upipe_sdl2_sink_mgr_alloc(void)
{
    struct upipe_sdl2_sink_mgr *sdl2_sink_mgr = malloc(sizeof (*sdl2_sink_mgr));
    if (unlikely(!sdl2_sink_mgr))
        return NULL;

    upipe_sdl2_sink_mgr_init_urefcount(sdl2_sink_mgr);

    struct upipe_mgr *mgr = upipe_sdl2_sink_mgr_to_mgr(sdl2_sink_mgr);
    memset(mgr, 0, sizeof (*mgr));
    mgr->refcount = upipe_sdl2_sink_mgr_to_urefcount(sdl2_sink_mgr);
    mgr->signature = UPIPE_SDL2_SINK_SIGNATURE;
    mgr->upipe_alloc = upipe_sdl2_sink_alloc;
    mgr->upipe_input = upipe_sdl2_sink_input;
    mgr->upipe_control = upipe_sdl2_sink_control;

    SDL_Init(SDL_INIT_EVERYTHING);

    return mgr;
}

static void upipe_sdl2_sink_mgr_free(struct upipe_sdl2_sink_mgr *sdl2_sink_mgr)
{
    SDL_Quit();
    upipe_sdl2_sink_mgr_clean_urefcount(sdl2_sink_mgr);
    free(sdl2_sink_mgr);
}
