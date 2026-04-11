/*
 * pdk_input.h — Shared input event queue definitions
 * Display server writes input events, ARM game reads them.
 */
#ifndef PDK_INPUT_H
#define PDK_INPUT_H

#include <stdint.h>

#define INPUT_SHM_NAME   "/webos_pdk_input"
#define INPUT_SHM_SIZE   (64 * 1024)  /* 64KB */

#define INPUT_QUEUE_SIZE 256

enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY_DOWN,
    INPUT_EVENT_KEY_UP,
    INPUT_EVENT_MOUSE_MOTION,
    INPUT_EVENT_MOUSE_BUTTON_DOWN,
    INPUT_EVENT_MOUSE_BUTTON_UP,
    INPUT_EVENT_QUIT,
    INPUT_EVENT_TOUCH_DOWN,
    INPUT_EVENT_TOUCH_UP,
    INPUT_EVENT_TOUCH_MOTION,
};

typedef struct {
    uint32_t type;
    int32_t  x, y;         /* mouse/touch position */
    uint32_t keycode;       /* SDL scancode */
    uint32_t button;        /* mouse button */
    uint32_t touch_id;      /* finger id for multitouch */
} InputEvent;

/*
 * SHM layout:
 *   [0..3]   uint32_t write_idx  — next write position (server)
 *   [4..7]   uint32_t read_idx   — next read position (ARM game)
 *   [8..11]  uint32_t screen_w   — display width
 *   [12..15] uint32_t screen_h   — display height
 *   [16..19] float    accel_x    — simulated accelerometer X
 *   [20..23] float    accel_y    — simulated accelerometer Y
 *   [24..27] float    accel_z    — simulated accelerometer Z
 *   [28..63] reserved
 *   [64..]   InputEvent events[INPUT_QUEUE_SIZE]
 */
#define INPUT_HEADER_SIZE 64

static inline uint32_t   *input_shm_write_idx(void *shm)  { return (uint32_t *)shm; }
static inline uint32_t   *input_shm_read_idx(void *shm)   { return (uint32_t *)((char *)shm + 4); }
static inline uint32_t   *input_shm_screen_w(void *shm)   { return (uint32_t *)((char *)shm + 8); }
static inline uint32_t   *input_shm_screen_h(void *shm)   { return (uint32_t *)((char *)shm + 12); }
static inline float      *input_shm_accel_x(void *shm)    { return (float *)((char *)shm + 16); }
static inline float      *input_shm_accel_y(void *shm)    { return (float *)((char *)shm + 20); }
static inline float      *input_shm_accel_z(void *shm)    { return (float *)((char *)shm + 24); }
static inline InputEvent *input_shm_events(void *shm)     { return (InputEvent *)((char *)shm + INPUT_HEADER_SIZE); }

#endif /* PDK_INPUT_H */
