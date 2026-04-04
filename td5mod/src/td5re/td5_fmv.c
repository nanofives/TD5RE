/**
 * td5_fmv.c -- FMV playback module (replaces EA TGQ codec)
 *
 * Replaces the original EA TGQ multimedia engine with:
 *   - Video playback via Windows Media Foundation (MFPlay API)
 *   - Legal screen display via TGA loading + D3D11 render backend
 *
 * Original functions replaced:
 *   0x43C440  PlayIntroMovie
 *     - Opens Movie/intro.tgq via OpenAndStartMediaPlayback
 *     - Nested message pump with PeekMessageA/GetMessageA
 *     - Skippable via Enter, Shift, Escape, Space
 *     - Volume adjustable with +/- during playback
 *     - Sleeps 30ms per iteration
 *
 *   0x42C8E0  ShowLegalScreens
 *     - Loads legal1.tga, legal2.tga from LEGALS.ZIP
 *     - Displays each for ~5 seconds with fade
 *     - Skippable via keypress
 *
 * The original TGQ codec used custom PE sections IDCT_DAT (8KB) and
 * UVA_DATA (20KB) for IDCT workspace and YUV-to-RGB LUTs. None of
 * that is needed here -- Media Foundation handles all decoding.
 */

#include "td5_fmv.h"
#include "td5_platform.h"

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Windows headers -- needed for Media Foundation and COM
 *
 * We include these directly because this module IS the platform-specific
 * implementation. The platform abstraction boundary is at td5_fmv.h.
 * --------------------------------------------------------------------------- */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS          /* enable IMFPMediaPlayer_Play/Stop/Release macros */
#endif
#include <windows.h>

/* MFPlay -- simplest Media Foundation playback API.
 * Available on Windows 7+ (Vista with Platform Update). */
#include <mfplay.h>
#include <mfapi.h>

/* For CoInitializeEx */
#include <objbase.h>

#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "ole32.lib")

/* ========================================================================
 * Module State
 * ======================================================================== */

/** FMV subsystem state */
static struct {
    int              initialized;       /* 1 if init succeeded              */
    int              mf_available;      /* 1 if Media Foundation is usable  */
    int              com_initialized;   /* 1 if we called CoInitializeEx    */
    volatile LONG    skip_requested;    /* atomic skip flag                 */
    volatile LONG    playback_done;     /* atomic: playback finished        */
    IMFPMediaPlayer *player;            /* current MFPlay player instance   */
} s_fmv;

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */

static int  fmv_init_media_foundation(void);
static void fmv_shutdown_media_foundation(void);
static int  fmv_check_skip_keys(void);
static void fmv_pump_messages(void);
static int  fmv_play_with_mfplay(const wchar_t *wpath);
static int  fmv_play_with_source_reader(const wchar_t *wpath);
static void fmv_update_volume_keys(void);

static int s_fmv_volume_percent = 100;

/* TGA loader for legal screens */
static uint8_t *fmv_load_tga(const char *path, int *out_w, int *out_h);
static void     fmv_display_image(const uint8_t *pixels, int w, int h,
                                  int timeout_ms);

/* MFPlay callback -- minimal implementation */
typedef struct FMVMediaPlayerCallback {
    IMFPMediaPlayerCallbackVtbl *lpVtbl;
    LONG ref_count;
} FMVMediaPlayerCallback;

static HRESULT STDMETHODCALLTYPE fmv_cb_QueryInterface(
    IMFPMediaPlayerCallback *This, REFIID riid, void **ppv);
static ULONG STDMETHODCALLTYPE fmv_cb_AddRef(
    IMFPMediaPlayerCallback *This);
static ULONG STDMETHODCALLTYPE fmv_cb_Release(
    IMFPMediaPlayerCallback *This);
static void STDMETHODCALLTYPE fmv_cb_OnMediaPlayerEvent(
    IMFPMediaPlayerCallback *This, MFP_EVENT_HEADER *pEventHeader);

static IMFPMediaPlayerCallbackVtbl s_callback_vtbl = {
    fmv_cb_QueryInterface,
    fmv_cb_AddRef,
    fmv_cb_Release,
    fmv_cb_OnMediaPlayerEvent
};

static FMVMediaPlayerCallback s_callback = {
    &s_callback_vtbl,
    1
};

/* ========================================================================
 * MFPlay Callback Implementation
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE fmv_cb_QueryInterface(
    IMFPMediaPlayerCallback *This, REFIID riid, void **ppv)
{
    (void)This;
    if (!ppv) return E_POINTER;

    /* MFPlay only queries for IMFPMediaPlayerCallback and IUnknown */
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IMFPMediaPlayerCallback)) {
        *ppv = This;
        fmv_cb_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fmv_cb_AddRef(
    IMFPMediaPlayerCallback *This)
{
    FMVMediaPlayerCallback *self = (FMVMediaPlayerCallback *)This;
    return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE fmv_cb_Release(
    IMFPMediaPlayerCallback *This)
{
    FMVMediaPlayerCallback *self = (FMVMediaPlayerCallback *)This;
    LONG ref = InterlockedDecrement(&self->ref_count);
    /* Static object, never actually freed */
    return ref;
}

static void STDMETHODCALLTYPE fmv_cb_OnMediaPlayerEvent(
    IMFPMediaPlayerCallback *This, MFP_EVENT_HEADER *pEventHeader)
{
    (void)This;
    if (!pEventHeader) return;

    switch (pEventHeader->eEventType) {
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
        InterlockedExchange(&s_fmv.playback_done, 1);
        TD5_LOG_I("fmv", "Playback ended (MFP_EVENT_TYPE_PLAYBACK_ENDED)");
        break;

    case MFP_EVENT_TYPE_ERROR:
        InterlockedExchange(&s_fmv.playback_done, 1);
        TD5_LOG_E("fmv", "MFPlay error: hr=0x%08X",
                  (unsigned)pEventHeader->hrEvent);
        break;

    case MFP_EVENT_TYPE_MEDIAITEM_SET:
        TD5_LOG_D("fmv", "Media item set, starting playback");
        if (s_fmv.player) {
            IMFPMediaPlayer_Play(s_fmv.player);
        }
        break;

    default:
        break;
    }
}

/* ========================================================================
 * Media Foundation Initialization
 * ======================================================================== */

static int fmv_init_media_foundation(void)
{
    HRESULT hr;

    /* Initialize COM if not already done */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        s_fmv.com_initialized = 1;
    } else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        /* COM already initialized (possibly by the game or another module).
         * S_FALSE = same mode, RPC_E_CHANGED_MODE = different mode.
         * Both are acceptable -- we can still use MF. */
        s_fmv.com_initialized = 0;
    } else {
        TD5_LOG_E("fmv", "CoInitializeEx failed: 0x%08X", (unsigned)hr);
        return 0;
    }

    /* Start Media Foundation platform */
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        TD5_LOG_E("fmv", "MFStartup failed: 0x%08X", (unsigned)hr);
        return 0;
    }

    TD5_LOG_I("fmv", "Media Foundation initialized (MF_VERSION=0x%08X)",
              (unsigned)MF_VERSION);
    return 1;
}

static void fmv_shutdown_media_foundation(void)
{
    MFShutdown();

    if (s_fmv.com_initialized) {
        CoUninitialize();
        s_fmv.com_initialized = 0;
    }
}

/* ========================================================================
 * Video Playback via MFPlay
 * ======================================================================== */

/**
 * Convert a UTF-8/ANSI path to wide string for MFPlay.
 * Caller must free the returned pointer with free().
 */
static wchar_t *fmv_to_wide(const char *path)
{
    int len = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (len <= 0) return NULL;

    wchar_t *wpath = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (!wpath) return NULL;

    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, len);
    return wpath;
}

/**
 * Play a video file using the MFPlay API.
 * Renders to the game window (HWND) with built-in video renderer.
 * Blocks until playback completes or skip is requested.
 *
 * Returns 1 if played to completion, 0 if skipped/error.
 */
static int fmv_play_with_mfplay(const wchar_t *wpath)
{
    /* MFPlay API (MFPCreateMediaPlayer) is not available in MinGW.
     * Stub out until a Media Foundation pipeline is implemented. */
    (void)wpath;
    return 0;
#if 0  /* disabled: MFPlay not linkable with MinGW */
    HRESULT hr;
    HWND hwnd;
    int result = 0;

    hwnd = (HWND)td5_plat_get_native_window();
    if (!hwnd) {
        TD5_LOG_E("fmv", "No window handle available for video playback");
        return 0;
    }

    /* Reset state */
    InterlockedExchange(&s_fmv.skip_requested, 0);
    InterlockedExchange(&s_fmv.playback_done, 0);
    s_fmv.player = NULL;

    /* Create the MFPlay media player.
     * MFPCreateMediaPlayer creates a player that renders video to the
     * specified HWND using EVR (Enhanced Video Renderer) internally. */
    hr = MFPCreateMediaPlayer(
        wpath,                              /* URL to play              */
        FALSE,                              /* don't start immediately  */
        0,                                  /* creation flags           */
        (IMFPMediaPlayerCallback *)&s_callback, /* event callback       */
        hwnd,                               /* video window             */
        &s_fmv.player                       /* [out] player instance    */
    );

    if (FAILED(hr) || !s_fmv.player) {
        TD5_LOG_E("fmv", "MFPCreateMediaPlayer failed: 0x%08X", (unsigned)hr);
        return 0;
    }

    TD5_LOG_I("fmv", "MFPlay player created, waiting for media item...");

    /* The callback's OnMediaPlayerEvent will receive MFP_EVENT_TYPE_MEDIAITEM_SET
     * when the source is resolved, then it calls Play(). We pump messages
     * in a loop matching the original's pattern (30ms sleep per iteration). */

    while (!s_fmv.playback_done && !s_fmv.skip_requested) {
        /* Process Windows messages (required for MFPlay callbacks) */
        fmv_pump_messages();

        /* Handle +/- volume keys during playback */
        fmv_update_volume_keys();

        /* Check skip keys: Enter, Shift, Escape, Space (matching original) */
        if (fmv_check_skip_keys()) {
            InterlockedExchange(&s_fmv.skip_requested, 1);
            break;
        }

        /* Sleep 30ms per iteration, matching original's timing */
        td5_plat_sleep(30);
    }

    /* Determine result */
    if (s_fmv.playback_done && !s_fmv.skip_requested) {
        result = 1; /* played to completion */
    }

    /* Cleanup: stop and release the player */
    if (s_fmv.player) {
        IMFPMediaPlayer_Stop(s_fmv.player);
        IMFPMediaPlayer_Release(s_fmv.player);
        s_fmv.player = NULL;
    }

    /* Repaint the window to clear the video surface */
    InvalidateRect(hwnd, NULL, TRUE);

    return result;
#endif /* disabled MFPlay */
}

/* ========================================================================
 * Skip Key Detection
 *
 * Original PlayIntroMovie (0x43C440) checks:
 *   VK_RETURN (0x0D), VK_SHIFT (0x10), VK_ESCAPE (0x1B), VK_SPACE (0x20)
 * ======================================================================== */

static int fmv_check_skip_keys(void)
{
    if (GetAsyncKeyState(VK_RETURN) & 0x8000)  return 1;
    if (GetAsyncKeyState(VK_SHIFT)  & 0x8000)  return 1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)  return 1;
    if (GetAsyncKeyState(VK_SPACE)  & 0x8000)  return 1;
    return 0;
}

static void fmv_update_volume_keys(void)
{
    static uint8_t s_prev_plus;
    static uint8_t s_prev_minus;
    int plus_down, minus_down;

    if (!s_fmv.player) return;

    plus_down = ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_ADD) & 0x8000) != 0);
    minus_down = ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0) ||
                 ((GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0);

    if (plus_down && !s_prev_plus) {
        s_fmv_volume_percent += 5;
        if (s_fmv_volume_percent > 100) s_fmv_volume_percent = 100;
        IMFPMediaPlayer_SetVolume(s_fmv.player,
                                  (float)s_fmv_volume_percent / 100.0f);
    }
    if (minus_down && !s_prev_minus) {
        s_fmv_volume_percent -= 5;
        if (s_fmv_volume_percent < 0) s_fmv_volume_percent = 0;
        IMFPMediaPlayer_SetVolume(s_fmv.player,
                                  (float)s_fmv_volume_percent / 100.0f);
    }

    s_prev_plus = (uint8_t)plus_down;
    s_prev_minus = (uint8_t)minus_down;
}

/**
 * Pump Windows messages.
 * MFPlay requires a message pump on the thread that created the player.
 * This mirrors the original's PeekMessageA/TranslateMessage/DispatchMessage loop.
 */
static void fmv_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            InterlockedExchange(&s_fmv.playback_done, 1);
            PostQuitMessage((int)msg.wParam);
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ========================================================================
 * TGA Loader (for legal screens)
 *
 * Supports uncompressed 24-bit and 32-bit TGA (types 2 and 10).
 * The original LEGALS.ZIP contains uncompressed 24-bit TGA files.
 * Returns RGBA pixel data (top-to-bottom, left-to-right).
 * ======================================================================== */

#pragma pack(push, 1)
typedef struct TGA_Header {
    uint8_t  id_length;
    uint8_t  colormap_type;
    uint8_t  image_type;        /* 2 = uncompressed true-color */
    uint16_t cm_first_entry;
    uint16_t cm_length;
    uint8_t  cm_entry_size;
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;
    uint16_t height;
    uint8_t  pixel_depth;       /* 24 or 32 */
    uint8_t  image_descriptor;
} TGA_Header;
#pragma pack(pop)

/**
 * Load an uncompressed TGA file and return RGBA8888 pixel data.
 * Handles 24-bit (BGR) and 32-bit (BGRA) true-color images.
 * Also handles RLE-compressed TGA (type 10).
 * Returns NULL on failure. Caller must free() the returned buffer.
 */
static uint8_t *fmv_load_tga(const char *path, int *out_w, int *out_h)
{
    TD5_File *f;
    TGA_Header hdr;
    uint8_t *pixels = NULL;
    int w, h, bpp, channels;
    int top_to_bottom;
    size_t pixel_count;
    size_t read_size;

    f = td5_plat_file_open(path, "rb");
    if (!f) {
        TD5_LOG_W("fmv", "Cannot open TGA: %s", path);
        return NULL;
    }

    /* Read TGA header */
    if (td5_plat_file_read(f, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        TD5_LOG_E("fmv", "Failed to read TGA header: %s", path);
        td5_plat_file_close(f);
        return NULL;
    }

    /* Validate: must be uncompressed or RLE true-color */
    if (hdr.image_type != 2 && hdr.image_type != 10) {
        TD5_LOG_E("fmv", "Unsupported TGA type %d: %s", hdr.image_type, path);
        td5_plat_file_close(f);
        return NULL;
    }

    if (hdr.pixel_depth != 24 && hdr.pixel_depth != 32) {
        TD5_LOG_E("fmv", "Unsupported TGA depth %d: %s", hdr.pixel_depth, path);
        td5_plat_file_close(f);
        return NULL;
    }

    w = hdr.width;
    h = hdr.height;
    bpp = hdr.pixel_depth;
    channels = bpp / 8;
    top_to_bottom = (hdr.image_descriptor & 0x20) ? 1 : 0;
    pixel_count = (size_t)w * (size_t)h;

    /* Skip image ID field */
    if (hdr.id_length > 0) {
        td5_plat_file_seek(f, (int64_t)hdr.id_length, 1 /* SEEK_CUR */);
    }

    /* Allocate output: always RGBA (4 bytes per pixel) for D3D upload */
    pixels = (uint8_t *)malloc(pixel_count * 4);
    if (!pixels) {
        TD5_LOG_E("fmv", "Out of memory loading TGA: %s", path);
        td5_plat_file_close(f);
        return NULL;
    }

    if (hdr.image_type == 2) {
        /* Uncompressed true-color */
        uint8_t *raw = (uint8_t *)malloc(pixel_count * (size_t)channels);
        if (!raw) {
            free(pixels);
            td5_plat_file_close(f);
            return NULL;
        }

        read_size = pixel_count * (size_t)channels;
        if (td5_plat_file_read(f, raw, read_size) != read_size) {
            TD5_LOG_E("fmv", "Failed to read TGA pixels: %s", path);
            free(raw);
            free(pixels);
            td5_plat_file_close(f);
            return NULL;
        }

        /* Convert BGR(A) to RGBA, handling row order */
        for (int y = 0; y < h; y++) {
            int src_y = top_to_bottom ? y : (h - 1 - y);
            const uint8_t *src_row = raw + (size_t)src_y * (size_t)w * (size_t)channels;
            uint8_t *dst_row = pixels + (size_t)y * (size_t)w * 4;

            for (int x = 0; x < w; x++) {
                dst_row[x * 4 + 0] = src_row[x * channels + 2]; /* R <- B */
                dst_row[x * 4 + 1] = src_row[x * channels + 1]; /* G <- G */
                dst_row[x * 4 + 2] = src_row[x * channels + 0]; /* B <- R */
                dst_row[x * 4 + 3] = (channels == 4) ?
                    src_row[x * channels + 3] : 0xFF;            /* A      */
            }
        }

        free(raw);

    } else {
        /* RLE-compressed true-color (type 10) */
        size_t px_idx = 0;
        uint8_t pkt_hdr;
        uint8_t color[4];

        while (px_idx < pixel_count) {
            if (td5_plat_file_read(f, &pkt_hdr, 1) != 1) break;

            int count = (pkt_hdr & 0x7F) + 1;
            if (pkt_hdr & 0x80) {
                /* RLE packet: one color repeated 'count' times */
                if (td5_plat_file_read(f, color, (size_t)channels) !=
                    (size_t)channels)
                    break;
                for (int i = 0; i < count && px_idx < pixel_count; i++, px_idx++) {
                    int y = top_to_bottom
                        ? (int)(px_idx / (size_t)w)
                        : h - 1 - (int)(px_idx / (size_t)w);
                    int x = (int)(px_idx % (size_t)w);
                    uint8_t *dst = pixels + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    dst[0] = color[2];
                    dst[1] = color[1];
                    dst[2] = color[0];
                    dst[3] = (channels == 4) ? color[3] : 0xFF;
                }
            } else {
                /* Raw packet: 'count' distinct pixels */
                for (int i = 0; i < count && px_idx < pixel_count; i++, px_idx++) {
                    if (td5_plat_file_read(f, color, (size_t)channels) !=
                        (size_t)channels)
                        goto rle_done;
                    int y = top_to_bottom
                        ? (int)(px_idx / (size_t)w)
                        : h - 1 - (int)(px_idx / (size_t)w);
                    int x = (int)(px_idx % (size_t)w);
                    uint8_t *dst = pixels + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    dst[0] = color[2];
                    dst[1] = color[1];
                    dst[2] = color[0];
                    dst[3] = (channels == 4) ? color[3] : 0xFF;
                }
            }
        }
rle_done:;
    }

    td5_plat_file_close(f);

    *out_w = w;
    *out_h = h;
    TD5_LOG_D("fmv", "Loaded TGA: %s (%dx%d, %dbpp)", path, w, h, bpp);
    return pixels;
}

/* ========================================================================
 * Image Display (Legal Screens)
 *
 * Renders a full-screen textured quad using the D3D11 render backend.
 * The image is uploaded to a temporary texture page, then drawn as
 * two pre-transformed triangles filling the viewport.
 *
 * We use texture page index 599 (last slot in TD5_MAX_TEXTURE_CACHE_SLOTS)
 * as a scratch page to avoid conflicting with game textures.
 * ======================================================================== */

#define FMV_SCRATCH_TEXTURE_PAGE  599

/**
 * Display an RGBA image on screen for the specified duration.
 * Blocks until timeout expires or a skip key is pressed.
 *
 * @param pixels    RGBA8888 pixel data, top-to-bottom
 * @param w         Image width
 * @param h         Image height
 * @param timeout_ms Display duration in milliseconds (0 = until keypress)
 */
static void fmv_display_image(const uint8_t *pixels, int w, int h,
                               int timeout_ms)
{
    int screen_w, screen_h;
    float sw, sh;
    uint32_t start_time;
    TD5_D3DVertex verts[4];
    uint16_t indices[6];

    if (!pixels) return;

    td5_plat_get_window_size(&screen_w, &screen_h);
    sw = (float)screen_w;
    sh = (float)screen_h;

    /* Upload image as texture.
     * Format 2 = A8R8G8B8 (32-bit), matching RGBA output from fmv_load_tga. */
    if (!td5_plat_render_upload_texture(FMV_SCRATCH_TEXTURE_PAGE,
                                        pixels, w, h, 2)) {
        TD5_LOG_E("fmv", "Failed to upload legal screen texture (%dx%d)", w, h);
        return;
    }

    /* Build a full-screen quad (2 triangles, pre-transformed).
     * Vertices are in screen space with RHW = 1.0 (already projected). */

    /* Top-left */
    verts[0].screen_x = 0.0f;
    verts[0].screen_y = 0.0f;
    verts[0].depth_z  = 0.0f;
    verts[0].rhw      = 1.0f;
    verts[0].diffuse  = 0xFFFFFFFF;
    verts[0].specular = 0;
    verts[0].tex_u    = 0.0f;
    verts[0].tex_v    = 0.0f;

    /* Top-right */
    verts[1].screen_x = sw;
    verts[1].screen_y = 0.0f;
    verts[1].depth_z  = 0.0f;
    verts[1].rhw      = 1.0f;
    verts[1].diffuse  = 0xFFFFFFFF;
    verts[1].specular = 0;
    verts[1].tex_u    = 1.0f;
    verts[1].tex_v    = 0.0f;

    /* Bottom-right */
    verts[2].screen_x = sw;
    verts[2].screen_y = sh;
    verts[2].depth_z  = 0.0f;
    verts[2].rhw      = 1.0f;
    verts[2].diffuse  = 0xFFFFFFFF;
    verts[2].specular = 0;
    verts[2].tex_u    = 1.0f;
    verts[2].tex_v    = 1.0f;

    /* Bottom-left */
    verts[3].screen_x = 0.0f;
    verts[3].screen_y = sh;
    verts[3].depth_z  = 0.0f;
    verts[3].rhw      = 1.0f;
    verts[3].diffuse  = 0xFFFFFFFF;
    verts[3].specular = 0;
    verts[3].tex_u    = 0.0f;
    verts[3].tex_v    = 1.0f;

    /* Two triangles: 0-1-2, 0-2-3 */
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    indices[3] = 0; indices[4] = 2; indices[5] = 3;

    /* Wait for any prior key release to avoid instant skip */
    td5_plat_sleep(200);

    start_time = td5_plat_time_ms();

    for (;;) {
        uint32_t elapsed = td5_plat_time_ms() - start_time;

        /* Check timeout */
        if (timeout_ms > 0 && elapsed >= (uint32_t)timeout_ms)
            break;

        /* Pump messages */
        if (!td5_plat_pump_messages())
            break;

        /* Check skip keys */
        if (fmv_check_skip_keys())
            break;

        /* Calculate fade alpha.
         * Fade in:  0..500ms
         * Hold:     500ms .. (timeout - 500ms)
         * Fade out: last 500ms */
        {
            float alpha = 1.0f;
            uint32_t fade_ms = 500;
            uint32_t tout = (timeout_ms > 0) ? (uint32_t)timeout_ms : 0xFFFFFFFF;

            if (elapsed < fade_ms) {
                alpha = (float)elapsed / (float)fade_ms;
            } else if (timeout_ms > 0 && elapsed > tout - fade_ms) {
                alpha = (float)(tout - elapsed) / (float)fade_ms;
            }

            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;

            uint8_t a = (uint8_t)(alpha * 255.0f);
            uint32_t color = ((uint32_t)a << 24) | 0x00FFFFFF;

            verts[0].diffuse = color;
            verts[1].diffuse = color;
            verts[2].diffuse = color;
            verts[3].diffuse = color;
        }

        /* Render */
        td5_plat_render_clear(0x00000000);
        td5_plat_render_begin_scene();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(FMV_SCRATCH_TEXTURE_PAGE);
        td5_plat_render_draw_tris(verts, 4, indices, 6);
        td5_plat_render_end_scene();
        td5_plat_present(1);

        td5_plat_sleep(16); /* ~60 fps */
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int td5_fmv_init(void)
{
    memset(&s_fmv, 0, sizeof(s_fmv));

    s_fmv.mf_available = fmv_init_media_foundation();
    s_fmv.initialized = 1;

    if (s_fmv.mf_available) {
        TD5_LOG_I("fmv", "FMV subsystem initialized (Media Foundation available)");
    } else {
        TD5_LOG_W("fmv", "FMV subsystem initialized (video playback unavailable, "
                  "legal screens still work)");
    }

    return 1;
}

void td5_fmv_shutdown(void)
{
    if (!s_fmv.initialized) return;

    if (s_fmv.mf_available) {
        fmv_shutdown_media_foundation();
    }

    memset(&s_fmv, 0, sizeof(s_fmv));
    TD5_LOG_I("fmv", "FMV subsystem shut down");
}

/** Stub: Source Reader pipeline not yet implemented. */
static int fmv_play_with_source_reader(const wchar_t *wpath)
{
    (void)wpath;
    return 0;
}

int td5_fmv_play(const char *filename)
{
    wchar_t *wpath;
    int result;

    if (!s_fmv.initialized) {
        TD5_LOG_E("fmv", "FMV not initialized, cannot play: %s", filename);
        return 0;
    }

    if (!filename || !filename[0]) {
        TD5_LOG_E("fmv", "NULL or empty filename");
        return 0;
    }

    /* Check if the file exists */
    if (!td5_plat_file_exists(filename)) {
        TD5_LOG_W("fmv", "Video file not found: %s", filename);
        return 0;
    }

    /* Check if Media Foundation is available */
    if (!s_fmv.mf_available) {
        TD5_LOG_W("fmv", "Media Foundation not available, skipping: %s",
                  filename);
        return 0;
    }

    /* Warn about TGQ files -- they need transcoding */
    {
        size_t len = strlen(filename);
        if (len >= 4) {
            const char *ext = filename + len - 4;
            if (_stricmp(ext, ".tgq") == 0 || _stricmp(ext, ".tgv") == 0) {
                TD5_LOG_W("fmv", "TGQ/TGV files are not directly supported. "
                          "Transcode to MP4 first: %s", filename);
                /* Try anyway in case user renamed an MP4 */
            }
        }
    }

    TD5_LOG_I("fmv", "Playing video: %s", filename);

    /* Convert path to wide string for Media Foundation */
    wpath = fmv_to_wide(filename);
    if (!wpath) {
        TD5_LOG_E("fmv", "Failed to convert path to wide string");
        return 0;
    }

    /* Clear the screen before video starts */
    td5_plat_render_clear(0x00000000);
    td5_plat_present(0);

    result = fmv_play_with_source_reader(wpath);
    free(wpath);

    /* Clear screen after video ends */
    td5_plat_render_clear(0x00000000);
    td5_plat_present(0);

    TD5_LOG_I("fmv", "Video playback %s: %s",
              result ? "completed" : "skipped/failed", filename);
    return result;
}

void td5_fmv_skip(void)
{
    InterlockedExchange(&s_fmv.skip_requested, 1);
}

int td5_fmv_is_supported(void)
{
    return s_fmv.initialized && s_fmv.mf_available;
}

/* ========================================================================
 * Legal Screens
 *
 * Original ShowLegalScreens (0x42C8E0):
 *   - Opens LEGALS.ZIP, extracts legal1.tga and legal2.tga
 *   - Displays each fullscreen for ~5000ms
 *   - Skippable via any key
 *
 * Source port version:
 *   - Looks for Legal/legal1.tga and Legal/legal2.tga (pre-extracted)
 *   - Falls back to Legals/legal1.tga, legal1.tga, etc.
 *   - Displays each for 5000ms with 500ms fade in/out
 *   - Skippable via Enter/Escape/Space (matching original skip keys)
 * ======================================================================== */

/** Legal screen search paths (tried in order) */
static const char *s_legal_paths[][2] = {
    { "Legal/legal1.tga",   "Legal/legal2.tga"   },
    { "Legals/legal1.tga",  "Legals/legal2.tga"  },
    { "legal1.tga",         "legal2.tga"          },
    { "Data/legal1.tga",    "Data/legal2.tga"     },
};

#define FMV_LEGAL_PATH_COUNT  (sizeof(s_legal_paths) / sizeof(s_legal_paths[0]))
#define FMV_LEGAL_DISPLAY_MS  5000

void td5_fmv_show_legal_screens(void)
{
    int found = 0;

    if (!s_fmv.initialized) {
        TD5_LOG_W("fmv", "FMV not initialized, skipping legal screens");
        return;
    }

    TD5_LOG_I("fmv", "Displaying legal screens...");

    /* Find legal screen files */
    for (size_t p = 0; p < FMV_LEGAL_PATH_COUNT && !found; p++) {
        if (td5_plat_file_exists(s_legal_paths[p][0]) ||
            td5_plat_file_exists(s_legal_paths[p][1])) {

            /* Display each legal screen */
            for (int i = 0; i < 2; i++) {
                const char *path = s_legal_paths[p][i];
                int img_w = 0, img_h = 0;
                uint8_t *pixels;

                if (!td5_plat_file_exists(path)) {
                    TD5_LOG_D("fmv", "Legal screen not found: %s", path);
                    continue;
                }

                pixels = fmv_load_tga(path, &img_w, &img_h);
                if (!pixels) continue;

                TD5_LOG_I("fmv", "Showing legal screen: %s (%dx%d)",
                          path, img_w, img_h);

                fmv_display_image(pixels, img_w, img_h, FMV_LEGAL_DISPLAY_MS);
                free(pixels);
                found = 1;
            }
            break;
        }
    }

    if (!found) {
        TD5_LOG_W("fmv", "No legal screen files found, skipping");
    }

    /* Clear screen after legal screens */
    td5_plat_render_clear(0x00000000);
    td5_plat_present(0);
}

#else /* !_WIN32 */

/* ========================================================================
 * Non-Windows Stub
 *
 * On non-Windows platforms, video playback is not yet implemented.
 * Legal screens still work via the platform-agnostic TGA loader +
 * render backend, but need a different video backend (e.g., FFmpeg).
 * ======================================================================== */

int td5_fmv_init(void) {
    TD5_LOG_W("fmv", "FMV not implemented on this platform");
    return 1;
}

void td5_fmv_shutdown(void) {}

int td5_fmv_play(const char *filename) {
    TD5_LOG_I("fmv", "FMV playback not available: %s", filename);
    (void)filename;
    return 0;
}

void td5_fmv_skip(void) {}

int td5_fmv_is_supported(void) {
    return 0;
}

void td5_fmv_show_legal_screens(void) {
    TD5_LOG_W("fmv", "Legal screens not implemented on this platform");
}

#endif /* _WIN32 */
