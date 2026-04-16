/**
 * td5_fmv.c -- FMV playback module (replaces EA TGQ codec)
 *
 * Replaces the original EA TGQ multimedia engine with:
 *   - Video playback via Windows Media Foundation (IMFSourceReader)
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
 *
 * Video playback uses IMFSourceReader to decode frames to RGB32,
 * then uploads each frame as a D3D11 texture and renders a fullscreen
 * quad. This avoids conflicts with the D3D11 swapchain (MFPlay's
 * built-in EVR uses D3D9 which would fight with our D3D11 device).
 *
 * Note: The original game ships intro.tgq (EA TGQ format). Media
 * Foundation cannot decode TGQ natively. Users must transcode to
 * MP4 (H.264/AAC) and place it alongside the original:
 *   Movie/intro.mp4  (preferred)
 *   Movie/intro.avi  (fallback)
 */

#include "td5_fmv.h"
#include "td5_asset.h"
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
#define COBJMACROS          /* enable IMFSourceReader/IMFSample/etc C macros */
#endif
#include <windows.h>

/* Media Foundation -- Source Reader API for frame-by-frame decoding.
 * Available on Windows 7+. */
#include <mfidl.h>          /* IMFMediaSink etc. (needed before mfreadwrite.h) */
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <mferror.h>

/* MFGetAttributeSize is declared as C++ inline in mfapi.h and is not
 * available in C compilation mode. Provide our own implementation that
 * unpacks the UINT64 attribute into width + height. */
static HRESULT fmv_get_attribute_size(IMFAttributes *pattr,
                                       REFGUID guid,
                                       UINT32 *pw, UINT32 *ph)
{
    UINT64 packed = 0;
    HRESULT hr = IMFAttributes_GetUINT64(pattr, guid, &packed);
    if (SUCCEEDED(hr)) {
        *pw = (UINT32)(packed >> 32);
        *ph = (UINT32)(packed & 0xFFFFFFFF);
    }
    return hr;
}

/* For CoInitializeEx */
#include <objbase.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
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
} s_fmv;

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */

static int  fmv_init_media_foundation(void);
static void fmv_shutdown_media_foundation(void);
static int  fmv_check_skip_keys(void);
static void fmv_pump_messages(void);
static int  fmv_play_with_source_reader(const wchar_t *wpath);

/* PNG loader for legal screens */
static uint8_t *fmv_load_png(const char *path, int *out_w, int *out_h);
static void     fmv_display_image(const uint8_t *pixels, int w, int h,
                                  int timeout_ms);

/* Scratch texture page for video frames and legal screens.
 * Index 599 (last slot in TD5_MAX_TEXTURE_CACHE_SLOTS) to avoid
 * conflicting with game textures. */
#define FMV_SCRATCH_TEXTURE_PAGE  599

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
 * Video Playback via IMFSourceReader
 *
 * Decodes video frames to RGB32 (X8R8G8B8) using the Source Reader,
 * then uploads each frame as a D3D11 texture and renders a fullscreen
 * quad. Timing is driven by sample timestamps from the source.
 * ======================================================================== */

/**
 * Convert a UTF-8/ANSI path to wide string for Media Foundation.
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
 * Render a decoded video frame as a fullscreen quad.
 * Pixels are RGB32 (X8R8G8B8), top-down, uploaded to scratch texture.
 */
static void fmv_render_video_frame(const uint8_t *pixels, int w, int h)
{
    int screen_w, screen_h;
    float sw, sh;
    TD5_D3DVertex verts[4];
    uint16_t indices[6];

    td5_plat_get_window_size(&screen_w, &screen_h);
    sw = (float)screen_w;
    sh = (float)screen_h;

    /* Upload frame. Format 2 = A8R8G8B8 (32-bit). */
    if (!td5_plat_render_upload_texture(FMV_SCRATCH_TEXTURE_PAGE,
                                        pixels, w, h, 2)) {
        return;
    }

    /* Build fullscreen quad (pre-transformed screen-space vertices) */
    verts[0].screen_x = 0.0f; verts[0].screen_y = 0.0f;
    verts[0].depth_z = 0.0f;  verts[0].rhw = 1.0f;
    verts[0].diffuse = 0xFFFFFFFF; verts[0].specular = 0;
    verts[0].tex_u = 0.0f; verts[0].tex_v = 0.0f;

    verts[1].screen_x = sw;   verts[1].screen_y = 0.0f;
    verts[1].depth_z = 0.0f;  verts[1].rhw = 1.0f;
    verts[1].diffuse = 0xFFFFFFFF; verts[1].specular = 0;
    verts[1].tex_u = 1.0f; verts[1].tex_v = 0.0f;

    verts[2].screen_x = sw;   verts[2].screen_y = sh;
    verts[2].depth_z = 0.0f;  verts[2].rhw = 1.0f;
    verts[2].diffuse = 0xFFFFFFFF; verts[2].specular = 0;
    verts[2].tex_u = 1.0f; verts[2].tex_v = 1.0f;

    verts[3].screen_x = 0.0f; verts[3].screen_y = sh;
    verts[3].depth_z = 0.0f;  verts[3].rhw = 1.0f;
    verts[3].diffuse = 0xFFFFFFFF; verts[3].specular = 0;
    verts[3].tex_u = 0.0f; verts[3].tex_v = 1.0f;

    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    indices[3] = 0; indices[4] = 2; indices[5] = 3;

    td5_plat_render_clear(0x00000000);
    td5_plat_render_begin_scene();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    td5_plat_render_bind_texture(FMV_SCRATCH_TEXTURE_PAGE);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
    td5_plat_render_end_scene();
    td5_plat_present(1);
}

/**
 * Play a video file using IMFSourceReader.
 * Decodes frames to RGB32, uploads as D3D11 textures, renders fullscreen.
 * Blocks until playback completes or skip is requested.
 *
 * Returns 1 if played to completion, 0 if skipped/error.
 */
static int fmv_play_with_source_reader(const wchar_t *wpath)
{
    HRESULT hr;
    IMFSourceReader *reader = NULL;
    IMFMediaType *output_type = NULL;
    UINT32 video_w = 0, video_h = 0;
    int result = 0;
    uint32_t playback_start_ms;

    /* Reset skip flag */
    InterlockedExchange(&s_fmv.skip_requested, 0);

    /* Create Source Reader from file URL */
    hr = MFCreateSourceReaderFromURL(wpath, NULL, &reader);
    if (FAILED(hr) || !reader) {
        TD5_LOG_E("fmv", "MFCreateSourceReaderFromURL failed: 0x%08X",
                  (unsigned)hr);
        return 0;
    }

    /* Request RGB32 (X8R8G8B8) output from the video stream.
     * The Source Reader will insert a color converter MFT automatically. */
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) {
        TD5_LOG_E("fmv", "MFCreateMediaType failed: 0x%08X", (unsigned)hr);
        IMFSourceReader_Release(reader);
        return 0;
    }

    IMFMediaType_SetGUID(output_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(output_type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);

    hr = IMFSourceReader_SetCurrentMediaType(reader,
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output_type);
    IMFMediaType_Release(output_type);
    output_type = NULL;

    if (FAILED(hr)) {
        TD5_LOG_E("fmv", "SetCurrentMediaType(RGB32) failed: 0x%08X",
                  (unsigned)hr);
        IMFSourceReader_Release(reader);
        return 0;
    }

    /* Read back the actual output type to get frame dimensions */
    {
        IMFMediaType *actual_type = NULL;
        hr = IMFSourceReader_GetCurrentMediaType(reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual_type);
        if (SUCCEEDED(hr) && actual_type) {
            fmv_get_attribute_size((IMFAttributes *)actual_type,
                               &MF_MT_FRAME_SIZE, &video_w, &video_h);
            IMFMediaType_Release(actual_type);
        }
    }

    if (video_w == 0 || video_h == 0) {
        TD5_LOG_E("fmv", "Could not determine video dimensions");
        IMFSourceReader_Release(reader);
        return 0;
    }

    TD5_LOG_I("fmv", "Video opened: %ux%u", video_w, video_h);

    /* Frame decode + render loop */
    playback_start_ms = td5_plat_time_ms();

    for (;;) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp_100ns = 0;
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer = NULL;
        BYTE *raw_pixels = NULL;
        DWORD buf_len = 0;

        /* Pump Windows messages */
        fmv_pump_messages();

        /* Check skip keys */
        if (fmv_check_skip_keys()) {
            InterlockedExchange(&s_fmv.skip_requested, 1);
            break;
        }

        if (s_fmv.skip_requested)
            break;

        /* Read next video sample */
        hr = IMFSourceReader_ReadSample(reader,
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,              /* no flags */
                &stream_index,
                &flags,
                &timestamp_100ns,
                &sample);

        if (FAILED(hr)) {
            TD5_LOG_E("fmv", "ReadSample failed: 0x%08X", (unsigned)hr);
            break;
        }

        /* End of stream */
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            result = 1;
            break;
        }

        /* Stream format changed -- re-read dimensions */
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            IMFMediaType *new_type = NULL;
            hr = IMFSourceReader_GetCurrentMediaType(reader,
                    (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &new_type);
            if (SUCCEEDED(hr) && new_type) {
                fmv_get_attribute_size((IMFAttributes *)new_type,
                                   &MF_MT_FRAME_SIZE, &video_w, &video_h);
                IMFMediaType_Release(new_type);
                TD5_LOG_D("fmv", "Video format changed: %ux%u",
                          video_w, video_h);
            }
        }

        if (!sample) {
            /* NULL sample but no end-of-stream: decoder still loading.
             * Sleep briefly and retry. */
            td5_plat_sleep(1);
            continue;
        }

        /* Wait for correct presentation time.
         * timestamp_100ns is in 100-nanosecond units from stream start.
         * Convert to milliseconds and wait. */
        {
            uint32_t target_ms = (uint32_t)(timestamp_100ns / 10000);
            uint32_t elapsed_ms = td5_plat_time_ms() - playback_start_ms;

            if (target_ms > elapsed_ms) {
                uint32_t wait_ms = target_ms - elapsed_ms;
                /* Cap wait to avoid hanging on bad timestamps */
                if (wait_ms > 500) wait_ms = 500;

                /* Sleep in small increments to stay responsive to skip */
                while (wait_ms > 0) {
                    uint32_t chunk = (wait_ms > 16) ? 16 : wait_ms;
                    td5_plat_sleep(chunk);
                    wait_ms -= chunk;

                    fmv_pump_messages();
                    if (fmv_check_skip_keys()) {
                        InterlockedExchange(&s_fmv.skip_requested, 1);
                        break;
                    }
                }

                if (s_fmv.skip_requested) {
                    IMFSample_Release(sample);
                    break;
                }
            }
        }

        /* Extract pixel data from sample */
        hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
        if (FAILED(hr) || !buffer) {
            IMFSample_Release(sample);
            continue;
        }

        hr = IMFMediaBuffer_Lock(buffer, &raw_pixels, NULL, &buf_len);
        if (SUCCEEDED(hr) && raw_pixels) {
            /* RGB32 from MF is bottom-up (DIB order). We need to flip
             * vertically for our top-down texture upload. */
            DWORD stride = video_w * 4;
            DWORD expected = stride * video_h;

            if (buf_len >= expected) {
                /* Flip in-place: swap rows top<->bottom */
                uint8_t *top_row, *bot_row;
                uint8_t *temp_row = (uint8_t *)_alloca(stride);
                UINT32 y;
                for (y = 0; y < video_h / 2; y++) {
                    top_row = raw_pixels + y * stride;
                    bot_row = raw_pixels + (video_h - 1 - y) * stride;
                    memcpy(temp_row, top_row, stride);
                    memcpy(top_row, bot_row, stride);
                    memcpy(bot_row, temp_row, stride);
                }

                fmv_render_video_frame(raw_pixels, (int)video_w, (int)video_h);
            }

            IMFMediaBuffer_Unlock(buffer);
        }

        IMFMediaBuffer_Release(buffer);
        IMFSample_Release(sample);
    }

    /* Cleanup */
    IMFSourceReader_Release(reader);

    return result;
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

/**
 * Pump Windows messages.
 * Required for COM/MF processing and to keep the window responsive.
 * Mirrors the original's PeekMessageA/TranslateMessage/DispatchMessage loop.
 */
static void fmv_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            InterlockedExchange(&s_fmv.skip_requested, 1);
            PostQuitMessage((int)msg.wParam);
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ========================================================================
 * PNG Loader (for legal screens)
 *
 * Uses td5_asset_decode_png_rgba32 to load PNG files.
 * Returns RGBA pixel data (top-to-bottom, left-to-right).
 * ======================================================================== */

/**
 * Load a PNG file and return BGRA32 pixel data (via td5_asset_decode_png_rgba32).
 * Returns NULL on failure. Caller must free() the returned buffer.
 */
static uint8_t *fmv_load_png(const char *path, int *out_w, int *out_h)
{
    void *pixels = NULL;
    if (!td5_asset_decode_png_rgba32(path, &pixels, out_w, out_h)) {
        TD5_LOG_W("fmv", "Cannot load PNG: %s", path);
        return NULL;
    }
    TD5_LOG_D("fmv", "Loaded PNG: %s (%dx%d)", path, *out_w, *out_h);
    return (uint8_t *)pixels;
}

/* ========================================================================
 * Image Display (Legal Screens)
 *
 * Renders a full-screen textured quad using the D3D11 render backend.
 * The image is uploaded to a temporary texture page, then drawn as
 * two pre-transformed triangles filling the viewport.
 * ======================================================================== */

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
     * Format 2 = B8G8R8A8 (32-bit), matching BGRA output from fmv_load_png. */
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
                TD5_LOG_W("fmv", "TGQ/TGV files are not supported by Media "
                          "Foundation. Transcode to MP4: %s", filename);
                return 0;
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
    { "re/assets/legals/legal1.png", "re/assets/legals/legal2.png" },
    { "Legal/legal1.png",   "Legal/legal2.png"   },
    { "Legals/legal1.png",  "Legals/legal2.png"  },
    { "legal1.png",         "legal2.png"          },
    { "Data/legal1.png",    "Data/legal2.png"     },
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

                pixels = fmv_load_png(path, &img_w, &img_h);
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
