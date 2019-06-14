#include <upipe/upipe.h>
#include <upipe/urefcount_helper.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow_formats.h>
#include <upipe/uref_dump.h>
#include <upipe/uclock.h>
#include <upipe-sdl2/upipe_sdl2_sink.h>
#include <upipe/config.h>

#define GL_GLEXT_PROTOTYPES

#include <SDL2/SDL.h>
#ifdef UPIPE_HAVE_GL
# include <SDL2/SDL_opengl.h>
#elif defined(UPIPE_HAVE_GLESV2)
# include <SDL2/SDL_opengles2.h>
#else
# error "No OpenGL support"
#endif
#include <limits.h>

#define APP_SHADER_SOURCE(...) #__VA_ARGS__;

#if 1
const char * const APP_VERTEX_SHADER = APP_SHADER_SOURCE(
    attribute vec2 vertex;
    varying vec2 tex_coord;

    void main() {
        tex_coord = vertex;
        gl_Position = vec4((vertex * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
    }
);
#else
const char * const APP_VERTEX_SHADER = APP_SHADER_SOURCE(
    layout (location = 0) in vec3 aPos;

    void main()
    {
        gl_Position = vec4(aPos.x, aPos.y, aPos.z, 1.0);
    }
);
#endif

const char * const APP_FRAGMENT_SHADER_YUV = APP_SHADER_SOURCE(
    uniform sampler2D texture_y;
    uniform sampler2D texture_u;
    uniform sampler2D texture_v;
    varying vec2 tex_coord;
    varying vec2 scale;

    mat4 rec601 = mat4(
        1.16438,  0.00000,  1.59603, -0.87079,
        1.16438, -0.39176, -0.81297,  0.52959,
        1.16438,  2.01723,  0.00000, -1.08139,
        0, 0, 0, 1
    );

    void main() {
        float y = texture2D(texture_y, tex_coord).r;
        float u = texture2D(texture_u, tex_coord).r;
        float v = texture2D(texture_v, tex_coord).r;

        //gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        gl_FragColor = vec4(y, u, v, 1.0) * rec601;
    }
);

const char * const APP_FRAGMENT_SHADER_RGB = APP_SHADER_SOURCE(
    uniform sampler2D texture_rgb;
    varying vec2 tex_coord;

    void main() {
        gl_FragColor = vec4(texture2D(texture_rgb, tex_coord).rgb, 1.0);
    }
);


const char *const APP_FRAGMENT_SHADER_ORANGE = APP_SHADER_SOURCE(
    void main() {
        gl_FragColor = vec4(1.0, 0.5, 0.2, 1.0);
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
    struct upump *timer;
    struct uclock *uclock;
    struct urequest uclock_request;
    struct uchain list;

    SDL_Window *window;
    SDL_GLContext gl;
    GLuint shader_program;

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
    uint64_t latency;
};

UPIPE_HELPER_UPIPE(upipe_sdl2_sink, upipe, UPIPE_SDL2_SINK_SIGNATURE);
UPIPE_HELPER_VOID(upipe_sdl2_sink);
UPIPE_HELPER_UREFCOUNT(upipe_sdl2_sink, urefcount, upipe_sdl2_sink_free);
UPIPE_HELPER_UPUMP_MGR(upipe_sdl2_sink, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_sdl2_sink, idler, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_sdl2_sink, timer, upump_mgr);
UPIPE_HELPER_UCLOCK(upipe_sdl2_sink, uclock, uclock_request, NULL,
                    upipe_throw_provide_request, NULL);

static void upipe_sdl2_sink_timeout(struct upump *timer);

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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

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
    upipe_sdl2_sink_init_timer(upipe);
    upipe_sdl2_sink_init_uclock(upipe);
    sdl2_sink->texture_mode = TEXTURE_MODE_NONE;
    ulist_init(&sdl2_sink->list);

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

    upipe_notice_va(upipe, "OpenGL version %s", glGetString(GL_VERSION));
    upipe_notice_va(upipe, "OpenGL GLSL version %s",
                    glGetString(GL_SHADING_LANGUAGE_VERSION));

    SDL_GL_SetSwapInterval(1);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    //SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    sdl2_sink->texture_mode = TEXTURE_MODE_YUV;
    const char *fsh = NULL;
    switch (sdl2_sink->texture_mode) {
        case TEXTURE_MODE_NONE:
            fsh = APP_FRAGMENT_SHADER_ORANGE;
            break;
        case TEXTURE_MODE_RGB565:
        case TEXTURE_MODE_RGB24:
            fsh = APP_FRAGMENT_SHADER_RGB;
            break;
        case TEXTURE_MODE_YUV:
            fsh = APP_FRAGMENT_SHADER_YUV;
            break;
    }
    if (unlikely(!fsh)) {
        upipe_release(upipe);
        return NULL;
    }

    GLuint vertex_shader =
        upipe_sdl2_sink_compile_shader(upipe, GL_FRAGMENT_SHADER, fsh);
    GLuint fragment_shader =
        upipe_sdl2_sink_compile_shader(upipe, GL_VERTEX_SHADER,
                                       APP_VERTEX_SHADER);

    sdl2_sink->shader_program = glCreateProgram();
    glAttachShader(sdl2_sink->shader_program, vertex_shader);
    glAttachShader(sdl2_sink->shader_program, fragment_shader);
    glLinkProgram(sdl2_sink->shader_program);
    glUseProgram(sdl2_sink->shader_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

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
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    struct uchain *uchain;
    while ((uchain = ulist_pop(&sdl2_sink->list))) {
        upipe_dbg(upipe, "deleting buffered item");
        uref_free(uref_from_uchain(uchain));
    }
    upipe_throw_dead(upipe);

    upipe_sdl2_sink_clean_uclock(upipe);
    upipe_sdl2_sink_clean_timer(upipe);
    upipe_sdl2_sink_clean_idler(upipe);
    upipe_sdl2_sink_clean_upump_mgr(upipe);
    upipe_sdl2_sink_clean_urefcount(upipe);

    upipe_sdl2_sink_free_void(upipe);
}

static void upipe_sdl2_sink_output(struct upipe *upipe, struct uref *uref)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);
    size_t width;
    size_t height;

    if (unlikely(!ubase_check(uref_pic_size(uref, &width, &height, NULL)))) {
        upipe_warn(upipe, "fail to get picture size");
        uref_free(uref);
        return;
    }


    // clear and draw quad with texture (could be in display callback)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

#if 1
    switch (sdl2_sink->texture_mode) {
        case TEXTURE_MODE_NONE:
            upipe_warn(upipe, "no texture mode set, dropping...");
            uref_free(uref);
            return;

        case TEXTURE_MODE_YUV: {
            struct {
                const char *name;
                size_t width;
                size_t height;
                GLuint texture;
                GLenum index;
                const uint8_t *data;
            } planes[] = {
                {
                    .name = "y8",
                    .width = width,
                    .height = height,
                    .texture = sdl2_sink->y,
                    .index = GL_TEXTURE0,
                    .data = NULL
                },
                {
                    .name = "u8",
                    .width = width / 2,
                    .height = height / 2,
                    .texture = sdl2_sink->u,
                    .index = GL_TEXTURE1,
                    .data = NULL
                },
                {
                    .name = "v8",
                    .width = width / 2,
                    .height = height / 2,
                    .texture = sdl2_sink->v,
                    .index = GL_TEXTURE2,
                    .data = NULL
                },
            };

            for (unsigned i = 0; i < UBASE_ARRAY_SIZE(planes); i++) {
                int err;
                size_t stride;
                uint8_t msize;

                err = uref_pic_plane_size(uref, planes[i].name, &stride,
                                             NULL, NULL, &msize);
                if (unlikely(!ubase_check(err))) {
                    upipe_warn_va(upipe, "fail to read plane size");
                    uref_free(uref);
                    return;
                }
                err = uref_pic_plane_read(uref, planes[i].name, 0, 0,
                                          -1, -1, &planes[i].data);
                if (unlikely(!ubase_check(err))) {
                    upipe_warn_va(upipe, "fail to read plane %s",
                                  planes[i].name);
                    uref_free(uref);
                    return;
                }
                glActiveTexture(planes[i].index);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / msize);
                glBindTexture(GL_TEXTURE_2D, planes[i].texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                             planes[i].width, planes[i].height, 0,
                             GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i].data);
                uref_pic_plane_unmap(uref, planes[i].name, 0, 0, -1, -1);
            }

            break;
        }

        case TEXTURE_MODE_RGB565: {
            const uint8_t *rgb;
            size_t stride;
            uint8_t msize;
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
            size_t stride;
            uint8_t msize;
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
#else
    uref_free(uref);
#endif

#if 0
    float scale = 1;

    size_t w = 0, h = 0;
    uref_pic_size(uref, &w, &h, NULL);
    scale = (float)w / (float)h;

    // Main movie "display"
    glPushMatrix();
    //glScalef(scale, 1, 1);
    glBegin(GL_QUADS);
    {
        glVertex3f(-1, -1, 0);
        glVertex3f(-1,  1, 0);
        glVertex3f( 1,  1, 0);
        glVertex3f( 1, -1, 0);
    }
    glEnd();
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0, 1);
        glTexCoord2f(0, 0);
        glTexCoord2f(1, 0);
        glTexCoord2f(1, 1);
    }
    glEnd();
    glPopMatrix();
#else
    float vertices[] = {
        0.0, 0.0, 0.0,
        1.0, 0.0, 0.0,
        1.0, 1.0, 0.0,
        0.0, 1.0, 0.0,
    };

    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);  

    GLuint VAO;
    //glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VAO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    //glDrawArrays(GL_TRIANGLES, 2, 3);
#endif
   
    SDL_GL_SwapWindow(sdl2_sink->window);
}

static void upipe_sdl2_sink_check_input(struct upipe *upipe)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);

    if (sdl2_sink->timer)
        /* already pending */
        return;

    struct uchain *uchain = ulist_pop(&sdl2_sink->list);
    if (!uchain)
        /* no buffer */
        return;

    bool is_last = ulist_empty(&sdl2_sink->list);

    struct uref *uref = uref_from_uchain(uchain);
    uint64_t pts;
    if (unlikely(!ubase_check(uref_clock_get_pts_sys(uref, &pts)))) {
        upipe_warn(upipe, "non-dated packet");
        uref_free(uref);
        goto retry;
    }
    pts += sdl2_sink->latency;

    if (!sdl2_sink->uclock) {
        upipe_warn(upipe, "no clock set");
        uref_free(uref);
        goto retry;
    }

    uint64_t now = uclock_now(sdl2_sink->uclock);
    if (pts + UCLOCK_FREQ / 25 < now) {
        upipe_warn(upipe, "late packet");
        uref_free(uref);
        goto retry;
    }

    if (pts > now) {
        /* wait for the next buffer */
        ulist_unshift(&sdl2_sink->list, uchain);
        upipe_sdl2_sink_wait_timer(upipe, pts - now, upipe_sdl2_sink_timeout);
        return;
    }

    /* renderer and retry */
    upipe_sdl2_sink_output(upipe, uref);

retry:
    if (is_last)
        upipe_release(upipe);
    else
        upipe_sdl2_sink_check_input(upipe);
}

static void upipe_sdl2_sink_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);
    if (ulist_empty(&sdl2_sink->list))
        /* make sure we output all packets */
        upipe_use(upipe);
    ulist_add(&sdl2_sink->list, uref_to_uchain(uref));
    upipe_sdl2_sink_check_input(upipe);
}

static void upipe_sdl2_sink_reshape(struct upipe *upipe)
{
    //struct upipe_sdl2_sink *sdl2_sink = upipe_sdl2_sink_from_upipe(upipe);
    //glViewport(0, 0, sdl2_sink->width, sdl2_sink->height);
    //glMatrixMode(GL_PROJECTION);
    //glLoadIdentity();
    //glOrtho(0, sdl2_sink->width, 0, sdl2_sink->height, -1, 1);
    //glMatrixMode(GL_MODELVIEW);
}

static void upipe_sdl2_sink_timeout(struct upump *timer)
{
    struct upipe *upipe = upump_get_opaque(timer, struct upipe *);
    upipe_sdl2_sink_set_timer(upipe, NULL);
    return upipe_sdl2_sink_check_input(upipe);
}

static void upipe_sdl2_sink_update_window(struct upipe *upipe,
                                          int width, int height)
{
    upipe_notice_va( upipe, "Window resized to %dx%d", width, height);
}

static void upipe_sdl2_sink_idle(struct upump *idler)
{
    struct upipe *upipe = upump_get_opaque(idler, struct upipe *);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                //FIXME: throw probe
                exit(1);
                break;

            case SDL_KEYUP:
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        exit(1);
                        break;
                }
                break;

            case SDL_WINDOWEVENT:
                switch (ev.window.event) {
                    case SDL_WINDOWEVENT_RESIZED:
                        upipe_sdl2_sink_update_window(upipe,
                                                      ev.window.data1,
                                                      ev.window.data2);
                        break;
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        upipe_notice_va(
                            upipe, "Window %d size changed to %dx%d",
                            ev.window.windowID, ev.window.data1,
                            ev.window.data2);
                        break;
                }
                break;
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

    upipe_sdl2_sink_reshape(upipe);
    SDL_SetWindowSize(sdl2_sink->window, hsize, vsize);
    sdl2_sink->latency = 0;
    uref_clock_get_latency(flow_def, &sdl2_sink->latency);

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

        case UPIPE_ATTACH_UCLOCK:
            upipe_sdl2_sink_set_idler(upipe, NULL);
            upipe_sdl2_sink_require_uclock(upipe);
            return UBASE_ERR_NONE;

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
        struct upump *idler = upump_alloc_timer(sdl2_sink->upump_mgr,
                                                upipe_sdl2_sink_idle,
                                                upipe, upipe->refcount,
                                                UCLOCK_FREQ, UCLOCK_FREQ / 100);
        if (unlikely(!idler))
            return UBASE_ERR_UPUMP;

        upump_start(idler);
        sdl2_sink->idler = idler;
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

    SDL_Init(SDL_INIT_VIDEO);
//    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
//    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
//    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
//                        SDL_GL_CONTEXT_PROFILE_CORE);
    return mgr;
}

static void upipe_sdl2_sink_mgr_free(struct upipe_sdl2_sink_mgr *sdl2_sink_mgr)
{
    SDL_Quit();
    upipe_sdl2_sink_mgr_clean_urefcount(sdl2_sink_mgr);
    free(sdl2_sink_mgr);
}
