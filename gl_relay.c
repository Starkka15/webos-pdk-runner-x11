/*
 * gl_relay.c — Native x86_64 GL relay for webOS PDK GLES games (X11 runner)
 *
 * Creates an SDL2+OpenGL window, creates GL/input SHM regions,
 * replays GLES 1.1 commands serialised by libGLES_CM.so running under qemu-arm,
 * and forwards input events back to the game via the input SHM.
 *
 * Audio is handled externally (SDL disk driver → FIFO → aplay in run.sh).
 *
 * Window sizing:
 *   WEBOS_SCREEN_W / WEBOS_SCREEN_H  — target screen dimensions; relay scales
 *                                       the window to fit while keeping aspect ratio
 *   WEBOS_WIN_W / WEBOS_WIN_H        — explicit window pixel size (overrides above)
 *   Default: 480×320 (typical webOS PDK portrait game)
 *
 * Window position:
 *   WEBOS_WIN_X / WEBOS_WIN_Y        — window position on virtual desktop
 *
 * Rotation:
 *   WEBOS_ROTATE=CW|CCW|180         — rotate rendered output
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "pdk_gl_cmd.h"
#include "pdk_input.h"
#include "pvrtc_decode.h"

/* Globals */
static SDL_Window    *window     = NULL;
static SDL_GLContext  gl_context = NULL;
static void          *gl_cmd_shm = NULL;
static void          *input_shm  = NULL;
static int            running    = 1;
static int            rotate_mode = 0;   /* 0=none 1=CW 2=CCW 3=180 */
static int            win_width  = 480;
static int            win_height = 320;
static int            game_w     = 0;    /* game's native render width */
static int            game_h     = 0;    /* game's native render height */
static int            user_sized = 0;    /* 1 = user specified size via env, skip auto-resize */
static int            auto_sized = 0;    /* 1 = auto-resized to game viewport already */

/* Rotation FBO */
static GLuint rot_fbo       = 0;
static GLuint rot_tex       = 0;
static GLuint rot_depth_rbo = 0;
static int    rot_game_w    = 0;
static int    rot_game_h    = 0;

/* GL object ID mapping: ARM-side ID → server-side GL ID */
#define MAX_GL_IDS 4096
static GLuint tex_map[MAX_GL_IDS];
static GLuint fbo_map[MAX_GL_IDS];
static GLuint rbo_map[MAX_GL_IDS];
static GLuint vbo_map[MAX_GL_IDS];

static GLuint map_id(GLuint *map, GLuint arm_id) {
    if (arm_id == 0) return 0;
    if (arm_id < MAX_GL_IDS) return map[arm_id] ? map[arm_id] : arm_id;
    return arm_id;
}
#define map_texture(id)      map_id(tex_map, (id))
#define map_framebuffer(id)  map_id(fbo_map, (id))
#define map_renderbuffer(id) map_id(rbo_map, (id))
#define map_buffer(id)       map_id(vbo_map, (id))

/* GL extension function pointers */
static PFNGLGENFRAMEBUFFERSPROC         pglGenFramebuffers         = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC      pglDeleteFramebuffers      = NULL;
static PFNGLBINDFRAMEBUFFERPROC         pglBindFramebuffer         = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC    pglFramebufferTexture2D    = NULL;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC pglFramebufferRenderbuffer = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC  pglCheckFramebufferStatus  = NULL;
static PFNGLGENRENDERBUFFERSPROC        pglGenRenderbuffers        = NULL;
static PFNGLDELETERENDERBUFFERSPROC     pglDeleteRenderbuffers     = NULL;
static PFNGLBINDRENDERBUFFERPROC        pglBindRenderbuffer        = NULL;
static PFNGLRENDERBUFFERSTORAGEPROC     pglRenderbufferStorage     = NULL;
static PFNGLGENBUFFERSPROC              pglGenBuffers              = NULL;
static PFNGLDELETEBUFFERSPROC           pglDeleteBuffers           = NULL;
static PFNGLBINDBUFFERPROC              pglBindBuffer              = NULL;
static PFNGLBUFFERDATAPROC              pglBufferData              = NULL;
static PFNGLBUFFERSUBDATAPROC           pglBufferSubData           = NULL;
static PFNGLACTIVETEXTUREPROC           pglActiveTexture           = NULL;
static PFNGLCOMPRESSEDTEXIMAGE2DPROC    pglCompressedTexImage2D    = NULL;
static PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC pglCompressedTexSubImage2D = NULL;
static PFNGLBLENDFUNCSEPARATEPROC       pglBlendFuncSeparate       = NULL;
static PFNGLBLENDEQUATIONPROC           pglBlendEquation           = NULL;
static PFNGLBLENDEQUATIONSEPARATEPROC   pglBlendEquationSeparate   = NULL;
static PFNGLGENERATEMIPMAPPROC          pglGenerateMipmap          = NULL;

static void load_gl_extensions(void) {
    pglGenFramebuffers         = (PFNGLGENFRAMEBUFFERSPROC)        SDL_GL_GetProcAddress("glGenFramebuffers");
    pglDeleteFramebuffers      = (PFNGLDELETEFRAMEBUFFERSPROC)     SDL_GL_GetProcAddress("glDeleteFramebuffers");
    pglBindFramebuffer         = (PFNGLBINDFRAMEBUFFERPROC)        SDL_GL_GetProcAddress("glBindFramebuffer");
    pglFramebufferTexture2D    = (PFNGLFRAMEBUFFERTEXTURE2DPROC)   SDL_GL_GetProcAddress("glFramebufferTexture2D");
    pglFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    pglCheckFramebufferStatus  = (PFNGLCHECKFRAMEBUFFERSTATUSPROC) SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    pglGenRenderbuffers        = (PFNGLGENRENDERBUFFERSPROC)       SDL_GL_GetProcAddress("glGenRenderbuffers");
    pglDeleteRenderbuffers     = (PFNGLDELETERENDERBUFFERSPROC)    SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    pglBindRenderbuffer        = (PFNGLBINDRENDERBUFFERPROC)       SDL_GL_GetProcAddress("glBindRenderbuffer");
    pglRenderbufferStorage     = (PFNGLRENDERBUFFERSTORAGEPROC)    SDL_GL_GetProcAddress("glRenderbufferStorage");
    pglGenBuffers              = (PFNGLGENBUFFERSPROC)             SDL_GL_GetProcAddress("glGenBuffers");
    pglDeleteBuffers           = (PFNGLDELETEBUFFERSPROC)          SDL_GL_GetProcAddress("glDeleteBuffers");
    pglBindBuffer              = (PFNGLBINDBUFFERPROC)             SDL_GL_GetProcAddress("glBindBuffer");
    pglBufferData              = (PFNGLBUFFERDATAPROC)             SDL_GL_GetProcAddress("glBufferData");
    pglBufferSubData           = (PFNGLBUFFERSUBDATAPROC)          SDL_GL_GetProcAddress("glBufferSubData");
    pglActiveTexture           = (PFNGLACTIVETEXTUREPROC)          SDL_GL_GetProcAddress("glActiveTexture");
    pglCompressedTexImage2D    = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)   SDL_GL_GetProcAddress("glCompressedTexImage2D");
    pglCompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)SDL_GL_GetProcAddress("glCompressedTexSubImage2D");
    pglBlendFuncSeparate       = (PFNGLBLENDFUNCSEPARATEPROC)      SDL_GL_GetProcAddress("glBlendFuncSeparate");
    pglBlendEquation           = (PFNGLBLENDEQUATIONPROC)          SDL_GL_GetProcAddress("glBlendEquation");
    pglBlendEquationSeparate   = (PFNGLBLENDEQUATIONSEPARATEPROC)  SDL_GL_GetProcAddress("glBlendEquationSeparate");
    pglGenerateMipmap          = (PFNGLGENERATEMIPMAPPROC)         SDL_GL_GetProcAddress("glGenerateMipmap");
}

/* --- SHM --- */

static void *create_shm(const char *name, size_t size) {
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return NULL; }
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return NULL; }
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) { perror("mmap"); return NULL; }
    memset(ptr, 0, size);
    return ptr;
}

static void cleanup_shm(void) {
    if (gl_cmd_shm) { munmap(gl_cmd_shm, GL_SHM_SIZE);    shm_unlink(GL_SHM_NAME); }
    if (input_shm)  { munmap(input_shm,  INPUT_SHM_SIZE); shm_unlink(INPUT_SHM_NAME); }
}

/* --- Input --- */

static void push_input_event(InputEvent *ev) {
    if (!input_shm) return;
    uint32_t *widx  = input_shm_write_idx(input_shm);
    InputEvent *evs = input_shm_events(input_shm);
    evs[*widx % INPUT_QUEUE_SIZE] = *ev;
    __sync_synchronize();
    *widx = (*widx + 1);
}

/* Map window coords → game coords, accounting for rotation and letterboxing. */
static void scale_mouse(int wx, int wy, int32_t *gx, int32_t *gy) {
    int gw = (game_w > 0) ? game_w : win_width;
    int gh = (game_h > 0) ? game_h : win_height;
    if (rotate_mode == 1) {      /* CW 90° */
        *gx = (int32_t)((float)(win_height - 1 - wy) / win_height * gw);
        *gy = (int32_t)((float)wx / win_width * gh);
    } else if (rotate_mode == 2) { /* CCW 90° */
        *gx = (int32_t)((float)wy / win_height * gw);
        *gy = (int32_t)((float)(win_width - 1 - wx) / win_width * gh);
    } else if (rotate_mode == 3) { /* 180° */
        *gx = (int32_t)((float)(win_width  - 1 - wx) / win_width  * gw);
        *gy = (int32_t)((float)(win_height - 1 - wy) / win_height * gh);
    } else {
        *gx = (int32_t)((float)wx / win_width  * gw);
        *gy = (int32_t)((float)wy / win_height * gh);
    }
}

static void handle_sdl_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        InputEvent ie = {0};
        int32_t gx, gy;
        switch (e.type) {
            case SDL_QUIT:
                ie.type = INPUT_EVENT_QUIT;
                push_input_event(&ie);
                running = 0;
                break;
            case SDL_KEYDOWN:
                ie.type    = INPUT_EVENT_KEY_DOWN;
                ie.keycode = e.key.keysym.scancode;
                push_input_event(&ie);
                break;
            case SDL_KEYUP:
                ie.type    = INPUT_EVENT_KEY_UP;
                ie.keycode = e.key.keysym.scancode;
                push_input_event(&ie);
                break;
            case SDL_MOUSEMOTION:
                scale_mouse(e.motion.x, e.motion.y, &gx, &gy);
                ie.type = INPUT_EVENT_MOUSE_MOTION;
                ie.x = gx; ie.y = gy;
                push_input_event(&ie);
                break;
            case SDL_MOUSEBUTTONDOWN:
                scale_mouse(e.button.x, e.button.y, &gx, &gy);
                ie.type   = INPUT_EVENT_MOUSE_BUTTON_DOWN;
                ie.x = gx; ie.y = gy;
                ie.button = e.button.button;
                push_input_event(&ie);
                break;
            case SDL_MOUSEBUTTONUP:
                scale_mouse(e.button.x, e.button.y, &gx, &gy);
                ie.type   = INPUT_EVENT_MOUSE_BUTTON_UP;
                ie.x = gx; ie.y = gy;
                ie.button = e.button.button;
                push_input_event(&ie);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE)
                    running = 0;
                break;
        }
    }

    /* Simulated accelerometer from keyboard */
    if (input_shm) {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        float tilt = 5.0f;
        float ax = 0.0f, ay = 0.0f, az = -9.8f;
        if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) ax += tilt;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) ax -= tilt;
        if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) ay += tilt;
        if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) ay -= tilt;
        *input_shm_accel_x(input_shm) = ax;
        *input_shm_accel_y(input_shm) = ay;
        *input_shm_accel_z(input_shm) = az;
    }
}

/* --- Rotation FBO --- */

static void rotation_init_fbo(int w, int h) {
    if (rot_fbo && rot_game_w == w && rot_game_h == h) return;
    if (rot_fbo)       { pglDeleteFramebuffers(1,  &rot_fbo);       rot_fbo       = 0; }
    if (rot_tex)       { glDeleteTextures(1,        &rot_tex);       rot_tex       = 0; }
    if (rot_depth_rbo) { pglDeleteRenderbuffers(1, &rot_depth_rbo); rot_depth_rbo = 0; }

    glGenTextures(1, &rot_tex);
    glBindTexture(GL_TEXTURE_2D, rot_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    pglGenRenderbuffers(1, &rot_depth_rbo);
    pglBindRenderbuffer(GL_RENDERBUFFER, rot_depth_rbo);
    pglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    pglBindRenderbuffer(GL_RENDERBUFFER, 0);

    pglGenFramebuffers(1, &rot_fbo);
    pglBindFramebuffer(GL_FRAMEBUFFER, rot_fbo);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rot_tex, 0);
    pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rot_depth_rbo);

    GLenum status = pglCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[gl_relay] Rotation FBO incomplete: 0x%x\n", status);
    rot_game_w = w;
    rot_game_h = h;
    fprintf(stderr, "[gl_relay] Rotation FBO ready: %dx%d\n", w, h);
}

static void rotation_present(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glMatrixMode(GL_TEXTURE);    glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(-1,1,-1,1,-1,1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_width, win_height);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_LIGHTING);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);

    float verts[] = { -1,-1, 1,-1, 1,1, -1,1 };
    float texc[8];
    if (rotate_mode == 1) {
        texc[0]=0; texc[1]=1;  texc[2]=0; texc[3]=0;
        texc[4]=1; texc[5]=0;  texc[6]=1; texc[7]=1;
    } else if (rotate_mode == 2) {
        texc[0]=1; texc[1]=0;  texc[2]=1; texc[3]=1;
        texc[4]=0; texc[5]=1;  texc[6]=0; texc[7]=0;
    } else if (rotate_mode == 3) {
        texc[0]=1; texc[1]=1;  texc[2]=0; texc[3]=1;
        texc[4]=0; texc[5]=0;  texc[6]=1; texc[7]=0;
    } else {
        texc[0]=0; texc[1]=0;  texc[2]=1; texc[3]=0;
        texc[4]=1; texc[5]=1;  texc[6]=0; texc[7]=1;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, rot_tex);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, texc);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glMatrixMode(GL_TEXTURE);    glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();

    pglBindFramebuffer(GL_FRAMEBUFFER, rot_fbo);
}

/* --- GL Fixed-point conversion --- */
#define GL_FIXED 0x140C
static float    *fixed_conv_buf = NULL;
static uint32_t  fixed_conv_cap = 0;

static const void *srv_convert_fixed(GLenum *type, GLint size, uint32_t stride,
                                      uint32_t count, const void *data) {
    if (*type != GL_FIXED) return data;
    uint32_t nfloats = (uint32_t)size * count;
    if (nfloats > fixed_conv_cap) {
        free(fixed_conv_buf);
        fixed_conv_buf = malloc(nfloats * sizeof(float));
        fixed_conv_cap = nfloats;
    }
    const char *src = (const char *)data;
    for (uint32_t v = 0; v < count; v++) {
        const int32_t *row = (const int32_t *)(src + v * stride);
        for (int c = 0; c < size; c++)
            fixed_conv_buf[v * size + c] = (float)row[c] / 65536.0f;
    }
    *type = GL_FLOAT;
    return fixed_conv_buf;
}

/* --- Vertex array state (for FIXED conversion) --- */
static struct { void *data; uint32_t size, type, stride, count; }
    srv_vertex = {0}, srv_texcoord = {0}, srv_color = {0}, srv_normal = {0};

/* --- GL Command Replay --- */

static void process_gl_commands(void *shm) {
    uint32_t write_pos = *gl_shm_write_pos(shm);
    void    *data      = gl_shm_data(shm);
    uint32_t pos       = 0;

    while (pos < write_pos) {
        uint32_t *hdr       = (uint32_t *)((char *)data + pos);
        uint32_t  cmd_id    = hdr[0];
        uint32_t  total_size = hdr[1];
        void     *payload   = (char *)hdr + 8;
        uint32_t *p         = (uint32_t *)payload;
        float    *fp        = (float *)payload;

        if (total_size < 8 || pos + total_size > GL_CMD_DATA_SIZE) break;

        switch (cmd_id) {
            /* State */
            case GL_CMD_ENABLE:         glEnable(p[0]);  break;
            case GL_CMD_DISABLE:        glDisable(p[0]); break;
            case GL_CMD_BLEND_FUNC:     glBlendFunc(p[0], p[1]); break;
            case GL_CMD_CLEAR_COLOR:    glClearColor(fp[0], fp[1], fp[2], fp[3]); break;
            case GL_CMD_CLEAR:          glClear(p[0]); break;
            case GL_CMD_VIEWPORT: {
                int vx = (int)p[0], vy = (int)p[1], vw = (int)p[2], vh = (int)p[3];
                /* Track game render resolution for mouse scaling */
                if (vw > 0 && vh > 0) { game_w = vw; game_h = vh; }
                /* Auto-resize window to match game's native resolution on first full-screen
                 * viewport, unless the user explicitly specified a window size. */
                if (!user_sized && !auto_sized && vx == 0 && vy == 0 && vw > 0 && vh > 0
                        && (vw != win_width || vh != win_height)) {
                    SDL_SetWindowSize(window, vw, vh);
                    win_width  = vw;
                    win_height = vh;
                    fprintf(stderr, "[gl_relay] auto-resize window to %dx%d\n", vw, vh);
                    auto_sized = 1;
                }
                /* For rotation, recreate the FBO at the game's actual resolution */
                if (rotate_mode && pglGenFramebuffers && vw > 0 && vh > 0
                        && (vw != rot_game_w || vh != rot_game_h)) {
                    rotation_init_fbo(vw, vh);
                    pglBindFramebuffer(GL_FRAMEBUFFER, rot_fbo);
                }
                glViewport(vx, vy, vw, vh);
                break;
            }
            case GL_CMD_SCISSOR:        glScissor((int)p[0], (int)p[1], (int)p[2], (int)p[3]); break;
            case GL_CMD_DEPTH_FUNC:     glDepthFunc(p[0]);  break;
            case GL_CMD_DEPTH_MASK:     glDepthMask(p[0]);  break;
            case GL_CMD_DEPTH_RANGEF:   glDepthRange(fp[0], fp[1]); break;
            case GL_CMD_COLOR4F:        glColor4f(fp[0], fp[1], fp[2], fp[3]); break;
            case GL_CMD_COLOR4UB:       glColor4ub(p[0], p[1], p[2], p[3]); break;
            case GL_CMD_ALPHA_FUNC:     glAlphaFunc(p[0], *(float*)&p[1]); break;
            case GL_CMD_SHADE_MODEL:    glShadeModel(p[0]); break;
            case GL_CMD_LINE_WIDTH:     glLineWidth(fp[0]); break;
            case GL_CMD_POINT_SIZE:     glPointSize(fp[0]); break;
            case GL_CMD_FRONT_FACE:     glFrontFace(p[0]);  break;
            case GL_CMD_CULL_FACE:      glCullFace(p[0]);   break;
            case GL_CMD_HINT:           glHint(p[0], p[1]); break;
            case GL_CMD_POLYGON_OFFSET: glPolygonOffset(fp[0], fp[1]); break;
            case GL_CMD_STENCIL_FUNC:   glStencilFunc(p[0], p[1], p[2]); break;
            case GL_CMD_STENCIL_OP:     glStencilOp(p[0], p[1], p[2]);   break;
            case GL_CMD_STENCIL_MASK:   glStencilMask(p[0]); break;
            case GL_CMD_COLOR_MASK:     glColorMask(p[0], p[1], p[2], p[3]); break;
            case GL_CMD_TEX_ENVF:       glTexEnvf(p[0], p[1], fp[2]); break;
            case GL_CMD_TEX_ENVI:       glTexEnvi(p[0], p[1], p[2]);  break;
            case GL_CMD_TEX_ENVFV:      glTexEnvfv(p[0], p[1], fp + 2); break;
            case GL_CMD_PIXEL_STOREI:   glPixelStorei(p[0], p[1]); break;

            /* Textures */
            case GL_CMD_GEN_TEXTURES: {
                uint32_t n = p[0];
                GLuint  *arm_ids = p + 1;
                GLuint   sids[64];
                if (n > 64) n = 64;
                glGenTextures(n, sids);
                for (uint32_t i = 0; i < n; i++)
                    if (arm_ids[i] < MAX_GL_IDS) tex_map[arm_ids[i]] = sids[i];
                break;
            }
            case GL_CMD_DELETE_TEXTURES: {
                uint32_t n = p[0];
                GLuint   sids[64];
                if (n > 64) n = 64;
                for (uint32_t i = 0; i < n; i++) {
                    GLuint a = p[1 + i];
                    sids[i] = map_texture(a);
                    if (a < MAX_GL_IDS) tex_map[a] = 0;
                }
                glDeleteTextures(n, sids);
                break;
            }
            case GL_CMD_BIND_TEXTURE:
                glBindTexture(p[0], map_texture(p[1]));
                break;
            case GL_CMD_TEX_IMAGE_2D: {
                GLenum target = p[0];
                GLint  level  = p[1];
                GLint  ifmt   = p[2];
                GLsizei w = p[3], h = p[4];
                GLenum fmt = p[5], type = p[6];
                uint32_t dsize = p[7];
                void *pix = dsize > 0 ? (p + 8) : NULL;
                /* Remap unsized internal formats for desktop GL */
                GLint gl_ifmt = ifmt;
                if (type == 0x8363 /* GL_UNSIGNED_SHORT_5_6_5 */)
                    gl_ifmt = GL_RGB;
                else if (type == 0x8033 /* GL_UNSIGNED_SHORT_4_4_4_4 */ ||
                         type == 0x8034 /* GL_UNSIGNED_SHORT_5_5_5_1 */)
                    gl_ifmt = GL_RGBA;
                glGetError(); /* clear */
                glTexImage2D(target, level, gl_ifmt, w, h, 0, fmt, type, pix);
                { GLenum e = glGetError(); if (e) fprintf(stderr, "[TEX] glTexImage2D GL error: 0x%x ifmt=0x%x->0x%x %dx%d\n", e, ifmt, gl_ifmt, w, h); }
                break;
            }
            case GL_CMD_TEX_SUB_IMAGE_2D: {
                GLenum target = p[0];
                GLint  level = p[1], xo = p[2], yo = p[3];
                GLsizei w = p[4], h = p[5];
                GLenum fmt = p[6], type = p[7];
                uint32_t dsize = p[8];
                void *pix = dsize > 0 ? (p + 9) : NULL;
                glTexSubImage2D(target, level, xo, yo, w, h, fmt, type, pix);
                break;
            }
            case GL_CMD_COMPRESSED_TEX_IMAGE_2D: {
                GLenum  target = p[0];
                GLint   level  = p[1];
                GLenum  ifmt   = p[2];
                GLsizei w = p[3], h = p[4];
                GLsizei isize  = p[5];
                void   *d      = p + 6;
                if (pvrtc_is_pvrtc(ifmt)) {
                    int out_size = 0;
                    uint8_t *rgba = pvrtc_decompress(ifmt, d, w, h, &out_size);
                    if (rgba) {
                        glTexImage2D(target, level, GL_RGBA, w, h, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        free(rgba);
                    } else { fprintf(stderr, "[TEX] PVRTC decompress failed\n"); }
                } else if (atc_is_atc(ifmt)) {
                    int out_size = 0;
                    uint8_t *rgba = atc_decompress(ifmt, d, w, h, &out_size);
                    if (rgba) {
                        glGetError();
                        glTexImage2D(target, level, GL_RGBA, w, h, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        { GLenum e = glGetError(); if (e) fprintf(stderr, "[TEX] ATC upload GL error: 0x%x\n", e); }
                        free(rgba);
                    } else { fprintf(stderr, "[TEX] ATC decompress failed\n"); }
                } else {
                    if (pglCompressedTexImage2D) {
                        GLenum orig = ifmt;
                        /* ETC1 → ETC2 (valid subset, same bitstream) */
                        if (ifmt == 0x8D64) ifmt = 0x9274;
                        /* 3DC normal map formats → RGTC (same bitstream, different enum) */
                        else if (ifmt == 0x87F9) ifmt = 0x8DBB; /* GL_3DC_X_AMD  → GL_COMPRESSED_RED_RGTC1  */
                        else if (ifmt == 0x87FA) ifmt = 0x8DBD; /* GL_3DC_XY_AMD → GL_COMPRESSED_RG_RGTC2  */
                        if (ifmt != orig) fprintf(stderr, "[TEX] remap 0x%x -> 0x%x\n", orig, ifmt);
                        glGetError();
                        pglCompressedTexImage2D(target, level, ifmt, w, h, 0, isize, d);
                        { GLenum e = glGetError(); if (e) fprintf(stderr, "[TEX] CompressedTexImage2D GL error: 0x%x (ifmt=0x%x)\n", e, ifmt); }
                    }
                }
                break;
            }
            case GL_CMD_TEX_PARAMETERF:  glTexParameterf(p[0], p[1], fp[2]); break;
            case GL_CMD_TEX_PARAMETERI:  glTexParameteri(p[0], p[1], p[2]);  break;
            case GL_CMD_ACTIVE_TEXTURE:
                if (pglActiveTexture) pglActiveTexture(p[0]);
                break;
            case GL_CMD_CLIENT_ACTIVE_TEXTURE:
                glClientActiveTexture(p[0]);
                break;

            /* Matrices */
            case GL_CMD_MATRIX_MODE:   glMatrixMode(p[0]);   break;
            case GL_CMD_LOAD_IDENTITY: glLoadIdentity();     break;
            case GL_CMD_LOAD_MATRIXF:  glLoadMatrixf(fp);    break;
            case GL_CMD_MULT_MATRIXF:  glMultMatrixf(fp);    break;
            case GL_CMD_PUSH_MATRIX:   glPushMatrix();       break;
            case GL_CMD_POP_MATRIX:    glPopMatrix();        break;
            case GL_CMD_ORTHOF:        glOrtho(fp[0], fp[1], fp[2], fp[3], fp[4], fp[5]); break;
            case GL_CMD_FRUSTUMF:      glFrustum(fp[0], fp[1], fp[2], fp[3], fp[4], fp[5]); break;
            case GL_CMD_TRANSLATEF:    glTranslatef(fp[0], fp[1], fp[2]); break;
            case GL_CMD_ROTATEF:       glRotatef(fp[0], fp[1], fp[2], fp[3]); break;
            case GL_CMD_SCALEF:        glScalef(fp[0], fp[1], fp[2]); break;

            /* Client state */
            case GL_CMD_ENABLE_CLIENT_STATE:  glEnableClientState(p[0]);  break;
            case GL_CMD_DISABLE_CLIENT_STATE: glDisableClientState(p[0]); break;

            /* Vertex arrays (inline data from ARM) */
            case GL_CMD_VERTEX_POINTER: {
                srv_vertex.size = p[1]; srv_vertex.type = p[2];
                srv_vertex.stride = p[3]; srv_vertex.count = p[4]; srv_vertex.data = p + 5;
                { GLenum t = p[2]; int wf = (t == GL_FIXED);
                  uint32_t in_stride = p[3] ? p[3] : p[1]*4;
                  const void *d = srv_convert_fixed(&t, p[1], in_stride, p[4], p + 5);
                  glVertexPointer(p[1], t, wf ? p[1]*4 : p[3], d); }
                break;
            }
            case GL_CMD_TEX_COORD_POINTER: {
                srv_texcoord.size = p[1]; srv_texcoord.type = p[2];
                srv_texcoord.stride = p[3]; srv_texcoord.count = p[4]; srv_texcoord.data = p + 5;
                { GLenum t = p[2]; int wf = (t == GL_FIXED);
                  uint32_t in_stride = p[3] ? p[3] : p[1]*4;
                  const void *d = srv_convert_fixed(&t, p[1], in_stride, p[4], p + 5);
                  glTexCoordPointer(p[1], t, wf ? p[1]*4 : p[3], d); }
                break;
            }
            case GL_CMD_COLOR_POINTER: {
                srv_color.size = p[1]; srv_color.type = p[2];
                srv_color.stride = p[3]; srv_color.count = p[4]; srv_color.data = p + 5;
                { GLenum t = p[2]; int wf = (t == GL_FIXED);
                  uint32_t in_stride = p[3] ? p[3] : p[1]*4;
                  const void *d = srv_convert_fixed(&t, p[1], in_stride, p[4], p + 5);
                  glColorPointer(p[1], t, wf ? p[1]*4 : p[3], d); }
                break;
            }
            case GL_CMD_NORMAL_POINTER: {
                srv_normal.type = p[2]; srv_normal.stride = p[3];
                srv_normal.count = p[4]; srv_normal.data = p + 5;
                { GLenum t = p[2]; int wf = (t == GL_FIXED);
                  uint32_t in_stride = p[3] ? p[3] : 3*4;
                  const void *d = srv_convert_fixed(&t, 3, in_stride, p[4], p + 5);
                  glNormalPointer(t, wf ? 3*4 : p[3], d); }
                break;
            }

            /* Draw */
            case GL_CMD_DRAW_ARRAYS:
                glDrawArrays(p[0], p[1], p[2]);
                break;
            case GL_CMD_DRAW_ELEMENTS: {
                GLenum  mode       = p[0];
                GLsizei count      = p[1];
                GLenum  type       = p[2];
                uint32_t has_inline = p[3];
                void *indices = has_inline ? (p + 4) : NULL;
                glDrawElements(mode, count, type, indices);
                break;
            }

            /* FBO */
            case GL_CMD_GEN_FRAMEBUFFERS:
                if (pglGenFramebuffers) {
                    uint32_t n = p[0];
                    GLuint  *arm_ids = p + 1;
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    pglGenFramebuffers(n, sids);
                    for (uint32_t i = 0; i < n; i++)
                        if (arm_ids[i] < MAX_GL_IDS) fbo_map[arm_ids[i]] = sids[i];
                }
                break;
            case GL_CMD_DELETE_FRAMEBUFFERS:
                if (pglDeleteFramebuffers) {
                    uint32_t n = p[0];
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    for (uint32_t i = 0; i < n; i++) {
                        GLuint a = p[1 + i];
                        sids[i] = map_framebuffer(a);
                        if (a < MAX_GL_IDS) fbo_map[a] = 0;
                    }
                    pglDeleteFramebuffers(n, sids);
                }
                break;
            case GL_CMD_BIND_FRAMEBUFFER:
                if (pglBindFramebuffer) {
                    GLuint fbo = map_framebuffer(p[1]);
                    /* When rotating, redirect FBO 0 → rotation FBO */
                    if (fbo == 0 && rotate_mode && rot_fbo)
                        fbo = rot_fbo;
                    pglBindFramebuffer(p[0], fbo);
                }
                break;
            case GL_CMD_FRAMEBUFFER_TEXTURE_2D:
                if (pglFramebufferTexture2D)
                    pglFramebufferTexture2D(p[0], p[1], p[2], map_texture(p[3]), p[4]);
                break;
            case GL_CMD_FRAMEBUFFER_RENDERBUFFER:
                if (pglFramebufferRenderbuffer)
                    pglFramebufferRenderbuffer(p[0], p[1], p[2], map_renderbuffer(p[3]));
                break;
            case GL_CMD_CHECK_FRAMEBUFFER_STATUS:
                if (pglCheckFramebufferStatus) pglCheckFramebufferStatus(p[0]);
                break;

            /* RBO */
            case GL_CMD_GEN_RENDERBUFFERS:
                if (pglGenRenderbuffers) {
                    uint32_t n = p[0];
                    GLuint  *arm_ids = p + 1;
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    pglGenRenderbuffers(n, sids);
                    for (uint32_t i = 0; i < n; i++)
                        if (arm_ids[i] < MAX_GL_IDS) rbo_map[arm_ids[i]] = sids[i];
                }
                break;
            case GL_CMD_DELETE_RENDERBUFFERS:
                if (pglDeleteRenderbuffers) {
                    uint32_t n = p[0];
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    for (uint32_t i = 0; i < n; i++) {
                        GLuint a = p[1 + i];
                        sids[i] = map_renderbuffer(a);
                        if (a < MAX_GL_IDS) rbo_map[a] = 0;
                    }
                    pglDeleteRenderbuffers(n, sids);
                }
                break;
            case GL_CMD_BIND_RENDERBUFFER:
                if (pglBindRenderbuffer) pglBindRenderbuffer(p[0], map_renderbuffer(p[1]));
                break;
            case GL_CMD_RENDERBUFFER_STORAGE:
                if (pglRenderbufferStorage) pglRenderbufferStorage(p[0], p[1], p[2], p[3]);
                break;

            /* VBO */
            case GL_CMD_GEN_BUFFERS:
                if (pglGenBuffers) {
                    uint32_t n = p[0];
                    GLuint  *arm_ids = p + 1;
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    pglGenBuffers(n, sids);
                    for (uint32_t i = 0; i < n; i++)
                        if (arm_ids[i] < MAX_GL_IDS) vbo_map[arm_ids[i]] = sids[i];
                }
                break;
            case GL_CMD_DELETE_BUFFERS:
                if (pglDeleteBuffers) {
                    uint32_t n = p[0];
                    GLuint   sids[64];
                    if (n > 64) n = 64;
                    for (uint32_t i = 0; i < n; i++) {
                        GLuint a = p[1 + i];
                        sids[i] = map_buffer(a);
                        if (a < MAX_GL_IDS) vbo_map[a] = 0;
                    }
                    pglDeleteBuffers(n, sids);
                }
                break;
            case GL_CMD_BIND_BUFFER:
                if (pglBindBuffer) pglBindBuffer(p[0], map_buffer(p[1]));
                break;
            case GL_CMD_BUFFER_DATA:
                if (pglBufferData) {
                    GLenum  target = p[0];
                    GLsizei size   = p[1];
                    GLenum  usage  = p[2];
                    void   *d = (total_size > 8 + 12) ? (p + 3) : NULL;
                    pglBufferData(target, size, d, usage);
                }
                break;
            case GL_CMD_BUFFER_SUB_DATA:
                if (pglBufferSubData) {
                    GLenum  target = p[0];
                    GLint   offset = p[1];
                    GLsizei size   = p[2];
                    pglBufferSubData(target, offset, size, p + 3);
                }
                break;

            /* Lighting */
            case GL_CMD_LIGHTF:        glLightf(p[0], p[1], fp[2]); break;
            case GL_CMD_LIGHTFV:       glLightfv(p[0], p[1], fp + 2); break;
            case GL_CMD_LIGHT_MODELF:  glLightModelf(p[0], fp[1]); break;
            case GL_CMD_LIGHT_MODELFV: glLightModelfv(p[0], fp + 1); break;
            case GL_CMD_GET_LIGHTFV:   break; /* handled ARM-side */
            case GL_CMD_MATERIALF:     glMaterialf(p[0], p[1], fp[2]); break;
            case GL_CMD_MATERIALFV:    glMaterialfv(p[0], p[1], fp + 2); break;

            /* Fog */
            case GL_CMD_FOGF:
                if (p[0] == 0x0B65) glFogi(0x0B65, (GLint)fp[1]);
                else glFogf(p[0], fp[1]);
                break;
            case GL_CMD_FOGFV: glFogfv(p[0], fp + 1); break;
            case GL_CMD_FOGI:  glFogi(p[0], p[1]);     break;

            /* Additional state */
            case GL_CMD_CLEAR_DEPTHF:   glClearDepth((double)fp[0]); break;
            case GL_CMD_CLEAR_STENCIL:  glClearStencil((GLint)p[0]); break;
            case GL_CMD_NORMAL3F:       glNormal3f(fp[0], fp[1], fp[2]); break;
            case GL_CMD_MULTI_TEX_COORD4F:
                glMultiTexCoord4f(p[0], fp[1], fp[2], fp[3], fp[4]);
                break;
            case GL_CMD_CLIP_PLANEF: {
                double eq[4] = { fp[1], fp[2], fp[3], fp[4] };
                glClipPlane(p[0], eq);
                break;
            }
            case GL_CMD_POINT_PARAMETERF:  glPointParameterf(p[0], fp[1]); break;
            case GL_CMD_POINT_PARAMETERFV: glPointParameterfv(p[0], fp + 1); break;
            case GL_CMD_SAMPLE_COVERAGE:   glSampleCoverage(fp[0], (GLboolean)p[1]); break;
            case GL_CMD_LOGIC_OP:          glLogicOp(p[0]); break;
            case GL_CMD_TEX_ENVIV:         glTexEnviv(p[0], p[1], (const GLint *)(p + 2)); break;
            case GL_CMD_TEX_PARAMETERFV:   glTexParameterfv(p[0], p[1], fp + 2); break;
            case GL_CMD_TEX_PARAMETERIV:   glTexParameteriv(p[0], p[1], (const GLint *)(p + 2)); break;
            case GL_CMD_COMPRESSED_TEX_SUB_IMAGE_2D: {
                GLenum  sub_target = p[0];
                GLint   sub_level  = p[1];
                GLint   sub_xoff = p[2], sub_yoff = p[3];
                GLsizei sub_w = p[4],   sub_h    = p[5];
                GLenum  sub_fmt    = p[6];
                GLsizei imgSize    = p[7];
                void   *sub_d      = (char *)payload + 32;
                if (pvrtc_is_pvrtc(sub_fmt)) {
                    int out_size = 0;
                    uint8_t *rgba = pvrtc_decompress(sub_fmt, sub_d, sub_w, sub_h, &out_size);
                    if (rgba) {
                        glTexSubImage2D(sub_target, sub_level, sub_xoff, sub_yoff,
                                        sub_w, sub_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        free(rgba);
                    }
                } else if (atc_is_atc(sub_fmt)) {
                    int out_size = 0;
                    uint8_t *rgba = atc_decompress(sub_fmt, sub_d, sub_w, sub_h, &out_size);
                    if (rgba) {
                        glTexSubImage2D(sub_target, sub_level, sub_xoff, sub_yoff,
                                        sub_w, sub_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        free(rgba);
                    }
                } else if (pglCompressedTexSubImage2D) {
                    pglCompressedTexSubImage2D(sub_target, sub_level, sub_xoff, sub_yoff,
                                               sub_w, sub_h, sub_fmt, imgSize, sub_d);
                }
                break;
            }
            case GL_CMD_COPY_TEX_IMAGE_2D:
                glCopyTexImage2D(p[0], p[1], p[2], p[3], p[4], p[5], p[6], 0);
                break;
            case GL_CMD_COPY_TEX_SUB_IMAGE_2D:
                glCopyTexSubImage2D(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                break;
            case GL_CMD_GENERATE_MIPMAP:
                if (pglGenerateMipmap) pglGenerateMipmap(p[0]);
                break;
            case GL_CMD_BLEND_FUNC_SEPARATE:
                if (pglBlendFuncSeparate) pglBlendFuncSeparate(p[0], p[1], p[2], p[3]);
                break;
            case GL_CMD_BLEND_EQUATION:
                if (pglBlendEquation) pglBlendEquation(p[0]);
                break;
            case GL_CMD_BLEND_EQUATION_SEPARATE:
                if (pglBlendEquationSeparate) pglBlendEquationSeparate(p[0], p[1]);
                break;

            /* Misc */
            case GL_CMD_FLUSH:        glFlush();  break;
            case GL_CMD_FINISH:       glFinish(); break;
            case GL_CMD_SWAP_BUFFERS: break; /* handled by frame_ready */

            default:
                fprintf(stderr, "[gl_relay] Unknown GL cmd: %u (size %u)\n", cmd_id, total_size);
                break;
        }

        pos += total_size;
    }
}

/* --- Signal Handler --- */

static void sigint_handler(int sig) { (void)sig; running = 0; }

/* --- Main --- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Window size from env (set by run.sh) */
    const char *sw_env = getenv("WEBOS_SCREEN_W");
    const char *sh_env = getenv("WEBOS_SCREEN_H");
    const char *ww_env = getenv("WEBOS_WIN_W");
    const char *wh_env = getenv("WEBOS_WIN_H");
    const char *rot_env = getenv("WEBOS_ROTATE");

    /* Explicit pixel size overrides screen-fit calculation */
    if (ww_env && wh_env) {
        win_width  = atoi(ww_env);
        win_height = atoi(wh_env);
        user_sized = 1;
    } else if (sw_env && sh_env) {
        /* We don't know the game's native resolution yet; use screen size as window
         * and let the game's first glViewport determine actual game res for mouse scaling.
         * For GLES games the window IS the render target, so just use the screen size. */
        win_width  = atoi(sw_env);
        win_height = atoi(sh_env);
        user_sized = 1;
    }
    if (win_width  <= 0) win_width  = 480;
    if (win_height <= 0) win_height = 320;

    if (rot_env) {
        if      (strcasecmp(rot_env, "CW")  == 0) rotate_mode = 1;
        else if (strcasecmp(rot_env, "CCW") == 0) rotate_mode = 2;
        else if (strcmp(rot_env, "180")     == 0) rotate_mode = 3;
    }

    /* Window position */
    const char *wx_env = getenv("WEBOS_WIN_X");
    const char *wy_env = getenv("WEBOS_WIN_Y");
    int win_x = wx_env ? atoi(wx_env) : SDL_WINDOWPOS_CENTERED;
    int win_y = wy_env ? atoi(wy_env) : SDL_WINDOWPOS_CENTERED;

    fprintf(stderr, "[gl_relay] window=%dx%d pos=(%d,%d) rotate=%d\n",
            win_width, win_height, win_x, win_y, rotate_mode);

    /* Initialise SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[gl_relay] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,   0);

    window = SDL_CreateWindow("webOS PDK Runner",
                              win_x, win_y,
                              win_width, win_height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "[gl_relay] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "[gl_relay] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);
    load_gl_extensions();
    fprintf(stderr, "[gl_relay] OpenGL: %s / %s\n",
            glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    /* Initial GL state — matches Adreno 220 GLES 1.1 defaults */
    glViewport(0, 0, win_width, win_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);

    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_TEXTURE_2D);   /* Adreno 220 default: on */
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    /* Create SHM regions */
    gl_cmd_shm = create_shm(GL_SHM_NAME,    GL_SHM_SIZE);
    input_shm  = create_shm(INPUT_SHM_NAME, INPUT_SHM_SIZE);
    if (!gl_cmd_shm || !input_shm) {
        fprintf(stderr, "[gl_relay] Failed to create SHM\n");
        cleanup_shm();
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    *input_shm_screen_w(input_shm) = win_width;
    *input_shm_screen_h(input_shm) = win_height;

    fprintf(stderr, "[gl_relay] Ready. Waiting for game...\n");

    /* Main loop */
    while (running) {
        handle_sdl_events();

        /* Mid-frame flush */
        if (*gl_shm_flush_request(gl_cmd_shm)) {
            process_gl_commands(gl_cmd_shm);
            *gl_shm_write_pos(gl_cmd_shm) = 0;
            __sync_synchronize();
            *gl_shm_flush_request(gl_cmd_shm) = 0;
        }

        /* Complete frame */
        if (*gl_shm_frame_ready(gl_cmd_shm)) {
            process_gl_commands(gl_cmd_shm);
            if (rotate_mode && rot_fbo)
                rotation_present();
            SDL_GL_SwapWindow(window);
            *gl_shm_write_pos(gl_cmd_shm) = 0;
            __sync_synchronize();
            *gl_shm_frame_ready(gl_cmd_shm) = 0;
        }

        /* Yield when idle */
        if (!*gl_shm_arm_waiting(gl_cmd_shm))
            SDL_Delay(1);
    }

    fprintf(stderr, "[gl_relay] Shutting down\n");
    cleanup_shm();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(fixed_conv_buf);
    return 0;
}
