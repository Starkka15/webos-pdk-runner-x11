/*
 * pdk_gl_cmd.h — Shared GL command buffer definitions
 * Used by both the ARM-side GLES relay and the x86_64 display server.
 *
 * Architecture: ARM game serializes GLES 1.1 calls into a 32MB SHM region.
 * The display server reads and replays them with native OpenGL.
 */
#ifndef PDK_GL_CMD_H
#define PDK_GL_CMD_H

#include <stdint.h>

#define GL_SHM_NAME   "/webos_pdk_gl"
#define GL_SHM_SIZE   (32 * 1024 * 1024)  /* 32MB command buffer */

/* Command IDs */
enum {
    GL_CMD_NONE = 0,

    /* State */
    GL_CMD_ENABLE,
    GL_CMD_DISABLE,
    GL_CMD_BLEND_FUNC,
    GL_CMD_CLEAR_COLOR,
    GL_CMD_CLEAR,
    GL_CMD_VIEWPORT,
    GL_CMD_SCISSOR,
    GL_CMD_DEPTH_FUNC,
    GL_CMD_DEPTH_MASK,
    GL_CMD_DEPTH_RANGEF,
    GL_CMD_COLOR4F,
    GL_CMD_COLOR4UB,
    GL_CMD_ALPHA_FUNC,
    GL_CMD_SHADE_MODEL,
    GL_CMD_LINE_WIDTH,
    GL_CMD_POINT_SIZE,
    GL_CMD_FRONT_FACE,
    GL_CMD_CULL_FACE,
    GL_CMD_HINT,
    GL_CMD_POLYGON_OFFSET,
    GL_CMD_STENCIL_FUNC,
    GL_CMD_STENCIL_OP,
    GL_CMD_STENCIL_MASK,
    GL_CMD_COLOR_MASK,
    GL_CMD_TEX_ENVF,
    GL_CMD_TEX_ENVI,
    GL_CMD_TEX_ENVFV,

    /* Textures */
    GL_CMD_GEN_TEXTURES,
    GL_CMD_DELETE_TEXTURES,
    GL_CMD_BIND_TEXTURE,
    GL_CMD_TEX_IMAGE_2D,
    GL_CMD_TEX_SUB_IMAGE_2D,
    GL_CMD_TEX_PARAMETERF,
    GL_CMD_TEX_PARAMETERI,
    GL_CMD_ACTIVE_TEXTURE,
    GL_CMD_CLIENT_ACTIVE_TEXTURE,
    GL_CMD_COMPRESSED_TEX_IMAGE_2D,
    GL_CMD_PIXEL_STOREI,

    /* Matrices */
    GL_CMD_MATRIX_MODE,
    GL_CMD_LOAD_IDENTITY,
    GL_CMD_LOAD_MATRIXF,
    GL_CMD_MULT_MATRIXF,
    GL_CMD_PUSH_MATRIX,
    GL_CMD_POP_MATRIX,
    GL_CMD_ORTHOF,
    GL_CMD_FRUSTUMF,
    GL_CMD_TRANSLATEF,
    GL_CMD_ROTATEF,
    GL_CMD_SCALEF,

    /* Vertex arrays */
    GL_CMD_ENABLE_CLIENT_STATE,
    GL_CMD_DISABLE_CLIENT_STATE,
    GL_CMD_VERTEX_POINTER,
    GL_CMD_TEX_COORD_POINTER,
    GL_CMD_COLOR_POINTER,
    GL_CMD_NORMAL_POINTER,
    GL_CMD_DRAW_ARRAYS,
    GL_CMD_DRAW_ELEMENTS,

    /* FBO */
    GL_CMD_GEN_FRAMEBUFFERS,
    GL_CMD_DELETE_FRAMEBUFFERS,
    GL_CMD_BIND_FRAMEBUFFER,
    GL_CMD_FRAMEBUFFER_TEXTURE_2D,
    GL_CMD_FRAMEBUFFER_RENDERBUFFER,
    GL_CMD_CHECK_FRAMEBUFFER_STATUS,

    /* RBO */
    GL_CMD_GEN_RENDERBUFFERS,
    GL_CMD_DELETE_RENDERBUFFERS,
    GL_CMD_BIND_RENDERBUFFER,
    GL_CMD_RENDERBUFFER_STORAGE,

    /* VBO */
    GL_CMD_GEN_BUFFERS,
    GL_CMD_DELETE_BUFFERS,
    GL_CMD_BIND_BUFFER,
    GL_CMD_BUFFER_DATA,
    GL_CMD_BUFFER_SUB_DATA,

    /* Lighting */
    GL_CMD_LIGHTF,
    GL_CMD_LIGHTFV,
    GL_CMD_LIGHT_MODELF,
    GL_CMD_LIGHT_MODELFV,
    GL_CMD_GET_LIGHTFV,
    GL_CMD_MATERIALF,
    GL_CMD_MATERIALFV,

    /* Fog */
    GL_CMD_FOGF,
    GL_CMD_FOGFV,
    GL_CMD_FOGI,

    /* Misc */
    GL_CMD_FLUSH,
    GL_CMD_FINISH,
    GL_CMD_READ_PIXELS,

    /* Additional state */
    GL_CMD_CLEAR_DEPTHF,
    GL_CMD_CLEAR_STENCIL,
    GL_CMD_NORMAL3F,
    GL_CMD_MULTI_TEX_COORD4F,
    GL_CMD_CLIP_PLANEF,
    GL_CMD_POINT_PARAMETERF,
    GL_CMD_POINT_PARAMETERFV,
    GL_CMD_SAMPLE_COVERAGE,
    GL_CMD_LOGIC_OP,
    GL_CMD_TEX_ENVIV,
    GL_CMD_TEX_PARAMETERFV,
    GL_CMD_TEX_PARAMETERIV,
    GL_CMD_COMPRESSED_TEX_SUB_IMAGE_2D,
    GL_CMD_COPY_TEX_IMAGE_2D,
    GL_CMD_COPY_TEX_SUB_IMAGE_2D,
    GL_CMD_GENERATE_MIPMAP,
    GL_CMD_BLEND_FUNC_SEPARATE,
    GL_CMD_BLEND_EQUATION,
    GL_CMD_BLEND_EQUATION_SEPARATE,

    /* Sync */
    GL_CMD_SWAP_BUFFERS,

    GL_CMD_MAX
};

/*
 * SHM layout:
 *   [0..3]   uint32_t write_pos      — next write offset (updated by ARM)
 *   [4..7]   uint32_t frame_ready    — 1 = frame complete, server should process
 *   [8..11]  uint32_t server_ack     — server sets to 1 after processing
 *   [12..15] uint32_t flush_request  — ARM sets to 1 for mid-frame flush
 *   [16..19] uint32_t arm_waiting    — ARM is waiting for server to catch up
 *   [20..63] reserved
 *   [64..]   command data
 */
#define GL_CMD_HEADER_SIZE  64
#define GL_CMD_DATA_START   64
#define GL_CMD_DATA_SIZE    (GL_SHM_SIZE - GL_CMD_DATA_START)

/* Each command in the buffer:
 *   uint32_t cmd_id
 *   uint32_t total_size  (including these 8 bytes)
 *   ... payload ...
 */

static inline uint32_t *gl_shm_write_pos(void *shm)    { return (uint32_t *)shm; }
static inline uint32_t *gl_shm_frame_ready(void *shm)   { return (uint32_t *)((char *)shm + 4); }
static inline uint32_t *gl_shm_server_ack(void *shm)    { return (uint32_t *)((char *)shm + 8); }
static inline uint32_t *gl_shm_flush_request(void *shm)  { return (uint32_t *)((char *)shm + 12); }
static inline uint32_t *gl_shm_arm_waiting(void *shm)   { return (uint32_t *)((char *)shm + 16); }
static inline void     *gl_shm_data(void *shm)          { return (char *)shm + GL_CMD_DATA_START; }

#endif /* PDK_GL_CMD_H */
