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

/** True when the game window holds OS foreground (keyboard) focus. Used to gate
 *  the cinematic-race (attract demo / view replay) ESC-to-abort so a background
 *  window's stale key state can't cut a self-running demo short. */
int  td5_plat_window_is_focused(void);

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

/** Rename/move a file (overwrites the destination). Returns 0 on success. */
int td5_plat_file_rename(const char *from, const char *to);

/* ------------------------------------------------------------------------
 * Human-readable INI config (used by td5_save.c after Config.td5/CupData.td5
 * were retired in favour of organized INI files: td5re_input.ini,
 * td5re_progress.ini, td5re_cup.ini). Thin wrappers over the Win32
 * GetPrivateProfile* / WritePrivateProfile* APIs so the rest of the port
 * stays free of <windows.h>.
 * ------------------------------------------------------------------------ */

/** Resolve a bare filename to an absolute path next to the running exe
 *  (same directory td5re.ini lives in). Falls back to the bare name. */
void td5_plat_ini_resolve_path(const char *filename, char *out, size_t out_n);

/** Read an integer key. Returns fallback if the key is absent. */
int  td5_plat_ini_get_int(const char *file, const char *section,
                          const char *key, int fallback);

/** Read a string key into out (zero-terminated). Returns the length read. */
int  td5_plat_ini_get_str(const char *file, const char *section,
                          const char *key, const char *fallback,
                          char *out, size_t out_n);

/** Write an integer key (creates the file/section/key as needed). */
void td5_plat_ini_set_int(const char *file, const char *section,
                          const char *key, int value);

/** Write a string key (creates the file/section/key as needed). */
void td5_plat_ini_set_str(const char *file, const char *section,
                          const char *key, const char *value);

/** Flush the Win32 private-profile cache for a file. Call after rewriting an
 *  INI through raw file I/O so a subsequent td5_plat_ini_get_* re-reads from
 *  disk instead of returning a stale cached value. */
void td5_plat_ini_flush(const char *file);

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

/** Apply rebound keyboard scancodes (control-config screen / Config.td5) to the
 *  live binding table the in-race poll reads. `scancodes` is the canonical
 *  10-action array (0=LEFT,1=RIGHT,2=ACCELERATE,3=BRAKE,4=HANDBRAKE,5=HORN,
 *  6=GEAR UP,7=GEAR DOWN,8=CHANGE VIEW,9=REAR VIEW). A 0 byte keeps the default. */
void td5_plat_input_set_keyboard_bindings(int player, const uint8_t *scancodes, int count);

/** Get raw keyboard state (256-byte scancode array). */
const uint8_t *td5_plat_input_get_keyboard(void);

/** Check if a specific key is pressed (scancode). */
int td5_plat_input_key_pressed(int scancode);

/** Scripted-key injection (td5_inputscript.c dev harness + StartScreen nav
 *  walker). Injected scancodes merge into the keyboard snapshot after every
 *  hardware fill (works with the window unfocused); arrow/Enter presses also
 *  enqueue a WM_KEYDOWN nav event and ESC sets the one-shot ESC latch, so
 *  frontend navigation reacts exactly as to a physical press. Inert while
 *  nothing is injected. */
void td5_plat_input_inject_key(int scancode, int down);
void td5_plat_input_inject_clear(void);

/** Accumulated mouse-wheel delta since the last call (cleared on read). One notch
 *  is ±120 (WHEEL_DELTA); positive = wheel-up/away from the user. */
int td5_plat_input_get_mouse_wheel(void);

/** Pop the next queued typed character (WM_CHAR); 0 if none. Frame-rate
 *  independent so text input is never dropped by slow frames / poll contention. */
int  td5_plat_input_get_char(void);
/** Discard pending typed characters (call when opening a text field). */
void td5_plat_input_flush_chars(void);

/** WM_KEYDOWN navigation-key event codes (one per genuine key press). Each is a
 *  single bit so the same value can also be OR'd as a mask where convenient. */
#define TD5_NAVKEY_LEFT  0x01u
#define TD5_NAVKEY_RIGHT 0x02u
#define TD5_NAVKEY_UP    0x04u
#define TD5_NAVKEY_DOWN  0x08u
#define TD5_NAVKEY_ENTER 0x10u

/** Pop the next queued menu-navigation event (WM_KEYDOWN FIFO). Returns a single
 *  TD5_NAVKEY_* code, or 0 when the queue is empty. Unlike a one-bit latch the
 *  queue preserves EVERY press and its order, so a burst of taps during a
 *  frame-time spike is drained press-for-press instead of being collapsed or
 *  dropped — the frame-rate-independent analogue of td5_plat_input_get_char() for
 *  menu nav. Auto-repeat is filtered in the window proc. */
unsigned td5_plat_input_nav_pop(void);

/** Discard all pending menu-nav events and the ESC latch. Call on leaving the
 *  menu, or while a text field is open, so stale presses don't fire next frame. */
void td5_plat_input_flush_nav(void);

/** One-shot: returns 1 (and clears) if an ESC press was captured (WM_KEYDOWN)
 *  since the last call. Lets a quick ESC tap the once-per-frame immediate read
 *  missed still back out — applied at most once per frame so a buffered burst
 *  can't pop several screens at once. */
int  td5_plat_input_esc_taken(void);

/** Enumerate available input devices. Returns count. */
int td5_plat_input_enumerate_devices(void);

/** Get device name by index. */
const char *td5_plat_input_device_name(int index);

/** Get device type by index. Returns 0=keyboard, 1=gamepad, 2=joystick/wheel. */
int td5_plat_input_device_type(int index);

/** Set the active device for a player slot. device_index 0 = keyboard,
 *  >=1 = enumerated joystick index (creates/acquires that joystick for the slot). */
void td5_plat_input_set_device(int slot, int device_index);

/** Apply the per-player 9-slot joystick binding table (control-config /
 *  Config.td5) to the live poll. Layout: [0]=active, [1]/[2]=axes (4/5),
 *  [3..8]=6 button actions (value 2..10 → physical button = value-2). */
void td5_plat_input_set_joystick_bindings(int slot, const int32_t *bindings, int count);

/** Raw physical joystick button bitmask for a device slot (bit i = physical
 *  button i currently pressed). Returns 0 if no device is bound to that slot.
 *  Used by the control-config per-button capture. [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_joystick_buttons(int device_slot);

/** [SPLIT-SCREEN DEVICE BLEED 2026-06-30] Physical-device key (HID instance
 *  path, collection token stripped) for an ENUMERATED joystick index. Two DI
 *  interfaces of the same physical pad share this key; two distinct pads (even
 *  identical models) do not. Returns 1 + fills `out` on success, 0 on failure
 *  (caller then treats the device as distinct — the de-dup safely no-ops). */
int td5_plat_input_device_phys_key(int enum_index, char *out, int outsz);

/** Same physical-device key but for the device BOUND to a player input slot
 *  (in-race), for diagnostics. Returns 0 for a keyboard / unbound slot. */
int td5_plat_input_slot_phys_key(int slot, char *out, int outsz);

/** [S27 2026-06-05] 1 if the joystick bound to this player slot has been
 *  persistently lost (physically disconnected); 0 for a keyboard slot or a
 *  live device. Drives the in-race controller-disconnect pause + reconnect
 *  modal. Source-port feature (no original DirectInput hot-plug handling). */
int td5_plat_input_joystick_is_lost(int device_slot);

/** [disconnect-modal 2026-06-21] 1 if the ENUMERATED device at this 1-based
 *  index (s_mp_join_device value) is persistently lost. Unlike
 *  td5_plat_input_joystick_is_lost (which reads the in-race exclusive per-player
 *  slots), this ACTIVELY polls the shared frontend scan handle each call and
 *  latches loss, so it works during the MP setup flow (lobby/picker/car-select)
 *  AND detects reconnection while the setup is frozen. 0 for the keyboard
 *  (index 0) or an out-of-range index. Source-port feature. */
int td5_plat_input_device_is_lost(int enum_index);

/* ------------------------------------------------------------------------
 * Per-action joystick binding (PORT ENHANCEMENT 2026-06)
 *
 * Each of the 10 driving actions can be mapped to a physical button OR an
 * axis/trigger direction. A binding is a uint32 "code":
 *   0                              = unbound
 *   TD5_JSBIND_BUTTON | btn        = physical button btn (0..63)
 *   TD5_JSBIND_AXIS | (axis<<1)|d  = axis (0..7), direction d (0=+, 1=-)
 * Action order matches k_ctrl_action_labels:
 *   0 LEFT 1 RIGHT 2 ACCELERATE 3 BRAKE 4 HANDBRAKE 5 HORN/SIREN
 *   6 GEAR UP 7 GEAR DOWN 8 CHANGE VIEW 9 REAR VIEW 10 PAUSE
 * ------------------------------------------------------------------------ */
#define TD5_JSBIND_NONE      0u
#define TD5_JSBIND_BUTTON    0x100u
#define TD5_JSBIND_AXIS      0x200u
#define TD5_JSBIND_ACTIONS   11

/** Apply the per-action binding codes for a player slot (count<=10). When set,
 *  the in-race poll maps each action through these instead of the fixed default. */
void td5_plat_input_set_action_bindings(int slot, const uint32_t *codes, int count);

/** The built-in DEFAULT per-action binding table (TD5_JSBIND_ACTIONS entries),
 *  the standard Xbox-controller-via-DirectInput map applied to any joystick with
 *  no explicit config. Read-only; lets the Control-Options UI seed/show the
 *  active defaults so per-action remaps keep the rest of the map. */
const uint32_t *td5_plat_input_default_action_bindings(void);

/** The LIVE per-action binding table actually used by the in-race poll for a
 *  player slot: the Control-Options per-action remap when one is set, otherwise
 *  the built-in default table. TD5_JSBIND_ACTIONS entries, read-only. Lets the
 *  tutorial overlay label each control with what it is currently bound to.
 *  [PORT ENHANCEMENT 2026-06] */
const uint32_t *td5_plat_input_player_action_bindings(int slot);

/** Snapshot the device's rest state for capture (call when a remap begins). */
void td5_plat_input_joystick_capture_begin(int device_slot);

/** Poll for a freshly-pressed button OR an axis/trigger moved past threshold
 *  (vs the capture-begin baseline). Returns 1 and writes the binding code to
 *  *out_code on a fresh input, else 0. */
int  td5_plat_input_joystick_capture_poll(int device_slot, uint32_t *out_code);

/** Describe a binding code into buf (e.g. "BTN 3", "AXIS 2+", "-"). */
void td5_plat_input_describe_binding(uint32_t code, char *buf, int cap);

/** 1 once the joystick has settled at rest (no buttons/dpad, sticks centred, all
 *  axes incl. triggers motionless for several frames) — gates per-action capture
 *  so it waits for the previous input's release travel to finish before listening. */
int  td5_plat_input_joystick_neutral(int device_slot);
/** Reset the neutral-detector's stability timer (call when entering a wait). */
void td5_plat_input_joystick_neutral_reset(void);
/** Learn the device's current axis positions as its REST reference. Call every
 *  frame while the binding UI is idle so a wait can tell held vs released. */
void td5_plat_input_joystick_learn_rest(int device_slot);

/** Re-enumerate devices; returns 1 if the count changed (hot-plug). */
int  td5_plat_input_rescan_devices(void);

/** Returns 1 (and clears) if a USB device hot-plug (WM_DEVICECHANGE) occurred
 *  since the last call; lets callers gate the costly rescan on actual changes. */
int  td5_plat_input_devices_changed(void);

/** Frontend navigation bitmask from a connected gamepad (any joystick):
 *  bit0 LEFT, bit1 RIGHT, bit2 UP, bit3 DOWN, bit4 A/confirm, bit5 B/back.
 *  Returns 0 if no joystick is present. [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_frontend_nav(void);

/** In-race navigation bitmask from a player's EXCLUSIVE joystick device (same
 *  encoding as td5_plat_input_frontend_nav) — for the pause menu, which can't use
 *  the released frontend scan handles while a race owns the device. */
uint32_t td5_plat_input_joystick_nav(int device_slot);

/** Navigation bitmask for ONE enumerated device, polled through the shared
 *  non-exclusive lobby/frontend scan handle (the same enum index used by
 *  td5_plat_input_scan_join: 1..count = joysticks). Same bit encoding as
 *  td5_plat_input_frontend_nav. Lets several controllers be read independently
 *  during simultaneous-multiplayer car select, BEFORE per-player exclusive
 *  devices are bound. Device 0 (keyboard) returns 0 — read the keyboard
 *  directly. [PORT ENHANCEMENT 2026-06-07] */
uint32_t td5_plat_input_device_nav(int enum_index);

/** Enumerated index of the joystick that last produced a confirm/nav (the
 *  "active controller" that drives the menus). -1 if none yet. */
int      td5_plat_input_active_joystick(void);

/** Multiplayer-lobby join scan: bit i set if device i's join control is held
 *  (device 0 = keyboard Enter; device i>=1 = joystick i button 0). */
uint32_t td5_plat_input_scan_join(void);
/** Release the lobby join-scan handles (call on leaving the lobby). */
void     td5_plat_input_scan_join_release(void);

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

/** Initialize force feedback for one player's joystick device. device_slot is
 *  the per-player joystick slot (0..TD5_PLAT_MAX_JS_SLOTS-1, == player index).
 *  Returns 0 on failure (no device, or device has no FF). Idempotent per slot.
 *  [PORT ENHANCEMENT 2026-06] N-way: each human player's device gets its own
 *  effect set so vibration is delivered per-player, not just on slot 0.
 *  [#1 2026-06-15] If the bound device is an XInput pad (8BitDo & most modern
 *  gamepads — these expose rumble via XInput, NOT DI force-feedback effects),
 *  this routes the slot's FF through XInput rumble and returns success even when
 *  the device reports no DirectInput FF capability. Knob TD5RE_FF_XINPUT
 *  (default ON; "0" = DI effects only). DI-effect FF is kept for wheels. */
int  td5_plat_ff_init(int device_slot);

/** Stop all effects on all devices and shutdown. */
void td5_plat_ff_shutdown(void);

/** Play an effect on one device. effect_slot 0..3, magnitude -10000..+10000.
 *  [#1 2026-06-15] On an XInput-routed device (gamepad) the effect slot maps to
 *  a motor ONLY for the event-driven effects, so a gamepad never buzzes during
 *  normal driving:
 *    slot 1 (crash + gear jolts) -> high-freq (light) motor;
 *    slot 2 (side-collision)     -> low-freq (heavy) motor;
 *    slots 0 (steering constant) and 3 (terrain periodic) are NOT routed to any
 *      motor (they are continuous wheel forces; on a pad they were the reported
 *      "rumbles all the time"). For the redline-in-manual buzz use
 *      td5_plat_ff_xinput_rumble below. DI force-feedback wheels are unaffected:
 *      they still receive all four slots as DI effects.
 *  The input layer (td5_input.c) calls this unchanged; the routing is internal. */
void td5_plat_ff_constant(int device_slot, int effect_slot, int magnitude);

/** [#1 2026-06-15] Drive a GAMEPAD's low-freq (heavy) motor with a dedicated
 *  continuous "rumble" contributor (magnitude 0..10000), used for the MAX-RPM-
 *  in-manual buzz. Kept separate from the DI effect slots so the gamepad motor
 *  picks up ONLY this (and the slot 1/2 event pulses), never terrain/steering/
 *  drift. No-op on a DirectInput FF wheel — there the redline buzz already rides
 *  on effect slot 3, so the wheel path is unchanged. magnitude 0 silences it. */
void td5_plat_ff_xinput_rumble(int device_slot, int magnitude);

/** Stop one active effect slot on one device. (On an XInput-routed device this
 *  clears that effect's motor contribution.) */
void td5_plat_ff_stop(int device_slot, int effect_slot);

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

/** Check if a channel handle still names a live voice this slot owns, ignoring
 *  the transient PLAYING bit (a parked/idle loop still counts). Used to avoid
 *  re-triggering a loop the mixer is still modulating. */
int td5_plat_audio_channel_valid(TD5_AudioChannel ch);

/** Set master volume (0-100). */
void td5_plat_audio_set_master_volume(int volume);

/** Force all SFX play/modify to silence (1) or restore (0). Used to mute race
 *  audio while the in-race pause menu is up. Does not affect CD/music. */
void td5_plat_audio_set_muted(int muted);

/** Mute/unmute ALL DirectSound output (per-voice SFX + the ambient stream)
 *  based on whether the game window is the foreground window. Call once per
 *  frame from the main loop. Independent of (ORs with) td5_plat_audio_set_muted
 *  so the focus mute and the pause-menu mute never clobber each other. */
void td5_plat_audio_update_focus_mute(void);

/** Stop and release every active SFX voice (not the primary/keepalive buffers).
 *  Used at race teardown so voice counts return to baseline between races and no
 *  looping voice survives into the menus or the next race. */
void td5_plat_audio_stop_all(void);

/** Number of currently-allocated SFX voices (DirectSound duplicate buffers).
 *  Used by the sound layer to confirm there is no monotonic voice leak. */
int td5_plat_audio_active_channels(void);

/** Log the voice allocation/release/steal/fail counters under `tag`. */
void td5_plat_audio_log_stats(const char *tag);

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

/* ========================================================================
 * Radio PCM streaming sink (push-based)
 *
 * Backs the internet-radio music backend (td5_radio.c). A worker thread decodes
 * a network stream and PUSHES PCM via td5_plat_radio_submit(); the per-frame
 * pump (main thread) feeds a looping DirectSound buffer from a thread-safe ring
 * buffer. All DirectSound calls stay on the main thread; the worker only
 * touches the ring (under a critical section). Format is published once by the
 * worker via td5_plat_radio_begin(); the DSound buffer is created lazily in the
 * pump. Safe no-ops if DirectSound is unavailable.
 * ======================================================================== */

/** Open the sink (main thread): init the lock + zero state. */
void td5_plat_radio_open(void);

/** Publish the decoded stream format (worker thread). Allocates the ring buffer
 *  and resets it. Returns 1 on success, 0 if params are invalid. */
int  td5_plat_radio_begin(int sample_rate, int channels, int bits_per_sample);

/** Enqueue decoded PCM (worker thread). Returns the number of bytes accepted
 *  (may be < count when the ring is full -- caller should retry the remainder). */
int  td5_plat_radio_submit(const void *pcm, int count);

/** Per-frame pump (main thread): create the DSound buffer once the format is
 *  known, then refill the half just played from the ring (silence on underrun). */
void td5_plat_radio_pump(void);

/** Race-level play (1) / stop (0) (main thread). One of three gates; the radio
 *  is audible only when playing AND in-race AND focused. */
void td5_plat_radio_set_playing(int on);

/** In-race gate (main thread): set per-frame from the game state so the radio
 *  is silent in menus / results / benchmark. */
void td5_plat_radio_set_active(int active);

/** Focus gate (main thread): mute while the window is minimized / not the
 *  foreground window. Driven from td5_plat_audio_update_focus_mute(). */
void td5_plat_radio_set_focus_muted(int muted);

/** True while the radio output is actually on (worker uses this to decide
 *  whether to enqueue PCM or discard the live stream). */
int  td5_plat_radio_wants_data(void);

/** Set radio output volume 0-100 (main thread). */
void td5_plat_radio_set_volume(int volume);

/** Bytes currently buffered in the ring (worker throttle helper). */
int  td5_plat_radio_pending_bytes(void);

/** Close the sink (main thread): stop + release the buffer, free the ring. */
void td5_plat_radio_close(void);

/** Enumerate available fullscreen display modes. */
int td5_plat_enum_display_modes(TD5_DisplayMode *modes, int max_count);

/** Apply a fullscreen display mode immediately. */
int td5_plat_apply_display_mode(int width, int height, int bpp);

/** [S01 2026-06-04] 3-way window mode: 0=fullscreen exclusive, 1=windowed,
 *  2=borderless. Owns the HWND style + swap-chain fullscreen state. */
int  td5_plat_set_window_mode(int mode);
int  td5_plat_get_window_mode(void);

/** [S01 2026-06-04] Toggle the present sync interval (VSync). on!=0 = wait for
 *  vblank (no tearing), 0 = uncapped/tearing. */
void td5_plat_set_vsync(int on);

/** [S01 2026-06-04] Current windowed/fullscreen resolution (tracks drag-resizes,
 *  excludes the borderless desktop override). Persisted as [Display] Width/Height. */
void td5_plat_get_chosen_resolution(int *w, int *h);

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

/** Override the pixel shader + sampler used by the indexed draw path until
 *  cleared. Used by the resolution-independent frontend (MSDF text). `ps` is an
 *  ID3D11PixelShader* (void* to keep this header D3D11-free); `sampler_idx` is
 *  a SAMP_* index. Set around a draw batch, then call clear. */
void td5_plat_render_set_ps_override(void *ps, int sampler_idx);
void td5_plat_render_clear_ps_override(void);

/** Procedural, texture-free particle/VFX pixel shaders. These replace the
 *  SMOKE / RAINDROP / FADEWHT / BRAKED / POLICELT_* atlas sprites with analytic
 *  shaders (animated noise smoke, gradient rain, feathered skid decals, radial
 *  glows) so no PNG is needed for particles. */
typedef enum TD5_FxShader {
    TD5_FX_SMOKE = 0,  /* animated value-noise smoke/dust puff */
    TD5_FX_RAIN  = 1,  /* gradient rain streak (screen-space) */
    TD5_FX_DECAL = 2,  /* feathered + grained tire/skid road decal */
    TD5_FX_GLOW  = 3   /* radial gaussian glow (taillight lamp / cop strobe) */
} TD5_FxShader;

/** Bind the procedural FX pixel shader `which` and upload its b1 params
 *  (global animation time + one shader-specific param `p0`). Returns 1 if the
 *  shader is ready — the caller then sets a render preset and issues ordinary
 *  td5_plat_render_draw_tris batches whose vertex COLOR0/COLOR1 carry the
 *  per-particle data. Returns 0 if the shader is unavailable (caller keeps its
 *  textured fallback). Pair every successful begin with td5_plat_fx_end(). */
int  td5_plat_fx_begin(TD5_FxShader which, float time_seconds, float p0);
void td5_plat_fx_end(void);

/** Soft-particle depth binding for smoke. Call td5_plat_fx_soft_begin() before
 *  the smoke td5_plat_fx_begin/draw and td5_plat_fx_soft_end() after; it binds
 *  the scene depth as a shader resource so the smoke shader can fade as it nears
 *  geometry (no hard intersection seam). Returns 1 if available — pass that as
 *  the smoke shader's soft flag (the `p0` of td5_plat_fx_begin). 0 = unavailable
 *  (draw normally, no soft fade). */
int  td5_plat_fx_soft_begin(void);
void td5_plat_fx_soft_end(void);

/** Draw pre-transformed lines (LINELIST, no texture, vertex color only).
 *  vert_count must be even — each consecutive pair forms one line segment.
 *  Used by the debug-collision overlay; depth test on, depth write off,
 *  no blending, no fog. Bypasses the regular state cache; the next call to
 *  td5_plat_render_draw_tris re-binds standard state. */
void td5_plat_render_draw_lines(const TD5_D3DVertex *verts, int vert_count);

/** Draw pre-transformed, FLAT (textureless) triangles: a white texture is
 *  modulated so the per-vertex diffuse becomes the pixel color directly; fog
 *  and alpha-test are off; opaque blend; depth test on, depth write off.
 *  Mirrors td5_plat_render_draw_lines but TRIANGLELIST + indexed. Used by the
 *  custom-track STRIP-ribbon render fallback (a level with no MODELS.DAT mesh).
 *  Bypasses the regular state cache; the next td5_plat_render_draw_tris
 *  re-binds standard state. */
void td5_plat_render_draw_tris_flat(const TD5_D3DVertex *verts, int vert_count,
                                    const uint16_t *indices, int index_count);

/** Set render state preset (0-3). */
void td5_plat_render_set_preset(TD5_RenderPreset preset);

/** Bind a texture page by index. */
void td5_plat_render_bind_texture(int page_index);

/** Configure fog parameters. */
void td5_plat_render_set_fog(int enable, uint32_t color, float start, float end, float density);

/* [DYNAMIC LIGHTS] Deferred screen-space light. One entry per dynamic light. */
typedef struct TD5_LightGPU {
    float x, y, z, range;      /* world position (float world units) + range     */
    float r, g, b, intensity;  /* colour (0..1) + peak intensity (0..1)           */
    float dx, dy, dz, cone;    /* beam dir (unit) + cos(outer half-angle);
                                * cone <= -1 => omni point light (no cone)         */
} TD5_LightGPU;

/* [DYNAMIC LIGHTS] Run the deferred light pass over the CURRENT viewport: the
 * backend samples scene depth, reconstructs each pixel's world position from the
 * camera params, and adds every light additively onto the scene. Call once per
 * viewport AFTER the opaque world geometry, BEFORE translucent VFX/HUD.
 *   cam_pos  : camera world position (3 floats)
 *   basis9   : camera basis, row-major {right[3], up[3], forward[3]}
 *   focal    : focal length; center_x/y : viewport projection centre (pane-relative)
 *   vp_x/vp_y: viewport origin in render-target pixels (for pane-relative recon)
 *   depth_scale/depth_bias : depth_z = (view_z - bias)/scale encoding
 * No-op if lighting is unsupported (shader/cbuffer/depth-SRV missing) or count<=0. */
void td5_plat_render_apply_lights(const float cam_pos[3], const float basis9[9],
                                  float focal, float center_x, float center_y,
                                  float vp_x, float vp_y,
                                  float depth_scale, float depth_bias,
                                  const TD5_LightGPU *lights, int light_count,
                                  int occl_steps, float pane_w, float pane_h);

/* [LIGHTING REWORK P2] Screen-space ray-marched sun-shadow pass over the
 * CURRENT viewport: darkens pixels whose path toward `sun_dir` is blocked by
 * on-screen geometry (depth-buffer march). Call AFTER the opaque world,
 * BEFORE td5_plat_render_apply_lights (additive lights must not be darkened).
 *   sun_dir  : surface->light direction, world space, unit length
 *   strength : 0..1 max darkening in full shadow
 *   steps/max_dist/thickness/start_off : march tuning (world / view-z units)
 *   pane_w/pane_h : viewport size in pixels (march clamps to the pane) */
void td5_plat_render_apply_shadow(const float cam_pos[3], const float basis9[9],
                                  float focal, float center_x, float center_y,
                                  float vp_x, float vp_y,
                                  float depth_scale, float depth_bias,
                                  const float sun_dir[3], float strength,
                                  int steps, float max_dist, float thickness,
                                  float start_off, float pane_w, float pane_h);

/* [LIGHTING REWORK P3] Screen-space reflections over the CURRENT viewport:
 * reflective materials (per-id reflectivity in refl8[0..7], + wet_boost on
 * up-facing DEFAULT-material pixels) mirror the already lit + shadowed scene
 * via depth-buffer ray marching. Call AFTER td5_plat_render_apply_lights,
 * BEFORE the translucent VFX/HUD.
 *   refl8     : base reflectivity per material id 0..7 (0..1)
 *   wet_boost : extra reflectivity for up-facing roads (rain), 0 = dry
 *   intensity : master reflection intensity 0..1 */
void td5_plat_render_apply_ssr(const float cam_pos[3], const float basis9[9],
                               float focal, float center_x, float center_y,
                               float vp_x, float vp_y,
                               float depth_scale, float depth_bias,
                               const float refl8[8], float wet_boost,
                               float intensity, int steps, float max_dist,
                               float thickness, float pane_w, float pane_h);

/* [LIGHTING REWORK P0] Per-frame G-buffer gate. on=1: the backend clears the
 * normal+material G-buffer and writes it from z-writing opaque draws (COLOR1
 * carries world normal + material id); the deferred light pass then applies
 * proper N.L. on=0: G-buffer off (classic behavior). Call once per rendered
 * race frame BEFORE the world pass. */
void td5_plat_render_set_gbuffer(int on);

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

/* [Phase B Stage 2b] Threaded pane recording (deferred contexts). pool_ensure
 * lazily creates `count` deferred-context bundles (1=ready, 0=unavailable ->
 * caller falls back to serial). pane_begin returns an opaque handle and routes
 * THIS thread's draws into bundle `index`; pane_end finishes its command list;
 * pane_execute replays bundle `index` onto the immediate context (call in pane
 * order on the main thread); restore_main_rt re-binds the swap RTV afterward. */
int   td5_plat_render_pane_pool_ensure(int count);
void *td5_plat_render_pane_begin(int index, int x, int y, int w, int h);
void  td5_plat_render_pane_end(void *rc);
void  td5_plat_render_pane_execute(int index);
void  td5_plat_render_restore_main_rt(void);

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

/** Configure runtime log filters. Called by main.c after INI is parsed.
 *  When `enabled` is 0 the logger drops every message before any formatting
 *  cost. `min_level` filters TD5_LOG_* by severity (default INFO). `cat_mask`
 *  is a bitmask over (1<<TD5_LOG_CAT_*) — bit 0 = frontend, bit 1 = race,
 *  bit 2 = engine. Defaults preserve current behavior (all categories on,
 *  min level INFO). */
#define TD5_LOG_CAT_BIT_FRONTEND (1u << 0)
#define TD5_LOG_CAT_BIT_RACE     (1u << 1)
#define TD5_LOG_CAT_BIT_ENGINE   (1u << 2)
void td5_plat_log_set_filters(int enabled, int min_level, unsigned int cat_mask);

#define TD5_LOG_D(mod, ...) td5_plat_log(TD5_LOG_DEBUG, mod, __VA_ARGS__)
#define TD5_LOG_I(mod, ...) td5_plat_log(TD5_LOG_INFO,  mod, __VA_ARGS__)
#define TD5_LOG_W(mod, ...) td5_plat_log(TD5_LOG_WARN,  mod, __VA_ARGS__)
#define TD5_LOG_E(mod, ...) td5_plat_log(TD5_LOG_ERROR, mod, __VA_ARGS__)

/** Running totals of WARN / ERROR log calls this session (counted at the call,
 *  independent of sink filters). The self-test suite diffs these around each
 *  step to assert "no new errors during scenario X". Either out-param may be
 *  NULL. */
void td5_plat_log_counts(unsigned *warns, unsigned *errors);

/** Game-heap traffic counters (td5_plat_heap_alloc / td5_plat_heap_free call
 *  totals) — an alloc/free imbalance across identical repeated races is a
 *  leak signal for the self-test suite. Either out-param may be NULL. */
void td5_plat_heap_stats(unsigned *allocs, unsigned *frees);

/** Set the OS window caption (self-test progress display; same window the
 *  TD5RE_WINDOW_TITLE env override names at boot). Safe no-op pre-window. */
void td5_plat_set_window_title(const char *title);

#endif /* TD5_PLATFORM_H */
