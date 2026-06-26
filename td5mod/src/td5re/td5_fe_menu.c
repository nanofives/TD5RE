/*
 * td5_fe_menu.c -- frontend screens: boot flow + main menu + options.
 *
 * Split out of td5_frontend.c (2026-06). Handlers: LocalizationInit[0],
 * PositionerDebugTool[1], AttractModeDemo[2], LanguageSelect[3],
 * LegalCopyright[4], MainMenu[5], RaceTypeCategory[6], OptionsHub[12],
 * GameOptions[13], ControlOptions[14], SoundOptions[15], DisplayOptions[16],
 * TwoPlayerOptions[17], ControllerBinding[18], MusicTestExtras[19],
 * ExtrasGallery[22], StartupInit[28]. Shared frontend state comes from
 * td5_frontend_internal.h; original binary addresses are noted per screen.
 */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_game.h"
#include "td5_input.h"
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_sound.h"
#include "td5_hud.h"
#include "td5_track_registry.h"  /* custom-track registry: selector bound */
#include "td5re.h"
#include "td5_snk_strings.h"
#include "td5_credits.h"
#include "td5_vectorui.h"
#include "td5_font.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "../../ddraw_wrapper/src/shaders/ps_msdf_bytes.h"       /* g_ps_msdf bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_roundrect_bytes.h"  /* g_ps_roundrect bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_arrow_bytes.h"       /* g_ps_arrow bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_cursor_bytes.h"      /* g_ps_cursor bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_gauge_bytes.h"       /* g_ps_gauge bytecode */

#define LOG_TAG "frontend"
#include "td5_color.h"
#include "td5_frontend_internal.h"


/* ---- file-local forward declarations (definitions in source order) ---- */
static int frontend_create_preview_button(const char *label, int x, int y, int w, int h);
static void frontend_release_button(int handle);
static void frontend_cd_play(int track);
static void frontend_init_fade(int color);
static int frontend_render_fade(void);
static void frontend_set_button_label(int idx, const char *text);
static void frontend_refresh_display_option_labels(void);
static int frontend_load_continue_cup_data(void);
static int frontend_validate_cup_checksum(void);
static void frontend_load_bg_gallery(void);
static void frontend_music_test_update_track_label(void);
static void frontend_music_test_update_now_playing(int idx);
static float frontend_credits_total_height(void);
static void ctrl_opts_refresh_devices(void);
static void ctrl_opts_cycle_device(int delta);
static void mp_build_buttons(void);
static int ctrl_bind_row_count(void);
static const char *ctrl_bind_row_label(int row);
static void ctrl_begin_capture(void);
static void ctrl_capture_advance(void);
/* -----------------------------------------------------------------------
 * Screen [19] MusicTestExtras — state shared between state machine + render
 * ----------------------------------------------------------------------- */
static int s_music_test_track_idx;      /* 0..11; mirrors g_selectedCdTrackIndex @ 0x465e14 */

/* Band names and song titles for tracks 0..11.
 * [CONFIRMED @ Ghidra 0x418460]: PTR_s_GRAVITY_KILLS_00465e1c (band) and
 * PTR_s_FALLING_00465e58 (title) pointer tables, decoded from binary data. */
static const char * const k_music_test_band[12] = {
    "GRAVITY KILLS", "KMFDM",        "PITCHSHIFTER", "PITCHSHIFTER",
    "JUNKIE XL",     "FEAR FACTORY", "FEAR FACTORY", "GRAVITY KILLS",
    "KMFDM",         "PITCHSHIFTER", "PITCHSHIFTER", "PITCHSHIFTER"
};

static const char * const k_music_test_title[12] = {
    "FALLING",           "ANARCHY",     "GENIUS",            "WYSIWYG",
    "DEF BEAT",          "21ST CENTURY","GENETIC BLUEPRINT",  "FALLING (DUB)",
    "MEGALOMANIAC (DUB)","GENIUS (DUB)","MICROWAVED (DUB)",   "WYSIWYG (DUB)"
};

static int  s_mp_btn_ok        = -1;

/* Main-menu EXIT confirm dialog: button-pool indices of the YES / NO! buttons,
 * recorded when the dialog is built (state 5) so the handler (state 6) dispatches
 * by index instead of by label text — the SNK labels are "YES"/"NO!", which never
 * matched the old strcmp(..,"Yes")/strcmp(..,"No") so EXIT did nothing. */
static int  s_exit_confirm_yes_idx = -1;

static int  s_exit_confirm_no_idx  = -1;

/*
 * Fade overlay color (RGB only, alpha is driven by s_fade_progress).
 * [CONFIRMED @ 0x411750 InitFrontendFadeColor]: original stores
 *   DAT_00494fc4 = param_1 >> 3 & 0x1f1f1f  (packed B/G/R channel levels)
 * We store the raw ARGB word and extract RGB in the renderer.
 */
static uint32_t s_fade_color;        /* packed 0x00RRGGBB from caller */

static int  s_split_screen_surface = 0;     /* SplitScreen.tga (Two Player layout preview)    */

static int      s_ctrl_opts_max_players = 1;

/* [PORT ENHANCEMENT 2026-06] "REMAP ALL" sequential mode: capture every action
 * one by one (the original's sequential flow, re-added as an option). */
static int      s_ctrl_remap_all     = 0;

/* [PORT ENHANCEMENT 2026-06] keyboard state snapshot at capture-begin, so a key
 * already held when remap starts (e.g. the Enter that confirmed the row) is
 * ignored — only a fresh rising-edge press is accepted. */
static uint8_t  s_ctrl_capture_kb_snapshot[256];

static int frontend_create_preview_button(const char *label, int x, int y, int w, int h) {
    int idx = frontend_create_button(label, x, y, w, h);
    if (idx >= 0) s_buttons[idx].disabled = 1;
    return idx;
}

static void frontend_release_button(int handle) {
    if (handle >= 0 && handle < FE_MAX_BUTTONS)
        s_buttons[handle].active = 0;
}

static void frontend_cd_play(int track) {
    td5_plat_cd_play(track + 2);
}

static void frontend_init_fade(int color) {
    s_fade_active = 1;
    s_fade_progress = 0;
    s_fade_direction = 1;
    /*
     * [CONFIRMED @ 0x411750 InitFrontendFadeColor]: color is a packed ARGB/RGB
     * word whose B/G/R channels are stored as luma multipliers after >> 3 & 0x1f.
     * Port: store raw 0x00RRGGBB for use in frontend_render_fade.
     */
    s_fade_color = (uint32_t)color & 0x00FFFFFFu;
}

static int frontend_render_fade(void) {
    if (!s_fade_active) return 1;
    s_fade_progress += 16 * s_fe_logic_ticks;
    if (s_fade_progress >= 256) {
        s_fade_active = 0;
        return 1;
    }
    /* Draw semi-transparent black overlay */
    if (s_draw_queue_count < FE_MAX_DRAW_CMDS) {
        int screen_w = 0, screen_h = 0;
        td5_plat_get_window_size(&screen_w, &screen_h);
        uint32_t alpha = (uint32_t)(s_fade_progress < 256 ? s_fade_progress : 255);
        FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
        cmd->type = FE_CMD_RECT;
        cmd->x = 0; cmd->y = 0; cmd->w = screen_w; cmd->h = screen_h;
        /* Use s_fade_color (set by frontend_init_fade) for the overlay RGB.
         * Alpha is driven by fade progress [CONFIRMED @ 0x411750 / 0x411780]. */
        cmd->color = (alpha << 24) | (s_fade_color & 0x00FFFFFFu);
        cmd->tex_page = -1;
    }
    return 0;
}

static void frontend_set_button_label(int idx, const char *text) {
    if (idx < 0 || idx >= s_button_count) return;
    strncpy(s_buttons[idx].label, text, sizeof(s_buttons[idx].label) - 1);
    s_buttons[idx].label[sizeof(s_buttons[idx].label) - 1] = '\0';
}

/* [S01 Display options 2026-06-04] 6 option rows + OK (Resolution row removed —
 * the window is freely resizable). Row order:
 *   0 Display Mode  1 VSync  2 Fogging
 *   3 Speed Readout 4 Show FPS  5 Camera Damping  6 OK */
static void frontend_refresh_display_option_labels(void) {
    frontend_set_button_label(0, "Display Mode");
    frontend_set_button_label(1, "VSync");
    frontend_set_button_label(2, "Fogging");
    frontend_set_button_label(3, "Speed Readout");
    frontend_set_button_label(4, "Show FPS");
    frontend_set_button_label(5, "Camera Damping");
    frontend_set_button_label(6, "OK");
}

/* Load continue cup data: read + decrypt + restore game state. */
static int frontend_load_continue_cup_data(void) {
    int ok = td5_save_load_cup_data(NULL);
    if (ok) {
        int restored_race = 0;
        int game_type = td5_save_sync_cup_to_game(&restored_race);
        s_selected_game_type = game_type;
        s_race_within_series = restored_race;
        TD5_LOG_I(LOG_TAG, "LoadContinueCupData: type=%d race=%d", game_type, restored_race);
    } else {
        TD5_LOG_W(LOG_TAG, "LoadContinueCupData: failed");
    }
    return ok;
}

/* Validate CupData.td5 checksum without restoring state. */
static int frontend_validate_cup_checksum(void) {
    return td5_save_is_cup_valid(NULL);
}

int td5_frontend_init_resources(void) {
    TD5_LOG_I(LOG_TAG, "InitializeFrontendResourcesAndState");
    frontend_init_font_metrics_default();

    /* [S31] A net race aborted via the auto quit-to-menu (peer vanished ->
     * dead lockstep) re-enters the frontend HERE, bypassing the lobby's
     * connection-lost cleanup -- tear the dead session down so NET PLAY can
     * start fresh instead of inheriting a zombie session. */
    if (s_network_active && td5_net_is_connection_lost()) {
        TD5_LOG_I(LOG_TAG, "frontend re-entry: dead net session, destroying");
        frontend_net_destroy();
        s_network_active = 0;
    }

    /* Create 1x1 white fallback texture for untextured draws */
    if (s_white_tex_page < 0) {
        s_white_tex_page = SHARED_PAGE_WHITE;
        uint32_t white = 0xFFFFFFFF;
        if (td5_plat_render_upload_texture(s_white_tex_page, &white, 1, 1, 2)) {
            TD5_LOG_I(LOG_TAG, "Fallback background texture loaded: white page=%d",
                      s_white_tex_page);
        } else {
            TD5_LOG_W(LOG_TAG, "Fallback background texture upload failed: page=%d",
                      s_white_tex_page);
        }
    }

    /* ---- MSDF text pixel shader (resolution-independent SDF rendering) ----
     * The shader is shared by the HUD/pause-menu SDF atlases (hudfont_sdf /
     * pausefont_sdf, loaded below) and the SmallText SDF atlas, so it is still
     * created here. On failure s_ps_msdf stays NULL and every SDF consumer falls
     * back to its bitmap path -- a missing shader never breaks the menu.
     *
     * [2026-06-16] The frontend BODY-TEXT MSDF atlas (BodyText_msdf.png/.json)
     * load is RETIRED: fe_draw_text rasterises the always-shipping menu TTF and
     * returns before any MSDF/bitmap path, so the body-text SDF atlas was never
     * sampled. s_msdf_font_page is left at -1 (the TTF is the sole vector path);
     * BodyText_msdf.png + BodyText_msdf.json are deletable. */
    if (g_td5.ini.vector_ui && !s_ps_msdf && g_backend.device) {
        HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
            g_ps_msdf, sizeof(g_ps_msdf), NULL, &s_ps_msdf);
        if (FAILED(hr)) {
            s_ps_msdf = NULL;
            TD5_LOG_W(LOG_TAG, "MSDF pixel shader create failed hr=0x%08lX",
                      (unsigned long)hr);
        }
    }

    /* ---- Procedural rounded-rect button shader + constant buffer (VectorUI) ----
     * On failure both stay NULL and the button loop falls back to the bitmap
     * 9-slice path (VectorUI-off only; ButtonBits.png retired). */
    if (g_td5.ini.vector_ui && g_backend.device) {
        if (!s_ps_roundrect) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_roundrect, sizeof(g_ps_roundrect), NULL, &s_ps_roundrect);
            if (FAILED(hr)) {
                s_ps_roundrect = NULL;
                TD5_LOG_W(LOG_TAG, "roundrect shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_roundrect && !s_rr_cb) {
            D3D11_BUFFER_DESC bd;
            ZeroMemory(&bd, sizeof(bd));
            bd.ByteWidth = sizeof(FE_RoundRectParams);   /* 96, 16-aligned */
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            HRESULT hr = ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &s_rr_cb);
            if (FAILED(hr)) {
                s_rr_cb = NULL;
                TD5_LOG_W(LOG_TAG, "roundrect cbuffer create failed hr=0x%08lX", (unsigned long)hr);
            } else {
                TD5_LOG_I(LOG_TAG, "Procedural roundrect button shader ready (VectorUI)");
            }
        }
        if (!s_ps_arrow) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_arrow, sizeof(g_ps_arrow), NULL, &s_ps_arrow);
            if (FAILED(hr)) {
                s_ps_arrow = NULL;
                TD5_LOG_W(LOG_TAG, "arrow shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (!s_ps_cursor) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_cursor, sizeof(g_ps_cursor), NULL, &s_ps_cursor);
            if (FAILED(hr)) {
                s_ps_cursor = NULL;
                TD5_LOG_W(LOG_TAG, "cursor shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_cursor && s_cursor_msdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/snkmouse_msdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_CURSOR_MSDF, pixels, mw, mh, 2))
                    s_cursor_msdf_page = SHARED_PAGE_CURSOR_MSDF;
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "snkmouse_msdf.png not found -- cursor falls back to bitmap");
            }
        }

        /* ---- Procedural analog gauge dial shader + constant buffer (VectorUI) ----
         * Drives the in-race HUD speedometer dial + the added RPM tachometer via
         * td5_vui_gauge. On failure both stay NULL and the HUD falls back to the
         * baked GDI dial texture. */
        if (!s_ps_gauge) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_gauge, sizeof(g_ps_gauge), NULL, &s_ps_gauge);
            if (FAILED(hr)) {
                s_ps_gauge = NULL;
                TD5_LOG_W(LOG_TAG, "gauge shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_gauge && !s_gauge_cb) {
            D3D11_BUFFER_DESC bd;
            ZeroMemory(&bd, sizeof(bd));
            bd.ByteWidth = sizeof(FE_GaugeParams);   /* 144, 16-aligned */
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            HRESULT hr = ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &s_gauge_cb);
            if (FAILED(hr)) {
                s_gauge_cb = NULL;
                TD5_LOG_W(LOG_TAG, "gauge cbuffer create failed hr=0x%08lX", (unsigned long)hr);
            } else {
                TD5_LOG_I(LOG_TAG, "Procedural gauge dial shader ready (VectorUI)");
            }
        }

        /* ---- In-race HUD font SDF (VectorUI) ----
         * Distance-field version of the original tpage5 HUD font; rendered via
         * ps_msdf so the HUD text keeps its typeface but stays crisp. On any
         * failure s_hudfont_sdf_page stays -1 and the HUD uses the bitmap font. */
        if (s_ps_msdf && s_hudfont_sdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/static/hudfont_sdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_HUDFONT_SDF, pixels, mw, mh, 2)) {
                    s_hudfont_sdf_page = SHARED_PAGE_HUDFONT_SDF;
                    TD5_LOG_I(LOG_TAG, "HUD font SDF atlas loaded: page=%d %dx%d (VectorUI on)",
                              s_hudfont_sdf_page, mw, mh);
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "hudfont_sdf.png not found -- HUD text falls back to bitmap font");
            }
        }

        /* ---- Pause-menu font SDF (VectorUI) ---- */
        if (s_ps_msdf && s_pausefont_sdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/static/pausefont_sdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_PAUSEFONT_SDF, pixels, mw, mh, 2)) {
                    s_pausefont_sdf_page = SHARED_PAGE_PAUSEFONT_SDF;
                    TD5_LOG_I(LOG_TAG, "Pause font SDF atlas loaded: page=%d %dx%d (VectorUI on)",
                              s_pausefont_sdf_page, mw, mh);
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "pausefont_sdf.png not found -- pause menu falls back to bitmap font");
            }
        }
    }

    /* ---- Font atlas (BodyText.tga) — BITMAP/GDI FALLBACK ONLY ----
     * The ACTUAL game font is BodyText.tga (240x552, 8bpp palette TGA with
     * red color key). 10 chars/row, 24px cells. DAT_0049626c in original.
     * Loaded to dedicated page 898 with red color key transparency; falls back
     * to a GDI-generated font if the TGA is missing.
     * [2026-06-16] Under the always-shipping menu TTF (td5_font_ready()), every
     * text path (fe_draw_text / fe_draw_small_text / the value wrappers) renders
     * via the TTF and returns before this bitmap page is ever sampled, so skip the
     * whole load + GDI build entirely — no wasted bitmap/GDI font under TTF, and
     * BodyText.png becomes deletable. s_font_glyph_advance is already seeded with
     * defaults (frontend_init_font_metrics_default at init) for the rare TTF-off,
     * bitmap path. */
    if (!td5_font_ready() && s_font_page < 0) {
        int font_w = 0, font_h = 0;
        if (frontend_load_tga_colorkey("BodyText.tga", "Front End/frontend.zip",
                                        SHARED_PAGE_FONT, &font_w, &font_h,
                                        TD5_COLORKEY_BLACK)) {
            s_font_page = SHARED_PAGE_FONT;
            TD5_LOG_I(LOG_TAG, "Font atlas loaded: BodyText.tga page=%d %dx%d",
                      s_font_page, font_w, font_h);
        } else {
            /* FALLBACK: generate font with GDI if BodyText.tga is unavailable.
             * Uses 240x240 (only 10 rows needed for ASCII 32-127). The UV math
             * in fe_draw_text still works because row/col calculations produce
             * coords within the 240px region; the atlas is just smaller. */
            TD5_LOG_W(LOG_TAG, "BodyText.tga not found, falling back to GDI font");
            int fw = FONT_TEX_W, fh = 240; /* 10 rows * 24px for GDI fallback */
            uint32_t *pixels = (uint32_t *)calloc((size_t)(fw * fh), 4);
            if (pixels) {
                HDC hdc = CreateCompatibleDC(NULL);
                BITMAPINFO bmi;
                memset(&bmi, 0, sizeof(bmi));
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = fw;
                bmi.bmiHeader.biHeight = -fh;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                void *bits = NULL;
                HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
                if (hbm && bits) {
                    HGDIOBJ old_bm = SelectObject(hdc, hbm);
                    AddFontResourceExA("../FunctionX Bold.ttf", FR_PRIVATE, NULL);
                    HFONT hfont = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "FunctionX");
                    HGDIOBJ old_font = SelectObject(hdc, hfont);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    for (int ch = 32; ch < 128; ch++) {
                        int col = (ch - 32) % FONT_COLS;
                        int row = (ch - 32) / FONT_COLS;
                        char c = (char)ch;
                        RECT rc = { col * FONT_CELL + 2, row * FONT_CELL + 2,
                                    (col + 1) * FONT_CELL, (row + 1) * FONT_CELL };
                        DrawTextA(hdc, &c, 1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);
                    }
                    GdiFlush();
                    memcpy(pixels, bits, (size_t)(fw * fh * 4));
                    for (int i = 0; i < fw * fh; i++) {
                        uint32_t p = pixels[i];
                        uint8_t b = (uint8_t)(p & 0xFF);
                        uint8_t g = (uint8_t)((p >> 8) & 0xFF);
                        uint8_t r = (uint8_t)((p >> 16) & 0xFF);
                        uint8_t a = (uint8_t)((r + g + b) / 3);
                        pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                    ((uint32_t)g << 8) | b;
                    }
                    SelectObject(hdc, old_font);
                    DeleteObject(hfont);
                    SelectObject(hdc, old_bm);
                    DeleteObject(hbm);
                }
                DeleteDC(hdc);
                s_font_page = SHARED_PAGE_FONT;
                frontend_init_font_metrics_from_pixels((const uint8_t *)pixels, fw, fh);
                if (td5_plat_render_upload_texture(s_font_page, pixels, fw, fh, 2)) {
                    TD5_LOG_I(LOG_TAG, "GDI font atlas loaded: page=%d %dx%d",
                              s_font_page, fw, fh);
                } else {
                    TD5_LOG_W(LOG_TAG, "GDI font atlas upload failed: page=%d %dx%d",
                              s_font_page, fw, fh);
                    s_font_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "GDI font atlas allocation failed");
            }
        }
    }

    /* ---- Small font (smalltext) — high-score / results table font ----
     * 252x132, 12x12 cells. NOTE: the extracted "smalltext.png" is BLACK-on-WHITE
     * (inverted), which would render opaque boxes; the "_white_on_black" variant is the
     * correctly-oriented atlas (black bg → keyed transparent, white glyphs → tintable).
     * Used by frontend_render_high_score_overlay (true lowercase + descenders).
     * [2026-06-16] BITMAP FALLBACK ONLY: fe_draw_small_text rasterises the menu
     * TTF (always-shipping) and returns before this page is touched. The SmallText
     * SDF atlas has been retired (see below), so this bitmap atlas is now the sole
     * non-TTF fallback. Skip the load when VectorUI is on — the smalltext*.png
     * atlases are deletable. (VectorUI-off + no-TTF still needs the bitmap.) */
    if (!g_td5.ini.vector_ui && s_smallfont_page < 0) {
        int sfw = 0, sfh = 0;
        if (frontend_load_tga_colorkey("smalltext_white_on_black.tga", "Front End/frontend.zip",
                                        SMALLFONT_PAGE, &sfw, &sfh, TD5_COLORKEY_BLACK)) {
            s_smallfont_page = SMALLFONT_PAGE;
            TD5_LOG_I(LOG_TAG, "Small font loaded: smalltext.tga page=%d %dx%d",
                      s_smallfont_page, sfw, sfh);
        } else {
            TD5_LOG_W(LOG_TAG, "smalltext.tga not found — high-score table will fall back to BodyText");
        }
    }

    /* ---- Vector (SDF) SmallText atlas ----
     * [2026-06-16] RETIRED: fe_draw_small_text rasterises the always-shipping menu
     * TTF and returns before any SDF/bitmap path, so the SmallText SDF atlas was
     * never sampled. The load is removed; s_smallfont_msdf_page stays -1 and the
     * small-text TTF path is authoritative. smalltext_msdf.png + smalltext_msdf.json
     * are deletable. */

    /* ---- ButtonBits (gradient source for button backgrounds) ----
     * [2026-06-16] RETIRED under VectorUI (the shipped default): every button
     * frame is drawn procedurally via the ps_roundrect path (fe_draw_button_frame
     * -> fe_draw_roundrect). The 56x100 ButtonBits 9-slice bitmap was only the
     * VectorUI-OFF fallback (fe_draw_button_9slice) and the now-removed CPU button
     * cache; under the default config it was loaded but NEVER blitted. The load is
     * removed so ButtonBits.png is deletable; s_buttonbits_tex_page stays -1 and the
     * residual 9-slice fallback code self-skips on (page < 0). */

    /* ---- ArrowButtonz (left/right scroll arrows on selector buttons) ----
     * [2026-06-16] RETIRED: the selector ◄► arrows are now always drawn
     * procedurally via ps_arrow (fe_draw_arrow_proc / fe_draw_option_arrows).
     * The 12x36 ArrowButtonz.tga sprite-sheet bitmap fallback and its load were
     * removed; the asset is deletable. */

    /* ---- ButtonLights (selection indicator dot) ----
     * 16x32 texture. Two 16x16 frames stacked vertically.
     * Black colorkey. */
    if (s_buttonlights_tex_page < 0) {
        s_buttonlights_tex_page = SHARED_PAGE_BTNLIGHTS;
        {
            void *pixels = NULL;
            int lw = 0, lh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/ButtonLights.png",
                                              TD5_COLORKEY_BLACK, &pixels, &lw, &lh)) {
                if (td5_plat_render_upload_texture(s_buttonlights_tex_page, pixels, lw, lh, 2)) {
                    s_buttonlights_w = lw;
                    s_buttonlights_h = lh;
                    TD5_LOG_I(LOG_TAG, "ButtonLights loaded (PNG): page=%d %dx%d",
                              s_buttonlights_tex_page, lw, lh);
                } else {
                    s_buttonlights_tex_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "ButtonLights.png not found (optional)");
                s_buttonlights_tex_page = -1;
            }
        }
    }

    /* ---- SnkMouse.TGA (cursor) ----
     * [2026-06-16] BITMAP FALLBACK ONLY: under VectorUI (default) the cursor is
     * drawn procedurally via ps_cursor + snkmouse_msdf.png (SDF) by
     * fe_draw_cursor_proc, and frontend_render_cursor returns before the bitmap
     * path, so the SnkMouse.png bitmap was retired. Skip the load entirely when
     * VectorUI is on (same pattern as the ArrowButtonz load above). The
     * VectorUI-off bitmap fallback (frontend_render_cursor) then uses the 22x30
     * default in fe_draw_cursor_proc, which equals SnkMouse.png's native size. */
    if (!g_td5.ini.vector_ui && s_cursor_tex_page < 0) {
        s_cursor_tex_page = SHARED_PAGE_CURSOR;
        /* SnkMouse.png has a red background → use red colorkey. */
        if (!frontend_load_tga_colorkey("snkmouse.tga", "Front End/frontend.zip",
                                         s_cursor_tex_page, &s_cursor_w, &s_cursor_h,
                                         TD5_COLORKEY_RED)) {
            TD5_LOG_W(LOG_TAG, "Failed to load snkmouse.tga cursor texture");
            s_cursor_tex_page = -1;
        } else {
            TD5_LOG_I(LOG_TAG, "Cursor texture loaded: page=%d %dx%d",
                      s_cursor_tex_page, s_cursor_w, s_cursor_h);
        }
    }

    /* Per-screen backgrounds are loaded on demand by each screen function.
     * No need to preload them here -- they go into the recyclable surface pool. */

    /* Create work surfaces for UI rendering */
    /* (Real implementation allocates DirectDraw surfaces) */

    /* Load frontend fonts (from Language.dll string table) */
    /* (Real implementation loads SNK_* string exports) */

    /* Initialize CD audio volume from saved settings */
    td5_plat_cd_set_volume(80);
    td5_sound_load_frontend_sfx();

    /* Car lock table: DAT_00463e4c (original binary).
     * Selector shows 23 cars (positions 0-22) in regular mode; DAT_00463e0c = 23.
     * Positions 0-20: unlocked (21 cars visible + selectable).
     *   Note: atp(16), ss1(17), 128(18), gtr(19), jag(20) are UNLOCKED in original.
     * Positions 21-22: visible but locked (cat=SUPER7, sp4=R390).
     * Positions 23-36: invisible in regular mode (cop-chase / cup unlock only). */
    /* Populate lock tables from save system (Config.td5 loaded by td5_save_init). */
    td5_save_get_car_lock_table(s_car_lock_table, TD5_BASE_CAR_COUNT);
    td5_save_get_track_lock_table(s_track_lock_table, 26);
    { int td6s; for (td6s = 26; td6s <= 36; td6s++) s_track_lock_table[td6s] = 0; } /* TD6 tracks always available */

    /* Compute total unlocked car count (visible + selectable in roster).
     * s_total_unlocked_cars = max visible car index (exclusive).
     * The original uses DAT_00463e0c which counts contiguous visible slots. */
    if (td5_save_get_all_cars_unlocked()) {
        s_total_unlocked_cars = 37;
    } else {
        s_total_unlocked_cars = td5_save_get_max_unlocked_car();
        if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21; /* minimum visible roster */
    }

    /* Compute total navigable track count. Race tracks (0-19) are always
     * navigable in the selector (locked ones show "LOCKED" but are visible).
     * Cup tracks (20-25) become navigable when unlocked via cup wins.
     * s_total_unlocked_tracks = exclusive upper bound for track cycling. */
    {
        int t;
        s_total_unlocked_tracks = 20; /* race tracks 0-19 always navigable */
        for (t = 20; t < 37; t++) {
            if (s_track_lock_table[t] == 0) /* 0 = unlocked in frontend table */
                s_total_unlocked_tracks = t + 1; /* extend range to include this cup track */
        }
        if (td5_track_registry_slot_max() > s_total_unlocked_tracks)
            s_total_unlocked_tracks = td5_track_registry_slot_max(); /* + custom registry tracks (>=37) */
    }
    TD5_LOG_I(LOG_TAG, "Progression: unlocked_cars=%d unlocked_tracks=%d cup_tier=0x%02X cheat_unlock=%d",
              s_total_unlocked_cars, s_total_unlocked_tracks, td5_save_get_cup_tier(), s_cheat_unlock_all);

    /* Background gallery slideshow (LoadExtrasGalleryImageSurfaces 0x40D590) */
    frontend_load_bg_gallery();

    return 1;
}

static void frontend_load_bg_gallery(void) {
    static const char * const png_names[5] = {
        "re/assets/extras/pic1.png",
        "re/assets/extras/pic2.png",
        "re/assets/extras/pic3.png",
        "re/assets/extras/pic4.png",
        "re/assets/extras/pic5.png"
    };
    if (s_bg_gal_loaded) return;
    for (int i = 0; i < 5; i++) {
        int page = SHARED_PAGE_BG_GALLERY + i;
        void *pixels = NULL; int w = 0, h = 0;

        /* Try PNG first */
        if (!td5_asset_load_png_to_buffer(png_names[i], TD5_COLORKEY_BLACK, &pixels, &w, &h)) {
            TD5_LOG_W(LOG_TAG, "BgGallery: failed to load %s", png_names[i]);
            continue;
        }

        if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
            s_bg_gallery[i].width  = w;
            s_bg_gallery[i].height = h;
            TD5_LOG_I(LOG_TAG, "BgGallery[%d]: %dx%d page=%d", i, w, h, page);
        }
        free(pixels);
    }
    s_bg_gal_current = 0;
    s_bg_gal_blend   = 0x100;
    s_bg_gal_x       = 140.0f;
    s_bg_gal_y       = 84.0f;
    s_bg_gal_loaded  = 1;
}

static void frontend_music_test_update_track_label(void) {
    int idx = s_music_test_track_idx;
    if (idx < 0)  idx = 0;
    if (idx > 11) idx = 11;
    /* format: "%d. %s" [CONFIRMED @ 0x465f74: "%d. %s"] */
    snprintf(s_music_test_track_label, sizeof(s_music_test_track_label),
             "%d. %s", idx + 1, k_music_test_band[idx]);
}

static void frontend_music_test_update_now_playing(int idx) {
    if (idx < 0)  idx = 0;
    if (idx > 11) idx = 11;
    snprintf(s_music_test_now_band,  sizeof(s_music_test_now_band),
             "%s", k_music_test_band[idx]);
    snprintf(s_music_test_now_title, sizeof(s_music_test_now_title),
             "%s", k_music_test_title[idx]);
    s_music_test_playing_set = 1;
}

/* total scroll-column height of all credit rows (used by the handler to know when done) */
static float frontend_credits_total_height(void) {
    float cy = 0.0f;
    for (int i = 0; i < K_CREDITS_COUNT; i++)
        cy += (k_credits[i][0] == '#') ? FE_CREDITS_PHOTO_H : FE_CREDITS_ROW_H;
    return cy;
}

void Screen_LocalizationInit(void) {
    /* [CONFIRMED @ 0x4269D0] g_attractModeControlEnabled three-state gate:
     *   0 = first entry: run full init, set ctrl=1, route to MAIN_MENU (screen 5)
     *   1 = normal re-entry: skip init, route to MAIN_MENU (screen 5)
     *   2 = resume-cup re-entry: set results_skip_display=1, route to RACE_RESULTS (screen 0x18=24) */
    if (s_attract_mode_ctrl == 2) {
        /* [CONFIRMED @ 0x42718A-0x4271A2]: DAT_00497a6c=1 then SetFrontendScreen(0x18) */
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: resume-cup path -> RACE_RESULTS (skip_display=1)");
        s_results_skip_display = 1;
        s_attract_mode_ctrl = 1;
        td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS);
        return;
    }

    if (s_attract_mode_ctrl == 1) {
        /* [CONFIRMED @ 0x427182-0x427188]: re-entry shortcut straight to MAIN_MENU */
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: re-entry shortcut -> MAIN_MENU");
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        return;
    }

    /* First entry (s_attract_mode_ctrl == 0) [CONFIRMED @ 0x4269D0 case 0]: */
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_LOCALIZATION_INIT);
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: first entry, loading resources");

        /* [CONFIRMED @ 0x4269D0 / 0x4267A8] LANGUAGE.DLL is a static PE import.
         * [CORRECTED 2026-06-01 — byte-verified from the English Language.dll export
         * SNK_LangDLL = "LANGDLL 0 : ENGLISH/US"]: byte[8] is the digit '0' = 0x30 for
         * English (the prior comment's "0x31" was WRONG). The font/text gate compares
         * byte[8] against 0x30 (CMP byte[reg+8],0x30 @0x00424568 / @0x004242b8 /
         * CreateMenuStringLabelSurface 0x00412e30); the ==0x30 (JZ-taken) branch is the
         * English/localized-blit path. (Other locales use different digits, but the
         * shipped English DLL is '0'.)
         * English entry name = "config.eng" [CONFIRMED @ 0x4667A8].
         * The original reads "config.eng" per car ZIP, sscanf's 17 tokens into
         * DAT_0049b90c (stride 0x330, 17 rows × 0x30 bytes each).
         * Port: re/assets/cars/<car>/config.nfo has the same 17-token layout (extracted
         * from the original ZIPs). frontend_load_car_spec_fields() reads all 17 tokens;
         * frontend_render_car_stats_overlay() displays them on the Stats sub-screen
         * (car-select state 15, button 2 "Stats"). */
        /* [CONFIRMED @ 0x4269D0] Car ZIP path table: handled in td5_asset.c */
        /* [CONFIRMED @ 0x426F80]: LoadPackedConfigTd5() reads config.td5 settings */
        /* [INFERRED] Enumerate display modes (handled in td5_render.c) */
        /* [CONFIRMED @ 0x427081]: Seed controller/input state from DXInput joystick exports
         *   g_player1InputSource=0, g_player2InputSource=7 if no saved controller match.
         *   Port omits: DXInput (M2DX) exports not available; td5_input.c handles this. */

        td5_frontend_init_resources();

        /* Mark init done so re-entry skips straight to menu [CONFIRMED @ 0x427060] */
        s_attract_mode_ctrl = 1;

        /* [CONFIRMED @ 0x427182]: SetFrontendScreen(5) = TD5_SCREEN_MAIN_MENU */
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;

    default:
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

void Screen_PositionerDebugTool(void) {
    switch (s_inner_state) {
    case 0: /* Load the menu font + clear (orig loads Positioner.tga as the cursor-bar
             * colour source, clears the backbuffer black and draws two guide scanlines).
             * The original creates NO on-screen buttons — the whole tool is keyboard-
             * driven (arrows move/edit, ESC saves), so the port draws none either. */
        frontend_init_return_screen(TD5_SCREEN_POSITIONER_DEBUG);
        frontend_load_tga("Front_End/Positioner.tga", "Front_End/FrontEnd.zip");
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: /* present (orig case 1) */
        frontend_present_buffer();
        s_inner_state = 2;
        break;
    case 2: /* init glyph selection (orig g_positionerSelectedGlyphIndex = 0) */
        s_anim_tick = 0;
        s_anim_complete = 1;   /* enable the shared ESC handler (-> return screen = main menu) */
        s_inner_state = 3;
        break;
    case 3: /* navigate the glyph strip (orig case 3: arrow bits LEFT=1/RIGHT=2/UP=4/DOWN=8;
             * ←/→ = ±1, ↓/↑ = ±8). ESC is handled by the shared escape path -> main menu. */
        if (s_input_ready && s_arrow_input) {
            if (s_arrow_input & 1) s_anim_tick -= 1;   /* LEFT  */
            if (s_arrow_input & 2) s_anim_tick += 1;   /* RIGHT */
            if (s_arrow_input & 8) s_anim_tick += 8;   /* DOWN  */
            if (s_arrow_input & 4) s_anim_tick -= 8;   /* UP    */
            frontend_play_sfx(1);
        }
        break;
    default:
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

void Screen_AttractModeDemo(void) {
    switch (s_inner_state) {
    case 0: /* Set attract mode flag */
        frontend_init_return_screen(TD5_SCREEN_ATTRACT_MODE);
        /* [CONFIRMED @ 0x4275B1] g_attractModeDemoActive = 1 */
        s_attract_demo_active = 1;
        frontend_present_buffer();
        frontend_set_cursor_visible(0);
        s_inner_state = 1;
        break;

    case 1: /* Release frontend buttons from main menu */
        /* [CONFIRMED @ 0x4275B7] ReleaseFrontendDisplayModeButtons() */
        frontend_reset_buttons();
        s_inner_state = 2;
        break;

    case 2: /* Present primary buffer */
        frontend_present_buffer();
        s_inner_state = 3;
        break;

    case 3: /* Present (2 frames of setup) */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Init fade-to-black */
        frontend_init_fade(0);
        s_inner_state = 5;
        break;

    case 5: /* Execute fade, then launch demo race */
        if (frontend_render_fade()) {
            /* Fade complete -- launch attract-demo race. The original demo is
             * AI-driven (no input playback) and shows the "DEMO MODE" status
             * text; set the demo flag AFTER frontend_init_race_schedule (which
             * clears it). Distinct from View Replay, which plays back input and
             * shows the REPLAY banner. [orig g_attractModeDemoActive path.] */
            frontend_init_race_schedule();
            td5_game_set_demo_mode(1);
            frontend_init_display_mode_state();
        }
        break;
    }
}

void Screen_LanguageSelect(void) {
    switch (s_inner_state) {
    case 0: /* Load Language.tga and LanguageScreen.tga */
        frontend_init_return_screen(TD5_SCREEN_LANGUAGE_SELECT);
        /* [FIXED 2026-06-01] Faithful to ScreenLanguageSelect @0x00427290: the original
         * draws LanguageScreen.tga (bg) + 4 FLAG IMAGE tiles from Language.tga (176x512,
         * four stacked 176x128 tiles, src V 0/128/256/384) as clickable hit-rects at the
         * four corners + a "LANGUAGE SELECT" header (in-EXE literal @0x4667c0). It has NO
         * text buttons. Port previously showed 4 text buttons — replaced: the 4 buttons
         * are now HIDDEN hit-rects at the confirmed flag dest rects (input still works via
         * s_button_index<4), and frontend_render_language_select_overlay draws the flags +
         * header + bg. Clicking any flag advances to LEGAL (no language global written —
         * CONFIRMED). Dest rects @640x480: TL(40,128) TR(424,128) BL(40,320) BR(424,320),
         * each 176x128. */
        s_language_bg_surface   = frontend_load_tga("Front_End/LanguageScreen.tga", "Front_End/FrontEnd.zip");
        s_language_flag_surface = frontend_load_tga("Front_End/Language.tga", "Front_End/FrontEnd.zip");
        {
            int fi;
            for (fi = 0; fi < 4; fi++) {
                int fx = (fi & 1) ? 424 : 40;     /* TL/BL left=40, TR/BR right=424 */
                int fy = (fi < 2) ? 128 : 320;    /* top row 128, bottom row 320 */
                int b = frontend_create_button("", fx, fy, 176, 128);
                if (b >= 0) s_buttons[b].hidden = 1;  /* invisible hit-rect; flag drawn in overlay */
            }
        }
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Tick */
        frontend_present_buffer();
        s_inner_state = 2;
        break;

    case 2:
        s_anim_tick += 2 * s_fe_logic_ticks;
        frontend_present_buffer();
        if (s_anim_tick >= 16) {
            s_inner_state = 3;
        }
        break;

    case 3: /* Interaction -- wait for language selection */
        if (s_input_ready && s_button_index >= 0 && s_button_index < 4) {
            s_flow_context = s_button_index;
            s_anim_tick = 0;
            s_inner_state = 4;
        }
        break;

    case 4:
    case 5:
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 8) {
            s_inner_state = 6;
        }
        break;

    case 6: /* Release surfaces, exit to Legal screen */
        td5_frontend_set_screen(TD5_SCREEN_LEGAL_COPYRIGHT);
        break;
    }
}

void Screen_LegalCopyright(void) {
    switch (s_inner_state) {
    case 0: /* Load + draw */
        frontend_init_return_screen(TD5_SCREEN_LEGAL_COPYRIGHT);
        frontend_load_tga("Front_End/LegalScreen.tga", "Front_End/FrontEnd.zip");
        /* Copyright text drawn live in render overlay via
         * DrawFrontendLocalizedStringSecondary @ 0x00424390.
         * Original renders "TEST DRIVE 5 COPYRIGHT 1998" [CONFIRMED @ 0x00466808]
         * at x=canvasW/10, y=0x20 (32px) and repeats each row down the screen.
         * Port renders it in frontend_render_legal_copyright_overlay below. */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Fade in [FIXED 2026-06-01: actually run the fade — was a no-op counter,
             * so the legal splash popped in. Orig case1 = RenderFrontendFadeEffect.] */
        if (s_anim_tick == 0) { frontend_init_fade(0x000000); s_anim_tick = 1; }
        if (frontend_render_fade()) {
            /* Store wall-clock start for 3-second guard [CONFIRMED @ 0x4274A0 case 1→2] */
            s_anim_tick = (int)timeGetTime();
            s_inner_state = 2;
        }
        break;

    case 2: /* 3-second timer [CONFIRMED @ 0x4274A0 case 2: timeGetTime() - stored > 2999] */
        if ((uint32_t)(timeGetTime() - (uint32_t)s_anim_tick) > 2999u) {
            s_anim_tick = 0;
            s_inner_state = 3;
        }
        break;

    case 3: /* Fade out + exit [FIXED 2026-06-01: run the fade, was a no-op counter.] */
        if (s_anim_tick == 0) { frontend_init_fade(0x000000); s_anim_tick = 1; }
        if (frontend_render_fade()) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

void Screen_MainMenu(void) {
    switch (s_inner_state) {
    case 0: /* Init: configure controller bindings, apply options, create 7 buttons, load MainMenu.tga */
        frontend_init_return_screen(TD5_SCREEN_MAIN_MENU);
        TD5_LOG_D(LOG_TAG, "MainMenu: state 0 - init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;

        /* [ATTRACT DEMO 2026-06-25] Re-arm the attract idle timer on every main-menu
         * (re-)entry. Without this, returning from a demo (or any race) leaves a stale
         * timestamp from minutes ago, so the idle check would fire again immediately and
         * the menu would bounce straight back into another demo. A fresh entry = a fresh
         * full idle window. (Input also re-arms it in case 4 below.) */
        s_attract_idle_timestamp = td5_plat_time_ms();

        /* [CONFIRMED @ 0x004155DE ScreenMainMenuAnd1PRaceFlow case 0] Original
         * copies the GameOptions shadow (DAT_00466000, range 0..3) into the
         * live runtime lap count: gCircuitLapCount = DAT_00466000 + 1.
         * Without this seed, a fresh boot leaves circuit_lap_count=0 and the
         * HUD's "%d/%d" lap label renders as "1/0". The original re-applies
         * this on every main-menu entry; ConfigureGameTypeFlags later may
         * overwrite it for cup tiers (case 2 hard-sets to 4), but the
         * baseline must be primed here so single races and quickrace work. */
        g_td5.circuit_lap_count = s_game_option_laps + 1;
        TD5_LOG_I(LOG_TAG, "MainMenu: seeded circuit_lap_count=%d (laps_option=%d)",
                  g_td5.circuit_lap_count, s_game_option_laps);

        /* [MP leak fix 2026-06-13] Clear any leftover multiplayer session state
         * on main-menu entry. After a local split-screen MP race, s_mp_flow +
         * s_two_player_mode + s_num_human_players stayed set, so the next Quick
         * Race (or any race-menu mode) reused the MP grid — e.g. a 3-player MP
         * race then launched a single Quick Race with 3 split-screen viewports
         * (frontend_init_race_schedule gates >1 human on these flags). Every
         * MP/2P flow re-arms these from its own lobby/menu, so main-menu
         * entry always means "no active session": reset the flags + player count
         * and drop the per-player EXCLUSIVE device bindings so all pads feed the
         * shared menu polling again. The single-player race path rebinds slot 0
         * to whichever controller navigated. */
        if (s_mp_flow || s_two_player_mode || s_num_human_players > 1) {
            int p;
            TD5_LOG_I(LOG_TAG,
                      "MainMenu: clearing stale MP session (mp_flow=%d 2p=%d humans=%d)",
                      s_mp_flow, s_two_player_mode, s_num_human_players);
            s_mp_flow           = 0;
            s_mp_simul          = 0;
            s_two_player_mode   = 0;
            s_num_human_players = 1;
            g_td5.split_screen_mode = 0;
            for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
                td5_input_set_input_source(p, 0);
        }

        /* Apply saved options from config shadow into live globals */
        /* Configure controller bindings for both player slots */

        /* Create 7 main menu buttons:
         * 0: SNK_RaceMenuButTxt
         * 1: SNK_QuickRaceButTxt
         * 2: SNK_TwoPlayerButTxt (or "Time Demo" in dev build)
         * 3: SNK_NetPlayButTxt
         * 4: SNK_OptionsButTxt
         * 5: SNK_HiScoreButTxt
         * 6: SNK_ExitButTxt
         */
        frontend_create_button(SNK_RaceMenuButTxt,   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_QuickRaceButTxt,  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_TwoPlayerButTxt,  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_NetPlayButTxt,    -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_OptionsButTxt,     -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_HiScoreButTxt, -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_ExitButTxt,        -0xE0, 0, 0xE0, 0x20);

        /* [CHANGELOG 2026-06-25] One extra button placed with EXPLICIT positive
         * coordinates so it does NOT consume an auto-layout slot (the auto-layout
         * branch only fires for x<0). Present in dev AND release.
         *   index 7: CHANGELOG — beside EXIT, to its right. EXIT renders at
         *            x=FE_CENTER_X-0xC2 (126), width 0xE0 (224); sit just past it.
         * (The PENDING TO TEST screen is reached from within the CHANGELOG screen.) */
        {
            int exit_right = (FE_CENTER_X - 0xC2) + s_buttons[6].w;   /* 126 + 224 = 350 */
            frontend_create_button("CHANGELOG", exit_right + 12, s_buttons[6].y, 160, s_buttons[6].h);
        }

        /* [2026-06-16] The per-button CPU surface cache (Phase 6) is retired:
         * under VectorUI every button frame is drawn procedurally per frame via
         * the ps_roundrect path, so no per-screen bake is needed. */

        frontend_set_cursor_visible(0);
        frontend_play_sfx(5); /* menu ready */
        s_inner_state = 1;
        break;

    case 1: /* Present buffer */
        frontend_present_buffer();
        s_inner_state = 2;
        break;

    case 2: /* Reset tick counter, rebuild button surfaces */
        frontend_begin_timed_animation();
        frontend_present_buffer();
        s_inner_state = 3;
        break;

    case 3: /* Slide-in animation: 7 buttons alternating L/R, title descends. 39 frames. */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            frontend_set_cursor_visible(1);
            frontend_play_sfx(4); /* ready chime */
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Main interaction loop: wait for button press */
        /* Reset attract idle on any input */
        if (s_input_ready) {
            s_attract_idle_timestamp = td5_plat_time_ms();
        }

        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* Race Menu */
                s_flow_context = 1;
                s_return_screen = TD5_SCREEN_RACE_TYPE_MENU;
                s_inner_state = 8; /* slide-out prep */
                break;

            case 1: /* Quick Race */
                /* Main Menu "Quick Race" is a single-race flow: stamp
                 * game_type=0 and run ConfigureGameTypeFlags so it copies
                 * s_game_option_traffic / s_game_option_cops into
                 * g_td5.traffic_enabled / g_td5.special_encounter_enabled.
                 * Without this, traffic stays at whatever the zero-init
                 * default is and AI traffic + cops never spawn on the track.
                 * Mirrors ConfigureGameTypeFlags @ 0x00410CA0 case 0. */
                s_selected_game_type = 0;
                ConfigureGameTypeFlags();
                s_flow_context = 2;
                s_return_screen = TD5_SCREEN_QUICK_RACE;
                s_inner_state = 8;
                TD5_LOG_I(LOG_TAG,
                          "MainMenu Quick Race: game_type=0 traffic=%d cops=%d",
                          g_td5.traffic_enabled, g_td5.special_encounter_enabled);
                break;

            case 2: /* Two Player / Time Demo
                     *
                     * Original ScreenMainMenuAnd1PRaceFlow @ 0x415BFC:
                     *   if (*(int*)(g_appExref+0x170) != 0) {
                     *       g_benchmarkModeActive = 1;
                     *       InitializeRaceSeriesSchedule();
                     *       return;
                     *   } else {
                     *       g_twoPlayerModeEnabled = 1;
                     *       g_selectedGameType = 0;
                     *   }
                     *
                     * `app+0x170` is never written anywhere in TD5_d3d.exe
                     * (zero write xrefs), so button 2 is always 2-Player in
                     * the shipped binary. The port exposes the benchmark
                     * path via the td5re.ini [Debug] EnableBenchmark=1
                     * option so that the existing TD5_GAMESTATE_BENCHMARK
                     * code path is reachable for testing.
                     * [RE basis: research agent xref scan of app+0x170] */
                if (g_td5.ini.enable_benchmark) {
                    TD5_LOG_I(LOG_TAG, "MainMenu: button 2 → benchmark mode (INI override)");
                    g_td5.benchmark_active = 1;
                    s_flow_context = 3;
                    s_return_screen = TD5_SCREEN_CAR_SELECTION;
                    s_inner_state = 8;
                } else {
                    /* [PORT ENHANCEMENT 2026-06] MULTIPLAYER → press-to-join lobby
                     * (which assigns players/devices then runs per-player car select).
                     * s_two_player_mode is engaged by the lobby's START, not here. */
                    s_flow_context = 3;
                    s_selected_game_type = 0;
                    s_return_screen = TD5_SCREEN_MP_LOBBY;
                    s_inner_state = 8;
                }
                break;

            case 3: /* Net Play */
                s_flow_context = 4;
                s_return_screen = TD5_SCREEN_CONNECTION_BROWSER;
                s_inner_state = 8;
                break;

            case 4: /* Options */
                s_flow_context = 5;
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 8;
                break;

            case 5: /* Hi-Score */
                s_flow_context = 6;
                s_return_screen = TD5_SCREEN_HIGH_SCORE;
                s_inner_state = 8;
                break;

            case 6: /* Exit -> Yes/No confirm dialog */
                TD5_LOG_I(LOG_TAG, "MainMenu: Exit pressed, showing confirm dialog");
                s_inner_state = 5;
                break;

            case 7: /* Changelog (button beside EXIT; PENDING TO TEST lives inside it) */
                s_return_screen = TD5_SCREEN_CHANGELOG;
                s_inner_state = 8;
                break;
            }
        }
        break;

    case 5: { /* Exit confirm dialog: create YES / NO! buttons */
        int exit_x = s_buttons[6].x;
        int exit_y = s_buttons[6].y;
        int exit_w = s_buttons[6].w;
        int exit_h = s_buttons[6].h;
        /* Split the EXIT button's footprint into two equal buttons with a clear
         * gap so YES and NO! read as two distinct, easy-to-hit targets (the old
         * layout left only a 4px gap between two 96px buttons). The pair is
         * centered under EXIT by construction. */
        const int yn_gap = 24;
        int yn_w = (exit_w - yn_gap) / 2;
        if (yn_w < 80) yn_w = 80;                 /* floor for unusually narrow EXIT */
        int yn_y = exit_y + exit_h + 10;
        int yes_idx = frontend_create_button(SNK_YesButTxt, exit_x,                  yn_y, yn_w, 32);
        int no_idx  = frontend_create_button(SNK_NoxButTxt,  exit_x + yn_w + yn_gap, yn_y, yn_w, 32);
        s_exit_confirm_yes_idx = yes_idx;
        s_exit_confirm_no_idx  = no_idx;
        if (yes_idx >= 0) s_selected_button = yes_idx;
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirm dialog created yes=%d no=%d", yes_idx, no_idx);
        s_inner_state = 6;
        break;
    }

    case 6: /* Exit confirm: wait for YES / NO! */
        if (s_input_ready && s_button_index >= 0) {
            /* Dispatch by the indices recorded in state 5, NOT by label text.
             * The SNK labels are "YES" / "NO!" (td5_snk_strings.h), so the old
             * strcmp(label,"Yes")/strcmp(label,"No") never matched and EXIT did
             * nothing. Index compare also tolerates the button-pool slot the
             * dialog happens to land in. (yes/no idx are -1 if creation failed,
             * and s_button_index is >= 0 here, so a failed button can't match.) */
            if (s_button_index == s_exit_confirm_yes_idx) {
                TD5_LOG_I(LOG_TAG, "MainMenu: exit YES selected, quitting");
                s_inner_state = 7;
            } else if (s_button_index == s_exit_confirm_no_idx) {
                /* Drop the YES / NO! buttons (release by index so the render loop,
                 * which gates on .active, actually stops drawing them) and return
                 * to the menu. */
                frontend_release_button(s_exit_confirm_yes_idx);
                frontend_release_button(s_exit_confirm_no_idx);
                s_exit_confirm_yes_idx = -1;
                s_exit_confirm_no_idx  = -1;
                /* Drop the YES/NO dialog buttons but KEEP the 8 menu buttons
                 * (0..6 column + 7 CHANGELOG). [2026-06-25] */
                if (s_button_count > 8) s_button_count = 8;
                s_selected_button = 6; /* re-focus on Exit */
                TD5_LOG_I(LOG_TAG, "MainMenu: exit NO selected, returning to menu");
                s_inner_state = 4;
            }
        }
        break;

    case 7: /* Confirm exit -- navigate to credits then quit */
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirmed, going to credits");
        td5_frontend_set_screen(TD5_SCREEN_EXTRAS_GALLERY);
        break;

    case 8: /* Slide-out prep: keep the software cursor visible for the next frontend screen */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out animation: buttons scatter, ~500ms.
             *
             * [ARCH-DIVERGENCE] Orig 0x004155DE checks the per-player input
             * source (joystick index 7 = none) before navigating, and routes
             * to states 0x14-0x17 (controller-required dialog → ControlOptions)
             * when a joystick is configured but missing. The port is
             * keyboard-first — `td5_plat_input_get_keyboard()` is always
             * available — so a missing joystick can never block navigation.
             * The validation gate and its dialog states are intentionally
             * dropped; replace with a direct screen transition. */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    case 10: /* Post-Yes exit: release buttons */
    case 11:
        frontend_post_quit();
        break;

    case 12: /* Scatter buttons for exit transition (~500ms) */
        if (s_anim_start_ms == 0) frontend_begin_timed_animation();
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            frontend_post_quit();
        }
        break;
    }
}

void Screen_RaceTypeCategory(void) {
    switch (s_inner_state) {
    case 0: /* Init: load MainMenu.tga, create 7 race-type buttons */
        frontend_init_return_screen(TD5_SCREEN_RACE_TYPE_MENU);
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: state 0 - init");
        /* [DA-T4 fix 2026-05-22] Clear wanted-mode flag — orig 0x004168D7 sets
         * g_wantedModeEnabled = 0 here. If user backs out of cop-chase race
         * without this, the wanted-HUD overlay can persist into the race-type
         * menu on the next entry. */
        g_td5.wanted_mode_enabled = 0;
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip"); /* original 0x4168B0: loads MainMenu.tga, not RaceMenu.tga */
        s_anim_complete = 0;

        /* [ARCH-DIVERGENCE] Orig allocated a 0x110x0xB4 DDraw surface here for
         * the description preview (`g_lobbyErrorDialogSurface` @ 0x004168D7) and
         * blitted text into it from state 4 on hover. Port renders the preview
         * directly per-frame via `frontend_render_race_type_description`
         * (td5_frontend.c:4021, dispatched at :5362), so no intermediate
         * surface is needed and the old state-4 update step is unreachable. */
        /* Create 7 buttons for race types.
         * [FIXED 2026-06-01, runtime @0x499c78] rows 0-5 at x=120, y=97 step40 (224-wide);
         * BACK bottom-center (176,377), half-width 0x70 (112). Port auto-layout gave 110/93 +
         * BACK stacked in-column at full width. Slide-in still animates X to rest. */
        frontend_create_button(SNK_SingleRaceButTxt, 120,  97, 0xE0, 0x20);
        frontend_create_button(SNK_CupRaceButTxt,    120, 137, 0xE0, 0x20);
        /* Continue Cup: greyed if no valid CupData.td5 */
        if (frontend_validate_cup_checksum())
            frontend_create_button(SNK_ContCupButTxt, 120, 177, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_ContCupButTxt, 120, 177, 0xE0, 0x20);
        frontend_create_button(SNK_TimeTrialsButTxt, 120, 217, 0xE0, 0x20);
        frontend_create_button(SNK_DragRaceButTxt,   120, 257, 0xE0, 0x20);
        frontend_create_button(SNK_CopChaseButTxt,   120, 297, 0xE0, 0x20);
        frontend_create_button(SNK_BackButTxt,       176, 377, 0x70, 0x20);

        s_selected_game_type = -1;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        if (frontend_update_timed_animation(0x20, 533) >= 1.0f) {
            s_inner_state = 2;
        }
        break;

    case 2: /* Tick until AdvanceFrontendTickAndCheckReady */
        if (frontend_advance_tick()) {
            s_anim_complete = 1;
            frontend_play_sfx(4); /* slide-in settle chime — the original
                                   * RaceTypeCategory plays Play(4) when the
                                   * slide-in completes [CONFIRMED @ 0x4168B0];
                                   * the port was missing it, so the race menu
                                   * appeared without the settle chime that every
                                   * other screen has. */
            s_inner_state = 3;
        }
        break;

    case 3: /* Main interaction loop */
        /* Buttons render via the standard frontend pass; description preview
         * is driven by s_selected_button (hover index) per-frame — no explicit
         * "preview update" state needed (see ARCH-DIVERGENCE note in state 0). */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* Single Race */
                s_selected_game_type = 0;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 1: /* Cup Race -> enter sub-menu */
                s_inner_state = 6;
                break;

            case 2: /* Continue Cup */
                if (frontend_validate_cup_checksum()) {
                    frontend_load_continue_cup_data();
                    s_return_screen = TD5_SCREEN_RACE_RESULTS;
                    s_inner_state = 5;
                } else {
                    frontend_play_sfx(10); /* rejection */
                }
                break;

            case 3: /* Time Trials */
                s_selected_game_type = 7;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 4: /* Drag Race */
                s_selected_game_type = 9;
                s_drag_carselect_pass = 0;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 5: /* Cop Chase */
                s_selected_game_type = 8;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 6: /* Back */
                s_return_screen = (s_network_active) ? TD5_SCREEN_CREATE_SESSION : TD5_SCREEN_MAIN_MENU;
                s_inner_state = 5;
                break;
            }
        }
        break;

    case 5: /* Slide-out prep: buttons scatter */
        frontend_play_sfx(5); /* slide-out whoosh — was missing here, so leaving
                               * the race menu to another screen was silent. Every
                               * other slide-out prep plays it (cf. Screen_MainMenu
                               * case 8). Placed at prep (not the 0x14 animation),
                               * matching the one-whoosh-per-transition pattern. */
        frontend_begin_timed_animation();
        s_inner_state = 0x14;
        break;

    /* --- Cup sub-menu (states 6-12) --- */

    case 6: /* Cup sub-menu: release top buttons, create cup tier buttons */
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: entering cup sub-menu");
        frontend_reset_buttons();
        /* Create 7 cup tier buttons */
        frontend_create_button(SNK_ChampionshipButTxt, -0xE0, 0, 0xE0, 0x20); /* always available */
        frontend_create_button(SNK_EraButTxt,          -0xE0, 0, 0xE0, 0x20); /* always available */

        /* Challenge: locked if s_cup_unlock_tier == 0 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button(SNK_ChallengeButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_ChallengeButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Pitbull: locked if s_cup_unlock_tier < 1 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button(SNK_PitbullButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_PitbullButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Masters: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button(SNK_MastersButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_MastersButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Ultimate: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button(SNK_UltimateButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_UltimateButTxt, -0xE0, 0, 0xE0, 0x20);

        frontend_create_button(SNK_BackButTxt, -0xE0, 0, 0xE0, 0x20);

        frontend_begin_timed_animation();
        s_inner_state = 7;
        break;

    case 7: /* Cup sub-menu slide-in: ~1000ms */
        if (frontend_update_timed_animation(0x20, 533) >= 1.0f) {
            s_inner_state = 8;
        }
        break;

    case 8: /* Cup sub-menu tick */
        if (frontend_advance_tick()) {
            s_inner_state = 9;
        }
        break;

    case 9: /* Cup sub-menu interaction */
        if (s_input_ready && s_button_index >= 0) {
            int cup_type = -1;
            switch (s_button_index) {
            case 0: cup_type = 1; break; /* Championship */
            case 1: cup_type = 2; break; /* Era */
            case 2: /* Challenge */
                if (s_cup_unlock_tier >= 1) cup_type = 3;
                else frontend_play_sfx(10);
                break;
            case 3: /* Pitbull */
                if (s_cup_unlock_tier >= 1) cup_type = 4;
                else frontend_play_sfx(10);
                break;
            case 4: /* Masters */
                if (s_cup_unlock_tier >= 2) cup_type = 5;
                else frontend_play_sfx(10);
                break;
            case 5: /* Ultimate */
                if (s_cup_unlock_tier >= 2) cup_type = 6;
                else frontend_play_sfx(10);
                break;
            case 6: /* Back to top-level */
                s_inner_state = 11;
                break;
            }
            if (cup_type >= 0) {
                s_selected_game_type = cup_type;
                s_race_within_series = 0;
                /* [CUP TRACK SELECT 2026-06-25] New cup → discard any prior
                 * player-chosen track list so the forked picker re-triggers at
                 * car-select (and the faithful schedule path stays the fallback). */
                s_cup_user_active = 0;
                /* [DA-T4 D.3 fix 2026-05-22] orig 0x004171F0 region:
                 *   g_selectedScheduleIndex = g_attractModeTrackIndex;
                 * Seeds the race's schedule index from the attract-mode
                 * preview track. Port previously skipped this — cup races
                 * could start on stale/wrong track. */
                s_selected_track = s_attract_track;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 10;
            }
        }
        break;

    case 10: /* Cup sub-menu slide-out -> Car Selection */
        s_anim_tick = 0;
        s_inner_state = 0x14;
        break;

    case 11: /* Back to top-level: rebuild top buttons */
        frontend_reset_buttons();
        s_inner_state = 0; /* re-init top menu */
        break;

    /* --- Return transition --- */
    case 0x14: /* Slide-out animation (~500ms), then navigate */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    default:
        break;
    }
}

void Screen_OptionsHub(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        frontend_init_return_screen(TD5_SCREEN_OPTIONS_HUB);
        TD5_LOG_D(LOG_TAG, "OptionsHub: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;

        /* [FIXED 2026-06-01, runtime @0x499c78] rows x=120 y=97 step40 (304-wide); OK (216,377) 96-wide. */
        frontend_create_button(SNK_GameOptionsButTxt,      120,  97, 0x130, 0x20);
        frontend_create_button(SNK_ControlOptionsButTxt,   120, 137, 0x130, 0x20);
        frontend_create_button(SNK_SoundOptionsButTxt,     120, 177, 0x130, 0x20);
        frontend_create_button(SNK_GraphicsOptionsButTxt,  120, 217, 0x130, 0x20);
        frontend_create_button(SNK_TwoPlayerOptionsButTxt, 120, 257, 0x130, 0x20);
        frontend_create_button(SNK_OkButTxt,               216, 377, 0x60,  0x20);

        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: /* Present */
    case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Animation stabilize */
    case 5:
        s_inner_state++;
        break;

    case 6: /* Interaction */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: s_return_screen = TD5_SCREEN_GAME_OPTIONS;       s_inner_state = 7; break;
            case 1: s_return_screen = TD5_SCREEN_CONTROL_OPTIONS;    s_inner_state = 7; break;
            case 2: s_return_screen = TD5_SCREEN_SOUND_OPTIONS;      s_inner_state = 7; break;
            case 3: s_return_screen = TD5_SCREEN_DISPLAY_OPTIONS;    s_inner_state = 7; break;
            case 4: s_return_screen = TD5_SCREEN_TWO_PLAYER_OPTIONS; s_inner_state = 7; break;
            case 5: /* OK -> return to main menu.
                     * PARITY NOTE (audit 2026-05-30): the original 0x0041D890 OK case
                     * commits the option shadows to live globals here (camera =
                     * collisions^1 @0x41dc8e, dynamics @0x41dc82, traffic/cops, and
                     * gRaceDifficultyTier @0x41dc9f). The port uses an equivalent but
                     * DEFERRED model: the option screens edit s_game_option_* directly
                     * and ConfigureGameTypeFlags applies them at race launch (see
                     * td5_frontend.c case 0 of the game-type switch: difficulty->tier,
                     * traffic, cops, collisions, dynamics, checkpoint timers). Race
                     * launch is the sole consumer path, so the user's choices still take
                     * effect without an explicit shadow->live commit here. Adding one
                     * would risk double-application. Left as-is intentionally. */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 7;
                break;
            }
        }
        break;

    case 7: /* Slide-out prep */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;

    case 9: /* Exit */
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

void Screen_GameOptions(void) {
    switch (s_inner_state) {
    case 0: /* Init: create option rows + OK */
        frontend_init_return_screen(TD5_SCREEN_GAME_OPTIONS);
        TD5_LOG_D(LOG_TAG, "GameOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* 6 option rows with left/right arrows:
         * Checkpoint Timers, Traffic, Cops, Difficulty, Dynamics, 3D Collisions.
         * [S02 (c) 2026-06-04] CIRCUIT LAPS was removed from this screen — the lap
         * count is now set in the Quick Race menu + the Track Selection screen
         * (both edit s_game_option_laps). The value still feeds race setup via
         * g_td5.circuit_lap_count = s_game_option_laps + 1; this screen no longer
         * owns it. Remaining rows shifted up one slot to close the gap. */
        /* [FIXED 2026-06-01, runtime button-table @0x499c78] explicit rests: rows at x=120,
         * y=97 step 40; OK bottom-center (216,377). (Port auto-layout gave 110/93 + OK stacked
         * in-column — 10px left / 4px high / OK mis-placed.) Slide-in still animates X to rest. */
        /* [ARCADE 2026-06-26] DYNAMICS (ARCADE/SIMULATION) row removed from Game
         * Options — it is now chosen on the Track Selection + Multiplayer screens
         * since it drives major gameplay (3x crashes, power-ups). Remaining rows
         * shifted up: 3D Collisions 5->4, OK 6->5. */
        frontend_create_button(SNK_CheckpointTimersButTxt, 120,  97, 0x128, 0x20);
        frontend_create_button(SNK_TrafficButTxt,          120, 137, 0x128, 0x20);
        frontend_create_button(SNK_CopsButTxt,             120, 177, 0x128, 0x20); /* orig label: POLICE */
        frontend_create_button(SNK_DifficultyButTxt,       120, 217, 0x128, 0x20);
        frontend_create_button(SNK_3dCollisionsButTxt,     120, 257, 0x128, 0x20);
        frontend_create_button(SNK_OkButTxt,               216, 377, 0x60,  0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Draw current values */
    case 5:
        /* Render current option values on the panel */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: arrow handlers per row */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            /* Each row cycles its respective global on arrow input.
             * OK button triggers exit. [S02 (c) 2026-06-04] Circuit Laps (old
             * idx 0) was removed; the remaining six rows shifted up one index. */
            if (delta != 0 && active_button >= 0 && active_button <= 4) {
                /* Nav beep on any selector-row change, matching the original's
                 * central arrow handler (DXSound::Play(2) @ 0x0042687c) and the
                 * other Options screens (Control/Sound). Rows 0..4 are all
                 * arrow-capable selectors; OK (row 5) is handled separately below
                 * and is not arrow-capable, so it stays silent on L/R.
                 * [ARCADE 2026-06-26] DYNAMICS row removed; Collisions is now 4. */
                frontend_play_sfx(2);
                if (active_button == 0) {
                    s_game_option_checkpoint_timers ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 1) {
                    /* [dynamic-traffic] 5-state volume: Off/Light/Normal/Heavy/
                     * Very-High, direction-aware so LEFT steps back through the
                     * cycle (delta may be negative). */
                    s_game_option_traffic =
                        ((s_game_option_traffic + delta) % TD5_TRAFFIC_VOLUME_COUNT
                         + TD5_TRAFFIC_VOLUME_COUNT) % TD5_TRAFFIC_VOLUME_COUNT;
                    s_inner_state = 4;
                } else if (active_button == 2) {
                    s_game_option_cops ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 3) {
                    s_game_option_difficulty += delta;
                    if (s_game_option_difficulty < 0) s_game_option_difficulty = 2;
                    if (s_game_option_difficulty > 2) s_game_option_difficulty = 0;
                    s_inner_state = 4;
                } else if (active_button == 4) {
                    s_game_option_collisions ^= 1;
                    s_inner_state = 4;
                }
            }
            if (s_button_index == 5) { /* OK */
                /* Sync the committed game options into g_td5.ini (the global the
                 * boot-override at frontend init reads) and write them back to
                 * td5re.ini so the selection survives a relaunch. The original
                 * persisted these to Config.td5 only, but the port's td5re.ini
                 * boot-override masks Config.td5, so the ini is the live config
                 * layer that must be kept in sync. [PART B 2026-06-02]
                 * NB: laps is intentionally NOT written here anymore — this
                 * screen no longer owns it (re-homed to Quick Race + Track
                 * Selection, which persist g_td5.ini.laps themselves).
                 * [S02 (c) 2026-06-04] */
                g_td5.ini.checkpoint_timers = s_game_option_checkpoint_timers;
                /* [dynamic-traffic] Persist the full 0..4 volume (5-state row). */
                g_td5.ini.traffic           = s_game_option_traffic;
                if (g_td5.ini.traffic < 0) g_td5.ini.traffic = 0;
                if (g_td5.ini.traffic > TD5_TRAFFIC_VOLUME_COUNT - 1)
                    g_td5.ini.traffic = TD5_TRAFFIC_VOLUME_COUNT - 1;
                g_td5.ini.cops              = s_game_option_cops;
                g_td5.ini.difficulty        = s_game_option_difficulty;
                g_td5.ini.dynamics          = s_game_option_dynamics;
                g_td5.ini.collisions        = s_game_option_collisions;
                td5_ini_persist_options();
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
            /* Arrow changes reset to state 4 for redraw */
        }
        break;

    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;

    case 9:
        td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        break;
    }
}

/* Recompute the player range from the live device count ("hot-swap") and re-seed
 * each player's source from the persisted device index, dropping any index that
 * now points at an unplugged device. */
static void ctrl_opts_refresh_devices(void)
{
    int dev_count = td5_input_enumerate_devices();
    int joys, n, p;
    if (dev_count < 1) dev_count = 1;
    joys = dev_count - 1;            /* device 0 = keyboard */
    if (joys < 0) joys = 0;
    /* The PLAYER selector lets you configure ANY of the up-to-9 split-screen
     * players, independent of how many joysticks are plugged in. The keyboard is
     * shareable and the per-player device is picked on the CONTROLLER SELECTION
     * row, so capping the player count at the joystick count was wrong — with N
     * pads you could only reach PLAYER N, blocking setups like "keyboard + pads"
     * or more players than pads. (The race's human count is still chosen on the
     * Quick Race / Multiplayer Options screens; this only governs which player
     * you are configuring here.) */
    n = TD5_MAX_HUMAN_PLAYERS;
    if (n < s_num_human_players) n = s_num_human_players;
    if (n < 1) n = 1;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    s_ctrl_opts_max_players = n;
    if (s_ctrl_opts_player >= n) s_ctrl_opts_player = n - 1;
    if (s_ctrl_opts_player < 0)  s_ctrl_opts_player = 0;

    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) {
        int idx = (int)td5_save_get_player_device_index(p);
        if (idx < 0 || idx >= dev_count) idx = 0;   /* unplugged → keyboard */
        td5_input_set_input_source(p, idx);
    }
    TD5_LOG_I(LOG_TAG, "ControlOptions: devices=%d joys=%d range=1..%d player=%d",
              dev_count, joys, s_ctrl_opts_max_players, s_ctrl_opts_player + 1);
}

/* Cycle the selected player's input device by delta. Keyboard (0) is always
 * available + shareable; joysticks (>=1) are exclusive (skip any already taken
 * by another player). */
static void ctrl_opts_cycle_device(int delta)
{
    int dev_count = td5_input_enumerate_devices();
    int src, guard = 0;
    if (dev_count < 1) dev_count = 1;
    src = td5_input_get_input_source(s_ctrl_opts_player);
    do {
        int taken = 0, p;
        src += delta;
        if (src < 0) src = dev_count - 1;
        if (src >= dev_count) src = 0;
        if (src == 0) break;                       /* keyboard: always OK / shareable */
        for (p = 0; p < s_ctrl_opts_max_players; p++) {
            if (p == s_ctrl_opts_player) continue;
            if (td5_input_get_input_source(p) == src) { taken = 1; break; }
        }
        if (!taken) break;
    } while (++guard < dev_count * 2);
    td5_input_set_input_source(s_ctrl_opts_player, src);
    td5_save_set_player_device_index(s_ctrl_opts_player, (uint32_t)src);
    TD5_LOG_I(LOG_TAG, "ControlOptions: player %d device -> %d",
              s_ctrl_opts_player + 1, src);
}

void Screen_ControlOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_CONTROL_OPTIONS);
        TD5_LOG_D(LOG_TAG, "ControlOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Controllers.tga icon must be BLACK-color-keyed (orig DDCKEY_SRCBLT @0x412030). */
        s_control_options_surface = frontend_load_tga_ck("Controllers.TGA", "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        /* Hot-swap: re-enumerate devices + refresh range/sources on every entry. */
        ctrl_opts_refresh_devices();
        /* Non-selector rows (labels bake); ◄► arrows drawn by the per-screen
         * dispatch, values by the overlay. PLAYER(0) + CONTROLLER SELECTION(1)
         * cycle on ◄►; CONFIGURE(2) + OK(3) are plain buttons. */
        frontend_reset_buttons();
        frontend_create_button(SNK_PlayerSelectButTxt,     120,  97, 0x100, 0x20);  /* 0 */
        frontend_create_button(SNK_ControllerSelectButTxt, 120, 177, 0x100, 0x20);  /* 1 */
        frontend_create_button(SNK_ConfigureButTxt,        120, 257, 0x100, 0x20);  /* 2 */
        frontend_create_button(SNK_OkButTxt,               200, 377, 0x60,  0x20);  /* 3 */
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4:
        s_inner_state = 5;
        break;
    case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int delta = frontend_option_delta();
            if (active_button == 0 && delta != 0) {
                /* PLAYER selector (wrap within the live range). */
                int n = s_ctrl_opts_player + delta;
                int range = (s_ctrl_opts_max_players > 0) ? s_ctrl_opts_max_players : 1;
                while (n < 0) n += range;
                n %= range;
                s_ctrl_opts_player = n;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == 1 && delta != 0) {
                /* CONTROLLER SELECTION device cycle. */
                ctrl_opts_cycle_device(delta);
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) {
                /* CONFIGURE → per-button remap for the selected player. */
                s_ctrl_player = s_ctrl_opts_player;
                s_return_screen = TD5_SCREEN_CONTROLLER_BINDING;
                s_inner_state = 7;
            } else if (s_button_index == 3) {
                /* OK → persist device assignments + return to hub. */
                td5_save_write_config(NULL);
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;
    case 9:
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

void Screen_SoundOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_SOUND_OPTIONS);
        TD5_LOG_D(LOG_TAG, "SoundOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [CONFIRMED @ 0x0041EA90] SFX-mode icon source is Controllers.tga (64x224 =
         * 7 rows of 64x32), blitted at row (sfx_mode+4): modes 0/1/2 -> rows 4/5/6.
         * Replaces the prior 2-state Stereo.tga/Mono.tga pair, which could not render
         * a distinct icon for the 3rd (surround) mode. */
        /* [FIXED 2026-06-01] black-color-keyed like the Control Options icon (was opaque
         * black box). Controllers.tga background is black; key it transparent. */
        s_sound_volumebox_surface  = frontend_load_tga("VolumeBox.tga", "Front End/frontend.zip");
        s_sound_volumefill_surface = frontend_load_tga("VolumeFill.tga","Front End/frontend.zip");
        /* [PORT REWORK 2026-06-05 / S15] SFX Mode row (was 120,97 + the
         * Controllers.tga icon load) removed. Remaining rows reflowed up one
         * slot, keeping their original 40/80/80 spacing:
         *   SFX Volume 97, Music Volume 137, Music Test 217, OK 297. */
        frontend_create_button(SNK_SfxVolumeButTxt,   120,  97, 0x100, 0x20);  /* btn 0 */
        frontend_create_button(SNK_MusicVolumeButTxt, 120, 137, 0x100, 0x20);  /* btn 1 */
        frontend_create_button(SNK_MusicTestButTxt,   120, 217, 0x100, 0x20);  /* btn 2 */
        frontend_create_button(SNK_OkButTxt,          200, 297, 0x60,  0x20);  /* btn 3 */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4: case 5:
        /* Render volume bars */
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            /* [PORT REWORK 2026-06-05 / S15] SFX Mode row removed; sliders are
             * now button 0 (SFX volume) and button 1 (Music volume). */
            if (delta != 0 && active_button >= 0 && active_button <= 1) {
                if (active_button == 0) {
                    /* SFX volume. REG-2 fix 2026-05-22: orig step is delta * 10. */
                    s_sound_option_sfx_volume += delta * 10;
                    if (s_sound_option_sfx_volume < 0) s_sound_option_sfx_volume = 0;
                    if (s_sound_option_sfx_volume > 100) s_sound_option_sfx_volume = 100;
                    td5_save_set_sfx_volume(s_sound_option_sfx_volume);
                    td5_sound_set_sfx_volume(s_sound_option_sfx_volume);
                } else { /* active_button == 1: Music volume */
                    /* REG-2 fix 2026-05-22: orig step delta * 10. */
                    s_sound_option_music_volume += delta * 10;
                    if (s_sound_option_music_volume < 0) s_sound_option_music_volume = 0;
                    if (s_sound_option_music_volume > 100) s_sound_option_music_volume = 100;
                    td5_save_set_music_volume(s_sound_option_music_volume);
                    td5_sound_set_music_volume(s_sound_option_music_volume);
                }
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) { /* Music Test */
                s_return_screen = TD5_SCREEN_MUSIC_TEST;
                s_inner_state = 7;
            } else if (s_button_index == 3) { /* OK */
                /* Persist sound options to td5re.ini so they survive a relaunch
                 * (see PART B note in Screen_GameOptions). Volume changes already
                 * applied live via td5_save_set_*; sync the committed values into
                 * g_td5.ini and write them back. [PART B 2026-06-02]
                 * sfx_mode is no longer user-editable here (row removed) but is
                 * still written so its loaded value is preserved across the save. */
                g_td5.ini.sfx_mode     = s_sound_option_sfx_mode;
                g_td5.ini.sfx_volume   = s_sound_option_sfx_volume;
                g_td5.ini.music_volume = s_sound_option_music_volume;
                td5_ini_persist_options();
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;
    case 9:
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

void Screen_DisplayOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_DISPLAY_OPTIONS);
        TD5_LOG_D(LOG_TAG, "DisplayOptions: init (display_mode_index=%d, count=%d)",
                  s_display_mode_index, s_display_mode_count);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [S01 Display options 2026-06-04] 6 option rows + OK. The discrete
         * Resolution row was removed — the window is now freely resizable
         * (drag the border / maximize, or pick Borderless/Fullscreen via Display
         * Mode), so a fixed resolution list is redundant. Labels are overridden
         * by frontend_refresh_display_option_labels (the SNK arg is a placeholder).
         * Rows top-to-bottom:
         *   0 Display Mode  1 VSync  2 Fogging
         *   3 Speed Readout 4 Show FPS  5 Camera Damping  6 OK
         * PARITY NOTE (audit 2026-05-30): orig 0x00420484 makes Fogging a DISABLED
         * preview button when DXD3D::CanFog()!=1; the D3D11 backend always supports
         * fog so the faithful result is an always-live Fogging row. */
        frontend_create_button(SNK_ResolutionButTxt,    120,  97, 0x120, 0x20); /* Display Mode */
        frontend_create_button(SNK_FoggingButTxt,       120, 137, 0x120, 0x20); /* VSync */
        frontend_create_button(SNK_FoggingButTxt,       120, 177, 0x120, 0x20); /* Fogging */
        frontend_create_button(SNK_SpeedReadoutButTxt,  120, 217, 0x120, 0x20); /* Speed Readout */
        frontend_create_button(SNK_SpeedReadoutButTxt,  120, 257, 0x120, 0x20); /* Show FPS */
        frontend_create_button(SNK_CameraDampingButTxt, 120, 297, 0x120, 0x20); /* Camera Damping */
        frontend_create_button(SNK_OkButTxt,            200, 377, 0x60,  0x20); /* OK */
        frontend_refresh_display_option_labels();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4:
        frontend_refresh_display_option_labels();
        s_inner_state = 5;
        break;
    case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int changed = 0;

            if (active_button == 0 && delta != 0) {
                /* Row 0 — Display Mode: Fullscreen(0) -> Windowed(1) -> Borderless(2) */
                s_display_window_mode += delta;
                if (s_display_window_mode < 0) s_display_window_mode = 2;
                if (s_display_window_mode > 2) s_display_window_mode = 0;
                g_td5.ini.window_mode = s_display_window_mode;
                td5_plat_set_window_mode(s_display_window_mode);
                changed = 1;
            } else if (active_button == 1 && delta != 0) {
                /* Row 1 — VSync on/off (applied live) */
                s_display_vsync = !s_display_vsync;
                g_td5.ini.vsync = s_display_vsync;
                td5_plat_set_vsync(s_display_vsync);
                changed = 1;
            } else if (active_button == 2 && delta != 0) {
                /* Row 2 — Fogging on/off */
                s_display_fog_enabled = !s_display_fog_enabled;
                g_td5.ini.fog_enabled = s_display_fog_enabled;
                changed = 1;
            } else if (active_button == 3 && delta != 0) {
                /* Row 3 — Speed Readout MPH/KPH (applied live to the HUD) */
                s_display_speed_units = !s_display_speed_units;
                g_td5.ini.speed_units = s_display_speed_units;
                td5_save_set_speed_units(s_display_speed_units);
                changed = 1;
            } else if (active_button == 4 && delta != 0) {
                /* Row 4 — Show FPS overlay on/off */
                s_display_show_fps = !s_display_show_fps;
                g_td5.ini.show_fps = s_display_show_fps;
                changed = 1;
            } else if (active_button == 5 && delta != 0) {
                /* Row 5 — Camera Damping 0..9 (clamp, no wrap) */
                s_display_camera_damping += delta;
                if (s_display_camera_damping < 0) s_display_camera_damping = 0;
                if (s_display_camera_damping > 9) s_display_camera_damping = 9;
                changed = 1;
            } else if (s_button_index == 6) {
                /* OK — persist every display option to td5re.ini. Resolution +
                 * window-mode/vsync already applied live; this writes them (plus
                 * fog / units / damping / W,H) so they survive a relaunch. */
                g_td5.ini.window_mode    = s_display_window_mode;
                g_td5.ini.vsync          = s_display_vsync;
                g_td5.ini.show_fps       = s_display_show_fps;
                g_td5.ini.fog_enabled    = s_display_fog_enabled;
                g_td5.ini.speed_units    = s_display_speed_units;
                g_td5.ini.camera_damping = s_display_camera_damping;
                td5_save_set_speed_units(s_display_speed_units);
                td5_ini_persist_options();
                s_inner_state = 7;
                break;
            }

            if (changed) {
                /* Nav beep on any selector-row change (rows 0..5), matching the
                 * original's central arrow handler (DXSound::Play(2) @ 0x0042687c)
                 * and the other Options screens. OK (row 6) breaks out above and
                 * is not arrow-capable, so it stays silent on L/R. */
                frontend_play_sfx(2);
                s_inner_state = 4;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        }
        break;
    }
}

/* [#9 2026-06-16] Knob: TD5RE_MP_OPT_NO_SPLIT (default ON). The split-screen
 * layout is now chosen in the multiplayer "choose your screen" position screen
 * (TD5_SCREEN_MP_POSITION), so the Multiplayer Options screen no longer owns it.
 * Default ON drops the SPLIT LAYOUT selector row AND the dependent empty-cell
 * (DISPLAY k) rows, re-flowing the remaining rows. Set TD5RE_MP_OPT_NO_SPLIT=0
 * to restore the old rows. The underlying state (s_mp_layout_sel /
 * s_mp_missing_content, defined in td5_frontend.c and still consumed by the
 * race-init layout resolve + the position screen) is left untouched. */
static int mp_opt_no_split(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_OPT_NO_SPLIT");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG,
                  "MP Options SPLIT LAYOUT row (#9) %s (TD5RE_MP_OPT_NO_SPLIT=%s)",
                  v ? "REMOVED" : "shown", e ? e : "default");
    }
    return v;
}

/* (Re)build the Multiplayer Options row buttons for the current player count. */
static void mp_build_buttons(void)
{
    int n = s_num_human_players;
    int cols = 0, rows = 0, missing = 0;
    int y;
    if (n < 1) n = 1;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    s_num_human_players = n;

    mp_split_layouts(n, &s_mp_layout_optcount);
    if (s_mp_layout_sel < 0 || s_mp_layout_sel >= s_mp_layout_optcount)
        s_mp_layout_sel = 0;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    /* [#9] When the SPLIT LAYOUT row is removed, the dependent empty-cell rows go
     * with it: zero the missing count so neither the rows below nor the td5_frontend.c
     * value/arrow render paths (which gate on s_mp_missing_count) appear. */
    if (mp_opt_no_split())
        missing = 0;
    s_mp_missing_count = missing;

    frontend_reset_buttons();
    s_mp_btn_players = s_mp_btn_catchup = s_mp_btn_layout = s_mp_btn_ok = -1;
    s_mp_btn_missing[0] = s_mp_btn_missing[1] = -1;
    s_mp_btn_nickname = -1;

    /* Rows are NON-selector buttons: the button renderer bakes their label (the
     * ◄► arrows are drawn by the per-screen dispatch, the value by the overlay).
     * This matches the Game/Sound/Display Options pattern; a real selector button
     * suppresses its label, which would leave the row blank. */
    y = 77;
    s_mp_btn_players = frontend_create_button(SNK_MpPlayersButTxt, 120, y, 0x100, 0x20);
    y += 50;
    /* [S05 2026-06-04] CATCHUP toggle row, between PLAYERS and SPLIT LAYOUT. The
     * value is the persisted AI rubber-band assist (td5_save get/set_catchup_assist,
     * default 1 = on); S06's td5_ai_get_catchup_level() consumes it (ON = softened
     * rubber-band, OFF = no player-distance boost/cut). */
    s_mp_btn_catchup = frontend_create_button(SNK_CatchupTxt, 120, y, 0x100, 0x20);
    y += 50;
    /* [#9] SPLIT LAYOUT selector + its empty-cell (DISPLAY k) rows are owned by
     * the MP "choose your screen" position screen now; default-on knob skips both
     * (s_mp_btn_layout / s_mp_btn_missing[] stay -1, missing is forced 0 above) so
     * the rows below re-flow up with no gap. TD5RE_MP_OPT_NO_SPLIT=0 restores them. */
    if (!mp_opt_no_split()) {
        s_mp_btn_layout = frontend_create_button(SNK_MpLayoutButTxt, 120, y, 0x100, 0x20);
        y += 50;
    }
    for (int k = 0; k < missing && k < 2; k++) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%s %d", SNK_MpDisplayButTxt, k + 1);
        s_mp_btn_missing[k] = frontend_create_button(lbl, 120, y, 0x100, 0x20);
        y += 50;
    }

    /* S10: NICKNAME row sits dynamically BELOW the split-layout + missing-cell
     * rows (whose count varies with player count / layout). Pressing it opens
     * the nickname-entry screen; the current nickname is shown as its value. */
    s_mp_btn_nickname = frontend_create_button("NICKNAME", 120, y, 0x100, 0x20);
    y += 50;

    s_mp_btn_ok = frontend_create_button(SNK_OkButTxt, 200, 377, 0x60, 0x20);

    TD5_LOG_I(LOG_TAG,
              "MultiplayerOptions buttons: n=%d optcount=%d missing=%d grid=%dx%d",
              n, s_mp_layout_optcount, missing, cols, rows);
}

void Screen_TwoPlayerOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_TWO_PLAYER_OPTIONS);
        TD5_LOG_D(LOG_TAG, "MultiplayerOptions: init players=%d layout_sel=%d",
                  s_num_human_players, s_mp_layout_sel);
        /* SplitScreen.tga (split-layout preview icon) — drawn OPAQUE, no colorkey. */
        s_split_screen_surface = frontend_load_tga("SplitScreen.tga", "Front End/frontend.zip");
        mp_build_buttons();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4: case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;

            if (active_button == s_mp_btn_players && delta != 0) {
                int n = s_num_human_players + delta;
                if (n < 1) n = 1;
                if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
                if (n != s_num_human_players) {
                    s_num_human_players = n;
                    if (s_num_ai_opponents > TD5_MAX_RACER_SLOTS - n)
                        s_num_ai_opponents = TD5_MAX_RACER_SLOTS - n;
                    s_mp_layout_sel = 0;     /* layout list changed → reset selection */
                    mp_build_buttons();      /* row set depends on N → rebuild */
                    s_selected_button = (s_mp_btn_players >= 0) ? s_mp_btn_players : 0;
                    frontend_play_sfx(2);
                }
                s_inner_state = 4;
            } else if (active_button == s_mp_btn_catchup && delta != 0) {
                /* [S05 2026-06-04] CATCHUP on/off toggle. Either arrow flips it;
                 * persisted via td5_save (organized td5re_input.ini [Assist]) and
                 * consumed by S06's td5_ai_get_catchup_level(). 0 = off, 1 = on. */
                int cur = td5_save_get_catchup_assist();
                td5_save_set_catchup_assist(cur > 0 ? 0 : 1);
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == s_mp_btn_layout && delta != 0 &&
                       s_mp_layout_optcount > 1) {
                int sel = s_mp_layout_sel + delta;
                while (sel < 0) sel += s_mp_layout_optcount;
                sel %= s_mp_layout_optcount;
                if (sel != s_mp_layout_sel) {
                    s_mp_layout_sel = sel;
                    mp_build_buttons();      /* missing-cell count may change → rebuild */
                    s_selected_button = (s_mp_btn_layout >= 0) ? s_mp_btn_layout : 0;
                    frontend_play_sfx(2);
                }
                s_inner_state = 4;
            } else if (delta != 0 &&
                       (active_button == s_mp_btn_missing[0] ||
                        active_button == s_mp_btn_missing[1])) {
                int k = (active_button == s_mp_btn_missing[1]) ? 1 : 0;
                int v = s_mp_missing_content[k] + delta;
                while (v < 0) v += MP_MISSING_CONTENT_COUNT;
                v %= MP_MISSING_CONTENT_COUNT;
                s_mp_missing_content[k] = v;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == s_mp_btn_nickname) {
                /* Open the nickname-entry screen; return here on confirm. */
                s_nickname_from_mpopts = 1;
                td5_frontend_set_screen(TD5_SCREEN_NET_NICKNAME);
                return;
            } else if (s_button_index == s_mp_btn_ok) {
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        }
        break;
    }
}

/* [PORT ENHANCEMENT 2026-06] Per-button remap row helpers. Both keyboard and
 * joystick players configure all 10 actions (LEFT/RIGHT/ACCELERATE/BRAKE +
 * HANDBRAKE..REAR VIEW). For joysticks each action maps to a button or an
 * axis/trigger direction (steer/accel/brake become analog when bound to axes). */
static int ctrl_bind_row_count(void)
{
    return TD5_JSBIND_ACTIONS;   /* 10 */
}

static const char *ctrl_bind_row_label(int row)
{
    return (row >= 0 && row < 10) ? k_ctrl_action_labels[row] : "?";
}

/* Begin capturing input for the currently-selected action. Capture is two-phase:
 * first we WAIT for the device to return to neutral (armed=0) so the confirm
 * press — or a previous bind's still-held key/stick — isn't re-captured; once
 * neutral we snapshot the rest state and arm (armed=1) to listen for ONE input.
 * Shared by the single-action remap and the REMAP ALL sequential pass.
 * [PORT 2026-06] "listen once, wait for release before the next" */
static void ctrl_begin_capture(void)
{
    s_ctrl_capturing     = 1;
    s_ctrl_capture_armed = 0;   /* wait for neutral; baseline snapshot happens then */
    if (s_ctrl_input_source != 0)
        td5_plat_input_joystick_neutral_reset();   /* fresh settle timer per action */
    TD5_LOG_I(LOG_TAG, "CtrlBind: capturing action %d (%s) player %d%s (waiting for neutral)",
              s_ctrl_sel_action, ctrl_bind_row_label(s_ctrl_sel_action),
              s_ctrl_player, s_ctrl_remap_all ? " [remap-all]" : "");
}

/* After a capture completes, advance the REMAP ALL sequence (or finish it). */
static void ctrl_capture_advance(void)
{
    s_ctrl_capturing = 0;
    if (s_ctrl_remap_all) {
        s_ctrl_sel_action++;
        if (s_ctrl_sel_action < TD5_JSBIND_ACTIONS) {
            s_selected_button = s_ctrl_sel_action;   /* keep the cursor on it */
            ctrl_begin_capture();
        } else {
            s_ctrl_remap_all = 0;
            s_ctrl_sel_action = 0;
        }
    }
}

void Screen_ControllerBinding(void) {
    switch (s_inner_state) {

    /* ------------------------------------------------------------------
     * State 0: Init — detect the configured player's device type, seed the
     * binding state, build the per-action row buttons, → slide-in (state 9).
     * ------------------------------------------------------------------ */
    case 0:
        frontend_init_return_screen(TD5_SCREEN_CONTROLLER_BINDING);
        TD5_LOG_I(LOG_TAG, "ControllerBinding: init player=%d", s_ctrl_player);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [2026-06-16] Joypad/Joystick/KeyboardIcon.tga retired: the binding
         * screen device label is drawn entirely in the menu TTF
         * (frontend_render_controller_binding_overlay / _labels via fe_draw_text).
         * The three device-icon bitmaps were loaded but NEVER blitted in the port,
         * so the loads + their now-unused surface handles are removed and the three
         * PNGs (JoypadIcon/JoystickIcon/KeyboardIcon.png) are deletable. */
        /* [2026-06-16] NoControllerText.tga retired: the binding screen draws its
         * header, hints and the per-action "NO CONTROLLER → keyboard" fallback
         * entirely in the menu TTF (frontend_render_controller_binding_overlay /
         * _labels via fe_draw_text). The legacy warning bitmap was never blitted
         * in the port, so the load is removed and the asset is deletable. */
        {
            /* s_ctrl_player was set by Control Options (the player whose CONFIGURE
             * was pressed). Pick the row model from its device type. */
            int dev_type = td5_input_get_device_type(s_ctrl_player);
            int j;
            if (dev_type < 0 || dev_type > 2) dev_type = 0;   /* no controller → keyboard list */
            s_ctrl_input_source = dev_type;

            if (dev_type == 0) {
                /* Seed the scancode buffer from this player's saved keyboard set
                 * (players 0/1 have their own; 2+ share player-1's set). */
                const uint8_t *src = (const uint8_t *)((s_ctrl_player == 0)
                    ? td5_save_get_p1_custom_bindings_mutable()
                    : td5_save_get_p2_custom_bindings_mutable());
                memcpy(s_ctrl_kb_scancodes, src, 16);
                /* PAUSE (index 10) isn't in the legacy 10-key buffer; default it
                 * to P (0x19) for display unless an in-session rebind set it. */
                if (s_ctrl_kb_scancodes[10] == 0 || s_ctrl_kb_scancodes[10] > 0xED)
                    s_ctrl_kb_scancodes[10] = 0x19;
            } else {
                /* Seed the per-action joystick bindings from the saved set. */
                const uint32_t *ab = td5_save_get_action_bindings_mutable();
                if (ab)
                    memcpy(s_ctrl_action_bind[s_ctrl_player],
                           ab + (size_t)s_ctrl_player * TD5_JSBIND_ACTIONS,
                           TD5_JSBIND_ACTIONS * sizeof(uint32_t));
                /* If this player has no per-action config yet (all zero), seed the
                 * edit copy with the built-in Xbox-style defaults so the screen
                 * SHOWS them (matching the in-race fallback) and a per-action remap
                 * keeps the rest of the map instead of unbinding it. Display/edit
                 * only — nothing persists unless the user presses OK. */
                {
                    int any = 0;
                    for (int a = 0; a < TD5_JSBIND_ACTIONS; a++)
                        if (s_ctrl_action_bind[s_ctrl_player][a]) { any = 1; break; }
                    if (!any) {
                        const uint32_t *def = td5_plat_input_default_action_bindings();
                        if (def)
                            memcpy(s_ctrl_action_bind[s_ctrl_player], def,
                                   TD5_JSBIND_ACTIONS * sizeof(uint32_t));
                    }
                }
            }

            s_ctrl_sel_action = 0;
            s_ctrl_capturing  = 0;
            s_ctrl_remap_all  = 0;
            s_ctrl_capture_armed = 0;

            /* Build the action buttons in TWO narrow columns (is_selector → the
             * renderer skips their label; the overlay draws a small label+value
             * inside each). Layout (PORT ENHANCEMENT 2026-06):
             *   TOP     REMAP ALL — centred, just under the CONTROLLER SETUP header
             *   GRID    10 driving actions (two columns of 5), pushed below it
             *   BOTTOM  "?" (= PAUSE mapping) / OK stacked at lower-left
             * Buttons are created actions-first so the handler's fixed indices
             * stay valid: 0..9 actions, 10 = "?"/PAUSE, 11 = REMAP ALL, 12 = OK. */
            {
                int colx[2] = { 130, 360 };  /* two aligned columns, shifted right off the
                                              * dark left edge of the background */
                int b;
                frontend_reset_buttons();
                for (j = 0; j < 10; j++) {                 /* 0..9 driving actions */
                    int c = j / 5, r = j % 5;
                    b = frontend_create_button("", colx[c], 156 + r * 32, 135, 26);
                    if (b >= 0) s_buttons[b].is_selector = 1;
                }
                /* Command buttons. Creation order is fixed (10=PAUSE, 11=REMAP ALL,
                 * 12=OK) so the handler's indices stay valid; only the on-screen
                 * positions changed. REMAP ALL now sits at the TOP (centred, just
                 * below the header), ahead of the action grid; PAUSE MENU + OK
                 * stack at the bottom-left below the grid, OK last. */
                b = frontend_create_button("", 130, 322, 135, 26);            /* 10 = PAUSE MENU */
                if (b >= 0) s_buttons[b].is_selector = 1;
                b = frontend_create_button("REMAP ALL", 240, 110, 160, 28);  /* 11 = REMAP ALL (top, centred) */
                if (b >= 0) s_buttons[b].is_selector = 1;
                b = frontend_create_button(SNK_OkButTxt, 130, 354, 135, 26); /* 12 = OK (bottom) */
                if (b >= 0) s_buttons[b].is_selector = 1;
            }
        }
        s_anim_tick = 0;
        s_anim_complete = 0;
        s_inner_state = 9;
        break;

    /* ------------------------------------------------------------------
     * State 9: Joystick slide-in animation
     * [CONFIRMED @ 0x40FE00] Counts g_frontendAnimFrameCounter to 0x1C,
     * then advances to state 10 and deactivates cursor overlay.
     * Port: uses s_anim_tick.
     * ------------------------------------------------------------------ */
    case 9:
        s_anim_tick += s_fe_logic_ticks;
        if (s_anim_tick >= 0x1C) {
            s_anim_tick = 0;
            s_anim_complete = 1;
            s_inner_state = 10;
        }
        break;

    /* ------------------------------------------------------------------
     * State 10: interactive list + per-action capture (PORT ENHANCEMENT).
     * Browse the action rows (generic button nav / mouse), confirm one to
     * (re)bind ONLY that action — keyboard captures a scancode, joystick captures
     * a button OR an axis/trigger direction. OK saves + slides out (state 11).
     * ------------------------------------------------------------------ */
    case 10: {
        /* Two-column per-action remap. Browse the action buttons (0..10 incl the
         * PAUSE "?" at 10), confirm one to (re)bind just it; REMAP ALL (11) runs
         * the sequential one-by-one pass; OK (12) saves + exits. [PORT 2026-06] */
        int rows         = ctrl_bind_row_count();   /* 11 actions (0..10) */
        int remapall_btn = rows;                     /* 11 */
        int ok_btn       = rows + 1;                 /* 12 */

        if (!s_ctrl_capturing) {
            /* While idle, continuously learn this joystick's REST positions so the
             * wait-for-release gate can tell a held trigger from a released one. */
            if (s_ctrl_input_source != 0)
                td5_plat_input_joystick_learn_rest(s_ctrl_player);
            if (s_input_ready) {
                if (s_button_index == ok_btn) {
                    /* Save this player's bindings (joystick per-action codes +
                     * keyboard set) and return to Control Options. */
                    {
                        uint32_t *ab = td5_save_get_action_bindings_mutable();
                        if (ab)
                            memcpy(ab + (size_t)s_ctrl_player * TD5_JSBIND_ACTIONS,
                                   s_ctrl_action_bind[s_ctrl_player],
                                   TD5_JSBIND_ACTIONS * sizeof(uint32_t));
                        td5_input_set_action_bindings(s_ctrl_player,
                            s_ctrl_action_bind[s_ctrl_player], TD5_JSBIND_ACTIONS);
                    }
                    {
                        /* Keyboard set: players 0/1 own a set; 2+ share player-1's. */
                        int kb_set = (s_ctrl_player == 0) ? 0 : 1;
                        uint8_t *dst = (uint8_t *)((kb_set == 0)
                            ? td5_save_get_p1_custom_bindings_mutable()
                            : td5_save_get_p2_custom_bindings_mutable());
                        memcpy(dst, s_ctrl_kb_scancodes, 16);
                        td5_plat_input_set_keyboard_bindings(kb_set, s_ctrl_kb_scancodes,
                                                             TD5_JSBIND_ACTIONS);
                    }
                    td5_save_write_config(NULL);
                    TD5_LOG_I(LOG_TAG, "CtrlBind: saved bindings for player %d", s_ctrl_player);
                    s_anim_tick = 0;
                    s_inner_state = 11;
                } else if (s_button_index == remapall_btn) {
                    /* REMAP ALL: configure every action one by one, from action 0. */
                    s_ctrl_remap_all  = 1;
                    s_ctrl_sel_action = 0;
                    s_selected_button = 0;
                    ctrl_begin_capture();
                    frontend_play_sfx(2);
                } else if (s_button_index >= 0 && s_button_index < rows) {
                    /* Begin capturing just the selected action. */
                    s_ctrl_remap_all  = 0;
                    s_ctrl_sel_action = s_button_index;
                    ctrl_begin_capture();
                    frontend_play_sfx(2);
                }
            }
        } else {
            /* --- Capture mode (ESC cancels; in REMAP ALL it cancels the run) --- */
            const uint8_t *kb = td5_plat_input_get_keyboard();
            if (kb[0x01]) {
                /* ESC always cancels (checked before the neutral gate so a held
                 * ESC can't wedge the wait). */
                s_ctrl_capturing = 0;
                s_ctrl_remap_all = 0;
                s_ctrl_capture_armed = 0;
                TD5_LOG_I(LOG_TAG, "CtrlBind: capture cancelled");
                break;
            }

            /* Phase 1: wait for the device to go neutral, THEN snapshot + arm.
             * This makes the remap "listen once" — a held stick/key from the
             * confirm press (or the previous bind) must be released before the
             * next input is captured. [PORT 2026-06] */
            if (!s_ctrl_capture_armed) {
                int neutral;
                if (s_ctrl_input_source == 0) {
                    int sc; neutral = 1;
                    for (sc = 1; sc < 256; sc++) if (kb[sc]) { neutral = 0; break; }
                } else {
                    neutral = td5_plat_input_joystick_neutral(s_ctrl_player);
                }
                if (neutral) {
                    s_ctrl_capture_armed = 1;
                    memcpy(s_ctrl_capture_kb_snapshot, kb, 256);
                    if (s_ctrl_input_source != 0)
                        td5_plat_input_joystick_capture_begin(s_ctrl_player);
                    TD5_LOG_D(LOG_TAG, "CtrlBind: armed for action %d", s_ctrl_sel_action);
                }
                break;   /* still releasing / just armed — don't capture this frame */
            }

            if (s_ctrl_input_source == 0) {
                /* Keyboard: first freshly-pressed key (rising edge vs snapshot). */
                int sc, found = -1;
                for (sc = 1; sc < 256 && found < 0; sc++)
                    if (kb[sc] && !s_ctrl_capture_kb_snapshot[sc] && sc != 0x01)
                        found = sc;
                if (found >= 0) {
                    if (s_ctrl_sel_action >= 0 && s_ctrl_sel_action < TD5_JSBIND_ACTIONS)
                        s_ctrl_kb_scancodes[s_ctrl_sel_action] = (uint8_t)found;
                    TD5_LOG_I(LOG_TAG, "CtrlBind: action %d -> scancode 0x%02X",
                              s_ctrl_sel_action, (unsigned)found);
                    frontend_play_sfx(3);
                    ctrl_capture_advance();
                }
            } else {
                /* Joystick: first fresh button press OR axis/trigger movement. */
                uint32_t code = 0;
                if (td5_plat_input_joystick_capture_poll(s_ctrl_player, &code)) {
                    if (s_ctrl_sel_action >= 0 && s_ctrl_sel_action < TD5_JSBIND_ACTIONS)
                        s_ctrl_action_bind[s_ctrl_player][s_ctrl_sel_action] = code;
                    TD5_LOG_I(LOG_TAG, "CtrlBind: action %d -> joystick code 0x%X",
                              s_ctrl_sel_action, (unsigned)code);
                    frontend_play_sfx(2);
                    ctrl_capture_advance();
                }
            }
        }
        break;
    }

    /* ------------------------------------------------------------------
     * State 11: Joystick slide-out animation (0x1C frames)
     * [CONFIRMED @ 0x40FE00] After animation, releases surfaces and
     * calls SetFrontendScreen(0xe) = TD5_SCREEN_CONTROL_OPTIONS.
     * ------------------------------------------------------------------ */
    case 11:
        s_anim_tick += s_fe_logic_ticks;
        if (s_anim_tick >= 0x1C) {
            s_anim_tick = 0;
            TD5_LOG_D(LOG_TAG, "CtrlBind: joystick slide-out done → ControlOptions");
            td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        }
        break;

    /* [PORT ENHANCEMENT 2026-06] The original's sequential keyboard-capture
     * states (19/20/25/26/27) and joystick-cycle state are gone — the unified
     * per-button-remap list in state 10 handles both device types. */
    default:
        td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        break;
    }
}

void Screen_MusicTestExtras(void) {
    switch (s_inner_state) {
    case 0: /* Fade transition + init */
        frontend_init_return_screen(TD5_SCREEN_MUSIC_TEST);
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: init");
        /* [CONFIRMED @ 0x418460 case 0]:
         *   ReleaseExtrasGalleryImageSurfaces() + LoadExtrasBandGalleryImages()
         *   CreateMenuStringLabelSurface(6) → DAT_00496358 (title surface)
         *   CreateTrackedFrontendSurface(0x170,0x28) → DAT_0049628c (track-name)
         *   CreateTrackedFrontendSurface(0x170,0x78) → DAT_00496400 (now-playing)
         *   Initial draw: sprintf "%d. %s" + NowPlayingTxt + band + title into surfaces
         * Port: no offscreen surfaces — strings are rendered live every frame via
         * frontend_render_music_test_overlay. Initialise them here.
         * Tier 4 port 2026-05-24 added the [ARCH-DIVERGENCE] footer entries
         * for ReleaseExtrasGalleryImageSurfaces / LoadExtrasBandGalleryImages /
         * CreateMenuStringLabelSurface — see end of file. */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [CONFIRMED @ 0x418460] CreateFrontendDisplayModeButton(SNK_SelectTrackButTxt, -0x120, 0, 0xA0, 0x20, 0)
         *                          CreateFrontendDisplayModeButton(SNK_OkButTxt, -0x120, 0, 0x60, 0x20, 0) */
        /* SNK_SelectTrackButTxt = "TRACK" (Language.dll, byte-faithful — port's prior
         * "Select Track" was a wrong guess). Buttons placed at their EXACT settled rest
         * positions (the orig auto-creates then MoveFrontendSpriteRects them; slide-in
         * settles at counter 0x27): [CONFIRMED @0x418460] TRACK (120,97) 160x32 top-left;
         * OK (216,377) 96x32 at the BOTTOM (below the 160x160 cover), NOT auto-stacked. */
        frontend_create_button(SNK_SelectTrackButTxt, 120, 97,  0xA0, 0x20);
        frontend_create_button(SNK_OkButTxt,    216, 377, 0x60, 0x20);
        /* Load the 5 band cover-art images (Extras.zip -> re/assets/extras).
         * Idempotent (frontend_load_tga returns the existing handle if reloaded). */
        {
            static const char *covers[5] = {
                "Fear Factory.tga", "Gravity Kills.tga", "Junkie XL.tga",
                "KMFDM.tga", "PitchShifter.tga"
            };
            int ci;
            for (ci = 0; ci < 5; ci++)
                s_band_cover_surface[ci] =
                    frontend_load_tga(covers[ci], "Front End/Extras/Extras.zip");
        }
        s_music_test_track_idx = 0;
        s_music_attract_track = 0;   /* cover/now-playing reflect the PLAYED track */
        s_music_test_playing_set = 0;
        s_music_test_now_band[0]  = '\0';
        s_music_test_now_title[0] = '\0';
        frontend_music_test_update_track_label();   /* "1. GRAVITY KILLS" */
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: track_label='%s'", s_music_test_track_label);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: cycle tracks, play, OK */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            /* Use the same selected-button fallback as every other option screen
             * (e.g. Game Options :7662). s_button_index is the CLICKED button and is
             * -1 when the user only presses LEFT/RIGHT arrow keys — so gating the
             * cycle on `s_button_index == 0` meant keyboard arrows never changed the
             * track (the reported bug). active_button resolves to the highlighted
             * selector (button 0) for keyboard arrow input. */
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (active_button == 0 && delta != 0) {
                /* Cycle track index 0..11.
                 * [CONFIRMED @ 0x4186A8]: g_selectedCdTrackIndex += DAT_0049b690 (arrow dir),
                 *   clamped 0..11; then BltColorFillToSurface + sprintf "%d. %s" redrawn
                 *   into DAT_0049628c (track-name surface). Port: update label string. */
                s_music_test_track_idx += delta;
                /* [CONFIRMED @ 0x00418460 case6/idx0, re-decompiled 2026-05-30 by two
                 * independent agents] original WRAPS 0<->0xB: LEFT underflow -> 0xB,
                 * RIGHT overflow -> 0. It does NOT clamp. The prior comment here (citing
                 * a 0x4186A8 clamp) was incorrect. Table length 12 (PTR_s_GRAVITY_KILLS /
                 * PTR_s_FALLING) matches the 0..11 wrap range. */
                if (s_music_test_track_idx < 0)  s_music_test_track_idx = 11;
                if (s_music_test_track_idx > 11) s_music_test_track_idx = 0;
                frontend_music_test_update_track_label();
                TD5_LOG_D(LOG_TAG, "MusicTestExtras: cycle -> '%s'", s_music_test_track_label);
            }
            if (s_button_index == 0 && s_arrow_input == 0) {
                /* Confirm "Select Track" -> play CD audio.
                 * [CONFIRMED @ 0x41864E]: DXSound::CDPlay(g_selectedCdTrackIndex+2, 1)
                 *   then redraw DAT_00496400 (now-playing surface):
                 *   row y=0:    "NOW PLAYING" text (SNK_NowPlayingTxt_exref)
                 *   row y=0x28: band name  (PTR_s_GRAVITY_KILLS_00465e1c[idx])
                 *   row y=0x50: song title (PTR_s_FALLING_00465e58[idx])
                 * Port: record now-playing strings; render overlay draws them live. */
                frontend_cd_play(s_music_test_track_idx);
                frontend_music_test_update_now_playing(s_music_test_track_idx);
                /* [FIXED 2026-06-01] orig sets g_attractCdTrackCandidate here on SELECT;
                 * the cover art + now-playing panel follow the PLAYED track, not the
                 * one being previewed with ◄►. */
                s_music_attract_track = s_music_test_track_idx;
                TD5_LOG_D(LOG_TAG, "MusicTestExtras: now playing '%s' / '%s'",
                          s_music_test_now_band, s_music_test_now_title);
            }
            if (s_button_index == 1) { /* OK */
                /* Set fade value for transition, exit to Sound Options */
                s_inner_state = 7;
            }
        }
        break;

    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms). Restore gallery images. */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* [CONFIRMED @ 0x00418bd2..0x00418bdc]:
             *   ReleaseExtrasGalleryImageSurfaces(); LoadExtrasGalleryImageSurfaces();
             * Port has no band-photo surface pool (case 0 documents the fold), so
             * release/reload is a no-op. The mugshot gallery (Screen_ExtrasGallery
             * at TD5_SCREEN_EXTRAS_GALLERY) maintains its own s_gallery_pic_surface
             * independently and is not coupled to this screen's transition. */
            td5_frontend_set_screen(TD5_SCREEN_SOUND_OPTIONS);
        }
        break;
    }
}

void Screen_ExtrasGallery(void) {
    switch (s_inner_state) {
    case 0: /* Init: load the dev mugshots, start the scroll reel */
        frontend_init_return_screen(TD5_SCREEN_EXTRAS_GALLERY);
        for (int i = 0; i < K_CREDIT_MUGSHOT_COUNT; i++) {
            if (s_credit_mugshot_surf[i] <= 0)
                s_credit_mugshot_surf[i] = frontend_load_tga(k_credit_mugshots[i], GALLERY_ZIP);
        }
        s_anim_tick = 0;
        TD5_LOG_I(LOG_TAG, "ExtrasGallery: credits scroll init (%d rows, %d photos)",
                  K_CREDITS_COUNT, K_CREDIT_MUGSHOT_COUNT);
        s_inner_state = 1;
        break;

    case 1: /* Brief delay to prevent input bleed from menu (~39 ticks) */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x27) {
            s_credits_start_ms = td5_plat_time_ms();
            s_inner_state = 2;
        }
        break;

    case 2: /* Scroll the credit reel; quit once it fully passes (orig exits after credits).
             * ESC/click also exits via the global escape handler. [FAITHFUL 2026-06-02 — replaces
             * the prior photo slideshow with the original's vertical scroll of SNK_CreditsText.] */
        {
            float scroll = (float)(td5_plat_time_ms() - s_credits_start_ms) * FE_CREDITS_SPEED;
            if (scroll > 480.0f + frontend_credits_total_height()) {
                TD5_LOG_I(LOG_TAG, "ExtrasGallery: credits complete, quitting");
                frontend_post_quit();
            }
        }
        break;
    }
}

void Screen_StartupInit(void) {
    switch (s_inner_state) {
    case 0: /* Create small surface, show OK button */
        frontend_init_return_screen(TD5_SCREEN_STARTUP_INIT);
        TD5_LOG_D(LOG_TAG, "StartupInit: state 0");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(SNK_OkButTxt, -100, 0, 100, 0x20);
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present blank (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Release surface, redirect to ScreenLocalizationInit */
        TD5_LOG_I(LOG_TAG, "StartupInit: redirecting to ScreenLocalizationInit");
        td5_frontend_set_screen(TD5_SCREEN_LOCALIZATION_INIT);
        break;
    }
}
