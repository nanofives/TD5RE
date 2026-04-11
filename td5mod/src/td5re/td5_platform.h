/**
 * td5_platform.h -- Platform abstraction layer for TD5RE
 *
 * Abstracts all OS/hardware dependencies: window management, input polling,
 * audio playback, file I/O, and timing. Game logic modules NEVER include
 * platform headers directly -- they go through this interface.
 *
 * Implementations:
 *   td5_platform_win32.c  -- Windows + D3D11 wrapper backend (primary)
 *   td5_platform_sdl.c    -- SDL2 backend (future cross-platform)
 */

#ifndef TD5_PLATFORM_H
#define TD5_PLATFORM_H

#include "td5_types.h"

#ifdef _WIN32
#include <windows.h>
#include <dinput.h>
#endif

/* ========================================================================
 * Window / Display
 * ======================================================================== */

typedef struct TD5_DisplayMode {
    int width;
    int height;
    int bpp;
    int fullscreen;
} TD5_DisplayMode;

/** Create the application window and initialize the graphics backend. */
int  td5_plat_window_create(const char *title, const TD5_DisplayMode *mode);

/** Destroy the window and release graphics resources. */
void td5_plat_window_destroy(void);

/** Process pending OS messages. Returns 0 if quit requested. */
int  td5_plat_pump_messages(void);

/** Present the back buffer (flip/swap). */
void td5_plat_present(int vsync);
void td5_plat_present_texture_page(int page_index, int vsync);

/** Get current window dimensions. */
void td5_plat_get_window_size(int *width, int *height);

/** Set fullscreen/windowed mode. */
void td5_plat_set_fullscreen(int fullscreen);

/** Get the native window handle (HWND on Win32, Window on X11, etc.).
 *  Returns NULL if no window has been created yet. */
void *td5_plat_get_native_window(void);

/* ========================================================================
 * Timing
 * ======================================================================== */

/** Get high-resolution timestamp in microseconds. */
uint64_t td5_plat_time_us(void);

/** Get millisecond timestamp (equivalent to timeGetTime). */
uint32_t td5_plat_time_ms(void);

/** Sleep for the specified number of milliseconds. */
void td5_plat_sleep(uint32_t ms);

/* ========================================================================
 * File I/O
 * ======================================================================== */

typedef struct TD5_File TD5_File;

/** Open a file. mode: "rb", "wb", etc. Returns NULL on failure. */
TD5_File *td5_plat_file_open(const char *path, const char *mode);

/** Close a file. */
void td5_plat_file_close(TD5_File *f);

/** Read bytes. Returns number of bytes actually read. */
size_t td5_plat_file_read(TD5_File *f, void *buf, size_t size);

/** Write bytes. Returns number of bytes actually written. */
size_t td5_plat_file_write(TD5_File *f, const void *buf, size_t size);

/** Seek. origin: 0=SET, 1=CUR, 2=END. */
int td5_plat_file_seek(TD5_File *f, int64_t offset, int origin);

/** Get current position. */
int64_t td5_plat_file_tell(TD5_File *f);

/** Get file size. Returns -1 on error. */
int64_t td5_plat_file_size(TD5_File *f);

/** Check if a file exists. */
int td5_plat_file_exists(const char *path);

/** Delete a file. Returns 0 on success. */
int td5_plat_file_delete(const char *path);

/* ========================================================================
 * Memory
 * ======================================================================== */

/** Allocate aligned memory (for render transform buffers, etc.) */
void *td5_plat_alloc_aligned(size_t size, size_t alignment);

/** Free aligned memory. */
void td5_plat_free_aligned(void *ptr);

/** Allocate from game heap. */
void *td5_plat_heap_alloc(size_t size);

/** Free game heap memory. */
void td5_plat_heap_free(void *ptr);

/** Reset the game heap (destroy + recreate). */
void td5_plat_heap_reset(void);

/* ========================================================================
 * Input
 * ======================================================================== */

/** Input state for one player (polled per frame). */
typedef struct TD5_InputState {
    uint32_t buttons;           /* TD5_InputBits bitmask */
    int16_t  analog_x;          /* steering axis, -32768..+32767 */
    int16_t  analog_y;          /* throttle/brake axis */
    int16_t  mouse_dx;          /* mouse delta X */
    int16_t  mouse_dy;          /* mouse delta Y */
    uint32_t mouse_buttons;     /* mouse button bitmask */
} TD5_InputState;

/** Initialize the input subsystem. */
int  td5_plat_input_init(void);

/** Shutdown the input subsystem. */
void td5_plat_input_shutdown(void);

/** Poll input for a player slot. */
void td5_plat_input_poll(int slot, TD5_InputState *out);

/** Get raw keyboard state (256-byte scancode array). */
const uint8_t *td5_plat_input_get_keyboard(void);

/** Check if a specific key is pressed (scancode). */
int td5_plat_input_key_pressed(int scancode);

/** Enumerate available input devices. Returns count. */
int td5_plat_input_enumerate_devices(void);

/** Get device name by index. */
const char *td5_plat_input_device_name(int index);

/** Get device type by index. Returns 0=keyboard, 1=gamepad, 2=joystick/wheel. */
int td5_plat_input_device_type(int index);

/** Set the active device for a player slot. */
void td5_plat_input_set_device(int slot, int device_index);

/* ========================================================================
 * Force Feedback
 * ======================================================================== */

typedef struct TD5_FFState {
#ifdef _WIN32
    IDirectInputDevice8 *device;
    IDirectInputEffect  *effects[4];
    GUID                 effectGuid;
    DWORD                axes[2];
    LONG                 directions[2];
    BOOL                 is_wheel;
#else
    void    *device;
    void    *effects[4];
    uint8_t  effectGuid[16];
    uint32_t axes[2];
    int32_t  directions[2];
    int      is_wheel;
#endif
} TD5_FFState;

/** Initialize force feedback for a device. Returns 0 on failure. */
int  td5_plat_ff_init(int device_index);

/** Stop all effects and shutdown. */
void td5_plat_ff_shutdown(void);

/** Play an effect. magnitude: -10000..+10000 for constant slots. */
void td5_plat_ff_constant(int slot, int magnitude);

/** Stop one active effect slot. */
void td5_plat_ff_stop(int slot);

/* ========================================================================
 * Audio
 * ======================================================================== */

/** Audio channel handle */
typedef int TD5_AudioChannel;

#define TD5_AUDIO_INVALID_CHANNEL (-1)

/** Initialize the audio subsystem. Returns 0 on failure. */
int  td5_plat_audio_init(void);

/** Shutdown the audio subsystem. */
void td5_plat_audio_shutdown(void);

/** Load a WAV file into a buffer. Returns buffer index, -1 on failure. */
int td5_plat_audio_load_wav(const void *data, size_t size);

/** Free a loaded audio buffer. */
void td5_plat_audio_free(int buffer_index);

/** Play a buffer. Returns channel, or -1 on failure. */
TD5_AudioChannel td5_plat_audio_play(int buffer_index, int loop, int volume, int pan, int frequency);

/** Stop a playing channel. */
void td5_plat_audio_stop(TD5_AudioChannel ch);

/** Modify a playing channel's parameters. */
void td5_plat_audio_modify(TD5_AudioChannel ch, int volume, int pan, int frequency);

/** Check if a channel is still playing. */
int td5_plat_audio_is_playing(TD5_AudioChannel ch);

/** Set master volume (0-100). */
void td5_plat_audio_set_master_volume(int volume);

/** Start streaming a WAV file through a double-buffered DirectSound buffer. */
int td5_plat_audio_stream_play(const char *wav_path, int loop);

/** Stop the active streamed audio track. */
void td5_plat_audio_stream_stop(void);

/** Refill the streamed audio buffer as playback advances. */
void td5_plat_audio_stream_refresh(void);

/** Check whether streamed audio is currently playing. */
int td5_plat_audio_stream_is_playing(void);

/* ========================================================================
 * CD Audio
 * ======================================================================== */

/** Play a CD audio track. */
void td5_plat_cd_play(int track);

/** Stop CD audio. */
void td5_plat_cd_stop(void);

/** Set CD audio volume (0-100). */
void td5_plat_cd_set_volume(int volume);

/** Enumerate available fullscreen display modes. */
int td5_plat_enum_display_modes(TD5_DisplayMode *modes, int max_count);

/** Apply a fullscreen display mode immediately. */
int td5_plat_apply_display_mode(int width, int height, int bpp);

/* ========================================================================
 * Rendering Backend
 *
 * These are the low-level calls that the render module uses to submit
 * geometry to the GPU. On the Win32 backend, these map to the D3D11
 * wrapper's IDirect3DDevice3 vtable calls.
 * ======================================================================== */

/** D3D vertex for pre-transformed geometry (32 bytes, FVF 0x1C4)
 *
 * Layout must match the D3D11 input layout in d3d11_backend.c:
 *   POSITION  R32G32B32A32_FLOAT  offset  0  (x, y, z, rhw)
 *   COLOR[0]  B8G8R8A8_UNORM     offset 16  (diffuse)
 *   COLOR[1]  B8G8R8A8_UNORM     offset 20  (specular — unused by pixel shaders)
 *   TEXCOORD  R32G32_FLOAT       offset 24  (u, v)
 */
typedef struct TD5_D3DVertex {
    float    screen_x, screen_y;
    float    depth_z, rhw;
    uint32_t diffuse;
    uint32_t specular;   /* COLOR[1]; always 0 — required for correct UV offset */
    float    tex_u, tex_v;
} TD5_D3DVertex;

/** Begin a render scene. */
void td5_plat_render_begin_scene(void);

/** End a render scene. */
void td5_plat_render_end_scene(void);

/** Draw pre-transformed triangles. */
void td5_plat_render_draw_tris(const TD5_D3DVertex *verts, int vertex_count,
                                const uint16_t *indices, int index_count);

/** Set render state preset (0-3). */
void td5_plat_render_set_preset(TD5_RenderPreset preset);

/** Bind a texture page by index. */
void td5_plat_render_bind_texture(int page_index);

/** Configure fog parameters. */
void td5_plat_render_set_fog(int enable, uint32_t color, float start, float end, float density);

/** Upload a texture page to the GPU. Returns 0 on failure. */
int td5_plat_render_upload_texture(int page_index, const void *pixels, int width, int height, int format);

/** Query the dimensions of an uploaded texture page.
 *  w and h are only written if the page has valid dimensions;
 *  caller should initialize them to a sensible fallback before calling. */
void td5_plat_render_get_texture_dims(int page_index, int *w, int *h);

/** Set viewport rectangle. */
void td5_plat_render_set_viewport(int x, int y, int width, int height);

/** Set the active rasterizer scissor rect (pixels in the current render
 *  target). Passing a rect covering the full backbuffer effectively
 *  disables clipping. Used by the HUD/minimap render path to trim
 *  translucent 2D draws to a sub-rect. */
void td5_plat_render_set_clip_rect(int left, int top, int right, int bottom);

/** Clear the back buffer. */
void td5_plat_render_clear(uint32_t color);

/* ========================================================================
 * Logging
 * ======================================================================== */

typedef enum TD5_LogLevel {
    TD5_LOG_DEBUG = 0,
    TD5_LOG_INFO  = 1,
    TD5_LOG_WARN  = 2,
    TD5_LOG_ERROR = 3
} TD5_LogLevel;

/** Log a message.  Module tag determines which log file receives it:
 *  - frontend.log : "frontend", "hud", "save", "input"
 *  - race.log     : "td5_game", "physics", "ai", "track", "camera", "vfx"
 *  - engine.log   : everything else ("render", "asset", "platform", "sound",
 *                   "net", "fmv", "main", unknown tags)
 */
void td5_plat_log(TD5_LogLevel level, const char *module, const char *fmt, ...);

/** Flush all open log files to disk. */
void td5_plat_log_flush(void);

/** Initialize the logging subsystem.  Creates log/ directory, rotates old
 *  sessions (keeps up to 5 previous), and opens fresh log files.
 *  Called from main before any other logging.  Safe to call multiple times. */
void td5_plat_log_init(void);

/** Return the log directory path (e.g. "C:/.../log/").  Only valid after
 *  td5_plat_log_init() has been called.  Useful for redirecting other
 *  subsystem logs into the same folder. */
const char *td5_plat_log_dir(void);

#define TD5_LOG_D(mod, ...) td5_plat_log(TD5_LOG_DEBUG, mod, __VA_ARGS__)
#define TD5_LOG_I(mod, ...) td5_plat_log(TD5_LOG_INFO,  mod, __VA_ARGS__)
#define TD5_LOG_W(mod, ...) td5_plat_log(TD5_LOG_WARN,  mod, __VA_ARGS__)
#define TD5_LOG_E(mod, ...) td5_plat_log(TD5_LOG_ERROR, mod, __VA_ARGS__)

#endif /* TD5_PLATFORM_H */
