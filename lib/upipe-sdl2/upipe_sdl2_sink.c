#include <upipe/upipe.h>
#include <upipe/urefcount_helper.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/uref_pic.h>
#include <upipe-sdl2/upipe_sdl2_sink.h>

#define GL_GLEXT_PROTOTYPES

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>

#define APP_SHADER_SOURCE(...) #__VA_ARGS__;

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
    SDL_Window *window;
    SDL_GLContext gl;
    GLuint shader_program;
    GLuint vertex_shader;
    GLuint fragment_shader;

    enum texture_mode {
        TEXTURE_MODE_YUV,
        TEXTURE_MODE_RGB,
    } texture_mode;
    GLuint y, u, v;
    GLuint rgb;

    int width;
    int height;
};

UPIPE_HELPER_UPIPE(upipe_sdl2_sink, upipe, UPIPE_SDL2_SINK_SIGNATURE);
UPIPE_HELPER_VOID(upipe_sdl2_sink);
UPIPE_HELPER_UREFCOUNT(upipe_sdl2_sink, urefcount, upipe_sdl2_sink_free);

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

    upipe_sdl2_sink_init_urefcount(upipe);

    upipe_throw_ready(upipe);

    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    sdl2_sink->window = SDL_CreateWindow(
        "Window",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
        );
    sdl2_sink->gl = SDL_GL_CreateContext(sdl2_sink->window);
    SDL_GL_SetSwapInterval(1);

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

    return upipe;
}

static void upipe_sdl2_sink_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

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
        GL_TEXTURE_2D, 0, GL_LUMINANCE, sdl2_sink->width, sdl2_sink->height, 0,
        GL_LUMINANCE, GL_UNSIGNED_BYTE, plane
    );
}

static void upipe_sdl2_sink_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    switch (sdl2_sink->texture_mode) {
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
        case TEXTURE_MODE_RGB: {
            const uint8_t *rgb;
            uref_pic_plane_read(uref, "rgb8", 0, 0, -1, -1, &rgb);
            glBindTexture(GL_TEXTURE_2D, sdl2_sink->rgb);
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGB, sdl2_sink->width, sdl2_sink->height, 0,
                GL_RGB, GL_UNSIGNED_BYTE, rgb
                );
            break;
        }
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glRectf(0.0, 0.0, 1.0, 1.0);
    SDL_GL_SwapWindow(sdl2_sink->window);
    uref_free(uref);
}

static int upipe_sdl2_sink_set_flow_def(struct upipe *upipe,
                                        struct uref *flow_def)
{
    return UBASE_ERR_NONE;
}

static int upipe_sdl2_sink_control(struct upipe *upipe, int cmd, va_list args)
{
    switch (cmd) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_sdl2_sink_set_flow_def(upipe, flow_def);
        }
    }
    return UBASE_ERR_UNHANDLED;
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

    SDL_Init(SDL_INIT_VIDEO);

    return mgr;
}

static void upipe_sdl2_sink_mgr_free(struct upipe_sdl2_sink_mgr *sdl2_sink_mgr)
{
    SDL_Quit();
    upipe_sdl2_sink_mgr_clean_urefcount(sdl2_sink_mgr);
    free(sdl2_sink_mgr);
}
