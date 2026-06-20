/*
 * td5_fe_race.c -- frontend screens: race setup + post-race flow.
 *
 * Split out of td5_frontend.c (2026-06). Handlers: QuickRaceMenu[7],
 * CarSelection[20] (incl. the simultaneous-MP grid), TrackSelection[21],
 * PostRaceHighScore[23], RaceResults[24], PostRaceNameEntry[25],
 * CupFailed[26], CupWon[27]. Shared frontend state comes from
 * td5_frontend_internal.h; original binary addresses are noted in the
 * per-screen comments.
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
#include "td5_track.h"
#include "td5_types.h"
#include "td5re.h"
#include "td5_snk_strings.h"
#include "td5_vectorui.h"
#include "td5_font.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_TAG "frontend"
#include "td5_color.h"
#include "td5_frontend_internal.h"

/* ====================================================================== *
 * [#6 / #11 2026-06-15] MP position-screen rework + profile management.
 *
 * Cross-file CONSUMERS (declared extern here; NOT defined in this file):
 *   - td5_input.c  : per-player rising-edge getters for the CHANGE CAMERA and
 *                    FRONT VIEW action buttons (keyboard + joystick), used by
 *                    Screen_MpPosition to (a) cycle the split-screen layout
 *                    [CHANGE CAMERA] and (b) cycle the empty-cell content
 *                    selector g_td5.split_missing_content[] [FRONT VIEW].
 *                    Both return 1 exactly once per physical press for `player`.
 *   - td5_save.h   : the TD5_Profile store API (already in the header).
 *   - td5_frontend.c shared frontend state (s_mp_* / s_mp_missing_content /
 *                    mp_resolve_layout / mp_split_layouts) via the internal hdr.
 *
 * Cross-file RENDER hooks REQUIRED in td5_frontend.c (one line each — see the
 * REPORT): repoint the TD5_SCREEN_MP_POSITION render to frontend_mp_position_render2
 * and call frontend_mp_setup_profile_render() after frontend_mp_setup_render().
 * ====================================================================== */

/* [#6] Per-player rising-edge getters for the CHANGE CAMERA and FRONT VIEW
 * action buttons. The authoritative implementations live in td5_input.c (which
 * owns the device/keyboard binding + debounce). They are declared + provided here
 * as WEAK fallbacks so this translation unit links standalone even if the input
 * module's strong definitions are not present yet: the strong symbol (when it
 * exists) overrides the weak one; otherwise these return 0 and the two features
 * (camera=layout-cycle, front-view=empty-cell content) simply never fire. Replace
 * / delete these stubs once td5_input.c exports the real getters. */
int td5_input_frontend_change_camera_pressed(int player);
int td5_input_frontend_front_view_pressed(int player);
__attribute__((weak)) int td5_input_frontend_change_camera_pressed(int player) {
    (void)player; return 0;
}
__attribute__((weak)) int td5_input_frontend_front_view_pressed(int player) {
    (void)player; return 0;
}

/* [#6] New, stable-pulse position-screen renderer (replaces the abrupt-sawtooth
 * frontend_mp_position_render in td5_frontend.c). Exported so the render switch
 * can call it; see the REPORT for the one-line td5_frontend.c change. */
void frontend_mp_position_render2(float sx, float sy);

/* [#11] Profile-management overlay renderer for the MP name/colour step.
 * Exported; td5_frontend.c calls it right after frontend_mp_setup_render(). Also
 * draws the [#4] "LEAVE? Y/N" confirm overlay (works with TD5RE_PROFILES off). */
void frontend_mp_setup_profile_render(float sx, float sy);

/* [#6] Grid cell that human player p occupies (s_mp_player_cell[p] when positions
 * are active, else identity). Exported so the MP CAR-SELECT grid render in
 * td5_frontend.c can place each player in THEIR chosen pane (see the REPORT for the
 * one-line render change). [#12] frontend_mp_flow_reset() clears the per-flow
 * "position picker already shown" latch so a NEW race re-offers the layout. */
int  frontend_mp_player_pane_cell(int p);
void frontend_mp_flow_reset(void);

/* Public small-font helpers (defined in td5_frontend.c; the only fe_draw_* the
 * linker exposes — fe_draw_quad/fe_draw_text/fe_draw_text_centered are static
 * there). Re-declared up here so the #6/#11 renderers below can use them; the
 * race-summary renderer further down re-declares the same prototypes locally. */
void  fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_measure_small_text(const char *text);

/* Centred small-font text (px in). Mirrors td5_frontend.c's static
 * mp_simul_small_centered, which we cannot link to. 4:3-locked glyph width =
 * min(sx,sy), matching fe_glyph_sx. */
static void mp_pos_small_centered(float cx_px, float y_px, const char *t,
                                  uint32_t col, float sx, float sy) {
    float gsx = (sx < sy) ? sx : sy;
    fe_draw_small_text(cx_px - fe_measure_small_text(t) * 0.5f * gsx, y_px, t, col, sx, sy);
}

/* [#3 2026-06-16] MP per-player panel MAX-WIDTH cap. With few players the split
 * layout makes each pane very wide (2 players -> cols=2 -> 320 px each; the
 * "CHOOSE YOUR SCREEN" cells span 280 px). Cap each pane at the width it would
 * have in a 3x3 grid (canvas/3) and centre the resulting row in the canvas.
 *
 * EXACT FORMULA (canvas width W = 640, cols from mp_resolve_layout):
 *   pane_full = W / cols
 *   cap       = W / 3                       == 213.333 px   (the 3x3-equivalent)
 *   pane_w    = min(pane_full, cap)
 *   row_used  = cols * pane_w
 *   row_x0    = (W - row_used) / 2          (centres the row; +0 when uncapped)
 *   px[col]   = row_x0 + col * pane_w
 * Capping kicks in only when cols < 3 (1->213, 2->213; 3+ already <= cap so
 * pane_w is unchanged and row_x0 == 0, i.e. the >=3-player layouts are
 * untouched). Height is left at canvas/rows (only WIDTH is capped, per spec).
 *
 * Knob TD5RE_MP_PANEL_CAP (default on; "0" restores edge-to-edge full-width
 * panes). Cached once, logged once.
 *
 * SCOPE NOTE: this only governs the per-player panels RENDERED IN THIS FILE (the
 * position-picker cells + the profile/PROFILE-chip overlay). The main name/colour
 * setup panes and the simultaneous car-select panes are drawn in td5_frontend.c
 * (frontend_mp_setup_render / frontend_mp_simul_carsel_render); for the overlay to
 * sit on top of them they must adopt this identical formula. See the REPORT. */
static int mp_panel_cap_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PANEL_CAP");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP per-player panel width cap (#3) %s (TD5RE_MP_PANEL_CAP=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Capped pane width + centred row origin for `cols` columns across `canvas_w`.
 * Writes the per-pane width to *pane_w and the row's left edge (col 0) to *row_x0.
 * No-op (full width, x0=0) when the cap knob is off. */
static void mp_panel_capped(float canvas_w, int cols, float *pane_w, float *row_x0) {
    float full = canvas_w / (float)(cols < 1 ? 1 : cols);
    float w = full;
    if (mp_panel_cap_on()) {
        float cap = canvas_w / 3.0f;
        if (w > cap) w = cap;
    }
    if (pane_w)  *pane_w  = w;
    if (row_x0)  *row_x0  = (canvas_w - (float)cols * w) * 0.5f;
}

/* [#6] Knob: TD5RE_MP_POSITIONS already gates the whole picker (mp_positions_enabled
 * in td5_frontend.c). This file reuses it; no separate knob added for #6. */
/* [#11] Knob: TD5RE_PROFILES (default ON; exactly "0" disables the profile panel
 * + the PROFILE button, reverting the name/colour step to NAME/COLOUR/OK only). */
/* [#3 2026-06-15] Exported (was file-static) so the idle nav-band layout in
 * frontend_mp_setup_render (td5_frontend.c) can widen the band to 4 slots
 * (NAME/COLOUR/PROFILE/OK) when profiles are enabled. Declared extern there. */
int mp_profiles_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_PROFILES");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP profile management (#11) %s (TD5RE_PROFILES=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* ---- file-local forward declarations (definitions in source order) ---- */
static int frontend_load_car_preview_surface(int car_index, int paint_index);
static int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey);
static int frontend_td5_car_cap_inclusive(void);
static int frontend_car_selectable(int i);
static int frontend_car_cycle_step(int cur, int delta, int lo, int hi);
static uint32_t td6_cursor_color(int col, int row);
static void frontend_color_panel_sync_index(void);
static int frontend_track_level_exists(int track_index);
static int frontend_track_is_cup_slot(int track_index);
static int frontend_track_excluded_from_selector(int track_index);
static void frontend_cycle_track(int delta, int track_min, int track_max);
static void frontend_load_selected_car_preview(void);
static void frontend_load_selected_track_preview(void);
static void frontend_apply_color_panel_layout(void);
static void frontend_set_color_panel(int open);
static int frontend_color_panel_mouse(void);
static int frontend_write_cup_data(void);
static void frontend_delete_cup_data(void);
static void frontend_quickrace_cycle_track(int delta);
static void frontend_quickrace_clamp_counts(void);
static uint32_t mp_simul_player_nav(int player);
static void mp_simul_drop_handle(int *cache, int player, int n);
static void mp_simul_refresh_pane(int player);
static void mp_simul_free_all_panes(int n);
static void mp_simul_cycle_paint(int p, int step, int sfx);
static void frontend_mp_simul_carsel_init(void);
static void mp_simul_back_to_lobby(int n);
static void frontend_mp_simul_carsel_update(void);
static void mp_setup_name_append(int p, char c);
static void mp_setup_name_backspace(int p);
static int mp_repeat_fire(int p, uint32_t held, uint32_t edge, uint32_t now);
static void frontend_mp_setup_init(void);
static void frontend_mp_setup_update(void);
static void frontend_mp_position_enter(void);   /* [#8] advance phase 0 -> position picker */
static int frontend_track_is_circuit(int track_slot);
static void frontend_update_laps_button_visibility(int laps_btn_idx);
static void frontend_update_difficulty_button_visibility(int diff_btn_idx);  /* [R5] hide when 0 opponents */
static void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label);
static int frontend_carsel_hold_enabled(void);   /* [#2/#7] TD5RE_CARSEL_HOLD gate (defined below) */
static int frontend_carsel_hold_repeat(void);    /* hold-to-scroll LEFT/RIGHT auto-repeat (defined below); reused by Quick Race */

/* [#2b/#10 2026-06-16] Cross-module hooks DEFINED in td5_frontend.c (extern'd
 * inline here, mirroring the existing extern-in-.c pattern, so no shared header
 * is touched).
 *   s_postrace_td6_level       — >0 = the post-race table is a TD6 track; this
 *                                FSM sets it, the high-score overlay reads it.
 *   frontend_qr_random_button_on / frontend_qr_roll_selector — the QR randomize
 *                                BUTTON gate + roll action reused by the dedicated
 *                                Randomize buttons (#10). */
extern int  s_postrace_td6_level;
extern int  frontend_qr_random_button_on(void);
extern void frontend_qr_roll_selector(int which);

/* [#10] Dedicated RANDOMIZE button geometry (Car/Track rows). MUST match the
 * identical definitions in td5_frontend.c (which computes the value-text reserve)
 * — duplicated in both .c files to avoid touching shared headers. Created LAST in
 * the QR init so these indices follow QR_BTN_SPAN (11) without disturbing it. */
#define QR_BTN_RAND_CAR   12
#define QR_BTN_RAND_TRACK 13
#define QR_RAND_BTN_W     104
#define QR_RAND_BTN_X     (FE_QR_SCREEN_W - FE_QR_RIGHT_MARGIN - QR_RAND_BTN_W) /* 524 */

static int      s_mp_player_color_idx[TD5_MAX_HUMAN_PLAYERS]; /* palette cursor for TD6 colour cycling */

static uint32_t s_mp_rep_ms[TD5_MAX_HUMAN_PLAYERS];          /* colour-grid auto-repeat next-fire time */

static int  s_car_preview_overlay;      /* DAT_0048f360            */

static int  s_car_roster_max;           /* max car index for current mode */

static int  s_car_roster_min;           /* min car index for current mode */

static int  s_p2_paint;

static int  s_p2_config;

static int  s_p2_transmission;

/* Dedicated PLAYER (pass-1) car storage for the drag-race 2-pass CarSelect.
 * The original keeps the player car in g_selectedCarIndex @0x0048f364 and never
 * overwrites it during the opponent pass (the user cycles a SEPARATE scratch
 * register). The port collapsed scratch + player slot into s_selected_car, so
 * the pass-2 navigation clobbered the player car -> both racers ended up with
 * the later (opponent) car. s_p1_car preserves the player's pass-1 choice across
 * the opponent pass. [CONFIRMED root cause @ 0x40dfc0 case-0/case-0x18, 0x40dac0] */
static int  s_p1_car;

static int  s_p1_paint;

static int  s_track_max;               /* max track index for current mode */

/* [#14 2026-06-15] Dedicated RANDOMIZE buttons placed ABOVE the selector on the
 * car- and track-selection screens. Created at the next free button index (so the
 * existing hard-coded button indices used by the action switches are untouched);
 * navigation is geometric (frontend_spatial_pick), so a higher index above the
 * selector is still reached by pressing UP. -1 = not created this entry. */
static int  s_carsel_rand_btn = -1;
static int  s_trksel_rand_btn = -1;
/* [R3-3 2026-06-19] Previously-focused button on the Track-select screen, so case
 * 4 can tell a focus-CHANGE onto the Track selector (button 0) apart from an
 * explicit LEFT/RIGHT pressed while already on it. -2 = "fresh, treat the next
 * frame as an entry" (reset at screen init so a stale L/R bit can't cycle on the
 * first interactive frame); set to the live focus each interactive frame. */
static int  s_trksel_prev_focus = -2;

/* Race results state */
static int  s_results_button;           /* DAT_00497a64 */

static int  s_results_panel_slide_dir;   /* +1 = right (next), -1 = left (prev) */

/* View Race Data sentinel: when the user clicks "View Race Data" in state
 * 0x0F, we re-enter screen 24 via td5_frontend_set_screen — but state 0's
 * cup-fail early-route and state 3's table-skip gate both check
 * td5_game_slot_is_finished(0) / companion_2, which can be false if the
 * player DNF'd or quit early. Orig relied on actor.slot._808_4_ to detect
 * "race data exists", a field the port doesn't materialize. This flag is
 * set in the View Race Data dispatch and cleared after the state-3 gate
 * passes, forcing the table to display even on a partial race. */
static int  s_results_view_data_request;

/* [FIX 2026-06-05 race-again-opponent-count] Snapshot the opponent count too,
 * so "Race Again" (and S15's pause-menu RESTART RACE, which reads this same
 * snapshot) reruns with the SAME field size instead of falling back to the
 * legacy 5-opponent default. Defaults to -1 = "no snapshot captured yet". */
static int  s_snap_num_ai_opponents = -1;

/* Post-race name entry state (Screen [25]) */
static int32_t s_post_race_score;       /* DAT_004951d0: player's score for qualification */

/* [#2b 2026-06-16] When the post-race track is TD6 (s_postrace_td6_level > 0),
 * the score does NOT go into a TD5 NPC group — it goes into the genuine TD6
 * record store (td5_save). This caches the chosen score TYPE (0 TIME / 1 LAP)
 * between the case-0 qualification step and the case-4 insert. */
static int s_postrace_td6_score_type = 0;

/* [#2a 2026-06-16] View-Replay post-race result SNAPSHOT.
 *
 * "View Replay" (RaceResults state 0x10 case 1) re-enters the race via
 * g_td5.race_requested, which calls td5_game_init_race_session — and that RESETS
 * the live race results (s_results / s_slot_state: slot 0 becomes "not finished",
 * its finish time / top / avg zeroed). So after returning from the replay, the
 * post-race high-score NAME-ENTRY (Screen_PostRaceNameEntry case 0) read ZERO for
 * the player's achieved time and slot_is_finished == 0 -> never qualified ->
 * rendered an EMPTY high-score table that then fell through to the MAIN MENU
 * (the user-reported "empty frontend then main menu"). The replay also can't
 * be guaranteed to re-finish (an ESC-aborted replay leaves slot 0 unfinished).
 *
 * Fix (frontend-only; the race<->frontend transition lives in td5_game.c which is
 * out of scope): capture the genuine post-race qualifying data the moment a
 * replay is launched, and have NAME-ENTRY fall back to it when the live results
 * are no longer valid. s_pr_snap_valid latches a captured snapshot; it is cleared
 * once consumed by NAME-ENTRY and on a fresh real race so a later non-replay race
 * uses its own live data. Gated by TD5RE_REPLAY_QUIT_FLOW (default on). */
static int     s_pr_snap_valid    = 0;
static int     s_pr_snap_finished = 0;
static int32_t s_pr_snap_primary  = 0;   /* result primary metric (finish time)  */
static int32_t s_pr_snap_secondary= 0;   /* result secondary metric (points/lap) */
static int32_t s_pr_snap_best_lap = 0;
static int32_t s_pr_snap_top      = 0;
static int32_t s_pr_snap_avg      = 0;

/* [#2a] TD5RE_REPLAY_QUIT_FLOW gate (default on). When on, the post-race replay
 * preserves the qualifying result across the replay so QUIT routes through the
 * high-score / name-entry flow instead of an empty frontend -> main menu; "0"
 * reverts to the prior behaviour (replay wipes the result). Cached + logged once. */
static int replay_quit_flow_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_REPLAY_QUIT_FLOW");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "post-race replay->quit flow (#2a) %s (TD5RE_REPLAY_QUIT_FLOW=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* [#2a] Capture the genuine post-race qualifying data from the LIVE race results
 * (slot 0) just before a replay re-inits the race and wipes them. */
static void frontend_postrace_snapshot_capture(void) {
    s_pr_snap_finished  = td5_game_slot_is_finished(0);
    s_pr_snap_primary   = td5_game_get_result_primary(0);
    s_pr_snap_secondary = td5_game_get_result_secondary(0);
    s_pr_snap_best_lap  = td5_game_get_best_lap_time(0);
    s_pr_snap_top       = td5_game_get_result_top_speed(0);
    s_pr_snap_avg       = td5_game_get_result_avg_speed(0);
    s_pr_snap_valid     = 1;
    TD5_LOG_I(LOG_TAG, "PostRace snapshot captured for replay: finished=%d primary=%d "
              "secondary=%d best_lap=%d top=%d avg=%d",
              s_pr_snap_finished, (int)s_pr_snap_primary, (int)s_pr_snap_secondary,
              (int)s_pr_snap_best_lap, (int)s_pr_snap_top, (int)s_pr_snap_avg);
}

/* Snapshotted at NAME_ENTRY case 0 (race-end transition) so case 4 can
 * insert non-zero values even if the actor pool has been torn down by
 * the time we reach the insert step. Without these the inserted entry's
 * AVG/TOP kph columns show 0 (user-reported 2026-05-26). */
static int32_t s_post_race_top_speed;

static int32_t s_post_race_avg_speed;

static int  s_car_preview_change_loaded;  /* state-11 load-once guard (a missing
                                           * preview, e.g. a TD6 car with no carpic,
                                           * returns 0 — without this the slide
                                           * transition would re-load forever). */

static char s_post_race_name[32];

static int frontend_load_car_preview_surface(int car_index, int paint_index) {
    char entry[32];
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return 0;
    snprintf(entry, sizeof(entry), "CarPic%d.tga", paint_index & 3);
    /* Car preview PNGs have a blue (0,0,90) background — key it out so the
     * car silhouette floats over the CarSel background.
     * [CONFIRMED]: all carpic*.png in re/assets/cars have corners (0,0,90,255). */
    return frontend_load_surface_keyed(entry, s_car_zip_paths[car_index], TD5_COLORKEY_BLUE88);
}

/* Load a TGA/PNG into a surface slot with a caller-specified color key.
 * colorkey = TD5_COLORKEY_BLACK: keys near-black pixels (track previews)
 * colorkey = TD5_COLORKEY_BLUE88: keys (0,0,~90) blue pixels (car previews)
 * colorkey = TD5_COLORKEY_NONE: no keying (opaque blit) */
static int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey) {
    int existing_handle;

    const char *bare_name = name;
    const char *slash = strrchr(name, '/');
    if (slash) bare_name = slash + 1;
    slash = strrchr(bare_name, '\\');
    if (slash) bare_name = slash + 1;

    const char *real_archive = archive;
    if (strstr(archive, "FrontEnd.zip") || strstr(archive, "frontend.zip"))
        real_archive = "Front End/frontend.zip";

    existing_handle = frontend_find_surface_by_source(bare_name, real_archive);
    if (existing_handle > 0) return existing_handle;

    /* Try PNG from re/assets first */
    void *pixels = NULL;
    int w = 0, h = 0;
    char png_path[256];

    if (td5_asset_resolve_png_path(bare_name, real_archive, png_path, sizeof(png_path)))
        td5_asset_load_png_to_buffer(png_path, colorkey, &pixels, &w, &h);

    if (!pixels) {
        TD5_LOG_W(LOG_TAG, "LoadSurfaceKeyed failed: %s from %s ck=%d (no PNG found)", name, archive, (int)colorkey);
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (!s_surfaces[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { free(pixels); return 0; }

    int page = FE_SURFACE_PAGE_BASE + slot;
    if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
        s_surfaces[slot].in_use = 1;
        s_surfaces[slot].tex_page = page;
        s_surfaces[slot].width = w;
        s_surfaces[slot].height = h;
        strncpy(s_surfaces[slot].source_name, bare_name, sizeof(s_surfaces[slot].source_name) - 1);
        s_surfaces[slot].source_name[sizeof(s_surfaces[slot].source_name) - 1] = '\0';
        strncpy(s_surfaces[slot].source_archive, real_archive, sizeof(s_surfaces[slot].source_archive) - 1);
        s_surfaces[slot].source_archive[sizeof(s_surfaces[slot].source_archive) - 1] = '\0';
        strncpy(s_surfaces[slot].png_path, png_path, sizeof(s_surfaces[slot].png_path) - 1);
        s_surfaces[slot].png_path[sizeof(s_surfaces[slot].png_path) - 1] = '\0';
        free(pixels);
        TD5_LOG_I(LOG_TAG, "LoadSurfaceKeyed OK: %s → slot=%d page=%d %dx%d ck=%d", bare_name, slot, page, w, h, (int)colorkey);
        return slot + 1;
    }
    free(pixels);
    return 0;
}

/* Highest selectable ORIGINAL (TD5) car index, inclusive, for normal cycling.
 * Mirrors the original unlock cap. s_total_unlocked_cars is a COUNT, so the
 * inclusive index is count-1 (this is the off-by-one that produced an
 * "UNKNOWN CAR" entry at index == count). Police (33-36) are excluded from
 * normal cycling by capping at 32. */
static int frontend_td5_car_cap_inclusive(void) {
    if (s_network_active) return TD5_BASE_CAR_COUNT - 1; /* 36 */
    if (s_cheat_unlock_all) return 32;
    int cap = s_total_unlocked_cars - 1;
    if (cap > 32) cap = 32;
    if (cap < 0)  cap = 0;
    return cap;
}

/* Is car index i reachable by normal car cycling for the current game type?
 *   Cop Chase (type 8): ONLY the cop cars (TD5 33-36 + TD6 cp1-4 = 46-49).
 *   Otherwise:          [0..td5_cap] unlocked non-police TD5 + [37..75] TD6,
 *                       with the cop cars excluded.
 * The gap (td5_cap+1 .. 36) — locked TD5 cars + police — is skipped. */
static int frontend_car_selectable(int i) {
    if (i < 0 || i >= TD5_CAR_COUNT) return 0;
    if (s_selected_game_type == 8)                  /* Cop Chase: cops only */
        return frontend_car_is_cop(i);
    if (frontend_car_is_cop(i)) return 0;           /* cops excluded everywhere else */
    if (i >= TD5_BASE_CAR_COUNT) return 1;          /* TD6 */
    return i <= frontend_td5_car_cap_inclusive();   /* unlocked TD5 (police excluded by cap) */
}

/* Step `cur` by `delta` within [lo,hi], skipping unreachable indices so TD6
 * cars (37-75) are cyclable but locked TD5 cars / police are not. For ranges
 * that don't reach the TD6 block (era cup, Masters, Cop Chase) it is a plain
 * wrap. Never returns an out-of-range index, so "UNKNOWN CAR" can't appear. */
static int frontend_car_cycle_step(int cur, int delta, int lo, int hi) {
    int span = hi - lo + 1;
    if (span <= 0) return cur;
    for (int n = 0; n < span; n++) {
        cur += delta;
        if (cur < lo) cur = hi;
        if (cur > hi) cur = lo;
        if (hi < TD5_BASE_CAR_COUNT) return cur;    /* no TD6/police gap in this range */
        if (frontend_car_selectable(cur)) return cur;
    }
    return cur;
}

/* Color at grid cell (col,row): rows 0-1 = predefined swatches; rows 2.. = map. */
static uint32_t td6_cursor_color(int col, int row) {
    if (row < 2) {
        int idx = row * TD6_CP_COLS + col;
        if (idx < 0) idx = 0;
        if (idx >= TD6_PALETTE_N) idx = TD6_PALETTE_N - 1;
        return s_td6_palette[idx];
    }
    float u = (float)col / (float)(TD6_CP_COLS - 1);
    float v = (float)(row - 2) / (float)(TD6_CP_MAP_ROWS - 1);
    return td6_map_color(u, v);
}

/* Move the cursor to the predefined swatch matching the current color (if any),
 * so reopening the panel highlights the active color. */
static void frontend_color_panel_sync_index(void) {
    for (int k = 0; k < TD6_PALETTE_N; k++)
        if (s_td6_palette[k] == (uint32_t)g_td5.ini.td6_paint_color) {
            s_color_cur_row = k / TD6_CP_COLS;
            s_color_cur_col = k % TD6_CP_COLS;
            return;
        }
}

/* Check whether level data for a given track index is present on disk.
 * Checks both the zip archive and the loose extracted directory. */
static int frontend_track_level_exists(int track_index) {
    char path[64];
    int level_num;
    if (track_index < 0) return 1; /* -1 = random, always "valid" */
    level_num = td5_asset_level_number(track_index);
    snprintf(path, sizeof(path), "level%03d.zip", level_num);
    if (td5_plat_file_exists(path)) return 1;
    /* Also check for extracted loose-file directory (re/assets/levels/levelNNN/STRIP.DAT) */
    snprintf(path, sizeof(path), "re/assets/levels/level%03d/STRIP.DAT", level_num);
    if (td5_plat_file_exists(path)) return 1;
    /* Pack-on-load: STRIP.DAT may be retired in favour of the strip.json
     * editable source — treat the track as present if either exists. */
    snprintf(path, sizeof(path), "re/assets/levels/level%03d/strip.json", level_num);
    return td5_plat_file_exists(path);
}

/* Schedule slots 20-25 are the six championship CUPS (CHAMPIONSHIP / ERA /
 * CHALLENGE / PITBULL / MASTERS / ULTIMATE), NOT single tracks — they belong to
 * the Championship game type, not the single-race / quick-race track selector.
 * They leaked into the selectors because s_total_unlocked_tracks is forced to 37
 * (TD6 slots 26-36 are always unlocked, extending the cycle bound past the cup
 * range) and td5_asset_level_number(20..25) falls back to level001.zip (so the
 * level-exists check doesn't skip them) — so they showed up as "LOCKED" entries.
 * Treat them as non-selectable so the cyclers hop over them, leaving 0-19 (TD5)
 * and 26-36 (TD6) selectable. The cups remain reachable via the Championship
 * menu and the High Scores browser (which index slots 20-25 directly, not via
 * these cyclers). [user request 2026-06-05] */
static int frontend_track_is_cup_slot(int track_index) {
    return (track_index >= 20 && track_index <= 25);
}

/* Slots the single-race / quick-race track selectors must NOT land on:
 *   - DRAG STRIP (slot 19) — its own race type, not a normal circuit/sprint track
 *   - the six championship CUPS (20-25, frontend_track_is_cup_slot)
 * Both remain reachable elsewhere: the drag strip via its dedicated race type,
 * and the cups via the Championship menu + the High Scores browser (which index
 * slots 20-25 directly, not through these cyclers). Quick Race already excluded
 * the drag strip; this unifies it with Track Selection. [user request 2026-06-05] */
static int frontend_track_excluded_from_selector(int track_index) {
    return track_index == FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX ||
           frontend_track_is_cup_slot(track_index);
}

/* Advance track by delta (+1 or -1), skipping indices whose level zips are absent
 * and any slot excluded from the selector (drag strip + cups; see
 * frontend_track_excluded_from_selector).
 * Stops after one full revolution to avoid an infinite loop when no tracks exist. */
static void frontend_cycle_track(int delta, int track_min, int track_max) {
    int start = s_selected_track;
    int attempts = track_max - track_min + 1;
    while (attempts-- > 0) {
        s_selected_track += delta;
        if (s_selected_track < track_min) s_selected_track = track_max - 1;
        if (s_selected_track >= track_max) s_selected_track = track_min;
        if (frontend_track_excluded_from_selector(s_selected_track)) continue;
        if (frontend_track_level_exists(s_selected_track)) return;
    }
    /* No available track found -- restore original selection */
    s_selected_track = start;
}

/* [#14] Pick a random SELECTABLE car within [lo,hi] for the RANDOMIZE button.
 * Uses the project rand() (the MSVC CRT override in td5_msvc_rand.c). Honours the
 * same selectability rules as cycling (locked TD5 cars / police gap skipped) by
 * landing on a random index then stepping forward to the next selectable one.
 * For the Masters roster (game type 5) the index space is the roster slot list,
 * so it just picks a non-AI slot. Returns the chosen index (or the input `cur`
 * unchanged if the range is empty). */
static int frontend_pick_random_car(int cur, int lo, int hi) {
    if (hi < lo) return cur;
    int cand[TD5_CAR_COUNT];
    int n = 0;
    for (int i = lo; i <= hi && n < (int)(sizeof(cand)/sizeof(cand[0])); i++) {
        int ok;
        if (s_selected_game_type == 5)
            ok = (i >= 0 && i < 15 && s_masters_roster_flags[i] != 1); /* roster slot, non-AI */
        else
            ok = frontend_car_selectable(i);                           /* unlocked, non-gap */
        if (ok && i != cur) cand[n++] = i;                            /* prefer a change */
    }
    if (n == 0) {
        /* Only the current car qualifies (or the range is a single car): keep it. */
        return cur;
    }
    return cand[rand() % n];
}

/* [#14] Pick a random track for the RANDOMIZE button: choose a uniformly random
 * SELECTABLE, present track in [0, track_max) (drag strip + cups excluded, absent
 * levels skipped), different from the current one when possible. Writes
 * s_selected_track and returns 1 if it changed, 0 if no alternative exists.
 * track_max is the exclusive upper bound the caller already computed. */
static int frontend_pick_random_track(int track_max) {
    if (track_max <= 0) return 0;
    /* Collect the valid candidate slots, then pick one. Bounded, deterministic
     * count of rand() draws (one), and never lands on an excluded/absent slot. */
    int cand[64];
    int n = 0;
    for (int t = 0; t < track_max && n < (int)(sizeof(cand)/sizeof(cand[0])); t++) {
        if (t == s_selected_track) continue;                      /* prefer a change */
        if (frontend_track_excluded_from_selector(t)) continue;
        if (!frontend_track_level_exists(t)) continue;
        cand[n++] = t;
    }
    if (n == 0) {
        /* Only the current track is valid (or none) -- allow re-picking it so the
         * button still resolves to a concrete, raceable track. */
        for (int t = 0; t < track_max && n < (int)(sizeof(cand)/sizeof(cand[0])); t++) {
            if (frontend_track_excluded_from_selector(t)) continue;
            if (!frontend_track_level_exists(t)) continue;
            cand[n++] = t;
        }
        if (n == 0) return 0;
    }
    int pick = cand[rand() % n];
    if (pick == s_selected_track) return 0;
    s_selected_track = pick;
    return 1;
}

/* [#14] Master switch for the dedicated RANDOMIZE button (car + track selection).
 * Default ON: shows the button ABOVE the selector and removes the legacy
 * random-as-a-list-entry (the 2P track selector's wrap-to -1 "?" slot). Set
 * TD5RE_RANDOM_BUTTON=0 to restore the old behavior (no button; 2P keeps "?"). */
static int frontend_random_button_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RANDOM_BUTTON");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "RANDOMIZE button (#14) %s (TD5RE_RANDOM_BUTTON=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* [item #7 2026-06-15] FORM of the randomize control: a compact ICON to the
 * RIGHT of the selector (NEW default) vs the legacy full-width "Randomize"
 * button. Gated separately from frontend_random_button_on() (which still decides
 * whether the control EXISTS at all) so all the existing random_button_on()
 * consumers — 2P "?" defaults, index guards — stay byte-identical. Default ON =
 * icon; TD5RE_RANDOM_ICON=0 falls back to the previous full-width button. */
static int frontend_random_icon_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RANDOM_ICON");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "RANDOMIZE control form: %s (TD5RE_RANDOM_ICON=%s)",
                  v ? "small ICON" : "full button", e ? e : "default");
    }
    return v;
}

/* [#2b 2026-06-16] TD6 high-score placeholder suppression. Default ON: a TD6
 * track's post-race high-score screen shows ONLY genuine records (the player's
 * real runs) — never the fake seed names a clamped TD5 NPC group would render.
 * "0" restores the legacy clamp-to-TD5-group-25 placeholder behaviour. Cached +
 * logged once. Pairs the same-named knob/store in td5_save.c. */
static int td6_no_placeholder_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_TD6_NO_PLACEHOLDER_SCORES");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "TD6 high-score placeholder suppression (#2b) %s "
                  "(TD5RE_TD6_NO_PLACEHOLDER_SCORES=%s)",
                  v ? "ENABLED (real records only)" : "disabled (legacy clamp)",
                  e ? e : "default");
    }
    return v;
}

/* [#2b] The active TD6 LEVEL for the post-race track (s_selected_track), or 0 if
 * the track is a normal TD5 track / the feature is off. Uses the established
 * frontend TD6-track test (td5_asset_td6_level_for_slot > 0); also treats any
 * track index >= 26 (past the 26 authored TD5 tracks) as TD6 for robustness. */
static int frontend_postrace_td6_level(void) {
    if (!td6_no_placeholder_on()) return 0;
    int t = s_selected_track;
    if (t < 0) return 0;
    int lvl = td5_asset_td6_level_for_slot(t);
    if (lvl > 0) return lvl;
    if (t >= 26) {                       /* past the 26 authored TD5 tracks */
        int n = td5_asset_level_number(t);
        return n > 0 ? n : t;            /* fall back to a stable per-track key */
    }
    return 0;
}

/* Canvas-space footprint of the randomize ICON (square chip). Kept here so the
 * button hit-rect (created in the car/track screens) and the drawer agree. */
#define FE_RAND_ICON_W 28
#define FE_RAND_ICON_H 28

/* [item #7 / #12 2026-06-15] Reusable randomize-icon drawer. (x,y) is the chip's
 * TOP-LEFT in the 640x480 virtual canvas; sx/sy = window/canvas scale (same
 * convention as every other frontend overlay). Draws a die "5" dot pattern (a
 * recognisable "roll/shuffle" glyph). The rounded CHIP + rim highlight is drawn
 * ONLY when `focused` — when unselected there is NO background, just the white
 * pips (user request #12: drop the gray background when unselected, show the chip
 * only when it is the selected/focused button). NON-STATIC so another screen can
 * reuse it (caller adds its own extern). */
void frontend_draw_randomize_icon(float x, float y, float sx, float sy, int focused) {
    const float w = (float)FE_RAND_ICON_W;
    const float h = (float)FE_RAND_ICON_H;
    const float px = x * sx, py = y * sy, pw = w * sx, ph = h * sy;

    const uint32_t dot_col    = 0xFFFFFFFFu;   /* white pips (always drawn) */

    /* [R6b 2026-06-19] ALWAYS draw the chip + border so the icon reads as a real
     * button like its OK/Back siblings (the prior "[#12] focused-only chip" left it
     * borderless/invisible when unselected — the reported regression). Unfocused =
     * the dim blue button frame (matches fe_draw_button_frame_fill's unselected
     * blue stops) over a transparent-ish steel interior; focused = the amber rim +
     * lit interior. */
    {
        uint32_t chip_c, rim_c;
        if (focused) { chip_c = 0xFF3A4660u; rim_c = 0xFFFFCC33u; }  /* lit steel + amber */
        else         { chip_c = 0x90202838u; rim_c = 0xFF496BDCu; }  /* dim steel + button-blue rim */
        /* Prefer the procedural neon rounded-rect (crisp at any res); fall back
         * to a solid quad + a manual 4-edge border when shapes aren't available. */
        if (td5_vui_shapes_available()) {
            td5_vui_roundrect(px, py, pw, ph,
                              5.0f * sy, 5.0f * sy,     /* corner radii */
                              2.0f * sx, 2.0f * sy,     /* rim thickness */
                              rim_c, rim_c, rim_c, chip_c, 1.0f);
        } else {
            td5_vui_quad(px, py, pw, ph, chip_c, -1, 0, 0, 0, 0);
            float bx = 2.0f * sx, byb = 2.0f * sy;
            td5_vui_quad(px, py, pw, byb, rim_c, -1, 0, 0, 0, 0);            /* top    */
            td5_vui_quad(px, py + ph - byb, pw, byb, rim_c, -1, 0, 0, 0, 0); /* bottom */
            td5_vui_quad(px, py, bx, ph, rim_c, -1, 0, 0, 0, 0);            /* left   */
            td5_vui_quad(px + pw - bx, py, bx, ph, rim_c, -1, 0, 0, 0, 0);  /* right  */
        }
    }

    /* Die "5" pip pattern: four corners + centre, inside the chip's inner area.
     * Coords in canvas px (relative to x,y), then scaled. */
    const float d = 4.0f;            /* pip size (canvas px) */
    const float m = 7.0f;            /* inset of corner pips from chip edge */
    const float cx = x + w * 0.5f - d * 0.5f;
    const float cy = y + h * 0.5f - d * 0.5f;
    const float lx = x + m,          rx = x + w - m - d;
    const float ty = y + m,          by = y + h - m - d;
    const float pip[5][2] = {
        { lx, ty }, { rx, ty },
        { cx, cy },
        { lx, by }, { rx, by },
    };
    for (int i = 0; i < 5; i++)
        td5_vui_quad(pip[i][0] * sx, pip[i][1] * sy, d * sx, d * sy,
                     dot_col, -1, 0, 0, 0, 0);
}

/* [item #7] Render-path drawers for the randomize ICON on the car- and track-
 * selection screens. The hit-rect button (s_*_rand_btn) is created HIDDEN so the
 * generic button loop skips its 9-slice + label; we paint the icon here instead,
 * at the button's own rect, lit when it holds focus. NON-STATIC so td5_frontend.c
 * can call them from its render switch (CAR_SELECTION / TRACK_SELECTION). Inert
 * when the control is off, in full-button form, or the handle isn't live. */
void frontend_render_carsel_randomize_icon(float sx, float sy) {
    if (!frontend_random_button_on() || !frontend_random_icon_on()) return;
    if (!s_anim_complete) return;                                 /* wait for slide-in to settle */
    int b = s_carsel_rand_btn;
    if (b < 0 || b >= FE_MAX_BUTTONS) return;
    if (!s_buttons[b].active || s_buttons[b].hidden == 0) return; /* full-button mode draws itself */
    if (s_buttons[b].disabled) return;                            /* hidden by the colour picker */
    frontend_draw_randomize_icon((float)s_buttons[b].x, (float)s_buttons[b].y,
                                 sx, sy, (s_selected_button == b));
}

void frontend_render_trksel_randomize_icon(float sx, float sy) {
    if (!frontend_random_button_on() || !frontend_random_icon_on()) return;
    if (!s_anim_complete) return;                                 /* wait for slide-in to settle */
    int b = s_trksel_rand_btn;
    if (b < 0 || b >= FE_MAX_BUTTONS) return;
    if (!s_buttons[b].active || s_buttons[b].hidden == 0) return;
    if (s_buttons[b].disabled) return;
    frontend_draw_randomize_icon((float)s_buttons[b].x, (float)s_buttons[b].y,
                                 sx, sy, (s_selected_button == b));
}

static void frontend_load_selected_car_preview(void) {
    int car_index = frontend_current_car_index();
    if (s_car_preview_surface > 0) {
        frontend_release_surface(s_car_preview_surface);
        s_car_preview_surface = 0;
    }
    s_car_preview_surface = frontend_load_car_preview_surface(car_index, s_selected_paint);
    /* Invalidate the paint-overlay cache so it RELOADS with the preview. On
     * car-select ENTRY (incl. returning from a race) the car index is unchanged,
     * so the lazy per-car load wouldn't refresh it — and a race may have reused
     * its texture page (it would otherwise paint the WHOLE body from stale
     * content). DROP the handle too (set to 0) — do NOT frontend_release_surface()
     * it: the old overlay slot was already freed by td5_frontend_set_screen's
     * recyclable sweep, and the preview we just loaded may now occupy that very
     * slot. Releasing the stale handle would free the fresh preview; uploading
     * CarPicPaint0 onto it then showed only the painted chassis. Zeroing the
     * handle leaks nothing (slot already free) and guarantees the lazy reload in
     * frontend_draw_car_paint_overlay takes the clean "no prior surface" path. */
    s_paint_overlay_surface = 0;
    s_paint_overlay_car = -1;
    TD5_LOG_I(LOG_TAG, "car preview (re)loaded: car=%d surf=%d; paint overlay cache invalidated",
              car_index, s_car_preview_surface);
}

static void frontend_load_selected_track_preview(void) {
    char entry[32];
    int tga_idx;
    if (s_track_preview_surface > 0) {
        frontend_release_surface(s_track_preview_surface);
        s_track_preview_surface = 0;
    }
    if (s_selected_track < 0) return;
    /* Use the original binary's gScheduleToPoolIndex table (DAT_00466894):
     * pool_index == trak TGA file number, NOT the schedule slot. */
    tga_idx = (s_selected_track < (int)(sizeof(s_track_schedule_to_tga_index)))
              ? s_track_schedule_to_tga_index[s_selected_track]
              : s_selected_track;
    snprintf(entry, sizeof(entry), "trak%04d.tga", tga_idx);
    /* Black background is color-keyed out so the track outline floats over the scene background. */
    s_track_preview_surface = frontend_load_surface_keyed(entry, "Front End/Tracks/Tracks.zip", TD5_COLORKEY_BLACK);
}

/* Idempotent: position the 6 car-select buttons for the current panel state.
 * Closed = the normal rows. Open = CAR/PAINT shift UP and Stats/Auto/OK/Back drop
 * BELOW the color panel, but compressed so OK/Back don't sit too low. Recomputed
 * from these tables every frame, so it survives button recreation (no drift). */
static void frontend_apply_color_panel_layout(void) {
    /* Closed layout: CAR/PAINT at the top, then a ~52px gap (y=241..293) holding
     * the non-interactive at-a-glance stat-bar panel, then MORE STATS/AUTO/OK/
     * BACK pushed down (gaps tightened 8->4px) so the bottom row still sits inside
     * the y<408 blue-fill area. Open layout (TD6 colour picker) is unchanged — the
     * stat panel is hidden while the picker fills that band. */
    static const int closed_y[6] = { 169, 205, 297, 333, 369, 369 };
    static const int open_y[6]   = { 150, 190, 336, 372, 408, 408 };
    const int *y = s_color_panel_visible ? open_y : closed_y;
    for (int i = 0; i < 6 && i < FE_MAX_BUTTONS; i++)
        if (s_buttons[i].active) s_buttons[i].y = y[i];
}

static void frontend_set_color_panel(int open) {
    s_color_panel_visible = open ? 1 : 0;
    if (s_color_panel_visible) frontend_color_panel_sync_index();
    frontend_apply_color_panel_layout();
}

/* Mouse pick inside the open panel: clicking a swatch or a map cell moves the
 * 2D cursor to it. Returns 1 if it set the cursor (caller applies the color).
 * Mouse coords are in 640x480 canvas space, same as the swatch/map rects. */
static int frontend_color_panel_mouse(void) {
    if (!s_mouse_clicked) return 0;
    int mx = s_mouse_x, my = s_mouse_y;
    for (int i = 0; i < TD6_PALETTE_N; i++) {
        int c = i % TD6_CP_COLS, r = i / TD6_CP_COLS;
        int x = TD6_CP_LIST_X + c * (TD6_CP_SW + TD6_CP_GAP);
        int y = TD6_CP_LIST_Y + r * (TD6_CP_SW + TD6_CP_GAP);
        if (mx >= x && mx < x + TD6_CP_SW && my >= y && my < y + TD6_CP_SW) {
            s_color_cur_col = c; s_color_cur_row = r;
            return 1;
        }
    }
    if (mx >= TD6_CP_MAP_X && mx < TD6_CP_MAP_X + TD6_CP_MAP_W &&
        my >= TD6_CP_MAP_Y && my < TD6_CP_MAP_Y + TD6_CP_MAP_H) {
        int col = (mx - TD6_CP_MAP_X) * TD6_CP_COLS / TD6_CP_MAP_W;
        int row = (my - TD6_CP_MAP_Y) * TD6_CP_MAP_ROWS / TD6_CP_MAP_H;
        if (col >= TD6_CP_COLS)     col = TD6_CP_COLS - 1;
        if (row >= TD6_CP_MAP_ROWS) row = TD6_CP_MAP_ROWS - 1;
        s_color_cur_col = col; s_color_cur_row = 2 + row;
        return 1;
    }
    return 0;
}

/* Write cup data: sync game state into save module, then write file. */
static int frontend_write_cup_data(void) {
    td5_save_sync_cup_from_game(s_race_within_series);
    int ok = td5_save_write_cup_data(NULL);
    TD5_LOG_I(LOG_TAG, "WriteCupData: result=%d race=%d type=%d",
              ok, s_race_within_series, (int)s_selected_game_type);
    return ok;
}

/* Delete cup data file.
 * [CONFIRMED @ 0x423ACD]: original calls _unlink(CupData.td5) directly in
 * ScreenCupWonDialog case 0. Port delegates to td5_plat_file_delete which
 * wraps DeleteFileA — semantically identical. */
static void frontend_delete_cup_data(void) {
    TD5_LOG_I(LOG_TAG, "frontend_delete_cup_data: removing td5re_cup.ini");
    td5_save_delete_cup_data();
}

/* Cycle s_selected_track by delta, skipping the Drag Strip (schedule index 19)
 * and any track whose level data is absent. One full revolution then give up. */
static void frontend_quickrace_cycle_track(int delta) {
    int track_max = s_total_unlocked_tracks; /* exclusive bound (net incl. TD6 26-36 now) */
    if (track_max <= 0) return;
    int start = s_selected_track;
    int attempts = track_max + 1;
    while (attempts-- > 0) {
        s_selected_track += delta;
        if (s_selected_track < 0) s_selected_track = track_max - 1;
        if (s_selected_track >= track_max) s_selected_track = 0;
        if (frontend_track_excluded_from_selector(s_selected_track)) continue; /* skip drag strip + cups */
        if (frontend_track_level_exists(s_selected_track)) return;
    }
    s_selected_track = start; /* nothing else available — restore (never lands on drag strip / cup) */
}

/* Clamp the human/AI counts so 1 <= humans <= 6 and 0 <= opponents <= 6-humans.
 * The counts render as value text to the right of the Players/Opponents buttons
 * (frontend_render_quick_race_overlay), so no button labels are touched here. */
static void frontend_quickrace_clamp_counts(void) {
    if (s_num_human_players < 1) s_num_human_players = 1;
    /* [PORT ENHANCEMENT] up to TD5_MAX_HUMAN_PLAYERS (9) local split-screen humans. */
    if (s_num_human_players > TD5_MAX_HUMAN_PLAYERS) s_num_human_players = TD5_MAX_HUMAN_PLAYERS;
    int opp_max = TD5_MAX_RACER_SLOTS - s_num_human_players;
    if (s_num_ai_opponents < 0) s_num_ai_opponents = 0;
    if (s_num_ai_opponents > opp_max) s_num_ai_opponents = opp_max;

    /* [2026-06-08] AI spectator panes: at most one pane per AI car, and the
     * total panes (humans + spectators) cannot exceed the viewport cap. */
    int spectate_max = TD5_MAX_VIEWPORTS - s_num_human_players;
    if (spectate_max > s_num_ai_opponents) spectate_max = s_num_ai_opponents;
    if (spectate_max < 0) spectate_max = 0;
    if (s_num_spectate_screens < 0) s_num_spectate_screens = 0;
    if (s_num_spectate_screens > spectate_max) s_num_spectate_screens = spectate_max;
}

void Screen_QuickRaceMenu(void) {
    switch (s_inner_state) {
    case 0: /* Init: validate indices, create the 8-row improved layout (row 6 =
            * dev Span Offset, row 7 = OK/Back; dev rows hidden in release) */
        frontend_init_return_screen(TD5_SCREEN_QUICK_RACE);
        TD5_LOG_D(LOG_TAG, "QuickRaceMenu: init");
        s_anim_complete = 0;
        /* Load background: same as main menu */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Validate car/track indices; never start on the Drag Strip or a cup
         * (excluded from the selector, see frontend_track_excluded_from_selector). */
        if (s_selected_car < 0) s_selected_car = 0;
        if (s_selected_track < 0) s_selected_track = 0;
        if (s_selected_track >= 26) s_selected_track = 0;
        if (frontend_track_excluded_from_selector(s_selected_track)) s_selected_track = 0;

        /* Improved layout (PORT ENHANCEMENT): caption selectors with the selected
         * value drawn to the RIGHT of each button (value column), plus OK/Back on
         * the bottom row. Rows are uniformly spaced (QR_ROW_Y0 + n*QR_ROW_DY). The
         * Direction row hides on forward-only/circuit tracks. Values are drawn by
         * frontend_render_quick_race_overlay at FE_QR_VALUE_X.
         *
         * [S02 (b) 2026-06-04] These caption rows are NOT marked is_selector: the
         * VectorUI procedural-button path (the default) draws a caption only for
         * non-selector buttons, so flagging them as selectors left the captions
         * blank. They render their captions through the shared fe_draw_text path
         * (same as the Track Selection option rows); the ◄► arrows are still drawn
         * by the explicit fe_draw_option_arrows loop in td5_frontend_render_ui_rects. */
        { int bi;
          frontend_create_button("Car",                 QR_COL_X, QR_ROW_Y(0), QR_BTN_W, 32); /* QR_BTN_CAR */
          frontend_create_button("Track",               QR_COL_X, QR_ROW_Y(1), QR_BTN_W, 32); /* QR_BTN_TRACK */
          frontend_create_button("Direction",           QR_COL_X, QR_ROW_Y(2), QR_BTN_W, 32); /* QR_BTN_DIRECTION */
          /* [PORT ENHANCEMENT 2026-06] Quick Race is single-player (driven by the
           * active controller); the Players row is hidden — local multiplayer is
           * the main-menu MULTIPLAYER lobby. The row is still CREATED (to keep the
           * QR_BTN_* indices stable) but hidden, and Opponents/Laps/OK/Back close
           * the gap. */
          bi = frontend_create_button("Players",             QR_COL_X, QR_ROW_Y(3), QR_BTN_W, 32); /* QR_BTN_PLAYERS */
          if (bi >= 0) { s_buttons[bi].hidden = 1; s_buttons[bi].disabled = 1; }
          frontend_create_button("Opponents",           QR_COL_X, QR_ROW_Y(3), QR_BTN_W, 32); /* QR_BTN_OPPONENTS */
          /* [S02 (c) 2026-06-04] Circuit laps, re-homed here from Game Options.
           * Mirrors the Track Selection laps row; edits s_game_option_laps. */
          frontend_create_button("Laps",                QR_COL_X, QR_ROW_Y(4), QR_BTN_W, 32); /* QR_BTN_LAPS */
          /* [2026-06-08] AI Screens (dev/profiling): render the first N AI cars
           * each in their own split-screen pane. Dev-only — the row is created
           * for index stability but hidden+disabled in release builds (the
           * SpectateScreens knob is also clamped to 0 there). */
          bi = frontend_create_button("AI Screens",        QR_COL_X, QR_ROW_Y(5), QR_BTN_W, 32); /* QR_BTN_SPLITSCREENS */
#ifdef TD5RE_RELEASE
          if (bi >= 0) { s_buttons[bi].hidden = 1; s_buttons[bi].disabled = 1; }
#else
          (void)bi;
#endif
        }
        frontend_create_button(SNK_OkButTxt,           QR_COL_X,       QR_ROW_Y(7),  96, 32); /* QR_BTN_OK */
        frontend_create_button(SNK_BackButTxt,         QR_COL_X + 108, QR_ROW_Y(7), 112, 32); /* QR_BTN_BACK */
        /* [2026-06-12] Dev-only toggles to the RIGHT of the Opponents row (y=row3):
         * PlayerIsAI (slot 0 = AI) and AutoThrottle (trace auto-throttle). They sit
         * past the Opponents value column; A/Enter flips them, the label shows state.
         * Created AFTER OK/Back so QR_BTN_OK/BACK indices stay 7/8. Hidden in release. */
        { int bp = frontend_create_button("Player AI", FE_QR_VALUE_X + 24, QR_ROW_Y(3), 134, 32); /* QR_BTN_PLAYERAI */
          int bt = frontend_create_button("Auto-Thr",  FE_QR_VALUE_X + 162, QR_ROW_Y(3), 134, 32); /* QR_BTN_AUTOTHR */
          if (bp >= 0) snprintf(s_buttons[bp].label, sizeof s_buttons[bp].label,
                                "Player AI: %s", g_td5.ini.player_is_ai ? "ON" : "OFF");
          if (bt >= 0) snprintf(s_buttons[bt].label, sizeof s_buttons[bt].label,
                                "Auto-Thr: %s", g_td5.ini.auto_throttle ? "ON" : "OFF");
#ifdef TD5RE_RELEASE
          if (bp >= 0) { s_buttons[bp].hidden = 1; s_buttons[bp].disabled = 1; }
          if (bt >= 0) { s_buttons[bt].hidden = 1; s_buttons[bt].disabled = 1; }
#endif
        }
        /* [2026-06-15 TASK A1] Dev-only "Span Offset" click-to-type button on its
         * OWN row below the "AI Screens" row (QR_BTN_SPLITSCREENS @ row5); OK/Back
         * moved down to row7 above. Created LAST so QR_BTN_OK/BACK/PLAYERAI/AUTOTHR
         * indices stay 7/8/9/10. Activating it toggles the span input-active state
         * (frontend_qr_span_toggle_active in td5_frontend.c, which owns the editable
         * buffer + the value/caret render). Hidden+disabled in release; also gated
         * by the existing TD5RE_DEV_SPAN_FIELD knob via the widget render path. */
        { int bs = frontend_create_button("Span Offset", QR_COL_X, QR_ROW_Y(6), QR_BTN_W, 32); /* QR_BTN_SPAN */
#ifdef TD5RE_RELEASE
          if (bs >= 0) { s_buttons[bs].hidden = 1; s_buttons[bs].disabled = 1; }
#else
          /* TD5RE_DEV_SPAN_FIELD=0 hides the whole field (button + value/caret); the
           * value/caret render in td5_frontend.c gates on the same knob. Cached here
           * (the canonical one-time log lives in frontend_qr_span_field_on). */
          static int s_qr_span_btn_on = -1;
          if (s_qr_span_btn_on < 0) {
              const char *e = getenv("TD5RE_DEV_SPAN_FIELD");
              s_qr_span_btn_on = (e && e[0] == '0') ? 0 : 1;
          }
          if (bs >= 0 && !s_qr_span_btn_on) { s_buttons[bs].hidden = 1; s_buttons[bs].disabled = 1; }
#endif
        }

        /* [#10 2026-06-16] Dedicated RANDOMIZE buttons for the Car and Track rows.
         * Created LAST (indices QR_BTN_RAND_CAR=12 / QR_BTN_RAND_TRACK=13) so every
         * hard-coded index above is unchanged. They are REAL frontend_create_button
         * entries — so (a) SHIFT+arrow geometric focus nav can land on them (same
         * row as Car/Track, to the right) and (b) the standard button renderer
         * draws them with the normal button format. Their presses are dispatched in
         * state 4 to frontend_qr_roll_selector. When the knob is off they are
         * created hidden+disabled and the legacy render-path icon hack runs instead
         * (frontend_qr_random_icon_on auto-suppresses itself in button mode). */
        {
            int rc = frontend_create_button("Randomize", QR_RAND_BTN_X, QR_ROW_Y(0), QR_RAND_BTN_W, 32); /* QR_BTN_RAND_CAR */
            int rt = frontend_create_button("Randomize", QR_RAND_BTN_X, QR_ROW_Y(1), QR_RAND_BTN_W, 32); /* QR_BTN_RAND_TRACK */
            if (!frontend_qr_random_button_on()) {
                if (rc >= 0) { s_buttons[rc].hidden = 1; s_buttons[rc].disabled = 1; }
                if (rt >= 0) { s_buttons[rt].hidden = 1; s_buttons[rt].disabled = 1; }
            }
        }

        /* Reset direction to Forwards on entry (matches TrackSelection); hide the
         * toggle on forward-only/circuit tracks (caption stays "Direction" —
         * manage_label=0). Clamp the player/opponent counts. */
        s_track_direction = 0;
        frontend_update_direction_button_visibility(QR_BTN_DIRECTION, 0);
        /* Hide the Laps row on point-to-point tracks (no laps); show on circuits. */
        frontend_update_laps_button_visibility(QR_BTN_LAPS);
        frontend_quickrace_clamp_counts();

        /* Mouse-interactive screen: restore the cursor in case the prior screen
         * (pad-driven MP grid, harness StartScreen jump) left it hidden.
         * [cursor-fix 2026-06-12] */
        frontend_set_cursor_visible(1);

        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Present + reset counter */
    case 2:
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Interactive: cycle car/track/direction/players/opponents, OK/Back */
        /* [hold-to-scroll on Quick Race 2026-06-16] Re-synthesize LEFT/RIGHT
         * auto-repeat while a value-cycling row is focused so HOLDING ◄► keeps
         * cycling, exactly like the car-select selector (reuses the same
         * frontend_carsel_hold_repeat / TD5RE_CARSEL_HOLD). Plain ◄► only — SHIFT
         * here is the horizontal focus-move, so skip while SHIFT is held; DIRECTION
         * is a toggle (not a cycler) and is excluded. */
        if (s_anim_complete && s_button_index < 0 &&
            !(td5_plat_input_key_pressed(0x2A) || td5_plat_input_key_pressed(0x36))) {
            int sb = s_selected_button;
            if (sb == QR_BTN_CAR || sb == QR_BTN_TRACK || sb == QR_BTN_OPPONENTS ||
                sb == QR_BTN_PLAYERS || sb == QR_BTN_LAPS || sb == QR_BTN_SPLITSCREENS) {
                int rep = frontend_carsel_hold_repeat();
                if (rep) { s_arrow_input |= rep; s_input_ready = 1; }
            }
        }
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int selected_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (selected_button == QR_BTN_CAR && delta != 0) {
                /* Cycle car. TD6 cars (37-75) are included; locked TD5 cars and
                 * police are skipped; the result is always a valid index, so the
                 * old off-by-one "UNKNOWN CAR" entry (index == unlocked count) is
                 * gone. */
                s_selected_car = frontend_car_cycle_step(s_selected_car, delta,
                                                         0, TD5_CAR_COUNT - 1);
                frontend_play_sfx(2); /* ping2.wav cycle */
            }

            if (selected_button == QR_BTN_TRACK && delta != 0) {
                /* Cycle track, skipping Drag Strip (index 19) + absent levels. */
                frontend_quickrace_cycle_track(delta);
                /* Re-evaluate the Direction toggle + Laps row for the new track. */
                frontend_update_direction_button_visibility(QR_BTN_DIRECTION, 0);
                frontend_update_laps_button_visibility(QR_BTN_LAPS);
                TD5_LOG_I(LOG_TAG, "QuickRace track cycle: s_selected_track=%d level=%d name=%s",
                          s_selected_track, td5_asset_level_number(s_selected_track),
                          frontend_get_track_name(s_selected_track));
                frontend_play_sfx(2); /* ping2.wav cycle */
            }

            /* Direction toggle: 0=Forwards, 1=Backwards. Inert (and hidden) on
             * forward-only/circuit tracks. Responds to either arrow or button
             * press (mirrors TrackSelection 0x00427630 button-1 behavior). The
             * value renders to the right of the button (caption stays "Direction"). */
            if (selected_button == QR_BTN_DIRECTION && !s_buttons[QR_BTN_DIRECTION].hidden &&
                (delta != 0 || s_button_index == QR_BTN_DIRECTION)) {
                s_track_direction = !s_track_direction;
                frontend_play_sfx(2);
            }

            /* Players (human) count: 1..6. Increasing past the 6-racer cap auto-
             * reduces Opponents (handled in frontend_quickrace_clamp_counts). */
            if (selected_button == QR_BTN_PLAYERS && delta != 0) {
                s_num_human_players += delta;
                frontend_quickrace_clamp_counts();
                frontend_play_sfx(2);
            }

            /* Opponents (AI) count: 0..(6 - humans). */
            if (selected_button == QR_BTN_OPPONENTS && delta != 0) {
                s_num_ai_opponents += delta;
                frontend_quickrace_clamp_counts();
                frontend_play_sfx(2);
            }

            /* [S02 (c) 2026-06-04] Circuit laps (re-homed from Game Options).
             * Stored 0..9, displayed value+1 (so 1..10 laps); race setup reads
             * g_td5.circuit_lap_count = s_game_option_laps + 1. Matches the Track
             * Selection laps row's range. */
            if (selected_button == QR_BTN_LAPS && !s_buttons[QR_BTN_LAPS].hidden &&
                delta != 0) {
                s_game_option_laps += delta;
                if (s_game_option_laps < 0) s_game_option_laps = 0;
                if (s_game_option_laps > 9) s_game_option_laps = 9;
                frontend_play_sfx(2);
            }

            /* [2026-06-08] AI Screens (dev/profiling): 0..min(opponents,
             * TD5_MAX_VIEWPORTS-1). Each step adds an AI car to its own pane.
             * Hidden+disabled in release (see case-0 creation). */
            if (selected_button == QR_BTN_SPLITSCREENS &&
                !s_buttons[QR_BTN_SPLITSCREENS].hidden && delta != 0) {
                s_num_spectate_screens += delta;
                frontend_quickrace_clamp_counts();
                frontend_play_sfx(2);
            }

            /* [2026-06-12] Dev toggles (right of Opponents row): flip on A/Enter.
             * Apply live to g_td5.ini (read at race start) + refresh the label. */
            if (s_button_index == QR_BTN_PLAYERAI && s_button_count > QR_BTN_PLAYERAI &&
                !s_buttons[QR_BTN_PLAYERAI].hidden) {
                g_td5.ini.player_is_ai = !g_td5.ini.player_is_ai;
                snprintf(s_buttons[QR_BTN_PLAYERAI].label, sizeof s_buttons[QR_BTN_PLAYERAI].label,
                         "Player AI: %s", g_td5.ini.player_is_ai ? "ON" : "OFF");
                frontend_play_sfx(2);
                TD5_LOG_I(LOG_TAG, "QuickRace dev toggle: PlayerIsAI=%d", g_td5.ini.player_is_ai);
            }
            if (s_button_index == QR_BTN_AUTOTHR && s_button_count > QR_BTN_AUTOTHR &&
                !s_buttons[QR_BTN_AUTOTHR].hidden) {
                g_td5.ini.auto_throttle = !g_td5.ini.auto_throttle;
                snprintf(s_buttons[QR_BTN_AUTOTHR].label, sizeof s_buttons[QR_BTN_AUTOTHR].label,
                         "Auto-Thr: %s", g_td5.ini.auto_throttle ? "ON" : "OFF");
                frontend_play_sfx(2);
                TD5_LOG_I(LOG_TAG, "QuickRace dev toggle: AutoThrottle=%d", g_td5.ini.auto_throttle);
            }

#ifndef TD5RE_RELEASE
            /* [2026-06-15 TASK A1] Dev "Span Offset" button: A/Enter (or a click)
             * toggles the click-to-type input-active state. While active, the
             * widget render path captures typed digits/backspace/'-' into the span
             * buffer and commits live to g_td5.ini.start_span_offset; pressing
             * A/Enter again deactivates (commits). State + buffer + render live in
             * td5_frontend.c (frontend_qr_span_toggle_active). */
            if (s_button_index == QR_BTN_SPAN && s_button_count > QR_BTN_SPAN &&
                !s_buttons[QR_BTN_SPAN].hidden) {
                frontend_qr_span_toggle_active();
            }
#endif

            /* [#10] Dedicated RANDOMIZE buttons (Car/Track rows). Activating one
             * rolls that selector to a random valid value (frontend_qr_roll_selector
             * also refreshes the Direction/Laps rows on a track roll + plays the
             * cycle cue). Only when the button mode is on (off => hidden+disabled,
             * never the active button). */
            if (frontend_qr_random_button_on() &&
                s_button_index == QR_BTN_RAND_CAR && s_button_count > QR_BTN_RAND_CAR) {
                frontend_qr_roll_selector(0);   /* roll CAR */
            }
            if (frontend_qr_random_button_on() &&
                s_button_index == QR_BTN_RAND_TRACK && s_button_count > QR_BTN_RAND_TRACK) {
                frontend_qr_roll_selector(1);   /* roll TRACK */
            }

            if (s_button_index == QR_BTN_OK) {
                /* Block if car or track is locked */
                int car_locked = (!s_cheat_unlock_all && !s_network_active &&
                                  s_selected_car >= 0 && s_selected_car < 37 &&
                                  s_car_lock_table[s_selected_car] != 0);
                int track_locked = (!s_cheat_unlock_all && !s_network_active &&
                                    s_selected_track >= 0 && s_selected_track < 37 &&
                                    s_track_lock_table[s_selected_track] != 0);
                if (car_locked || track_locked) {
                    frontend_play_sfx(10); /* rejection */
                } else {
                    /* Commit the selected direction; counts are read by
                     * frontend_init_race_schedule (gated to Quick Race). */
                    g_td5.reverse_direction = s_track_direction;
                    /* [S02 (c) 2026-06-04] Persist the lap choice (re-homed from
                     * Game Options' OK, which no longer owns this setting). */
                    g_td5.ini.laps = s_game_option_laps;
                    td5_ini_persist_options();
                    TD5_LOG_I(LOG_TAG,
                              "QuickRace OK: track=%d dir=%s humans=%d opponents=%d laps=%d",
                              s_selected_track, s_track_direction ? "Backwards" : "Forwards",
                              s_num_human_players, s_num_ai_opponents, s_game_option_laps + 1);
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 5;
                }
            }
            if (s_button_index == QR_BTN_BACK) {
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 5;
            }
        }
        break;

    case 5: /* Prep slide-out */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 6;
        break;

    case 6: /* Slide-out: ~500ms, then dispatch */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            if (s_return_screen == -1) {
                /* Start race */
                frontend_init_race_schedule();
                frontend_init_display_mode_state();
            } else {
                td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
            }
        }
        break;
    }
}

/* This player's navigation bits (same encoding as the platform nav helpers:
 * 1 LEFT 2 RIGHT 4 UP 8 DOWN 0x10 A/confirm 0x20 B/back). Joystick players poll
 * their own device through the shared scan handle; the keyboard player (device 0)
 * reads arrows + Enter/Esc directly. */
static uint32_t mp_simul_player_nav(int player) {
    int dev = s_mp_join_device[player];
    uint32_t b = 0;
    if (dev > 0) return td5_plat_input_device_nav(dev);
    if (td5_plat_input_key_pressed(0xCB)) b |= 1;     /* Left          */
    if (td5_plat_input_key_pressed(0xCD)) b |= 2;     /* Right         */
    if (td5_plat_input_key_pressed(0xC8)) b |= 4;     /* Up            */
    if (td5_plat_input_key_pressed(0xD0)) b |= 8;     /* Down          */
    if (td5_plat_input_key_pressed(0x1C) || td5_plat_input_key_pressed(0x39)) b |= 0x10; /* Enter/Space */
    if (td5_plat_input_key_pressed(0x0E)) b |= 0x20;  /* Backspace = back (ESC handled globally) */
    return b;
}

/* Release a pane's cached surface handle, but only if no OTHER pane currently
 * aliases it — the surface cache dedups identical car+paint loads, so two panes
 * picking the same car share one handle. Sets the slot to 0. */
static void mp_simul_drop_handle(int *cache, int player, int n) {
    int old = cache[player];
    if (old > 0) {
        int q, shared = 0;
        for (q = 0; q < n; q++)
            if (q != player && cache[q] == old) { shared = 1; break; }
        if (!shared) frontend_release_surface(old);
    }
    cache[player] = 0;
}

/* (Re)load a pane's carpic preview + (TD6) body-paint overlay for its current
 * car/paint. Loads the new handle BEFORE dropping the old one so the loader
 * can't reuse the old slot mid-swap. */
static void mp_simul_refresh_pane(int player) {
    int n = s_num_human_players;
    int car = s_mp_player_car[player];
    int td6 = frontend_car_is_td6(car);
    int paint = td6 ? 0 : s_mp_player_paint[player];
    int prev_h = frontend_load_car_preview_surface(car, paint);
    int over_h = (td6 && frontend_car_paintable(car))
                 ? frontend_load_car_paint_overlay_surface(car) : 0;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    if (s_mp_pane_preview[player] != prev_h) {
        mp_simul_drop_handle(s_mp_pane_preview, player, n);
        s_mp_pane_preview[player] = prev_h;
    }
    if (s_mp_pane_overlay[player] != over_h) {
        mp_simul_drop_handle(s_mp_pane_overlay, player, n);
        s_mp_pane_overlay[player] = over_h;
    }
}

/* Free every pane's cached surfaces (on commit / cancel). */
static void mp_simul_free_all_panes(int n) {
    int p;
    for (p = 0; p < n; p++) {
        mp_simul_drop_handle(s_mp_pane_preview, p, n);
        mp_simul_drop_handle(s_mp_pane_overlay, p, n);
    }
}

/* Cycle a pane's paint/colour by `step` (+1/-1). TD6 cars walk the body-colour
 * palette; TD5 cars cycle their 4 paint schemes (no-op for paintless cars).
 * `sfx`=0 mutes the cycle ping — hold-to-repeat (Task #7) passes 0 so the sound
 * plays only on the initial press, not at the repeat rate while held. */
static void mp_simul_cycle_paint(int p, int step, int sfx) {
    int car = s_mp_player_car[p];
    if (frontend_car_paintable(car)) {
        int idx = s_mp_player_color_idx[p] + step;
        if (idx < 0) idx = TD6_PALETTE_N - 1;
        if (idx >= TD6_PALETTE_N) idx = 0;
        s_mp_player_color_idx[p] = idx;
        s_mp_player_color[p]     = (int)s_td6_palette[idx];
        if (sfx) frontend_play_sfx(2);
    } else if (!frontend_car_is_td6(car) && frontend_car_has_paint(car)) {
        int pa = s_mp_player_paint[p] + step;
        if (pa < 0) pa = 3;
        if (pa > 3) pa = 0;
        s_mp_player_paint[p] = pa;
        mp_simul_refresh_pane(p);
        if (sfx) frontend_play_sfx(2);
    }
}

static void frontend_mp_simul_carsel_init(void) {
    int p, n = s_num_human_players;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    frontend_init_return_screen(TD5_SCREEN_CAR_SELECTION);
    frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
    frontend_reset_buttons();
    frontend_set_color_panel(0);
    frontend_set_cursor_visible(0);

    /* Drop any per-player EXCLUSIVE devices so the shared non-exclusive scan
     * handles can poll each pad again. On first entry these aren't bound (the
     * lobby skipped that for simul); on re-entry from track select they are. */
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        td5_input_set_input_source(p, 0);

    /* Full single-race roster (TD6 cars reachable; cycle_step skips locked gaps). */
    s_car_roster_min = 0;
    s_car_roster_max = TD5_CAR_COUNT - 1;

    for (p = 0; p < n; p++) {
        /* [MP SESSION PERSISTENCE 2026-06] Phase-1 grid: prefer the session-saved
         * car/paint/color/trans (already mirrored into the live arrays by the
         * lobby START restore) over the per-entry defaults, so a returning player
         * keeps their previous pick. Recover the palette cursor from the restored
         * colour where it matches a swatch (so the paint cycler resumes there).
         * Gated by TD5RE_MP_SESSION (off / not-yet-valid => defaults, as before). */
        int has_session = (mp_session_is_valid() && p < mp_session_count());
        int car = s_mp_player_car[p];
        if (car < s_car_roster_min) car = s_car_roster_min;
        if (car > s_car_roster_max) car = s_car_roster_max;
        if (!frontend_car_selectable(car))
            car = frontend_car_cycle_step(car, 1, s_car_roster_min, s_car_roster_max);
        s_mp_player_car[p]       = car;
        s_mp_player_ready[p]     = 0;
        if (has_session) {
            int k;
            s_mp_player_color_idx[p] = p % TD6_PALETTE_N;      /* fallback cursor */
            for (k = 0; k < TD6_PALETTE_N; k++)
                if ((int)s_td6_palette[k] == s_mp_player_color[p]) { s_mp_player_color_idx[p] = k; break; }
            /* s_mp_player_color / s_mp_player_paint / s_mp_player_trans keep their
             * restored values. */
        } else {
            s_mp_player_color_idx[p] = p % TD6_PALETTE_N;      /* distinct default colour */
            s_mp_player_color[p]     = (int)s_td6_palette[s_mp_player_color_idx[p]];
            s_mp_player_trans[p]     = 0;                      /* Automatic by default */
        }
        s_mp_pane_btn[p]         = MP_BTN_CAR;
        s_mp_pane_substate[p]    = 0;
        s_mp_pane_spec_car[p]    = -1;
        s_mp_pane_preview[p]     = 0;
        s_mp_pane_overlay[p]     = 0;
        s_mp_rep_ms[p]           = 0;   /* [#7] clean CAR/PAINT hold-repeat timer */
        /* Seed the edge tracker with whatever's held now (the START press is
         * probably still down) so it isn't read as an instant action. */
        s_mp_pane_nav_prev[p]    = mp_simul_player_nav(p);
        mp_simul_refresh_pane(p);
    }
    s_mp_simul_ready_ms = 0;
    s_mp_simul_anim_ms  = td5_plat_time_ms();   /* start the lobby->grid slide-in */
    s_anim_complete = 1;
    s_inner_state = 0x20;                        /* animation phase */
    /* poll_input is skipped in grid mode, so freeze the shared gamepad-nav cache
     * (a stale held B could make frontend_check_escape fire a spurious back). */
    s_fe_gamepad_nav = 0;
    /* Grid mode reads keys/pads directly (level + own edge detect) and never
     * drains the menu nav FIFO, so clear it on entry/exit to keep stray queued
     * presses from leaking into this screen or the next one. */
    td5_plat_input_flush_nav();
    frontend_check_escape();                     /* swallow any pending ESC latch */
    frontend_play_sfx(4);
    TD5_LOG_I(LOG_TAG, "CarSelect: simultaneous grid for %d players", n);
}

/* Leave the grid back to the MULTIPLAYER LOBBY (B / ESC). Keeps the scan handles
 * alive (the lobby polls them) and re-arms a fresh join. */
static void mp_simul_back_to_lobby(int n) {
    mp_simul_free_all_panes(n);
    td5_plat_input_flush_nav();
    s_mp_simul = 0;
    TD5_LOG_I(LOG_TAG, "CarSelect grid: back -> multiplayer lobby");
    td5_frontend_set_screen(TD5_SCREEN_MP_LOBBY);
}

static int mp_repeat_fire(int p, uint32_t held, uint32_t edge, uint32_t now); /* fwd: defined with the setup window below */

static void frontend_mp_simul_carsel_update(void) {
    int p, n = s_num_human_players;
    int all_ready = 1, want_back = 0;
    uint32_t now = td5_plat_time_ms();
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    /* Slide-in animation phase: no input; keep edge trackers fresh so a held
     * START button isn't treated as a press once interaction begins. */
    if (s_inner_state == 0x20) {
        for (p = 0; p < n; p++) s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
        frontend_check_escape();                 /* keep the ESC latch clear during the anim */
        if (now - s_mp_simul_anim_ms >= MP_SIMUL_ANIM_MS) s_inner_state = 0x21;
        return;
    }

    for (p = 0; p < n; p++) {
        uint32_t bits = mp_simul_player_nav(p);
        uint32_t edge = bits & ~s_mp_pane_nav_prev[p];
        int car;
        s_mp_pane_nav_prev[p] = bits;

        if (edge & 0x20) want_back = 1;          /* any pad's B -> back to lobby */

        /* Stats spec sheet: A/B closes it. */
        if (s_mp_pane_substate[p] == 1) {
            if (edge & 0x10) { s_mp_pane_substate[p] = 0; frontend_play_sfx(5); }
            continue;
        }
        /* Already locked in: A toggles back to editing. */
        if (s_mp_player_ready[p]) {
            if (edge & 0x10) { s_mp_player_ready[p] = 0; s_mp_simul_ready_ms = 0; frontend_play_sfx(5); }
            continue;
        }

        car = s_mp_player_car[p];
        if (edge & 4) { s_mp_pane_btn[p] = (s_mp_pane_btn[p] + MP_BTN_COUNT - 1) % MP_BTN_COUNT; frontend_play_sfx(2); }
        if (edge & 8) { s_mp_pane_btn[p] = (s_mp_pane_btn[p] + 1) % MP_BTN_COUNT;                 frontend_play_sfx(2); }
        {
            int act = (edge & 0x10) != 0;
            /* [#7 2026-06-15 MP hold-to-change] CAR/PAINT ◄► auto-repeat: first fire
             * on the rising edge, then a steady repeat while the direction is HELD —
             * reusing the same mp_repeat_fire helper (320ms arm / 130ms rate) the
             * colour grid already uses, gated by the SAME TD5RE_CARSEL_HOLD knob as
             * the single-player selector. With the knob OFF this collapses to
             * edge-only (one step per press, the previous behaviour). Direction bits
             * are pre-masked to LEFT|RIGHT so vertical row nav stays strictly
             * edge-driven; on the (rare) both-held frame RIGHT wins. Only CAR/PAINT
             * repeat — STATS/TRANS/OK stay edge-only (act). */
            int on_lr  = (s_mp_pane_btn[p] == MP_BTN_CAR || s_mp_pane_btn[p] == MP_BTN_PAINT);
            int lr_fire = !on_lr ? 0
                        : frontend_carsel_hold_enabled()
                              ? mp_repeat_fire(p, bits & 3u, edge & 3u, now)  /* hold-repeat */
                              : ((edge & 3u) != 0);                            /* edge-only fallback */
            int lr_edge = (edge & 3u) != 0;   /* initial press keeps the cycle sound;
                                               * repeat fires are silent (user request) */
            int left  = lr_fire && !(bits & 2u);
            int right = lr_fire && (bits & 2u) != 0;
            switch (s_mp_pane_btn[p]) {
            case MP_BTN_CAR:
                if (left || right) {
                    car = frontend_car_cycle_step(car, right ? +1 : -1, s_car_roster_min, s_car_roster_max);
                    s_mp_player_car[p]   = car;
                    s_mp_player_paint[p] = 0;   /* reset paint on car change (matches single-player) */
                    mp_simul_refresh_pane(p);
                    if (lr_edge) frontend_play_sfx(5);
                }
                break;
            case MP_BTN_PAINT:
                if (left || right) mp_simul_cycle_paint(p, right ? +1 : -1, lr_edge);
                break;
            case MP_BTN_STATS:
                if (act) { mp_simul_load_pane_spec(p, car); s_mp_pane_substate[p] = 1; frontend_play_sfx(3); }
                break;
            case MP_BTN_TRANS:
                if (act || left || right) { s_mp_player_trans[p] = !s_mp_player_trans[p]; frontend_play_sfx(3); }
                break;
            case MP_BTN_OK:
                if (act) { s_mp_player_ready[p] = 1; frontend_play_sfx(3); }
                break;
            }
            /* [#19] START (0x40) is a one-press "ready up" from any focused
             * button (mirrors the OK action) so a pad player can lock in without
             * navigating to OK first. */
            if (edge & 0x40) { s_mp_player_ready[p] = 1; frontend_play_sfx(3); }
        }
    }

    if (frontend_check_escape()) want_back = 1;  /* keyboard ESC / aggregated gamepad B */
    if (want_back) { mp_simul_back_to_lobby(n); return; }

    for (p = 0; p < n; p++)
        if (!s_mp_player_ready[p]) { all_ready = 0; break; }

    if (all_ready) {
        if (s_mp_simul_ready_ms == 0) s_mp_simul_ready_ms = now;
        if (now - s_mp_simul_ready_ms >= 500u) {
            int q;
            for (q = 0; q < n; q++) {
                /* Bind each player's own device for the race (exclusive). */
                td5_input_set_input_source(q, s_mp_join_device[q]);
                td5_save_set_player_device_index(q, (uint32_t)s_mp_join_device[q]);
            }
            mp_simul_free_all_panes(n);
            td5_plat_input_scan_join_release();
            td5_plat_input_flush_nav();
            s_selected_car          = s_mp_player_car[0];
            s_selected_paint        = s_mp_player_paint[0];
            s_selected_transmission = s_mp_player_trans[0];
            /* Keep s_mp_simul set so backing here from track select re-enters the
             * car grid (phase 1) with picks intact; the Screen_CarSelection
             * intercept clears it when the MP flow ends. */
            TD5_LOG_I(LOG_TAG, "CarSelect grid: all %d ready -> track select", n);
            td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
            return;
        }
    } else {
        s_mp_simul_ready_ms = 0;
    }
}

static void mp_setup_name_append(int p, char c) {
    int l = (int)strlen(s_mp_player_name[p]);
    if (l < (int)sizeof(s_mp_player_name[p]) - 1) {
        s_mp_player_name[p][l] = c;
        s_mp_player_name[p][l + 1] = '\0';
    }
}

static void mp_setup_name_backspace(int p) {
    int l = (int)strlen(s_mp_player_name[p]);
    if (l > 0) s_mp_player_name[p][l - 1] = '\0';
}

/* Directional auto-repeat for the colour grid: fires once on the rising edge,
 * then (after a hold delay) keeps firing at a steady moderate rate while the
 * direction stays held. Works for joystick AND keyboard (both feed level bits).
 * `held`/`edge` are the direction bits (0x0F). Returns 1 on a fire frame. */
static int mp_repeat_fire(int p, uint32_t held, uint32_t edge, uint32_t now) {
    if (!(held & 0x0Fu)) { s_mp_rep_ms[p] = 0; return 0; }
    if (edge & 0x0Fu)    { s_mp_rep_ms[p] = now + 320u; return 1; }   /* first press + arm delay */
    if (s_mp_rep_ms[p] && now >= s_mp_rep_ms[p]) { s_mp_rep_ms[p] = now + 130u; return 1; } /* repeat */
    return 0;
}

/* [#11] MP profile-management constants + per-player panel cursor state. Declared
 * here (before frontend_mp_setup_init) so init can reset them; the helper
 * functions + the panel-input handler are defined just below frontend_mp_setup_init. */
#define MP_SET_PROFILE 3                          /* button id (the header enum only goes to OK=2) */
#define MP_PROF_ACT_COUNT 3                       /* SAVE / LOAD / DELETE actions */
enum { MP_PROF_ACT_SAVE = 0, MP_PROF_ACT_LOAD, MP_PROF_ACT_DELETE };
static int s_mp_prof_focus[TD5_MAX_HUMAN_PLAYERS];   /* 0 = action row, 1 = list */
static int s_mp_prof_act[TD5_MAX_HUMAN_PLAYERS];     /* MP_PROF_ACT_* */
static int s_mp_prof_sel[TD5_MAX_HUMAN_PLAYERS];     /* selected list index */

/* [#3 2026-06-15] PROFILE sits BETWEEN COLOUR and OK (order NAME, COLOUR, PROFILE,
 * OK) — both in the up/down NAV sequence and in the on-screen button stack. The
 * shared header enum is fixed (NAME=0, COLOUR=1, OK=2) and MP_SET_PROFILE=3 keeps
 * the button id stable, so we drive navigation through an explicit ORDER table
 * instead of plain modulo on the id. mp_set_nav_order(profiles_on) returns the
 * visible sequence; mp_set_nav_step() advances s_mp_setup_btn within it. The same
 * order index also fixes the render slot (see frontend_mp_setup_profile_render and
 * the companion td5_frontend.c band change documented in the REPORT). */
static const int k_mp_set_order_prof[4] = { MP_SET_NAME, MP_SET_COLOUR, MP_SET_PROFILE, MP_SET_OK };
static const int k_mp_set_order_noprof[3] = { MP_SET_NAME, MP_SET_COLOUR, MP_SET_OK };
static const int *mp_set_nav_order(int profiles_on, int *count) {
    if (profiles_on) { *count = 4; return k_mp_set_order_prof; }
    *count = 3; return k_mp_set_order_noprof;
}
/* Visible-slot index (0-based, top-to-bottom) of a button id in the current order;
 * -1 if not present. Used by the render to place PROFILE between COLOUR and OK. */
static int mp_set_slot_of(int btn, int profiles_on) {
    int i, cnt; const int *ord = mp_set_nav_order(profiles_on, &cnt);
    for (i = 0; i < cnt; i++) if (ord[i] == btn) return i;
    return -1;
}
/* Step s_mp_setup_btn[p] by +/-1 through the visible order (wraps). */
static void mp_set_nav_step(int p, int dir, int profiles_on) {
    int i, cnt; const int *ord = mp_set_nav_order(profiles_on, &cnt);
    int cur = mp_set_slot_of(s_mp_setup_btn[p], profiles_on);
    if (cur < 0) cur = 0;
    i = (cur + (dir < 0 ? cnt - 1 : 1)) % cnt;
    s_mp_setup_btn[p] = ord[i];
}

/* [#11 2026-06-15] PROFILE in-use is tracked PER PLAYER (which stored profile name
 * each player currently HOLDS) rather than a write-once name set, so freeing /
 * re-loading / leaving a player releases its profile. "In use" = some CURRENT
 * holder claims that name. s_mp_prof_held[p][0]=='\0' means player p holds none.
 * mp_prof_set_held() upserts player p's holder (releasing the previous, which is
 * implicit since each slot stores exactly one name); the per-name query scans the
 * holders. (Replaces the old append-only s_prof_loaded_names[]/count.) */
static char s_mp_prof_held[TD5_MAX_HUMAN_PLAYERS][16];
/* [profile-persist 2026-06-16] Cross-phase snapshot of the per-player profile
 * holders, kept in sync by mp_prof_set_held/mp_prof_release. frontend_mp_setup_init
 * restores from this when RE-entering setup (e.g. BACK from car-select) so a loaded
 * profile is NOT lost and the player needn't reload it; frontend_mp_flow_reset
 * clears it on a fresh race. Knob TD5RE_MP_PROFILE_PERSIST (default on). */
static char s_mp_prof_held_saved[TD5_MAX_HUMAN_PLAYERS][16];
static int mp_profile_persist_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PROFILE_PERSIST");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP profile persist across car-select back %s (TD5RE_MP_PROFILE_PERSIST=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

static void frontend_mp_setup_init(void) {
    int p, n = s_num_human_players;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    frontend_init_return_screen(TD5_SCREEN_CAR_SELECTION);
    frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
    frontend_reset_buttons();
    frontend_set_color_panel(0);
    frontend_set_cursor_visible(0);
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        td5_input_set_input_source(p, 0);   /* drop exclusives so scan handles poll */

    /* [#11] Fresh setup = nobody is HOLDING a profile yet (releases anything a
     * prior setup/race pinned IN USE). Holders are (re)claimed only by an explicit
     * SAVE/LOAD below; restoring a player's name/accent does NOT re-pin a profile.
     * [profile-persist 2026-06-16] EXCEPT on a BACK from car-select: the snapshot
     * still holds each player's loaded profile, so restore it (the player keeps
     * their selection and needn't reload). A fresh flow cleared the snapshot in
     * frontend_mp_flow_reset, so this restores empty there (legacy behavior). */
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) {
        if (mp_profile_persist_on()) {
            strncpy(s_mp_prof_held[p], s_mp_prof_held_saved[p], sizeof(s_mp_prof_held[p]) - 1);
            s_mp_prof_held[p][sizeof(s_mp_prof_held[p]) - 1] = '\0';
        } else {
            s_mp_prof_held[p][0] = '\0';
        }
    }

    for (p = 0; p < n; p++) {
        /* [MP SESSION PERSISTENCE 2026-06] Phase-0 setup: prefer the session-saved
         * name + accent for a returning player so the NAME field and identity
         * colour come up pre-filled (the lobby START restore already mirrors them
         * into the live arrays; this keeps it correct regardless of entry order).
         * Gated by TD5RE_MP_SESSION. */
        if (mp_session_is_valid() && p < mp_session_count())
            mp_session_restore_player(p);
        if (s_mp_player_accent[p] == 0)
            s_mp_player_accent[p] = (int)(k_mp_player_colors[p % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu);
        s_mp_player_ready[p]  = 0;
        s_mp_setup_sub[p]     = 0;
        s_mp_setup_btn[p]     = MP_SET_NAME;
        s_mp_kbd_col[p]       = 0;
        s_mp_kbd_row[p]       = 0;
        s_mp_col_col[p]       = 0;
        s_mp_col_row[p]       = 0;
        s_mp_prof_focus[p]    = 0;   /* [#11] profile-panel cursor reset */
        s_mp_prof_act[p]      = MP_PROF_ACT_SAVE;
        s_mp_prof_sel[p]      = 0;
        s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
    }
    s_mp_simul_ready_ms = 0;
    s_mp_simul_anim_ms  = td5_plat_time_ms();
    s_anim_complete = 1;
    s_inner_state = 0x20;
    s_fe_gamepad_nav = 0;
    td5_plat_input_flush_nav();
    td5_plat_input_flush_chars();
    frontend_check_escape();
    frontend_play_sfx(4);
    TD5_LOG_I(LOG_TAG, "MP setup window for %d players", n);
}

/* ====================================================================== *
 * [#11 2026-06-15] MP PROFILE MANAGEMENT (name/colour step).
 *
 * Reachable as a 4th item (PROFILE) on each player's NAME/COLOUR/OK pane. Opens
 * a panel (s_mp_setup_sub[p] == 3) that lets the player SAVE the just-configured
 * identity (name + accent + car + paint + colour + transmission) as a persistent
 * TD5_Profile, or LOAD an existing one (applies its name + accent + car to this
 * player; EDIT = load, tweak NAME/COLOUR, re-SAVE — upsert-by-name in the store).
 *
 * "Load once per session": a profile already LOADED by some player this session
 * cannot be loaded again by another. Tracked by NAME (the store upserts by name,
 * so names are unique and stable across the index shuffles a DELETE causes).
 * The set is process-lifetime (session-static); cleared only by re-launch. The
 * load list greys out + skips already-loaded entries.
 * (Constants + per-player cursor statics are declared above frontend_mp_setup_init
 * so that init can reset them.)
 * ====================================================================== */

/* [#11] Is `name` currently HELD by any player OTHER than `except_p` (pass -1 to
 * mean "by anyone")? Replaces the append-only loaded-set query: scans the live
 * per-player holders so a release (load-different / clear / leave) immediately
 * frees the name. Both the LOAD gate and the list "(IN USE)" greying use it. */
static int mp_prof_name_in_use_ex(const char *name, int except_p) {
    int q;
    if (!name || !name[0]) return 0;
    for (q = 0; q < TD5_MAX_HUMAN_PLAYERS; q++) {
        if (q == except_p) continue;
        if (_stricmp(s_mp_prof_held[q], name) == 0) return 1;
    }
    return 0;
}
/* Set player p's CURRENT holder to `name` (NULL/empty releases). Implicitly drops
 * whatever p held before, so loading a different profile frees the old one. */
static void mp_prof_set_held(int p, const char *name) {
    if (p < 0 || p >= TD5_MAX_HUMAN_PLAYERS) return;
    if (!name || !name[0]) { s_mp_prof_held[p][0] = '\0'; s_mp_prof_held_saved[p][0] = '\0'; return; }
    strncpy(s_mp_prof_held[p], name, sizeof(s_mp_prof_held[p]) - 1);
    s_mp_prof_held[p][sizeof(s_mp_prof_held[p]) - 1] = '\0';
    /* [profile-persist] mirror into the cross-phase snapshot so a BACK from
     * car-select restores this holder (see frontend_mp_setup_init). */
    strncpy(s_mp_prof_held_saved[p], s_mp_prof_held[p], sizeof(s_mp_prof_held_saved[p]) - 1);
    s_mp_prof_held_saved[p][sizeof(s_mp_prof_held_saved[p]) - 1] = '\0';
}
static void mp_prof_release(int p) {                      /* p stops holding any profile */
    if (p >= 0 && p < TD5_MAX_HUMAN_PLAYERS) { s_mp_prof_held[p][0] = '\0'; s_mp_prof_held_saved[p][0] = '\0'; }
}

/* Build current player identity into a TD5_Profile for SAVE. */
static void mp_prof_fill_from_player(int p, TD5_Profile *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->name, s_mp_player_name[p], sizeof(out->name) - 1);
    out->accent = s_mp_player_accent[p];
    out->car    = s_mp_player_car[p];
    out->paint  = s_mp_player_paint[p];
    out->color  = s_mp_player_color[p];
    out->trans  = s_mp_player_trans[p];
}

/* Apply a loaded profile to a player's live identity (name + accent + car). */
static void mp_prof_apply_to_player(int p, const TD5_Profile *pr) {
    strncpy(s_mp_player_name[p], pr->name, sizeof(s_mp_player_name[p]) - 1);
    s_mp_player_name[p][sizeof(s_mp_player_name[p]) - 1] = '\0';
    s_mp_player_accent[p] = pr->accent;
    s_mp_player_color[p]  = pr->color;
    s_mp_player_paint[p]  = pr->paint;
    s_mp_player_trans[p]  = pr->trans;
    if (pr->car >= 0 && pr->car < TD5_CAR_COUNT) s_mp_player_car[p] = pr->car;
}

/* Clamp the per-player list cursor into [0, count). */
static void mp_prof_clamp_sel(int p) {
    int cnt = td5_save_profile_count();
    if (cnt <= 0) { s_mp_prof_sel[p] = 0; return; }
    if (s_mp_prof_sel[p] < 0)    s_mp_prof_sel[p] = 0;
    if (s_mp_prof_sel[p] >= cnt) s_mp_prof_sel[p] = cnt - 1;
}

/* [#3 2026-06-15] Profile-LIST nav fix. The old handler jumped focus list->actions
 * on ANY up press (BEFORE the list-scroll block), so UP never decremented the
 * selection and index 0 was unreachable/unselectable. Knob TD5RE_MP_PROFILE_LIST_NAV
 * (default on; "0" restores the old "any-up-leaves-the-list" behaviour). */
static int mp_profile_list_nav_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PROFILE_LIST_NAV");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP profile list nav (#3) %s (TD5RE_MP_PROFILE_LIST_NAV=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Handle one player's input while the profile panel (sub==3) is open. Returns
 * nothing; sets s_mp_setup_sub[p]=0 to close. */
static void mp_prof_panel_input(int p, uint32_t bits, uint32_t edge, uint32_t now) {
    int cnt = td5_save_profile_count();
    mp_prof_clamp_sel(p);

    if (mp_profile_list_nav_enabled()) {
        /* [#3] LEFT/RIGHT pick the action (SAVE/LOAD/DELETE) on the action row.
         * Vertical nav:
         *   - on the ACTION row, DOWN enters the LIST at the current selection
         *     (clamped) so index 0 stays reachable (we do NOT advance past it);
         *   - on the LIST, UP DECREMENTS the selection (auto-repeat) and only
         *     hands focus back to the action row when ALREADY at the top (sel<=0);
         *   - on the LIST, DOWN INCREMENTS the selection (auto-repeat).
         * A on the list LOADs the selected profile (handled below). */
        if (s_mp_prof_focus[p] == 0) {
            if (edge & 1) { s_mp_prof_act[p] = (s_mp_prof_act[p] + MP_PROF_ACT_COUNT - 1) % MP_PROF_ACT_COUNT; frontend_play_sfx(2); }
            if (edge & 2) { s_mp_prof_act[p] = (s_mp_prof_act[p] + 1) % MP_PROF_ACT_COUNT;                     frontend_play_sfx(2); }
            if ((edge & 8) && cnt > 0) {                 /* DOWN: actions -> list */
                s_mp_prof_focus[p] = 1;
                mp_prof_clamp_sel(p);                    /* land on current selection (e.g. index 0) */
                s_mp_rep_ms[p] = now + 320u;             /* arm so the entering press doesn't also scroll */
                frontend_play_sfx(2);
            }
        } else {
            /* On the list: UP at the top returns to actions; otherwise UP/DOWN
             * scroll the selection with auto-repeat. */
            if ((edge & 4) && s_mp_prof_sel[p] <= 0) {   /* already top -> back to actions */
                s_mp_prof_focus[p] = 0;
                s_mp_rep_ms[p] = 0;
                frontend_play_sfx(2);
            } else if (mp_repeat_fire(p, bits & 0x0Cu, edge & 0x0Cu, now)) {
                if (bits & 4) s_mp_prof_sel[p]--;
                if (bits & 8) s_mp_prof_sel[p]++;
                mp_prof_clamp_sel(p);
            }
        }
    } else {
    /* LEFT/RIGHT pick the action (SAVE/LOAD/DELETE) when focus is the action row;
     * UP/DOWN move between the action row and the list, and scroll the list. */
    if (edge & 4) {  /* UP */
        if (s_mp_prof_focus[p] == 1) s_mp_prof_focus[p] = 0;       /* list -> actions */
        frontend_play_sfx(2);
    }
    if (edge & 8) {  /* DOWN */
        if (s_mp_prof_focus[p] == 0 && cnt > 0) s_mp_prof_focus[p] = 1; /* actions -> list */
        frontend_play_sfx(2);
    }
    if (s_mp_prof_focus[p] == 0) {
        if (edge & 1) { s_mp_prof_act[p] = (s_mp_prof_act[p] + MP_PROF_ACT_COUNT - 1) % MP_PROF_ACT_COUNT; frontend_play_sfx(2); }
        if (edge & 2) { s_mp_prof_act[p] = (s_mp_prof_act[p] + 1) % MP_PROF_ACT_COUNT;                     frontend_play_sfx(2); }
    } else {
        /* list scroll with auto-repeat */
        if (mp_repeat_fire(p, bits & 0x0Cu, edge & 0x0Cu, now)) {
            if (bits & 4) s_mp_prof_sel[p]--;
            if (bits & 8) s_mp_prof_sel[p]++;
            mp_prof_clamp_sel(p);
        }
    }
    }

    if (edge & 0x10) {  /* A = activate */
        int act = s_mp_prof_act[p];
        if (s_mp_prof_focus[p] == 1) act = MP_PROF_ACT_LOAD;   /* A on the list = LOAD it */
        if (act == MP_PROF_ACT_SAVE) {
            if (s_mp_player_name[p][0]) {
                TD5_Profile pr;
                mp_prof_fill_from_player(p, &pr);
                int slot = td5_save_profile_save(&pr);
                /* [#11] Saving makes it THIS player's held profile for the session
                 * (so a second player can't also load it); releases whatever p held
                 * before. Per-player, so it frees when p loads another / leaves. */
                if (slot >= 0) mp_prof_set_held(p, pr.name);
                frontend_play_sfx(slot >= 0 ? 3 : 10);
                TD5_LOG_I(LOG_TAG, "MP profile: P%d SAVE '%s' -> slot %d (held)", p, pr.name, slot);
            } else {
                frontend_play_sfx(10);   /* need a name first */
            }
        } else if (act == MP_PROF_ACT_LOAD) {
            TD5_Profile pr;
            if (cnt > 0 && td5_save_profile_get(s_mp_prof_sel[p], &pr)) {
                /* [#11] Block only if ANOTHER player currently holds it; p re-loading
                 * its own held profile is fine. Loading releases p's previous hold. */
                if (mp_prof_name_in_use_ex(pr.name, p)) {
                    frontend_play_sfx(10);   /* in use by another player -> blocked */
                    TD5_LOG_I(LOG_TAG, "MP profile: P%d LOAD '%s' BLOCKED (in use by another)", p, pr.name);
                } else {
                    mp_prof_apply_to_player(p, &pr);
                    mp_prof_set_held(p, pr.name);   /* releases p's prior hold implicitly */
                    frontend_play_sfx(3);
                    TD5_LOG_I(LOG_TAG, "MP profile: P%d LOAD '%s' (car=%d, now held)", p, pr.name, pr.car);
                }
            } else {
                frontend_play_sfx(10);
            }
        } else { /* DELETE */
            TD5_Profile pr;
            if (cnt > 0 && td5_save_profile_get(s_mp_prof_sel[p], &pr) &&
                td5_save_profile_delete(s_mp_prof_sel[p])) {
                int q;
                /* [#11] A deleted profile can't be in use by anyone — release every
                 * holder of that name (the store reindexes; holders are by name). */
                for (q = 0; q < TD5_MAX_HUMAN_PLAYERS; q++)
                    if (_stricmp(s_mp_prof_held[q], pr.name) == 0) mp_prof_release(q);
                frontend_play_sfx(3);
                mp_prof_clamp_sel(p);
                TD5_LOG_I(LOG_TAG, "MP profile: P%d DELETE '%s'", p, pr.name);
            } else {
                frontend_play_sfx(10);
            }
        }
    }
    if (edge & 0x20) {  /* B = close panel (back to NAME/COLOUR/PROFILE/OK) */
        s_mp_setup_sub[p] = 0;
        s_mp_rep_ms[p] = 0;
        frontend_play_sfx(5);
    }
}

/* [#4 2026-06-15] Back/cancel out of the MP NAME/COLOUR setup screen drops the
 * whole roster (names, colours, profiles, positions) back to the lobby, so a
 * stray B is costly. Guard it with a CONFIRM prompt: the first Back ARMS a
 * "LEAVE? Y/N" overlay (drawn by frontend_mp_setup_profile_render); A/confirm
 * leaves, B/ESC cancels, and it auto-disarms after a few seconds so nobody gets
 * stuck. Knob TD5RE_MP_BACK_CONFIRM (default on; "0" = old instant back). */
static int mp_back_confirm_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_BACK_CONFIRM");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP setup back-confirm (#4) %s (TD5RE_MP_BACK_CONFIRM=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}
static int      s_mp_setup_confirm_back = 0;   /* 1 = "LEAVE? Y/N" prompt is up */
static uint32_t s_mp_setup_confirm_ms   = 0;   /* arm time (for the auto-disarm timeout) */
#define MP_BACK_CONFIRM_TIMEOUT_MS 5000u

static void frontend_mp_setup_update(void) {
    int p, n = s_num_human_players;
    int all_ready = 1, want_back = 0;
    uint32_t now = td5_plat_time_ms();
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    if (s_inner_state == 0x20) {   /* slide-in animation */
        for (p = 0; p < n; p++) s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
        frontend_check_escape();
        s_mp_setup_confirm_back = 0;   /* never carry a stale prompt into a fresh slide-in */
        if (now - s_mp_simul_anim_ms >= MP_SIMUL_ANIM_MS) s_inner_state = 0x21;
        return;
    }

    /* [#4] "LEAVE? Y/N" prompt is up: modal — A (any player) confirms the exit,
     * B/ESC cancels, and it auto-disarms after the timeout. Swallow this frame's
     * nav so the keypress doesn't also drive the panes underneath. */
    if (s_mp_setup_confirm_back) {
        int confirm = 0, cancel = 0;
        for (p = 0; p < n; p++) {
            uint32_t bits = mp_simul_player_nav(p);
            uint32_t edge = bits & ~s_mp_pane_nav_prev[p];
            s_mp_pane_nav_prev[p] = bits;
            if (edge & 0x10) confirm = 1;   /* A = yes, leave */
            if (edge & 0x20) cancel  = 1;   /* B = no, stay   */
        }
        if (frontend_check_escape()) cancel = 1;
        if (!confirm && now - s_mp_setup_confirm_ms >= MP_BACK_CONFIRM_TIMEOUT_MS) cancel = 1;
        if (confirm) {
            s_mp_setup_confirm_back = 0;
            TD5_LOG_I(LOG_TAG, "MP setup: back CONFIRMED -> lobby");
            mp_simul_back_to_lobby(n);
            return;
        }
        if (cancel) {
            s_mp_setup_confirm_back = 0;
            frontend_play_sfx(5);
            TD5_LOG_I(LOG_TAG, "MP setup: back cancelled");
        }
        return;   /* hold here until resolved */
    }

    for (p = 0; p < n; p++) {
        uint32_t bits = mp_simul_player_nav(p);
        uint32_t edge = bits & ~s_mp_pane_nav_prev[p];
        int isk = (s_mp_join_device[p] == 0);
        s_mp_pane_nav_prev[p] = bits;

        if (s_mp_setup_sub[p] == 1) {            /* NAME entry */
            if (isk) {
                int ch;
                while ((ch = td5_plat_input_get_char()) != 0) {
                    if (ch == '\r' || ch == '\n') { s_mp_setup_sub[p] = 0; frontend_play_sfx(3); break; }
                    else if (ch == '\b') mp_setup_name_backspace(p);
                    else if (ch >= 0x20 && ch < 0x7f) mp_setup_name_append(p, (char)ch);
                }
            } else {
                int row = s_mp_kbd_row[p], col = s_mp_kbd_col[p];
                int rowlen = (row < MP_KBD_LETTER_ROWS) ? (int)strlen(k_mp_kbd_rows[row]) : 3;
                /* [#15b] Hold-to-move: the d-pad cursor auto-repeats while a
                 * direction stays held (first step on the rising edge, then a
                 * steady rate) — same mp_repeat_fire idiom as the colour grid. */
                if (mp_repeat_fire(p, bits & 0x0Fu, edge & 0x0Fu, now)) {
                    if (bits & 1) { if (col > 0) col--; }
                    if (bits & 2) { if (col < rowlen - 1) col++; }
                    if (bits & 4) { if (row > 0) row--; }
                    if (bits & 8) { if (row < MP_KBD_ROWS - 1) row++; }
                    frontend_play_sfx(2);
                }
                rowlen = (row < MP_KBD_LETTER_ROWS) ? (int)strlen(k_mp_kbd_rows[row]) : 3;
                if (col > rowlen - 1) col = rowlen - 1;
                s_mp_kbd_row[p] = row; s_mp_kbd_col[p] = col;
                if (edge & 0x10) {
                    if (row < MP_KBD_LETTER_ROWS) { mp_setup_name_append(p, k_mp_kbd_rows[row][col]); frontend_play_sfx(2); }
                    else if (col == 0) { mp_setup_name_append(p, ' '); frontend_play_sfx(2); }
                    else if (col == 1) { mp_setup_name_backspace(p); frontend_play_sfx(2); }
                    else { s_mp_setup_sub[p] = 0; frontend_play_sfx(3); }   /* DONE */
                }
                /* [#15a] X (0x80) = delete one character (same as BACKSPACE). */
                if (edge & 0x80) { mp_setup_name_backspace(p); frontend_play_sfx(2); }
                if (edge & 0x40) { s_mp_setup_sub[p] = 0; frontend_play_sfx(3); }  /* Start = finish */
                if (edge & 0x20) { s_mp_setup_sub[p] = 0; frontend_play_sfx(5); }
            }
            all_ready = 0;
            continue;
        }

        if (s_mp_setup_sub[p] == 2) {            /* COLOUR picker (16x16 HSV grid) */
            /* Auto-repeat: hold a direction to keep scrolling at a moderate rate. */
            if (mp_repeat_fire(p, bits, edge, now)) {
                if (bits & 1) { if (s_mp_col_col[p] > 0) s_mp_col_col[p]--; }
                if (bits & 2) { if (s_mp_col_col[p] < MP_COL_COLS - 1) s_mp_col_col[p]++; }
                if (bits & 4) { if (s_mp_col_row[p] > 0) s_mp_col_row[p]--; }
                if (bits & 8) { if (s_mp_col_row[p] < MP_COL_ROWS - 1) s_mp_col_row[p]++; }
                s_mp_player_accent[p] = (int)mp_setup_grid_color(s_mp_col_col[p], s_mp_col_row[p]);
                frontend_play_sfx(2);
            }
            if (edge & 0x10) { s_mp_setup_sub[p] = 0; s_mp_rep_ms[p] = 0; frontend_play_sfx(3); }
            if (edge & 0x20) { s_mp_setup_sub[p] = 0; s_mp_rep_ms[p] = 0; frontend_play_sfx(5); }
            all_ready = 0;
            continue;
        }

        if (s_mp_setup_sub[p] == 3) {            /* [#11] PROFILE management panel */
            mp_prof_panel_input(p, bits, edge, now);
            all_ready = 0;
            continue;
        }

        if (s_mp_player_ready[p]) {
            if (edge & 0x10) { s_mp_player_ready[p] = 0; s_mp_simul_ready_ms = 0; frontend_play_sfx(5); }
            if (edge & 0x20) want_back = 1;
            continue;
        }

        /* idle: navigate NAME / COLOUR / [PROFILE] / OK. [#3] PROFILE sits BETWEEN
         * COLOUR and OK — drive the cursor through the explicit ORDER table (not
         * plain modulo on the id) so UP/DOWN visit NAME->COLOUR->PROFILE->OK. With
         * TD5RE_PROFILES off the order is NAME->COLOUR->OK (PROFILE absent). */
        {
            int pon = mp_profiles_enabled();
            if (mp_set_slot_of(s_mp_setup_btn[p], pon) < 0) s_mp_setup_btn[p] = MP_SET_NAME; /* knob toggled / stale id */
            if (edge & 4) { mp_set_nav_step(p, -1, pon); frontend_play_sfx(2); }
            if (edge & 8) { mp_set_nav_step(p, +1, pon); frontend_play_sfx(2); }
        }
        if (edge & 0x10) {
            if (s_mp_setup_btn[p] == MP_SET_NAME)   { s_mp_setup_sub[p] = 1; if (isk) td5_plat_input_flush_chars(); frontend_play_sfx(3); }
            else if (s_mp_setup_btn[p] == MP_SET_COLOUR) { s_mp_setup_sub[p] = 2; frontend_play_sfx(3); }
            else if (s_mp_setup_btn[p] == MP_SET_PROFILE && mp_profiles_enabled()) {
                s_mp_setup_sub[p]   = 3;            /* open profile panel */
                s_mp_prof_focus[p]  = 0;
                s_mp_prof_act[p]    = MP_PROF_ACT_SAVE;
                s_mp_rep_ms[p]      = 0;
                mp_prof_clamp_sel(p);
                frontend_play_sfx(3);
            }
            else { s_mp_player_ready[p] = 1; frontend_play_sfx(3); }   /* OK */
        }
        if (edge & 0x20) want_back = 1;
    }

    if (frontend_check_escape()) {
        /* A keyboard player mid name-entry OR in the profile panel: ESC just
         * leaves that sub-screen. Otherwise ESC backs the whole setup out to the
         * lobby. */
        int handled = 0;
        for (p = 0; p < n; p++)
            if (s_mp_join_device[p] == 0 &&
                (s_mp_setup_sub[p] == 1 || s_mp_setup_sub[p] == 3)) {
                s_mp_setup_sub[p] = 0; s_mp_rep_ms[p] = 0; handled = 1;
            }
        if (!handled) want_back = 1;
    }
    if (want_back) {
        /* [#4] Confirm before dropping everyone's setup. First Back arms the
         * "LEAVE? Y/N" overlay; the modal block above resolves it next frames.
         * Knob off -> the legacy instant back. */
        if (mp_back_confirm_enabled()) {
            s_mp_setup_confirm_back = 1;
            s_mp_setup_confirm_ms   = now;
            td5_plat_input_flush_nav();   /* don't let the same B leak into the prompt */
            frontend_play_sfx(2);
            TD5_LOG_I(LOG_TAG, "MP setup: back requested -> confirm prompt");
            return;
        }
        mp_simul_back_to_lobby(n);
        return;
    }

    for (p = 0; p < n; p++)
        if (!s_mp_player_ready[p]) { all_ready = 0; break; }

    if (all_ready) {
        if (s_mp_simul_ready_ms == 0) s_mp_simul_ready_ms = now;
        if (now - s_mp_simul_ready_ms >= 500u) {
            /* Name + colour done. [#8] Insert the split-screen POSITION picker
             * here (after lobby + colour, before the car grid) when the feature is
             * active and not already settled for this roster; otherwise jump
             * straight to the car grid (phase 1). */
            s_mp_simul_ready_ms = 0;
            frontend_mp_position_enter();
            return;
        }
    } else {
        s_mp_simul_ready_ms = 0;
    }
}

/* [hold-to-scroll 2026-06-12] Which ◄►-cycling row started the current car-
 * preview reload animation (0=CAR, 1=PAINT, 2=CONFIG; -1 = none). When the
 * slide-in completes (case 14) and the direction is STILL held on that row,
 * the next cycle fires immediately so holding the key/button keeps the
 * change animation looping. The original is strictly edge-triggered (edge
 * mask @0x00414BC4 fires once per press; presses during the anim are
 * discarded) — this is a port enhancement, no original constants exist. */
static int s_carsel_hold_btn = -1;

/* Current LEFT/RIGHT hold state for the single-player car-select FSM:
 * keyboard level (DIK arrows) OR'd with the aggregated gamepad nav bits
 * (s_fe_gamepad_nav, refreshed by frontend_poll_input every frame, including
 * during the preview animation states). Returns -1 / +1 / 0 (0 also when
 * both directions are held). */
static int carsel_held_lr(void) {
    int l = td5_plat_input_key_pressed(0xCB) || (s_fe_gamepad_nav & 1u);
    int r = td5_plat_input_key_pressed(0xCD) || (s_fe_gamepad_nav & 2u);
    if (l && !r) return -1;
    if (r && !l) return +1;
    return 0;
}

/* Apply one ◄► value-cycle on car-select row `btn` (0=CAR, 1=PAINT for TD5
 * cars, 2=CONFIG scheme). Shared by the case-7 edge path and the case-14
 * hold-to-scroll continuation. Returns 1 when the change must kick the
 * preview-reload animation (inner state 10), 0 when nothing changed. */
static int carsel_apply_cycle(int btn, int delta) {
    switch (btn) {
    case 0: /* Car: cycle index */
        if (s_selected_game_type == 5) {
            /* Masters: cycle through roster, skip AI slots */
            int attempts = 0;
            do {
                s_selected_car += delta;
                if (s_selected_car < 0) s_selected_car = 14;
                if (s_selected_car > 14) s_selected_car = 0;
                attempts++;
            } while (s_masters_roster_flags[s_selected_car] == 1 && attempts < 15);
        } else {
            /* Default/era/cop ranges. For the full single-race roster
             * this skips the locked-TD5/police gap so TD6 cars (37-75)
             * are reachable; narrower ranges wrap plainly. */
            s_selected_car = frontend_car_cycle_step(s_selected_car, delta,
                                                     s_car_roster_min, s_car_roster_max);
        }
        /* [FIXED 2026-06-01] orig (0x40E8xx) resets paint + wheel/config
         * scheme to 0 on EVERY car change. s_paint_active is NOT cleared on
         * car change: the chosen paint colour carries over to the next car
         * (and survives a race) — it's a single remembered colour for all
         * TD6 cars. */
        s_selected_paint = 0;
        s_selected_config = 0;
        return 1;

    case 1: { /* Paint: TD5 cars cycle paint 0-3 (TD6 colour panel is a PRESS,
               * never a cycle; cop cars 0x1C-0x24 have no paint). */
        int actual_car = (s_selected_game_type == 5) ?
                         s_masters_roster[s_selected_car] : s_selected_car;
        if (frontend_car_paintable(frontend_current_car_index())) return 0;
        if (actual_car >= 0x1C && actual_car <= 0x24) return 0;
        s_selected_paint += delta;
        if (s_selected_paint < 0) s_selected_paint = 3;
        if (s_selected_paint > 3) s_selected_paint = 0;
        return 1;
    }

    case 2: /* Stats row ◄►: wheel/config scheme 0..3 wrap.
             * [CONFIRMED @ 0x40DFC0 case 7 g_frontendButtonIndex==2] */
        s_selected_config += delta;
        if (s_selected_config < 0) s_selected_config = 3;
        if (s_selected_config > 3) s_selected_config = 0;
        return 1;
    }
    return 0;
}

/* [#12 2026-06-15] RE-ASK THE LAYOUT EACH RACE. The old gate (s_mp_session
 * pos_assigned, set on first commit) was "once per session" — a brand-new race
 * setup with the same player count silently reused the prior layout and never
 * offered the position screen again. We relax it WITHOUT touching the session
 * struct (owned by td5_frontend.c): a per-FLOW flag, cleared at fresh MP-flow
 * entry (Screen_CarSelection's !s_mp_simul branch -> frontend_mp_flow_reset),
 * forces the picker the first time positions are entered in each new flow. Within
 * a single setup (back out of the picker to name/colour and re-confirm) the flag
 * stays set, so mp_session_restore_positions can skip it and we don't nag.
 * Knob TD5RE_MP_POS_REASK (default on; "0" = legacy once-per-session-remembered). */
static int mp_pos_reask_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_POS_REASK");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP position re-ask each race (#12) %s (TD5RE_MP_POS_REASK=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}
static int s_mp_pos_shown_this_flow = 0;   /* picker already offered in the current MP flow */

/* Called from Screen_CarSelection when a FRESH MP flow begins (s_mp_simul 0->1),
 * so the next position-enter re-offers the picker for the new race. */
void frontend_mp_flow_reset(void) {
    s_mp_pos_shown_this_flow = 0;
    /* [profile-persist 2026-06-16] A brand-new race starts with no held profiles,
     * so clear the cross-phase snapshot (frontend_mp_setup_init restores from it on
     * a BACK from car-select, but a fresh flow must begin clean). */
    for (int rp = 0; rp < TD5_MAX_HUMAN_PLAYERS; rp++) s_mp_prof_held_saved[rp][0] = '\0';
}

/* [#8] Decide what happens after name/colour setup completes: show the position
 * picker, or skip it. Gate "re-ask each race / re-show on new player": show when
 * positions aren't assigned for the CURRENT human count, OR (#12) this is the
 * first position-enter of a fresh race flow. When skipping we restore the stored
 * cells (count matches) or fall back to identity and go straight to the car grid. */
static void frontend_mp_position_enter(void) {
    int n = s_num_human_players;
    int reask = mp_pos_reask_enabled() && !s_mp_pos_shown_this_flow;   /* [#12] */
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    if (!mp_positions_enabled()) {
        /* Feature off: identity mapping, straight to the car grid. */
        mp_positions_reset_identity();
        s_mp_phase = 1;
        s_inner_state = 0;            /* carsel_init runs next frame */
        TD5_LOG_I(LOG_TAG, "MP setup: positions disabled -> car select (%d players)", n);
        return;
    }
    /* [#12] On a fresh flow, ALWAYS offer the picker (skip the remembered-skip);
     * thereafter within the flow the remembered cells may be reused without nag. */
    if (!reask && mp_session_restore_positions(n)) {
        /* Already chosen this session for this exact count: reuse, skip picker. */
        s_mp_phase = 1;
        s_inner_state = 0;
        TD5_LOG_I(LOG_TAG, "MP setup: positions remembered -> car select (%d players)", n);
        return;
    }
    /* Show the picker. Clear ready latches, seed per-player edge trackers. Start
     * from identity, EXCEPT when re-asking with a remembered layout for this exact
     * count — then pre-seed the last cells so the player tweaks rather than redoes
     * (mp_session_restore_positions populates s_mp_player_cell[] when it returns 1). */
    {
        int p;
        if (!(reask && mp_session_restore_positions(n)))
            mp_positions_reset_identity();
        for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) {
            s_mp_player_ready[p]  = 0;
            s_mp_rep_ms[p]        = 0;
            s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
        }
    }
    s_mp_pos_shown_this_flow = 1;   /* [#12] offered once this flow; don't nag on re-confirm */
    td5_frontend_set_screen(TD5_SCREEN_MP_POSITION);   /* resets s_inner_state, buttons */
    TD5_LOG_I(LOG_TAG, "MP setup: -> position picker (%d players, reask=%d)", n, reask);
}

/* [#6 2026-06-15] Grid CELL (viewport, row-major) that human player p occupies in
 * the chosen split layout. Single source of truth for "which pane is player p's",
 * shared by the MP CAR-SELECT grid render so a player assigned bottom-right in the
 * position screen ALSO sits bottom-right while picking a car (and an empty cell
 * stays empty). Returns s_mp_player_cell[p] when positions are active+committed,
 * else identity p — so AutoRace / the knob-off path are byte-identical to before.
 * Exported: frontend_mp_simul_carsel_render (td5_frontend.c) calls this instead of
 * the identity `p % cols, p / cols` (see the REPORT for the one-line change). The
 * game-side viewport map already consumes the inverse via td5_frontend_mp_view_actor_slot. */
int frontend_mp_player_pane_cell(int p) {
    if (p < 0 || p >= TD5_MAX_HUMAN_PLAYERS) return p;
    if (!mp_positions_enabled()) return p;
    {
        int cell = s_mp_player_cell[p];
        return (cell >= 0 && cell < TD5_MAX_HUMAN_PLAYERS) ? cell : p;
    }
}

/* [#6 2026-06-15 rework] Split-screen POSITION picker ("CHOOSE YOUR SCREEN").
 *
 * Per-player controls:
 *   D-PAD          move this player's cell (auto-repeat). UP/DOWN/LEFT/RIGHT are
 *                  ALL cell movement now (the old "P1 L/R changes the grid mode"
 *                  was removed — see below). Moving onto an UNREADY player's cell
 *                  SWAPS the two; moving onto a READY player's cell is BLOCKED
 *                  (a ready player cannot be displaced by anyone else).
 *   A              ready latch (toggles back off when already ready)
 *   B / ESC        back to the name/colour setup
 *   CHANGE CAMERA  cycle the split-screen LAYOUT/grid mode (was LEFT/RIGHT —
 *                  now its own button, keyboard + joystick, any player)
 *   FRONT VIEW     cycle WHAT fills the empty grid cells (EMPTY / MAP /
 *                  STANDINGS, s_mp_missing_content[]) — only when the current
 *                  layout actually HAS empty cells (missing > 0)
 *
 * All ready -> commit cells to the session and advance to the car grid. */
void Screen_MpPosition(void) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0, ncells;
    int all_ready = 1, want_back = 0;
    uint32_t now = td5_plat_time_ms();
    static uint32_t s_pos_ready_ms = 0;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    ncells = cols * rows;
    if (ncells > TD5_MAX_VIEWPORTS) ncells = TD5_MAX_VIEWPORTS;

    s_anim_complete = 1;   /* no slide-in; allow normal flow (and our own ESC) */

    /* [#6] CHANGE CAMERA cycles the split LAYOUT (replaces the removed LEFT/RIGHT
     * grid-mode change). Any player's CHANGE CAMERA press advances the layout;
     * shrinking the grid re-packs every cell so the permutation stays a valid
     * bijection.
     *
     * [R3 2026-06-19] The prior pad path went through the AGGREGATED scan reader
     * (td5_plat_input_frontend_nav, via td5_input_frontend_change_camera_pressed),
     * which DECODES buttons 0/1/2 (A/B/X) but NOT button 7 (START/0x40) — so the
     * pad's START never produced a 0x40 bit and the layout never cycled. The
     * per-player device reader td5_plat_input_device_nav DOES decode START (0x40),
     * so detect the camera/layout button straight from each player's OWN pad here
     * with a per-player edge latch. Also accept X (0x80) — that IS in the scan set
     * and is free on this screen — so either a START or an X press cycles the grid.
     * Keyboard CHANGE-VIEW still flows via the getter. */
    {
        int lcnt = 1, cam = 0;
        static uint8_t s_pos_cam_held[TD5_MAX_HUMAN_PLAYERS];
        mp_split_layouts(n, &lcnt);
        for (p = 0; p < n; p++) {
            int dev = s_mp_join_device[p];
            uint32_t db = (dev > 0) ? td5_plat_input_device_nav(dev) : 0u;
            int pad_now = (db & (0x40u | 0x80u)) ? 1 : 0;       /* START or X */
            int pad_edge = (pad_now && !s_pos_cam_held[p]) ? 1 : 0;
            s_pos_cam_held[p] = (uint8_t)pad_now;
            if (pad_edge || td5_input_frontend_change_camera_pressed(p)) { cam = 1; }
        }
        if (cam && lcnt > 1) {
            int q, nc, sel = (s_mp_layout_sel + 1) % lcnt;
            s_mp_layout_sel = sel;
            mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            ncells = cols * rows;
            if (ncells > TD5_MAX_VIEWPORTS) ncells = TD5_MAX_VIEWPORTS;
            /* Clamp / de-dup cells: any out-of-range or duplicate cell gets the
             * lowest free cell, so the permutation stays a valid bijection. */
            for (q = 0; q < n; q++) {
                int dup = 0, r2;
                if (s_mp_player_cell[q] < 0 || s_mp_player_cell[q] >= ncells) dup = 1;
                for (r2 = 0; r2 < q && !dup; r2++)
                    if (s_mp_player_cell[r2] == s_mp_player_cell[q]) dup = 1;
                if (dup) {
                    for (nc = 0; nc < ncells; nc++) {
                        int taken = 0, r3;
                        for (r3 = 0; r3 < n; r3++)
                            if (r3 != q && s_mp_player_cell[r3] == nc) { taken = 1; break; }
                        if (!taken) { s_mp_player_cell[q] = nc; break; }
                    }
                }
            }
            frontend_play_sfx(2);
            TD5_LOG_I(LOG_TAG, "MP position: CHANGE CAMERA -> layout %d (%dx%d, %d empty)",
                      s_mp_layout_sel, cols, rows, missing);
        }
    }

    /* [#6] FRONT VIEW cycles the empty-cell content selector, but only when the
     * current layout has empty cells. s_mp_missing_content[] is the SAME array the
     * Multiplayer Options screen edits and the HUD reads (g_td5.split_missing_content).
     * We advance both empty-cell slots together (one button, simple couch UX). */
    if (missing > 0) {
        int fv = 0;
        for (p = 0; p < n; p++)
            if (td5_input_frontend_front_view_pressed(p)) { fv = 1; break; }
        if (fv) {
            int k, lim = (missing < 2) ? 1 : 2;
            for (k = 0; k < lim; k++) {
                int v = s_mp_missing_content[k] + 1;
                v %= MP_MISSING_CONTENT_COUNT;
                s_mp_missing_content[k] = v;
            }
            frontend_play_sfx(2);
            TD5_LOG_I(LOG_TAG, "MP position: FRONT VIEW -> empty-cell content [%d,%d]",
                      s_mp_missing_content[0], s_mp_missing_content[1]);
        }
    }

    for (p = 0; p < n; p++) {
        uint32_t bits = mp_simul_player_nav(p);
        uint32_t edge = bits & ~s_mp_pane_nav_prev[p];
        s_mp_pane_nav_prev[p] = bits;

        if (s_mp_player_ready[p]) {
            if (edge & 0x10) { s_mp_player_ready[p] = 0; s_pos_ready_ms = 0; frontend_play_sfx(5); }
            if (edge & 0x20) want_back = 1;
            continue;
        }
        all_ready = 0;

        /* D-pad move with auto-repeat. Compute the target cell from the current
         * (col,row); if occupied by an UNREADY player, swap; if occupied by a
         * READY player, BLOCK the move (ready players are locked in place and can
         * only be moved by themselves). LEFT/RIGHT now move the cell for EVERY
         * player (the layout cycler moved to the CHANGE CAMERA button). */
        if (mp_repeat_fire(p, bits, edge, now)) {
            int cell = s_mp_player_cell[p];
            int col = cell % cols, row = cell / cols;
            int ncol = col, nrow = row, target;
            if (bits & 1) ncol--;
            if (bits & 2) ncol++;
            if (bits & 4) nrow--;
            if (bits & 8) nrow++;
            if (ncol < 0) ncol = 0;
            if (ncol > cols - 1) ncol = cols - 1;
            if (nrow < 0) nrow = 0;
            if (nrow > rows - 1) nrow = rows - 1;
            target = nrow * cols + ncol;
            if (target >= ncells) target = ncells - 1;
            if (target != cell) {
                int q, blocked = 0, swap_q = -1;
                for (q = 0; q < n; q++) {
                    if (q != p && s_mp_player_cell[q] == target) {
                        if (s_mp_player_ready[q]) blocked = 1;  /* locked occupant */
                        else                      swap_q  = q;  /* free to swap   */
                        break;
                    }
                }
                if (blocked) {
                    frontend_play_sfx(10);   /* rejection cue: can't displace ready */
                } else {
                    if (swap_q >= 0) s_mp_player_cell[swap_q] = cell;  /* swap occupants */
                    s_mp_player_cell[p] = target;
                    frontend_play_sfx(2);
                }
            }
        }

        if (edge & 0x10) { s_mp_player_ready[p] = 1; frontend_play_sfx(3); }  /* A = ready */
        if (edge & 0x20) want_back = 1;                                       /* B = back  */
    }

    if (frontend_check_escape()) want_back = 1;

    if (want_back) {
        /* Back to the name/colour setup (phase 0). Keep s_mp_simul set so
         * CarSelection re-enters phase 0 cleanly; clear ready latches. */
        int q;
        for (q = 0; q < TD5_MAX_HUMAN_PLAYERS; q++) s_mp_player_ready[q] = 0;
        s_pos_ready_ms = 0;
        s_mp_phase = 0;
        s_inner_state = 0;
        td5_plat_input_flush_nav();
        TD5_LOG_I(LOG_TAG, "MP position: back -> name/colour setup");
        td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
        return;
    }

    for (p = 0; p < n; p++)
        if (!s_mp_player_ready[p]) { all_ready = 0; break; }

    if (all_ready) {
        if (s_pos_ready_ms == 0) s_pos_ready_ms = now;
        if (now - s_pos_ready_ms >= 400u) {
            /* Commit the chosen cells to the session, then advance to the car grid. */
            mp_session_commit_positions(n);
            s_pos_ready_ms = 0;
            s_mp_phase = 1;
            s_inner_state = 0;        /* CarSelection runs carsel_init next frame */
            for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) s_mp_player_ready[p] = 0;
            td5_plat_input_flush_nav();
            TD5_LOG_I(LOG_TAG, "MP position: committed -> car select (%d players)", n);
            td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
            return;
        }
    } else {
        s_pos_ready_ms = 0;
    }
}

/* [#6] Smooth, SUBTLE pulse in [lo,hi] driven off wall-clock `now`.
 *
 * The OLD render (frontend_mp_position_render in td5_frontend.c) used
 *   0.55 + 0.45 * ((now/60) % 16) / 15
 * which is a fast 16-step SAWTOOTH that snaps from 1.0 back to 0.55 every ~960ms
 * and swings a full 0.45 in alpha — that hard reset + wide swing is the
 * "pulsating" the user reported. This is a continuous TRIANGLE wave (no snap)
 * over a ~1.6s period with a gentle amplitude, so a player's own cell breathes
 * just enough to spot it without flickering. No <math.h> dependency. */
static float mp_pos_pulse(uint32_t now, float lo, float hi) {
    uint32_t period = 1600u;                 /* full up+down cycle (ms) */
    uint32_t t = now % period;
    float tri = (t < period / 2u)            /* 0..1 up, then 1..0 down */
                ? (float)t / (float)(period / 2u)
                : 1.0f - (float)(t - period / 2u) / (float)(period / 2u);
    return lo + (hi - lo) * tri;
}

/* [#18b] Standard frontend screen header in the Lunatica title face, drawn for
 * the fe_race screens that own their own render (so the global title-strip path
 * in td5_frontend.c — which is suppressed while s_mp_simul is set — doesn't draw
 * it). Mirrors frontend_draw_screen_title (static in td5_frontend.c) using the
 * PUBLIC title-font API + the public td5_vui_quad textured-glyph primitive; the
 * only cosmetic difference is no faux-italic shear (td5_vui_quad is axis-aligned).
 * Left-aligned with the first letter at left_x, cap tops landing near top_y. */
#define FE_RACE_TITLE_CAP_PX 24.0f    /* design cap height (px at 480-tall reference) */
#define FE_RACE_TITLE_LEFT_X 126.0f   /* design x where the first letter starts (= td5_frontend FE_TITLE_LEFT_X) */
#define FE_RACE_TITLE_TRACK  (-1.5f)  /* extra letter tracking (design px; negative = tighter) */
static void fe_race_draw_screen_title(const char *text, float left_x, float top_y,
                                      uint32_t color, float sx, float sy) {
    if (!text || !td5_titlefont_ready()) return;
    const float cap_px   = FE_RACE_TITLE_CAP_PX * sy;
    const float baseline = top_y + cap_px;                /* cap tops land near top_y */
    const float hscale   = (sx < sy) ? (sx / sy) : 1.0f;  /* condense like fe_draw_text on narrow windows */
    const float trkn     = FE_RACE_TITLE_TRACK * sy * hscale;
    float pen = left_x;
    int i;
    for (i = 0; text[i]; i++) {                            /* pass 1: rasterise into atlas */
        td5_glyph g; td5_titlefont_get(toupper((unsigned char)text[i]), cap_px, &g);
        (void)g;
    }
    td5_font_flush_uploads();                              /* one GPU upload for new glyphs */
    for (i = 0; text[i]; i++) {                            /* pass 2: draw (cache hits) */
        int ch = toupper((unsigned char)text[i]);
        td5_glyph g; td5_titlefont_get(ch, cap_px, &g);
        if (g.valid && g.w > 0.0f) {
            float gx = pen + g.xoff * hscale;
            float gy = baseline + g.yoff;                 /* glyph quad top edge */
            td5_vui_quad(gx, gy, g.w * hscale, g.h, color, g.page, g.u0, g.v0, g.u1, g.v1);
        }
        pen += g.advance * hscale + trkn;
    }
}

/* [#6] Replacement renderer for the "CHOOSE YOUR SCREEN" position picker.
 * Drawn entirely with the public VectorUI primitives (td5_vui_quad / td5_vui_text)
 * so it lives in this file; repoint the TD5_SCREEN_MP_POSITION case in
 * td5_frontend.c from frontend_mp_position_render to this. Differences vs the old
 * renderer: (1) stable subtle pulse (mp_pos_pulse); (2) READY cells lock-tinted +
 * a small lock glyph hint; (3) empty cells show the SELECTED content label
 * (EMPTY/MAP/STANDINGS) not a hard-coded "EMPTY"; (4) footer documents the new
 * CHANGE CAMERA = LAYOUT and FRONT VIEW = empty-cell-content bindings. */
void frontend_mp_position_render2(float sx, float sy) {
    int p, c, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    int ncells, all_ready = 1;
    int owner[TD5_MAX_VIEWPORTS];
    float pulse = mp_pos_pulse(now, 0.78f, 1.0f);   /* subtle: 0.78..1.0 */
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    ncells = cols * rows;
    if (ncells > TD5_MAX_VIEWPORTS) ncells = TD5_MAX_VIEWPORTS;

    /* cell -> occupying player (or -1 = empty). */
    for (c = 0; c < TD5_MAX_VIEWPORTS; c++) owner[c] = -1;
    for (p = 0; p < n; p++) {
        int cell = s_mp_player_cell[p];
        if (cell >= 0 && cell < ncells) owner[cell] = p;
        if (!s_mp_player_ready[p]) all_ready = 0;
    }

    /* [R4a 2026-06-19] The full-screen darkening overlay (0xC0 alpha) made this
     * screen read too OPAQUE — removed at user request so the MainMenu art shows
     * through like the other MP setup steps. The per-cell tints + footer band below
     * still provide enough contrast for the grid. */
    /* [#18b] Standard top header (Lunatica face) to match the other menus; fall
     * back to the old plain centred text only if the title font isn't ready. */
    if (td5_titlefont_ready())
        fe_race_draw_screen_title("CHOOSE YOUR SCREEN", FE_RACE_TITLE_LEFT_X * sx, 17.0f * sy,
                                  0xFFE3D708u, sx, sy);
    else
        td5_vui_text_centered(320.0f * sx, 10.0f * sy, "CHOOSE YOUR SCREEN", 0xFFFFE060u, sx, sy);

    /* Layout grid occupies a centred area below the title, above the footer. */
    {
        const float gx = 40.0f, gy = 40.0f, gw = 560.0f, gh = 372.0f;
        /* [#3] Cap cell WIDTH at the 3x3-equivalent (gw/3) and centre the row in
         * the gw band so 1-2 cells don't stretch edge to edge. */
        float cw, ch = gh / (float)rows, row_x0 = 0.0f;
        mp_panel_capped(gw, cols, &cw, &row_x0);
        for (c = 0; c < ncells; c++) {
            int col = c % cols, row = c / cols;
            float px = gx + row_x0 + (float)col * cw, py = gy + (float)row * ch;
            float ccx = px + cw * 0.5f;
            int occ = owner[c];
            uint32_t rgb = (occ >= 0) ? ((uint32_t)s_mp_player_accent[occ] & 0x00FFFFFFu) : 0x303040u;
            int ready = (occ >= 0) && s_mp_player_ready[occ];
            char buf[40];

            /* cell fill (faint tint of the owner's colour; ready = slightly stronger). */
            td5_vui_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, (ch - 6) * sy,
                         (occ >= 0) ? (rgb | (ready ? 0x60000000u : 0x40000000u)) : 0x40181820u,
                         -1, 0, 0, 1, 1);

            /* border: owned cells breathe gently (subtle pulse); READY cells are
             * SOLID + thicker (locked, no pulse); empty cells are a thin steady line. */
            {
                float bt = (occ >= 0) ? (ready ? 3.5f : 3.0f) : 1.5f;
                uint32_t a;
                if (occ < 0)        a = 0x80000000u;
                else if (ready)     a = 0xFF000000u;                 /* locked: full, steady */
                else                a = (uint32_t)(0x60 + (int)(0x9F * pulse)) << 24;
                uint32_t bc = (rgb | a);
                td5_vui_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                td5_vui_quad((px + 3) * sx, (py + ch - 3 - bt) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                td5_vui_quad((px + 3) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
                td5_vui_quad((px + cw - 3 - bt) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
            }

            /* big cell number (1-based) so players can call out "I'm on 3". */
            snprintf(buf, sizeof buf, "%d", c + 1);
            td5_vui_text_centered(ccx * sx, (py + ch * 0.30f) * sy, buf,
                                  (occ >= 0) ? 0xFFFFFFFFu : 0xFF707080u, sx, sy);

            if (occ >= 0) {
                if (s_mp_player_name[occ][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[occ]);
                else                          snprintf(buf, sizeof buf, "PLAYER %d", occ + 1);
                mp_pos_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy, buf,
                                        rgb | 0xFF000000u, sx, sy);
                mp_pos_small_centered(ccx * sx, (py + ch * 0.30f + 40.0f) * sy,
                                        ready ? "READY (LOCKED)" : "MOVE: D-PAD",
                                        ready ? 0xFF40FF40u : 0xFFB0B0B0u, sx, sy);
            } else {
                /* [#6] empty cell shows the SELECTED content (FRONT VIEW cycles it). */
                int cidx = s_mp_missing_content[0];
                if (cidx < 0 || cidx >= MP_MISSING_CONTENT_COUNT) cidx = 0;
                mp_pos_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy,
                                        k_mp_missing_content[cidx], 0xFF8088A0u, sx, sy);
                mp_pos_small_centered(ccx * sx, (py + ch * 0.30f + 40.0f) * sy,
                                        "FRONT VIEW: CHANGE", 0xFF707080u, sx, sy);
            }
        }
    }

    /* Footer: controls + layout/content state + all-ready hint. */
    td5_vui_quad(0.0f, 420.0f * sy, 640.0f * sx, 60.0f * sy, 0xB0080810u, -1, 0, 0, 1, 1);
    {
        int lcnt = 1;
        const MpSplitLayout *opts = mp_split_layouts(n, &lcnt);
        char lbuf[80];
        const char *lname = (opts && s_mp_layout_sel >= 0 && s_mp_layout_sel < lcnt)
                            ? opts[s_mp_layout_sel].label : "SINGLE";
        /* [R4b 2026-06-19] ONE clean WHITE instructions line — the change-camera
         * (layout) hint is folded into it and the separate YELLOWISH "LAYOUT [..]"
         * line that overlapped it is removed. START or X on the pad (kbd CHANGE
         * VIEW) cycles the split LAYOUT; the current layout name is shown inline. */
        if (lcnt > 1)
            snprintf(lbuf, sizeof lbuf,
                     "D-PAD: MOVE   A: READY   B: BACK   START/X: LAYOUT [%s]", lname);
        else
            snprintf(lbuf, sizeof lbuf,
                     "D-PAD: MOVE   A: READY   B: BACK   LAYOUT: %s", lname);
        td5_vui_text_centered(320.0f * sx, 422.0f * sy, lbuf, 0xFFFFFFFFu, sx, sy);
        if (missing > 0) {
            int cidx = s_mp_missing_content[0];
            if (cidx < 0 || cidx >= MP_MISSING_CONTENT_COUNT) cidx = 0;
            snprintf(lbuf, sizeof lbuf, "FRONT VIEW: EMPTY-CELL CONTENT [%s]",
                     k_mp_missing_content[cidx]);
            mp_pos_small_centered(320.0f * sx, 454.0f * sy, lbuf, 0xFF90C0FFu, sx, sy);
        } else if (all_ready) {
            mp_pos_small_centered(320.0f * sx, 454.0f * sy, "ALL READY - STARTING CARS...",
                                    0xFF80FF80u, sx, sy);
        }
        if (missing > 0 && all_ready)
            mp_pos_small_centered(320.0f * sx, 466.0f * sy, "ALL READY - STARTING CARS...",
                                    0xFF80FF80u, sx, sy);
    }
}

/* [#11] Profile-management overlay for the MP name/colour step. Drawn ON TOP of
 * frontend_mp_setup_render (which owns the NAME/COLOUR/OK pane); td5_frontend.c
 * must call this right after it (already wired). Two parts per pane:
 *   (a) idle: the "PROFILE" button drawn BETWEEN COLOUR and OK ([#3]; slot 2 of a
 *       4-slot band, highlighted when the player's nav cursor is on MP_SET_PROFILE);
 *   (b) sub==3: a centred panel over the pane with the SAVE/LOAD/DELETE action
 *       row + the scrollable profile list (in-use entries greyed).
 * PLUS [#4]: the "LEAVE? Y/N" confirm overlay, drawn for ALL knob states (it sits
 * before the TD5RE_PROFILES early-return). Inert per-part when its knob is off. */

/* [#2 2026-06-15] The idle PROFILE chip used to draw a dark steel interior fill +
 * faint half-alpha rim when unfocused, so it READ AS DISABLED next to its enabled
 * NAME/COLOUR/OK siblings (which draw a transparent interior + full-opacity accent
 * rim, accent fill when focused). When on, the unfocused PROFILE chip drops the
 * interior fill and uses a full-opacity accent rim, matching the siblings. Knob
 * TD5RE_MP_PROFILE_BTN_STYLE (default on; "0" restores the old steel/faint look). */
static int mp_profile_btn_style_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PROFILE_BTN_STYLE");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP profile button style (#2) %s (TD5RE_MP_PROFILE_BTN_STYLE=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* [#7 2026-06-16] Vertically centre the "LEAVE SETUP?" confirm-modal text within
 * its box rect. The title + the two sub-lines were anchored with hard-coded top
 * offsets (+12/+36/+50) that left the block sitting high in the 70px box; when ON
 * the block is centred. Knob TD5RE_LEAVE_MODAL_CENTER (default on; "0" restores
 * the old fixed offsets). */
static int leave_modal_center_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_LEAVE_MODAL_CENTER");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "LEAVE-SETUP modal text centring (#7) %s (TD5RE_LEAVE_MODAL_CENTER=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* [#6 2026-06-16] Draw ONE name/colour-pane chip pixel-identically to its
 * NAME/COLOUR/OK siblings. The siblings render through td5_frontend.c's static
 * mp_simul_draw_btn() -> static fe_draw_button_frame_fill(), neither of which is
 * linkable from this translation unit. fe_draw_button_frame_fill's live path
 * (VectorUI, g_td5.ini.vector_ui defaults to 1) is a single fe_draw_roundrect
 * call whose PUBLIC mirror is td5_vui_roundrect (identical args) — so this
 * replicates mp_simul_draw_btn EXACTLY: the same frame constants, the same
 * focused accent-fill / unfocused border-only treatment, the same readable-label
 * luminance pick, and the same SMALLFONT-cap-centred label. (No arrows / value /
 * swatch — PROFILE is a plain button like OK.) Geometry is supplied by the caller
 * so it lands in the shared 4-slot band. */
static void mp_profile_chip_draw(float x, float y, float w, float h,
                                 const char *label, int focused, uint32_t pcol,
                                 float sx, float sy) {
    /* SMALLFONT_TTF_CAP (9.0f) and fe_glyph_sx (min(sx,sy)) are file-static in
     * td5_frontend.c; mirror the exact same values here. */
    const float smallcap = 9.0f;
    const float gsx = (sx < sy) ? sx : sy;
    uint32_t rgb = pcol & 0x00FFFFFFu;
    uint32_t tc  = 0xFFFFFFFFu;
    float ty = (y + (h - smallcap) * 0.5f) * sy;   /* vertically centred (= mp_simul_draw_btn) */

    /* fe_draw_button_frame_fill(.., bb_state = focused?0:1, interior = rgb|FF, ..)
     * VectorUI path -> fe_draw_roundrect with these EXACT constants (td5_frontend.c
     * lines 8350-8358). bb_state 0 (focused/selected) fills with the accent; the
     * unfocused state draws the blue rim with a transparent interior. */
    if (g_td5.ini.vector_ui && td5_vui_shapes_available()) {
        uint32_t mid_c, inner_c, outer_c;
        float fillA;
        if (focused) { mid_c = 0xFFD9CA00u; inner_c = 0xFFA08C00u; outer_c = 0xFF3C2F00u; fillA = 1.0f; }
        else         { mid_c = 0xFF7995FFu; inner_c = 0xFF496BDCu; outer_c = 0xFF001675u; fillA = 0.0f; }
        td5_vui_roundrect(x * sx, y * sy, w * sx, h * sy,
                          20.0f * sy /*r_large TL/BR*/, 5.0f * sy /*r_small TR/BL*/,
                          6.0f * sy  /*border side*/,  2.0f * sy /*border top/bot*/,
                          mid_c, inner_c, outer_c, rgb | 0xFF000000u, fillA);
    } else {
        /* VectorUI off: fe_draw_button_frame_fill's 9-slice path (static, not
         * linkable). Fall back to a flat accent frame so the chip still reads as
         * a sibling button; gated identically to the proc path above. */
        uint32_t bc = rgb | 0xFF000000u;
        float t = 2.0f;
        if (focused)
            td5_vui_quad(x * sx, y * sy, w * sx, h * sy, rgb | 0xC0000000u, -1, 0, 0, 1, 1);
        td5_vui_quad(x * sx, y * sy, w * sx, t * sy, bc, -1, 0, 0, 1, 1);
        td5_vui_quad(x * sx, (y + h - t) * sy, w * sx, t * sy, bc, -1, 0, 0, 1, 1);
        td5_vui_quad(x * sx, y * sy, t * sx, h * sy, bc, -1, 0, 0, 1, 1);
        td5_vui_quad((x + w - t) * sx, y * sy, t * sx, h * sy, bc, -1, 0, 0, 1, 1);
    }

    /* Focused label colour = readable over the accent fill (mp_simul_draw_btn's
     * luminance test); unfocused stays white. */
    if (focused) {
        int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        int lum = (r * 30 + g * 59 + b * 11) / 100;
        tc = (lum > 150) ? 0xFF101010u : 0xFFFFFFFFu;
    }
    {
        float lw = fe_measure_small_text(label) * gsx;
        fe_draw_small_text((x + w * 0.5f) * sx - lw * 0.5f, ty, label, tc, sx, sy);
    }
}

void frontend_mp_setup_profile_render(float sx, float sy) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;

    /* [#4] LEAVE? Y/N — independent of TD5RE_PROFILES; only needs the name/colour
     * step active and the prompt armed by frontend_mp_setup_update. */
    if (s_mp_simul && s_mp_phase == 0 && s_inner_state == 0x21 && s_mp_setup_confirm_back) {
        float bw = 240.0f, bh = 70.0f;
        float bx = 320.0f - bw * 0.5f, by = 200.0f;
        td5_vui_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xA0000000u, -1, 0, 0, 1, 1); /* dim all */
        td5_vui_quad(bx * sx, by * sy, bw * sx, bh * sy, 0xF0101820u, -1, 0, 0, 1, 1);
        {
            float t = 2.0f; uint32_t bc = 0xFFFFCC33u;
            td5_vui_quad(bx * sx, by * sy, bw * sx, t * sy, bc, -1, 0, 0, 1, 1);
            td5_vui_quad(bx * sx, (by + bh - t) * sy, bw * sx, t * sy, bc, -1, 0, 0, 1, 1);
            td5_vui_quad(bx * sx, by * sy, t * sx, bh * sy, bc, -1, 0, 0, 1, 1);
            td5_vui_quad((bx + bw - t) * sx, by * sy, t * sx, bh * sy, bc, -1, 0, 0, 1, 1);
        }
        /* [#7] Vertically centre the title + the two sub-lines within the box.
         * Glyph CAP heights: the main (title) font and the small font; the gaps
         * between the three baselines preserve the original visual rhythm
         * (title, then sub, then options). The whole stack is offset so it sits
         * centred in the bh-tall box (was hard-anchored near the top at +12). */
        if (leave_modal_center_on()) {
            const float title_cap = 14.0f;       /* main-font cap (title) */
            const float small_cap = 9.0f;        /* small-font cap (sub-lines) */
            const float gap_title = 10.0f;       /* title cap bottom -> sub top */
            const float gap_sub   = 5.0f;        /* sub cap bottom  -> options top */
            float block_h = title_cap + gap_title + small_cap + gap_sub + small_cap;
            float ty0 = by + (bh - block_h) * 0.5f;            /* title top */
            float ty1 = ty0 + title_cap + gap_title;           /* sub top   */
            float ty2 = ty1 + small_cap + gap_sub;             /* options top */
            td5_vui_text_centered(320.0f * sx, ty0 * sy, "LEAVE SETUP?", 0xFFFFE060u, sx, sy);
            mp_pos_small_centered(320.0f * sx, ty1 * sy,
                                  "LOSES EVERYONE'S NAMES + COLOURS", 0xFFB0B0B0u, sx, sy);
            mp_pos_small_centered(320.0f * sx, ty2 * sy,
                                  "A = YES, LEAVE     B = NO, STAY", 0xFFFFFFFFu, sx, sy);
        } else {
            td5_vui_text_centered(320.0f * sx, (by + 12.0f) * sy, "LEAVE SETUP?", 0xFFFFE060u, sx, sy);
            mp_pos_small_centered(320.0f * sx, (by + 36.0f) * sy,
                                  "LOSES EVERYONE'S NAMES + COLOURS", 0xFFB0B0B0u, sx, sy);
            mp_pos_small_centered(320.0f * sx, (by + 50.0f) * sy,
                                  "A = YES, LEAVE     B = NO, STAY", 0xFFFFFFFFu, sx, sy);
        }
        return;   /* the prompt is modal; don't draw the chips/panels underneath */
    }

    if (!mp_profiles_enabled()) return;
    if (!s_mp_simul || s_mp_phase != 0) return;          /* only the name/colour step */
    if (s_inner_state != 0x21) return;                   /* skip during slide-in */
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    (void)missing;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    {
        /* [#3] Cap pane WIDTH at the 3x3-equivalent (640/3) and centre the row so
         * 1-2 panes don't render edge to edge. MUST match the (capped) setup panes
         * in td5_frontend.c so this overlay's PROFILE chip + panel sit on top of
         * them — see mp_panel_capped's SCOPE NOTE and the REPORT.
         * [R1 2026-06-19] Mirror frontend_mp_setup_render's top title band so the
         * PROFILE chip/panel track the panes that were pushed below the title.
         * [R3-2 2026-06-19] Mirror the SHORTER pane band (top 50 + bottom 44) so the
         * PROFILE chip stays glued to the (now shorter, mid-screen) setup panes —
         * these two constants MUST equal frontend_mp_setup_render's. */
        const float mp_title_band  = 50.0f;
        const float mp_bottom_band = 44.0f;
        float pane_w, row_x0 = 0.0f;
        float pane_h = (480.0f - mp_title_band - mp_bottom_band) / (float)rows;
        mp_panel_capped(640.0f, cols, &pane_w, &row_x0);
        for (p = 0; p < n; p++) {
            int col = p % cols, row = p / cols;
            float px = row_x0 + (float)col * pane_w, py = mp_title_band + (float)row * pane_h;
            float cx = px + pane_w * 0.5f;
            uint32_t rgb = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;
            int sub = s_mp_setup_sub[p];

            /* (a) [#3] idle PROFILE button — placed in the SAME 4-slot band the
             * companion td5_frontend.c band draws (NAME=0, COLOUR=1, PROFILE=2,
             * OK=3), so PROFILE sits visually between COLOUR and OK. Geometry MUST
             * match frontend_mp_setup_render's idle band (see the REPORT for the
             * required 3->4 slot change there): with the slide-in done (rise=0)
             *   ay=py+22, bsy=ay+4, room=(py+pane_h-12)-bsy, bh=room/4-3 (clamp
             *   12..26); slot i top = bsy + i*(bh+3). We draw slot 2. */
            if (!s_mp_player_ready[p] && sub == 0) {
                float ay  = py + 22.0f;
                float bsy = ay + 4.0f;
                float room = (py + pane_h - 12.0f) - bsy;
                float bh = room / 4.0f - 3.0f;       /* 4 slots: NAME/COLOUR/PROFILE/OK */
                float bx = px + 8.0f, bw = pane_w - 16.0f;
                float yy;
                int focus = (s_mp_setup_btn[p] == MP_SET_PROFILE);
                if (bh < 12.0f) bh = 12.0f;
                if (bh > 26.0f) bh = 26.0f;
                yy = bsy + 2.0f * (bh + 3.0f);       /* slot index 2 (between COLOUR and OK) */
                if (mp_profile_btn_style_enabled()) {
                    /* [#6] Route through the SAME drawing the NAME/COLOUR/OK
                     * siblings use (mp_simul_draw_btn -> fe_draw_button_frame_fill),
                     * replicated exactly in mp_profile_chip_draw (the siblings'
                     * helper is file-static in td5_frontend.c and not linkable
                     * here). This includes the label centring, so the explicit
                     * mp_pos_small_centered "PROFILE" below is no longer needed. */
                    mp_profile_chip_draw(bx, yy, bw, bh, "PROFILE", focus,
                                         rgb | 0xFF000000u, sx, sy);
                } else {
                    /* [#6 disabled] legacy steel/amber inline look. */
                    td5_vui_quad(bx * sx, yy * sy, bw * sx, bh * sy,
                                 focus ? (rgb | 0xC0000000u) : 0x70202838u, -1, 0, 0, 1, 1);
                    {
                        float t = 2.0f;
                        uint32_t bc = focus ? 0xFFFFCC33u : 0xA05A6680u;
                        td5_vui_quad(bx * sx, yy * sy, bw * sx, t * sy, bc, -1, 0, 0, 1, 1);
                        td5_vui_quad(bx * sx, (yy + bh - t) * sy, bw * sx, t * sy, bc, -1, 0, 0, 1, 1);
                        td5_vui_quad(bx * sx, yy * sy, t * sx, bh * sy, bc, -1, 0, 0, 1, 1);
                        td5_vui_quad((bx + bw - t) * sx, yy * sy, t * sx, bh * sy, bc, -1, 0, 0, 1, 1);
                    }
                    mp_pos_small_centered(cx * sx, (yy + (bh - 9.0f) * 0.5f) * sy,
                                          "PROFILE", 0xFFFFFFFFu, sx, sy);
                }
            }

            /* (b) open profile panel. */
            if (sub == 3) {
                float panx = px + 6.0f, pany = py + 22.0f;
                float panw = pane_w - 12.0f, panh = pane_h - 28.0f;
                int cnt = td5_save_profile_count();
                int i, max_rows, list_top;
                char buf[48];
                /* dim the pane + panel frame */
                td5_vui_quad(panx * sx, pany * sy, panw * sx, panh * sy, 0xE00C0C16u, -1, 0, 0, 1, 1);
                td5_vui_quad(panx * sx, pany * sy, panw * sx, 2.0f * sy, rgb | 0xFF000000u, -1, 0, 0, 1, 1);

                mp_pos_small_centered(cx * sx, (pany + 3.0f) * sy, "PROFILE", 0xFFFFE060u, sx, sy);

                /* action row: SAVE / LOAD / DELETE */
                {
                    static const char *acts[MP_PROF_ACT_COUNT] = { "SAVE", "LOAD", "DELETE" };
                    float ar_y = pany + 16.0f;
                    float seg = panw / (float)MP_PROF_ACT_COUNT;
                    int a;
                    for (a = 0; a < MP_PROF_ACT_COUNT; a++) {
                        float axp = panx + seg * (float)a;
                        int on = (s_mp_prof_focus[p] == 0 && s_mp_prof_act[p] == a);
                        td5_vui_quad((axp + 1) * sx, ar_y * sy, (seg - 2) * sx, 13.0f * sy,
                                     on ? 0xD0FFCC33u : 0x60303848u, -1, 0, 0, 1, 1);
                        mp_pos_small_centered((axp + seg * 0.5f) * sx, (ar_y + 2.0f) * sy,
                                              acts[a], on ? 0xFF101010u : 0xFFD0D0D0u, sx, sy);
                    }
                }

                /* profile list (greyed if already loaded this session). */
                list_top = (int)(pany + 33.0f);
                max_rows = (int)((panh - 33.0f - 10.0f) / 11.0f);
                if (max_rows < 1) max_rows = 1;
                if (cnt == 0) {
                    mp_pos_small_centered(cx * sx, (float)list_top * sy + 6.0f * sy,
                                          "(NO SAVED PROFILES)", 0xFF808890u, sx, sy);
                } else {
                    int start = 0;
                    if (s_mp_prof_sel[p] >= max_rows) start = s_mp_prof_sel[p] - max_rows + 1;
                    for (i = 0; i < max_rows && (start + i) < cnt; i++) {
                        TD5_Profile pr;
                        int idx = start + i;
                        float ry = (float)list_top + (float)i * 11.0f;
                        int sel = (s_mp_prof_focus[p] == 1 && s_mp_prof_sel[p] == idx);
                        int loaded;
                        if (!td5_save_profile_get(idx, &pr)) continue;
                        /* [#11] "in use" = held by ANOTHER player (mirrors the LOAD
                         * gate): this player's OWN held profile stays selectable, so
                         * a release elsewhere ungreys it immediately. */
                        loaded = mp_prof_name_in_use_ex(pr.name, p);
                        if (sel)
                            td5_vui_quad((panx + 2) * sx, ry * sy, (panw - 4) * sx, 10.0f * sy,
                                         0x90303848u, -1, 0, 0, 1, 1);
                        /* name + a small swatch of the profile's accent. */
                        snprintf(buf, sizeof buf, "%s%s", pr.name, loaded ? " (IN USE)" : "");
                        fe_draw_small_text((panx + 14) * sx, ry * sy, buf,
                                           loaded ? 0xFF606870u : (sel ? 0xFFFFFFFFu : 0xFFC8C8C8u),
                                           sx, sy);
                        td5_vui_quad((panx + 3) * sx, (ry + 1.0f) * sy, 8.0f * sx, 8.0f * sy,
                                     ((uint32_t)pr.accent & 0x00FFFFFFu) | 0xFF000000u, -1, 0, 0, 1, 1);
                    }
                }

                mp_pos_small_centered(cx * sx, (pany + panh - 9.0f) * sy,
                                      "A: DO   UP/DN: PICK   B: BACK", 0xFFB0B0B0u, sx, sy);
            }
        }
    }
}

/* [#2/#7 2026-06-15] Shared gate for car-select LEFT/RIGHT hold-to-cycle, used by
 * BOTH the single-player selector (frontend_carsel_hold_repeat) and the
 * simultaneous-MP grid (Task #7). Cached once, logged once.
 * Knob: TD5RE_CARSEL_HOLD (default on; "0" restores single-step-per-press). */
static int frontend_carsel_hold_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("TD5RE_CARSEL_HOLD");
        enabled = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "car-select hold-to-cycle (#2/#7) %s (TD5RE_CARSEL_HOLD=%s)",
                  enabled ? "ENABLED" : "disabled", e ? e : "default");
    }
    return enabled;
}

/* [#2 2026-06-15 hold-to-cycle] Single-player car-select LEFT/RIGHT auto-repeat.
 *
 * Root cause of the regression: keyboard menu nav is drained from the platform
 * WM_KEYDOWN FIFO, which DELIBERATELY filters OS key auto-repeat
 * (td5_platform_win32.c WM_KEYDOWN: `if (!(lParam & (1L<<30)))`), and the
 * gamepad path is pure rising-edge (`!s_prev_*_state`). So `s_arrow_input` —
 * which frontend_option_delta() reads — fires exactly ONCE per physical press,
 * and holding a direction no longer kept cycling cars. The simultaneous-MP grid
 * (Task #7) shares the same fix via mp_repeat_fire gated on the SAME knob.
 *
 * Fix, contained to this screen: read the LIVE held direction (keyboard arrows +
 * any gamepad's nav bits) and, after a short arm delay, re-synthesize the
 * s_arrow_input LEFT/RIGHT bit at a steady rate. The first press is still
 * delivered by the FIFO as before; this only adds the repeats. Returns the arrow
 * bit to OR into s_arrow_input on a fire frame (1=LEFT, 2=RIGHT), else 0.
 *
 * Knob: TD5RE_CARSEL_HOLD (default on; "0" restores single-step-per-press). */
static int frontend_carsel_hold_repeat(void) {
    static uint32_t next_ms = 0;     /* next allowed repeat fire (0 = disarmed) */
    static uint32_t prev_held = 0;   /* held bits last frame (for edge detect)  */

    if (!frontend_carsel_hold_enabled()) return 0;

    /* Live HELD direction: keyboard arrows (scancodes) + aggregate gamepad nav.
     * s_fe_gamepad_nav is the held pad snapshot frontend_poll_input already took
     * this frame (bit 0 = LEFT, bit 1 = RIGHT), so we reuse it instead of polling
     * DirectInput a second time. bit 1 = LEFT, bit 2 = RIGHT in s_arrow_input. */
    uint32_t held = 0;
    if (td5_plat_input_key_pressed(0xCB) || (s_fe_gamepad_nav & 0x01)) held |= 1; /* Left  */
    if (td5_plat_input_key_pressed(0xCD) || (s_fe_gamepad_nav & 0x02)) held |= 2; /* Right */
    /* [R2 2026-06-19] If BOTH show at once (a noisy/biased analog X axis on one
     * pad can leave the opposite bit asserted), keep the freshly-pressed direction
     * instead of zeroing the pair — the old `held == 3 -> 0` cancelled a legitimate
     * LEFT whenever a lingering RIGHT bit was present, which read as "LEFT does
     * nothing". prev_held isolates the newly-risen bit. */
    if (held == 3) { uint32_t fresh = held & ~prev_held; held = fresh ? fresh : 0; }

    uint32_t now = td5_plat_time_ms();
    uint32_t edge = held & ~prev_held;
    prev_held = held;

    if (!held) { next_ms = 0; return 0; }
    /* Rising edge: the FIFO already delivered this press as the first step, so
     * here we only ARM the repeat timer (do NOT fire, to avoid a double step). */
    if (edge) { next_ms = now + 360u; return 0; }
    /* Held past the arm delay: fire at a steady moderate rate. */
    if (next_ms && now >= next_ms) { next_ms = now + 110u; return (int)held; }
    return 0;
}

/* [#21] Track-selection LEFT/RIGHT hold-to-scroll. Same pattern + knob as the
 * car-select repeater above (own statics so the two screens don't share the
 * edge/arm latch). frontend_option_delta() already delivers the FIRST press from
 * the WM_KEYDOWN FIFO / gamepad rising edge; this ADDS the repeats while a
 * direction stays held, so holding LEFT/RIGHT on the track selector auto-cycles.
 * Returns the s_arrow_input direction bit to OR in on a fire frame (1=LEFT,
 * 2=RIGHT), else 0. Knob: TD5RE_CARSEL_HOLD (shared with car-select). */
static int frontend_trksel_hold_repeat(void) {
    static uint32_t next_ms = 0;     /* next allowed repeat fire (0 = disarmed) */
    static uint32_t prev_held = 0;   /* held bits last frame (for edge detect)  */

    if (!frontend_carsel_hold_enabled()) return 0;

    uint32_t held = 0;
    if (td5_plat_input_key_pressed(0xCB) || (s_fe_gamepad_nav & 0x01)) held |= 1; /* Left  */
    if (td5_plat_input_key_pressed(0xCD) || (s_fe_gamepad_nav & 0x02)) held |= 2; /* Right */
    /* [R2 2026-06-19] Prefer the freshly-pressed direction when both bits show at
     * once (see frontend_carsel_hold_repeat) instead of zeroing the pair, so a
     * legitimate LEFT isn't cancelled by a lingering/biased RIGHT bit. */
    if (held == 3) { uint32_t fresh = held & ~prev_held; held = fresh ? fresh : 0; }

    uint32_t now = td5_plat_time_ms();
    uint32_t edge = held & ~prev_held;
    prev_held = held;

    if (!held) { next_ms = 0; return 0; }
    if (edge) { next_ms = now + 360u; return 0; }   /* first step came via the FIFO; just arm */
    if (next_ms && now >= next_ms) { next_ms = now + 110u; return (int)held; }
    return 0;
}

void Screen_CarSelection(void) {
    /* Multiplayer simultaneous flow (2+ humans) takes over this screen with its
     * own per-pane panes + per-device input: phase 0 = name/colour setup window,
     * phase 1 = the car-select grid. Single-player / classic-2P / network fall
     * through to the original state machine below. */
    if (s_mp_flow && s_num_human_players >= 2) {
        if (!s_mp_simul) { s_mp_simul = 1; s_mp_phase = 0; s_inner_state = 0;
                           frontend_mp_flow_reset(); }   /* [#12] re-offer the position screen for this new race */
        /* 0x20 = slide-in animation, 0x21 = interactive; anything else (fresh
         * inner-state 0 on entry / phase change) (re)initialises the phase. */
        if (s_mp_phase == 0) {
            if (s_inner_state == 0x20 || s_inner_state == 0x21) frontend_mp_setup_update();
            else                                                frontend_mp_setup_init();
        } else {
            if (s_inner_state == 0x20 || s_inner_state == 0x21) frontend_mp_simul_carsel_update();
            else                                                frontend_mp_simul_carsel_init();
        }
        return;
    }
    s_mp_simul = 0;
    switch (s_inner_state) {
    case 0: /* Init: determine car roster, load UI assets */
        frontend_init_return_screen(TD5_SCREEN_CAR_SELECTION);
        TD5_LOG_D(LOG_TAG, "CarSelection: state 0 - init");
        s_anim_complete = 0;
        s_carsel_rand_btn = -1;   /* [#14] (re)assigned when buttons are built in case 4 */

        /* Reload MainMenu.tga background so that returning from TrackSelection
         * (which loads TrackSelect.tga) restores the correct background.
         * Original relies on preserved primary surface from RaceTypeCategory
         * (0x4168B0 loads MainMenu.tga at 0x00416940), but our clear-per-frame
         * renderer needs the background surface explicitly set.
         * [CONFIRMED @ Ghidra 0x00416940: RaceTypeCategory loads MainMenu.tga] */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Load overlay UI assets.
         * All three overlays use opaque blit (flag 0x10) in the original binary
         * [CONFIRMED @ Ghidra 0x0040E1CD–0x0040E20F: all loaded via FUN_00412030].
         * CarSelCurve and CarSelTopBar have black areas that are intentionally
         * opaque — they cover the background, not show through it. */
        s_carsel_bar_surface    = frontend_load_tga("Front_End/CarSelBar1.tga",   "Front_End/FrontEnd.zip");
        s_carsel_curve_surface  = frontend_load_tga("Front_End/CarSelCurve.tga",  "Front_End/FrontEnd.zip");
        s_carsel_topbar_surface = frontend_load_tga("Front_End/CarSelTopBar.tga", "Front_End/FrontEnd.zip");
        s_graphbars_surface     = frontend_load_tga("Front_End/GraphBars.tga",    "Front_End/FrontEnd.zip");

        /* Create 1x1 solid blue fill surface for the car preview background.
         * Original uses FillPrimaryFrontendRect(0x5c, ...) which fills the
         * DDraw primary surface directly. Our renderer clears each frame, so
         * we create a tiny fill texture drawn via fe_draw_surface_opaque —
         * the same proven path used by the overlay TGAs above. */
        {
            int slot = -1;
            for (int i = 0; i < FE_MAX_SURFACES; i++) {
                if (!s_surfaces[i].in_use) { slot = i; break; }
            }
            if (slot >= 0) {
                int page = FE_SURFACE_PAGE_BASE + slot;
                /* BGRA pixel: B=0x5C(92), G=0, R=0, A=0xFF */
                uint32_t blue_pixel = 0xFF00005C;
                if (td5_plat_render_upload_texture(page, &blue_pixel, 1, 1, 2)) {
                    s_surfaces[slot].in_use = 1;
                    s_surfaces[slot].tex_page = page;
                    s_surfaces[slot].width = 1;
                    s_surfaces[slot].height = 1;
                    strncpy(s_surfaces[slot].source_name, "_fill_blue", sizeof(s_surfaces[slot].source_name) - 1);
                    s_carsel_fill_surface = slot + 1;
                    TD5_LOG_I(LOG_TAG, "CarSel: blue fill surface created: slot=%d page=%d", slot, page);
                }
            }
        }

        /* Show the mouse cursor on car-select (it's a mouse-interactive screen,
         * incl. the TD6 color map). Normal flow inherits a visible cursor from the
         * prior screen, but a direct StartScreen jump (test harness) does not, so
         * set it explicitly here. */
        frontend_set_cursor_visible(1);

        /* Determine car roster range by game type */
        s_car_roster_min = 0;
        switch (s_selected_game_type) {
        case 2: /* Era: 0..15 */
            s_car_roster_max = 15;
            break;
        case 5: /* Masters: use random roster */
            s_car_roster_max = 14; /* index into s_masters_roster[] */
            break;
        case 8: /* Cop Chase: TD5 police 33-36 + TD6 cops cp1-4 (46-49).
                 * frontend_car_cycle_step skips the non-cop gap (37-45). */
            s_car_roster_min = 33;
            s_car_roster_max = TD6_COP_LAST;   /* 49 */
            break;
        default:
            /* [CONFIRMED @ 0x0040E8F8 CarSelectionScreenStateMachine case 7]
             * Original wrap-around in non-COPS mode is gated on
             * DAT_004962ac/DAT_00463e6d: when DAT_004962ac == 0 (the common
             * case — no runtime writer in the decomp) the upper bound is
             * 0x20 (32), excluding police indices 0x21..0x24. Police are
             * only reachable when DAT_004962ac is set (Cop Chase / network
             * special) or when game_type == 8 (handled above).
             *
             * Port: cap the default roster at 32 so police indices stay out
             * of normal cycling regardless of unlock state. The cheat
             * "unlock all" path still extends to 36 only when paired with
             * the network-special context (matches the original's "both
             * flags must be set" gate at 0x0042140B). */
            /* TD6 cars (37-75) are always selectable, so the cycle range runs to
             * the full roster. frontend_car_cycle_step skips the locked-TD5 /
             * police gap (cap+1 .. 36), so the visible original-car set is
             * unchanged while the TD6 cars become reachable. */
            s_car_roster_max = TD5_CAR_COUNT - 1;
            break;
        }

        /* Handle 2P mode:
         * (two_player_mode & 3) == 1: P1 selecting
         * (two_player_mode & 3) == 2: P2 selecting */
        if ((s_two_player_mode & 3) == 2) {
            /* Create P2 label */
        }

        /* [PORT ENHANCEMENT 2026-06] Multiplayer flow: start each player at their
         * own prior pick (so Back/forward keeps each player's car). */
        if (s_mp_flow && s_mp_car_player >= 0 && s_mp_car_player < TD5_MAX_HUMAN_PLAYERS)
            s_selected_car = s_mp_player_car[s_mp_car_player];

        /* Clamp initial car to valid range; if it landed in the locked/police
         * gap (e.g. a stale default_car), step to the next selectable car. */
        if (s_selected_car < s_car_roster_min) s_selected_car = s_car_roster_min;
        if (s_selected_car > s_car_roster_max) s_selected_car = s_car_roster_max;
        if (s_car_roster_max >= TD5_BASE_CAR_COUNT && !frontend_car_selectable(s_selected_car))
            s_selected_car = frontend_car_cycle_step(s_selected_car, 1,
                                                     s_car_roster_min, s_car_roster_max);

        /* Create label surface */
        s_car_preview_overlay = 0;
        s_carsel_hold_btn = -1;   /* no hold-to-scroll chain pending on entry */
        frontend_load_selected_car_preview();
        s_inner_state = 1;
        break;

    case 1: /* Reset tick counter */
        frontend_begin_timed_animation();
        s_inner_state = 2;
        break;

    case 2: /* Sidebar slide-in: bar slides from right, curve+topbar from left */
        /* Skip if returning from network car select, 2P round 2, or drag-race 2nd pass */
        if ((s_two_player_mode & 4) != 0 || s_network_active ||
            s_drag_carselect_pass != 0) {
            s_inner_state = 3;
        } else if (frontend_update_timed_animation(75, FE_CARSEL_SLIDE_IN_MS) >= 1.0f) {
            s_inner_state = 3;
        }
        break;

    case 3: /* Present + copy primary to secondary */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Button creation: 6 buttons along the left column.
             * Original layout places the button column on the left side and the
             * 408x280 car preview on the right side. */
        /* Button column. Original Ghidra layout was 169/209/249/289/329 (state 4
         * @0x40DFC0); the port pushes STATS/AUTO/OK/BACK down to open a ~52px slot
         * at y=241..293 for the at-a-glance stat-bar panel between PAINT and MORE
         * STATS. These create-time Y values MUST match closed_y in
         * frontend_apply_color_panel_layout (which re-applies them every frame and
         * shifts for the TD6 colour picker) so the column is correct on frame 0,
         * before any input arrives. */
        /* Drag race FORCES Manual transmission and renders the toggle
         * non-interactive [CONFIRMED @ 0x0040e119 cmp gameType!=7(orig drag) /
         * 0x0040e167 write g_carSelectManualTransmissionToggle = (gameType==7)].
         * Port game_type 9 == drag. Force the value here so the button shows
         * "Manual"; case 3 below refuses to toggle it back. */
        if (g_td5.drag_race_enabled)
            s_selected_transmission = 1;
        frontend_create_button(SNK_CarButTxt,   46, 169, 168, 32);
        frontend_create_button(SNK_PaintButTxt, 46, 205, 168, 32);
        frontend_create_button(SNK_ConfigButTxt, 46, 297, 168, 32);
        frontend_create_button(s_selected_transmission ? "Manual" : "Automatic",
                               46, 333, 168, 32);
        frontend_create_button(SNK_OkButTxt,   46, 369,  64, 32);
        if (!s_network_active)
            frontend_create_button(SNK_BackButTxt, 118, 369, 96, 32);
        /* [#14] RANDOMIZE: a dedicated control for the Car selector. Created LAST
         * so the Car/Paint/Config/Auto/OK/Back indices used by the action switch
         * (cases 0..5) are unchanged. Activating it rolls a random selectable car.
         * (frontend_apply_color_panel_layout only repositions buttons 0..5, so this
         * one keeps its fixed position.) Skipped in network mode so the Back button
         * (created only when !network) keeps index 5 and RANDOMIZE can't collide
         * with the case-5 (Back) switch arm; net car-select picks are a deliberate
         * per-client choice anyway, not randomized.
         * [item #7 2026-06-15] NEW default form = a small ICON just to the RIGHT of
         * the Car selector (x=218, on its OWN sub-row y=171 — 2px below the Car row
         * y=169). The hit-rect is created HIDDEN so the generic button loop skips
         * its frame+label; the icon is painted in frontend_render_carsel_randomize_icon
         * (chip only when focused, #12).
         * [#12 2026-06-15 nav-selectable] KEY POINT: the icon's .y (171) is kept
         * DISTINCT from the Car selector's .y (169) on purpose. The car screen
         * navigates by INDEX+ROW (frontend_cycle_selected_button_by_row): a DISTINCT
         * row means (a) the Car row's LEFT/RIGHT keeps cycling the car VALUE (no
         * same-row neighbour to hijack the key), and (b) this icon — the highest
         * button index, on a different row — is reached by pressing UP from the Car
         * row (the vertical cycler wraps past the top to the highest different-row
         * button). It is `active`, only `hidden` (skips the 9-slice loop) and never
         * `disabled` except while the colour picker is open, so neither
         * frontend_cycle_selected_button_by_row NOR frontend_spatial_pick (both
         * ignore `hidden`) exclude it. Sharing the Car row's .y would have BROKEN
         * value cycling, so do NOT "align" it. The cross-file note (report) covers
         * enabling true geometric nav for an even more natural RIGHT-to-icon feel.
         * TD5RE_RANDOM_ICON=0 restores the legacy full-width button. */
        if (frontend_random_button_on() && !s_network_active) {
            if (frontend_random_icon_on()) {
                s_carsel_rand_btn = frontend_create_button(NULL, 218, 171,
                                                           FE_RAND_ICON_W, FE_RAND_ICON_H);
                if (s_carsel_rand_btn >= 0) s_buttons[s_carsel_rand_btn].hidden = 1;
            } else {
                s_carsel_rand_btn = frontend_create_button("Randomize", 46, 133, 168, 32);
            }
        }
        frontend_apply_color_panel_layout();   /* fix positions immediately (picker may be open) */

        /* Time Trials: grey out Manual button */
        /* Load inline string table SNK_CarSelect_MT1 */
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 5;
        break;

    case 5: /* Button slide-in: 24 frames (original 0x18) */
        if (frontend_update_timed_animation(24, 400) >= 1.0f) {
            s_inner_state = 6;
        }
        break;

    case 6: /* Tick until ready */
        if (frontend_advance_tick()) {
            s_anim_complete = 1;
            frontend_play_sfx(4); /* screen-entry slide-in settle chime — the
                                   * original CarSelection plays Play(4) on its
                                   * slide-in settle [CONFIRMED @ 0x40DFC0]. The
                                   * port only chimed on the per-CAR preview
                                   * slide-in (case 14), so ENTERING the car-select
                                   * screen had no chime. */
            s_inner_state = 7;
        }
        break;

    case 7: /* Main interaction loop: car preview + input */
        {
            /* DEV harness: TD5RE_DEMO_CARSTATS=1 jumps straight to the STATS spec sheet so it
             * can be frame-dumped (frontend nav uses DirectInput polling, which SendKeys can't
             * drive). Inert unless the env var is set. */
            static int s_demo_cs_init = 0, s_demo_cs = 0;
            if (!s_demo_cs_init) { s_demo_cs_init = 1;
                const char *e = getenv("TD5RE_DEMO_CARSTATS"); s_demo_cs = (e && e[0] && e[0] != '0'); }
            if (s_demo_cs) {
                frontend_load_car_spec_fields(frontend_current_car_index());
                s_car_preview_overlay = 1;
                s_inner_state = 15;
                return;
            }
        }
        /* [#2 hold-to-cycle] Re-synthesize LEFT/RIGHT auto-repeat while a
         * direction is HELD on the Car selector (button 0). The platform nav FIFO
         * drops OS key auto-repeat and the gamepad path is rising-edge only, so
         * without this a held arrow cycled the car exactly once. Only fires while
         * the Car row is focused and no modal (colour picker) is open; the result
         * is fed through the SAME s_arrow_input -> frontend_option_delta -> case 0
         * path as a real press, so the cycle/skip/preview-slide logic is unchanged.
         * Track-row hold-to-cycle (TrackSelection) is intentionally untouched — #2
         * is scoped to car selection. */
        if (!s_color_panel_visible && s_button_index < 0 && s_selected_button == 0) {
            int rep = frontend_carsel_hold_repeat();
            if (rep) { s_arrow_input |= rep; s_input_ready = 1; }
        }

        /* [#14] Keep RANDOMIZE out of the way while the TD6 colour picker is open
         * (the picker fills the column band and is modal). Disabled so it can't be
         * navigated to or clicked; restored when the picker closes.
         * [item #7] In ICON form the hit-rect is permanently hidden (the icon is
         * drawn from the render path, not the button loop) — only toggle disabled
         * there; in the legacy full-button form keep the original hidden toggle so
         * that path stays byte-identical. */
        if (s_carsel_rand_btn >= 0 && s_carsel_rand_btn < FE_MAX_BUTTONS &&
            s_buttons[s_carsel_rand_btn].active) {
            if (!frontend_random_icon_on())
                s_buttons[s_carsel_rand_btn].hidden = s_color_panel_visible;
            s_buttons[s_carsel_rand_btn].disabled = s_color_panel_visible;
            if (s_color_panel_visible && s_selected_button == s_carsel_rand_btn)
                s_selected_button = 0;
        }

        /* Render car preview overlay */
        /* Car-select also enters on a bare mouse CLICK while the colour panel is
         * open, so swatch/map clicks reach the panel — a click doesn't set
         * s_input_ready (only keyboard/button input does). */
        if (s_input_ready || (s_color_panel_visible && s_mouse_clicked)) {
            int delta = frontend_option_delta();
            int active_button = s_button_index;
            /* [FIXED 2026-06-01] include button 2 (Stats) — orig slot2 is also a ◄►
             * cycler (wheel/config scheme), so keyboard arrows over it must resolve. */
            if (active_button < 0 && delta != 0 &&
                (s_selected_button == 0 || s_selected_button == 1 ||
                 s_selected_button == 2 || s_selected_button == 3)) {
                active_button = s_selected_button;
            }
            /* Keep Stats/Auto/OK/Back positioned for the current panel state every
             * frame (idempotent; survives button recreation on car change). */
            frontend_apply_color_panel_layout();

            /* TD6 color panel is MODAL while open: all 4 arrows move a 2D cursor
             * over the picker (rows 0-1 = predefined swatches, rows 2+ = the color
             * map) and the color under it is applied live; the mouse sets the
             * cursor by clicking a swatch / map cell. A button PRESS exits the
             * picker: PAINT just closes; any other button closes AND acts. There's
             * no OK/close inside the panel — re-press PAINT (or click it) to hide. */
            if (s_color_panel_visible) {
                int moved = 0, confirm = 0;
                if (s_arrow_input & 1) { s_color_cur_col = (s_color_cur_col + TD6_CP_COLS - 1) % TD6_CP_COLS; moved = 1; }
                if (s_arrow_input & 2) { s_color_cur_col = (s_color_cur_col + 1) % TD6_CP_COLS; moved = 1; }
                if (s_arrow_input & 4) {         /* UP: move up, or exit the picker from the top row */
                    if (s_color_cur_row > 0) { s_color_cur_row--; moved = 1; }
                    else confirm = 1;
                }
                if (s_arrow_input & 8) { if (s_color_cur_row < TD6_CP_GRID_ROWS - 1) { s_color_cur_row++; moved = 1; } }
                if (frontend_color_panel_mouse()) moved = 1;
                if (moved) {
                    /* Live preview only: no animation while navigating — the
                     * body-only overlay shows the colour instantly (modulate). */
                    g_td5.ini.td6_paint_color = (int)td6_cursor_color(s_color_cur_col, s_color_cur_row);
                    frontend_play_sfx(2);
                }
                if (s_button_index == 1) confirm = 1;   /* PAINT re-pressed -> confirm */
                if (confirm) {                   /* commit the colour: it stays on the
                                                  * preview (no animation on hide — the
                                                  * colour already changed live). */
                    s_paint_active = 1;
                    frontend_set_color_panel(0);
                    td5_ini_persist_options();
                    frontend_play_sfx(3);
                    active_button = -1;
                } else if (s_button_index >= 0) { /* other button -> keep colour, close + act */
                    s_paint_active = 1;
                    frontend_set_color_panel(0);
                    td5_ini_persist_options();
                    active_button = s_button_index;
                } else {
                    active_button = -1;          /* arrows are modal: no button action */
                }
            }
            if (active_button >= 0) switch (active_button) {
            case 0: /* Car: L/R arrows cycle car index (the color panel stays open
                     * across TD6->TD6 changes; it auto-hides after the switch if
                     * the new car is a TD5 car — see the post-switch check). */
                if (delta != 0 && carsel_apply_cycle(0, delta)) {
                    s_carsel_hold_btn = 0;  /* arm hold-to-scroll continuation */
                    s_inner_state = 10;     /* trigger new car image load */
                }
                break;

            case 1: /* Paint (TD5: ◄► cycle paint 0-3) / Color (TD6: open panel) */
                if (frontend_car_paintable(frontend_current_car_index())) {
                    /* Pressing PAINT opens the modal color panel. (Closing is
                     * handled by the modal block above when it's already open, so
                     * this branch only runs while the panel is closed.) */
                    if (s_button_index == 1) {
                        frontend_set_color_panel(1);
                        frontend_play_sfx(3);
                    }
                } else if (delta != 0 && carsel_apply_cycle(1, delta)) {
                    s_carsel_hold_btn = 1;
                    s_inner_state = 10; /* re-render */
                }
                break;

            case 2: /* Stats: ◄► cycles wheel/config scheme 0..3; press opens spec sheet.
                     * [CONFIRMED @ 0x40DFC0 case 7 g_frontendButtonIndex==2: arrow cycles
                     * g_carSelectWheelSchemeTransient 0..3 wrap; press enters state 0xf.] */
                if (delta != 0) {
                    if (carsel_apply_cycle(2, delta)) {
                        s_carsel_hold_btn = 2;
                        s_inner_state = 10; /* re-render preview with new scheme */
                    }
                } else if (s_button_index == 2) {
                    s_car_preview_overlay = 1;
                    s_inner_state = 15;
                }
                break;

            case 3: /* Auto/Manual toggle */
                /* Drag race locks the transmission to Manual [CONFIRMED @
                 * 0x0040e167 — orig makes the button a non-interactive Preview
                 * for game_type 7 (=drag there); port drag == game_type 9].
                 * The pre-existing `!= 7` guard keeps the port's Time-Trial
                 * behavior unchanged; the added drag guard is the faithful fix. */
                if (!g_td5.drag_race_enabled &&
                    s_selected_game_type != 7 && (s_button_index >= 0 || delta != 0)) {
                    s_selected_transmission = !s_selected_transmission;
                    strncpy(s_buttons[3].label,
                            s_selected_transmission ? "Manual" : "Automatic",
                            sizeof(s_buttons[3].label) - 1);
                    s_buttons[3].label[sizeof(s_buttons[3].label) - 1] = '\0';
                }
                break;

            case 4: /* OK: accept car */
            {
                int actual_car = (s_selected_game_type == 5) ?
                                 s_masters_roster[s_selected_car] : s_selected_car;
                /* Lock enforcement */
                int locked = (actual_car >= 0 && actual_car < 37 &&
                             s_car_lock_table[actual_car] != 0);
                if (locked && !s_network_active &&
                    s_selected_game_type != 8 && s_selected_game_type != 5) {
                    frontend_play_sfx(10); /* rejection */
                } else {
                    /* Accept selection → forward to track selection.
                     * [S31] Net-lobby flow (host AND client): CHANGE CAR
                     * returns to the LOBBY — the track is the host's call
                     * via SELECT TRACK, and a client must never reach the
                     * track picker (it can launch a race). */
                    if (s_selected_game_type == 5) {
                        s_masters_roster_flags[s_selected_car] = 2; /* taken */
                    }
                    s_return_screen = s_network_active
                                        ? TD5_SCREEN_NETWORK_LOBBY
                                        : TD5_SCREEN_TRACK_SELECTION;
                    s_inner_state = 0x14; /* slide-out prep */
                }
            }
                break;

            case 5: /* Back */
                s_drag_carselect_pass = 0;
                s_return_screen = s_network_active
                                    ? TD5_SCREEN_NETWORK_LOBBY   /* [S31] */
                                    : TD5_SCREEN_RACE_TYPE_MENU;
                s_inner_state = 0x14;
                break;
            }

            /* [#14] RANDOMIZE (index beyond the fixed 0..5 switch): roll a random
             * selectable car, then run the SAME change flow as a manual cycle
             * (reset paint/config, slide-in via state 10). Pressed on confirm. */
            if (active_button == s_carsel_rand_btn && s_carsel_rand_btn >= 0) {
                int old = s_selected_car;
                s_selected_car = frontend_pick_random_car(s_selected_car,
                                                          s_car_roster_min, s_car_roster_max);
                s_selected_paint  = 0;
                s_selected_config = 0;
                if (s_color_panel_visible) frontend_set_color_panel(0);
                frontend_play_sfx(3);
                TD5_LOG_I(LOG_TAG, "CarSel RANDOMIZE: %d -> %d (roster %d..%d)",
                          old, s_selected_car, s_car_roster_min, s_car_roster_max);
                s_inner_state = 10;  /* trigger new-car image load/slide */
            }

            /* Auto-hide the color panel + restore PAINT arrows when the selection
             * is no longer a TD6 car (e.g. cycled back to a TD5 car while open). */
            if (s_color_panel_visible && !frontend_car_paintable(frontend_current_car_index()))
                frontend_set_color_panel(0);
        }

        /* Network: process messages, check disconnect */
        if (s_network_active) {
            if (s_kicked_flag) {
                s_network_active = 0;
                frontend_net_destroy();
                s_return_screen = -1;
                s_inner_state = 0x14;
            }
        }
        break;

    case 8: /* Blit cached rect, wait 2 frames then return to 7 */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 2) {
            s_inner_state = 7;
        }
        break;

    case 10: /* Clear car preview area, prep for new image load */
        s_car_spec_car = -1; /* invalidate spec cache on car/paint change */
        s_car_preview_change_loaded = 0; /* reset state-11 load-once guard */
        s_anim_complete = 1;
        frontend_play_sfx(5); /* car-change animation START whoosh — fires once when
                               * a car/paint/scheme change begins (case 10 is reached
                               * ONLY from the cycle cases, never the initial load).
                               * The original plays Play(5) on the change
                               * [CONFIRMED @ 0x40DFC0]; it pairs with the Play(4)
                               * settle chime at case 14 when the new preview finishes
                               * sliding in. The port had only the finish chime. */
        s_inner_state = 11;
        break;

    case 11: /* Old car slides out to the right (~433ms, 13 frames @30fps) — 0x40DFC0 state 11 */
    {
        int actual_car = frontend_current_car_index();
        if (!s_car_preview_change_loaded) {
            /* Load the new preview exactly once on entering the slide. A car with
             * no carpic (e.g. a ported TD6 car) returns <=0; guarding on the
             * surface value would re-load + reset the timer every frame and hang
             * the transition, so use a dedicated load-once flag instead. */
            s_car_preview_change_loaded = 1;
            s_car_preview_prev_surface = s_car_preview_surface;
            s_car_preview_next_surface = frontend_load_car_preview_surface(actual_car, s_selected_paint);
            frontend_begin_timed_animation();
        }
        if (frontend_update_timed_animation(13, 433) >= 1.0f) {
            if (s_car_preview_prev_surface > 0 && s_car_preview_prev_surface != s_car_preview_next_surface) {
                frontend_release_surface(s_car_preview_prev_surface);
            }
            s_car_preview_surface = s_car_preview_next_surface;
            s_car_preview_prev_surface = 0;
            s_car_preview_next_surface = 0;
            /* If the newly-displayed car is NOT paintable (a TD5 car), invalidate
             * the TD6 paint-overlay cache. A TD5 car never loads an overlay, so
             * s_paint_overlay_car would otherwise keep pointing at a PREVIOUSLY
             * viewed TD6 car — and when this TD5 car later slides OUT to a TD6 car,
             * the state-11 paint draw (whose show_paint gate keys on the INCOMING
             * TD6 car) would stamp that stale TD6 chassis paint onto the outgoing
             * TD5 body. Resetting the cache here means the slide-out paint draws
             * only when the overlay genuinely belongs to a paintable outgoing car.
             * NO RE BASIS — the TD6 colour-overlay preview is a port-only feature. */
            if (!frontend_car_paintable(actual_car))
                s_paint_overlay_car = -1;
            s_inner_state = 12;
        }
    }
        break;

    case 12: /* Reset timer so state 14 slide-in starts from t=0 */
        frontend_begin_timed_animation();
        s_inner_state = 14;
        break;

    case 14: /* Car preview slide-in from right, 25 frames (~833ms @30fps) — 0x40DFC0 state 14 */
        if (frontend_update_timed_animation(0x19, 833) >= 1.0f) {
            /* Original 0x40DFC0 case 0xE @ 0x0040EE3F plays Play(4) once when
             * frame_counter == 0x19 (slide-in complete) — fires every car cycle. */
            frontend_play_sfx(4);
            s_inner_state = 7; /* return to interaction */
            /* [hold-to-scroll 2026-06-12] If the direction that started this
             * cycle is STILL held and the highlight is still on the same row,
             * chain straight into the next change so holding the key/button
             * keeps the car-change animation looping (port enhancement; the
             * original discards presses during the anim — states 8-0x13 never
             * read nav @0x40DFC0). Releasing, swapping rows (UP/DOWN queue
             * during the anim) or the modal colour panel breaks the chain. */
            {
                int dir = carsel_held_lr();
                if (dir != 0 && s_carsel_hold_btn >= 0 &&
                    s_selected_button == s_carsel_hold_btn &&
                    !s_color_panel_visible &&
                    carsel_apply_cycle(s_carsel_hold_btn, dir)) {
                    s_inner_state = 10; /* loop the change animation */
                } else {
                    s_carsel_hold_btn = -1;
                }
            }
        }
        break;

    case 15: /* Stats sub-screen (0x40DFC0 state 0xF): SNK_Config_Hdrs + config.nfo values */
        frontend_load_car_spec_fields(frontend_current_car_index());
        if (s_input_ready && s_button_index >= 0) {
            s_car_preview_overlay = 2;
            s_inner_state = 7;
        }
        break;

    case 17: /* Info sub-screen [0x40DFC0 state 0x11]: draws 10 strings from
              * SNK_Info_Values_exref (Language.dll export, 10 × char* pointers,
              * centered via MeasureOrCenterFrontendString [CONFIRMED @ 0x0040F184]).
              * Language.dll is unavailable in the port; fall through to return. */
        s_inner_state = 18;
        break;

    case 18: /* Return from info */
        s_car_preview_overlay = 2;
        s_inner_state = 7;
        break;

    case 0x14: /* Prep slide-out */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 0x15;
        break;

    case 0x15: /* Cross-fade (~133ms) */
        if (frontend_update_timed_animation(8, 133) >= 1.0f) {
            s_inner_state = 0x16;
        }
        break;

    case 0x16: /* Release car surface */
    case 0x17:
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 0x18;
        break;

    case 0x18: /* Button slide-out (~400ms) */
        if (frontend_update_timed_animation(0x18, 400) >= 1.0f) {
            frontend_begin_timed_animation();
            s_inner_state = 0x19;
        }
        break;

    case 0x19: /* Screen wipe: vertical bar sweep (~267ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 0x1A;
        }
        break;

    case 0x1A: /* Exit dispatch: release all, determine next screen */
    {
        int actual_car = (s_selected_game_type == 5) ?
                         s_masters_roster[s_selected_car] : s_selected_car;

        /* [PORT ENHANCEMENT 2026-06] Multiplayer lobby flow: sequential per-player
         * car select. Each player picks in turn (join order); after the last,
         * proceed to the track selector. OK advances; Back steps to the previous
         * player (or back to the lobby from player 0). */
        if (s_mp_flow) {
            if (s_return_screen == TD5_SCREEN_TRACK_SELECTION) {   /* OK */
                if (s_mp_car_player >= 0 && s_mp_car_player < TD5_MAX_HUMAN_PLAYERS) {
                    s_mp_player_car[s_mp_car_player]   = actual_car;
                    s_mp_player_paint[s_mp_car_player] = s_selected_paint;
                }
                TD5_LOG_I(LOG_TAG, "CarSelect MP: player %d car=%d", s_mp_car_player + 1, actual_car);
                s_mp_car_player++;
                if (s_mp_car_player < s_num_human_players) {
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);   /* next player */
                    return;
                }
                s_selected_car   = s_mp_player_car[0];   /* slot 0 = player 1's car */
                s_selected_paint = s_mp_player_paint[0];
                td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                return;
            } else {   /* Back */
                if (s_mp_car_player > 0) {
                    s_mp_car_player--;
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                    return;
                }
                /* Backing out of player 1's car select abandons the whole
                 * multiplayer setup -> return to the MAIN MENU (not the join
                 * lobby). [2026-06-07 user request] */
                s_mp_flow = 0;
                s_two_player_mode = 0;
                td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                return;
            }
        }

        /* Two-player flow dispatch [CONFIRMED @ 0x0040DFC0 case 0x1A].
         * Original gates the 2P advance on g_returnToScreenIndex == -1 (OK
         * was pressed); Back has its own arm at lines 880-887 of the archived
         * decomp. The port mirrors this by gating on s_return_screen ==
         * TRACK_SELECTION (case 4 OK sets that; case 5 Back sets
         * RACE_TYPE_MENU). Without the gate, Back from P1 falls into the
         * P1→P2 branch and re-enters CarSelection — exactly the user-reported
         * "back moves forward" loop. */
        if (s_two_player_mode != 0) {
            if (s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
                /* OK */
                if ((s_two_player_mode & 3) == 1) {
                    /* P1 selected: save P1 choices, re-enter for P2 */
                    s_selected_car = actual_car; /* finalize */
                    s_two_player_mode = 6; /* set bit flags for P2 round */
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P1 OK car=%d → re-enter for P2", actual_car);
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                    return;
                }
                if ((s_two_player_mode & 3) == 2) {
                    /* P2 selected: save P2 choices, proceed to track selection */
                    s_p2_car = actual_car;
                    s_p2_paint = s_selected_paint;
                    s_p2_config = s_selected_config;
                    s_p2_transmission = s_selected_transmission;
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P2 OK car=%d → TrackSelection", actual_car);
                    td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                    return;
                }
            } else {
                /* Back [CONFIRMED @ 0x0040DFC0 lines 880-887].
                 * mode==1 (P1) → MainMenu. mode!=1 (P2 with mode=6) →
                 * sentinel mode=5, re-enter CarSelection so user can
                 * re-pick P1's car. */
                if (s_two_player_mode == 1) {
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P1 Back → MainMenu");
                    td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                    return;
                }
                s_two_player_mode = 5;
                TD5_LOG_I(LOG_TAG, "CarSelect 2P: P2 Back → CarSelection (mode=5)");
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;
            }
        }

        /* Cup-style modes (game_type 1..6): predefined track schedule —
         * SKIP TrackSelection and launch the race directly with the cup's
         * scheduled track.
         * [CONFIRMED @ 0x0040f744 LAB_0040f78b — original CarSelectionScreen
         *  case 0x1A path for any g_selectedGameType in {1..6} calls
         *  InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState
         *  directly with no SetFrontendScreen.]
         * Without this branch the port falls through to the generic
         * td5_frontend_set_screen(s_return_screen) at the bottom (which case 4
         * OK sets to TRACK_SELECTION), forcing cup users into a track picker
         * the original game never showed. The cup schedule track index lives
         * at s_cup_schedules[game_type-1][race_within_series] (CONFIRMED @
         * 0x464098, see s_cup_schedules definition above). */
        if (s_selected_game_type >= 1 && s_selected_game_type <= 6 &&
            !s_two_player_mode &&
            s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
            int sched_idx = s_selected_game_type - 1;
            if (s_race_within_series >= 0 && s_race_within_series < 13) {
                int sched_track = s_cup_schedules[sched_idx][s_race_within_series];
                if (sched_track >= 0 && sched_track != 99) {
                    s_selected_track = sched_track;
                }
            }
            s_selected_car = actual_car;
            TD5_LOG_I(LOG_TAG,
                      "CarSelect: cup game_type=%d race=%d track=%d -> skip track select, init schedule",
                      s_selected_game_type, s_race_within_series, s_selected_track);
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }

        /* Drag-race 2-pass CarSelect [CONFIRMED @ 0x0040f744].
         * Original: game_type==7 (=drag race there) && pass_marker==0 → re-enter
         * CarSelect with pass_marker=1, showing "CAR 2". After pass 2, skip
         * TrackSelection entirely and call InitializeRaceSeriesSchedule +
         * InitializeFrontendDisplayModeState directly (drag strip is fixed).
         * Port convention: case 4 OK sets s_return_screen=TRACK_SELECTION while
         * case 5 Back sets RACE_TYPE_MENU — only fire on OK. */
        if (g_td5.drag_race_enabled &&
            s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
            if (s_drag_carselect_pass == 0) {
                /* Pass 1 = PLAYER car. Stash it in dedicated storage so the
                 * pass-2 navigation cursor (s_selected_car) cannot clobber it.
                 * [Mirrors orig g_selectedCarIndex @0x0048f364 being preserved
                 *  across the opponent pass — the port's root-cause fix.] */
                s_p1_car   = actual_car;
                s_p1_paint = s_selected_paint;
                s_drag_carselect_pass = 1;
                /* Seed the pass-2 cursor from the opponent slot (orig loads its
                 * scratch from DAT_00463e08 on the opponent pass) so pass 2 does
                 * not start showing the player's just-picked car. Screen case 0
                 * clamps to the valid roster range. */
                s_selected_car   = s_p2_car;
                s_selected_paint = s_p2_paint;
                TD5_LOG_I(LOG_TAG,
                          "CarSelect: drag-race pass1 PLAYER car=%d saved → re-enter for opponent (cursor seed=%d)",
                          s_p1_car, s_selected_car);
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;
            } else {
                /* Pass 2 = OPPONENT car. */
                s_p2_car = actual_car;
                s_p2_paint = s_selected_paint;
                s_p2_config = s_selected_config;
                s_p2_transmission = s_selected_transmission;
                s_drag_carselect_pass = 0;
                /* Restore the PLAYER car/paint into the live selector BEFORE the
                 * schedule reads slot 0 from s_selected_car. Without this, slot 0
                 * (player) inherits the opponent car the user just cycled to ->
                 * both racers get the same car (the reported bug). */
                s_selected_car   = s_p1_car;
                s_selected_paint = s_p1_paint;
                TD5_LOG_I(LOG_TAG,
                          "CarSelect: drag-race pass2 OPPONENT car=%d, restored PLAYER car=%d → start race",
                          s_p2_car, s_selected_car);
                frontend_init_race_schedule();
                frontend_init_display_mode_state();
                return;
            }
        }

        /* Single-player: OK → track selection, Back → return screen */
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
    }
        break;

    default:
        s_inner_state = 7;
        break;
    }
}

/* Is the menu-selected track a CIRCUIT (laps meaningful) or point-to-point (no
 * laps)? Determined WITHOUT loading the race — LEVELINF.DAT isn't read until race
 * init — so:
 *   - TD6 tracks: the registry is authoritative (finish_span>0 = P2P, ==0 =
 *     circuit), matching the same source the in-race lap counter now uses.
 *   - Native TD5 tracks: circuit tracks ship NO reverse strip (forward-only) and
 *     P2P tracks DO, so reverse-data presence is an exact proxy (consistent with
 *     the Direction-toggle model and the per-track shipped data).
 * Random / 2P "?" (slot<0) defaults to circuit so the Laps row stays available. */
static int frontend_track_is_circuit(int track_slot) {
    if (track_slot < 0) return 1;
    int td6_level = td5_asset_td6_level_for_slot(track_slot);
    if (td6_level > 0)
        return td5_asset_td6_finish_span_for_level(td6_level) > 0 ? 0 : 1;
    return td5_asset_track_has_reverse(track_slot) ? 0 : 1;
}

/* Hide/show the "Laps" selector for the currently selected track. Point-to-point
 * tracks have no laps, so the row is hidden+disabled — the vertical nav, the
 * selector arrows (fe_draw_option_arrows) and the value draw all skip a hidden
 * button, exactly like the Direction toggle. Circuit tracks keep the row. Called
 * on screen entry and whenever the selected track changes. The persisted lap
 * value (s_game_option_laps) is untouched and is simply ignored by P2P races
 * (which finish at the finish line, not after N laps). */
static void frontend_update_laps_button_visibility(int laps_btn_idx) {
    if (laps_btn_idx < 0 || laps_btn_idx >= s_button_count) return;
    int is_circuit = frontend_track_is_circuit(s_selected_track);
    s_buttons[laps_btn_idx].hidden   = !is_circuit;
    s_buttons[laps_btn_idx].disabled = !is_circuit;
    TD5_LOG_I(LOG_TAG, "Laps row: track=%d circuit=%d -> %s",
              s_selected_track, is_circuit, is_circuit ? "SHOWN" : "hidden");
    /* Don't leave the highlight parked on a now-hidden row. */
    if (!is_circuit && s_selected_button == laps_btn_idx) s_selected_button = 0;
}

/* [R5 2026-06-19] Per-race AI difficulty is meaningless with no AI cars, so HIDE
 * the difficulty row (button 6) whenever the opponent count is 0. Also stays
 * hidden in Quick Race flow (flow_context==2), which already hides it at create
 * time. Called on init and whenever the opponent count changes, so toggling
 * opponents to/from 0 shows/hides the row live. Don't leave nav focus parked on
 * a hidden row. */
static void frontend_update_difficulty_button_visibility(int diff_btn_idx) {
    if (diff_btn_idx < 0 || diff_btn_idx >= s_button_count) return;
    int hide = (s_num_ai_opponents <= 0) || (s_flow_context == 2);
    s_buttons[diff_btn_idx].hidden   = hide;
    s_buttons[diff_btn_idx].disabled = hide;
    TD5_LOG_I(LOG_TAG, "Difficulty row: opponents=%d flow=%d -> %s",
              s_num_ai_opponents, s_flow_context, hide ? "hidden" : "SHOWN");
    if (hide && s_selected_button == diff_btn_idx) s_selected_button = 0;
}

static void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label) {
    if (dir_btn_idx < 0 || dir_btn_idx >= s_button_count) return;
    int has_reverse = (s_selected_track < 0)
                      ? 1
                      : td5_asset_track_has_reverse(s_selected_track);
    /* Diagnostic: per-track Direction-toggle decision. has_reverse is driven by
     * reverse-asset presence (STRIPB.DAT + LEFTB/RIGHTB.TRK) — TD6 tracks and TD5
     * P2P show the toggle; TD5 circuit tracks (no STRIPB) hide it. */
    TD5_LOG_I(LOG_TAG,
              "Direction toggle: track=%d level=%d has_reverse=%d -> %s",
              s_selected_track,
              (s_selected_track >= 0) ? td5_asset_level_number(s_selected_track) : -1,
              has_reverse, has_reverse ? "SHOWN" : "hidden");
    s_buttons[dir_btn_idx].hidden   = !has_reverse;
    s_buttons[dir_btn_idx].disabled = !has_reverse;
    if (!has_reverse) {
        s_track_direction = 0;
        if (manage_label) {
            strncpy(s_buttons[dir_btn_idx].label, "Forwards", sizeof(s_buttons[dir_btn_idx].label) - 1);
            s_buttons[dir_btn_idx].label[sizeof(s_buttons[dir_btn_idx].label) - 1] = '\0';
        }
        /* Don't leave the highlight focus parked on the now-hidden button. */
        if (s_selected_button == dir_btn_idx) s_selected_button = 0;
    }
}

void Screen_TrackSelection(void) {
    switch (s_inner_state) {
    case 0: /* Init: validate track for cup modes, create buttons, load TrackSelect.tga */
        frontend_init_return_screen(TD5_SCREEN_TRACK_SELECTION);
        TD5_LOG_D(LOG_TAG, "TrackSelection: init");
        s_anim_complete = 0;
        s_trksel_rand_btn = -1;   /* [#14] (re)assigned with the buttons below */
        s_trksel_prev_focus = -2; /* [R3-3] treat the first interactive frame as a focus-entry (no cycle) */

        /* Validate track index for cup modes: skip locked/invalid NPC groups */
        /* Determine track max for current mode. [2026-06-19] Net play can now
         * pick the migrated TD6 tracks (menu slots 26-36) too: both peers share
         * identical assets and the selected slot index propagates in the race
         * config, so it loads deterministically. Previously net was capped at 18
         * (the 19 native TD5 tracks), hiding every TD6 track from the host's
         * picker. All modes now use the full unlocked bound. */
        s_track_max = s_total_unlocked_tracks;
        /* [#14] With the RANDOMIZE button on (default), the 2P "?" (-1) random list
         * entry is retired, so default an out-of-range/stale selection to track 0 in
         * every mode. (TD5RE_RANDOM_BUTTON=0 restores the legacy 2P -1 default.) */
        if (s_selected_track >= s_track_max)
            s_selected_track = (s_two_player_mode && !frontend_random_button_on()) ? -1 : 0;
        if (s_selected_track < 0 && (!s_two_player_mode || frontend_random_button_on()))
            s_selected_track = 0;
        /* Never open parked on a selector-excluded slot — drag strip (19) or a
         * championship cup (20-25), see frontend_track_excluded_from_selector. */
        if (frontend_track_excluded_from_selector(s_selected_track)) s_selected_track = 0;

        /* Create buttons. Ghidra settles these at x=120 for Track/Forwards/OK
         * and x=232 for Back, with OK/Back sharing the bottom row. */
        frontend_create_button(SNK_TrackButTxt,     120,  97, 224, 32); /* 0: with L/R arrows */
        frontend_create_button(SNK_ForwardsButTxt,  120, 137, 224, 32); /* 1: direction toggle */
        /* [PORT ENHANCEMENT 2026-06] race-option rows: AI opponents, laps, traffic,
         * police. They drive s_num_ai_opponents + s_game_option_* which apply to
         * single/multiplayer races via ConfigureGameTypeFlags (cup game-types
         * override them, so they're inert there). Present on every track-select
         * entry (regular single race, quick race, multiplayer). */
        frontend_create_button(SNK_OpponentsButTxt, 120, 177, 224, 32); /* 2: AI count */
        frontend_create_button(SNK_LapsButTxt,      120, 217, 224, 32); /* 3: laps */
        frontend_create_button(SNK_TrafficButTxt,   120, 257, 224, 32); /* 4: traffic */
        frontend_create_button(SNK_CopsButTxt,      120, 297, 224, 32); /* 5: police */
        /* [PORT ENHANCEMENT 2026-06-12] Per-race AI difficulty. Seeded from the
         * tier ConfigureGameTypeFlags derived at mode selection (= the Game
         * Options global for game-type 0), applied to g_td5.difficulty_tier on
         * OK. Quick Race (flow context 2) keeps the Game Options global — the
         * row is still CREATED there (index stability: OK=6→7, Back=7→8) but
         * hidden+disabled, exactly like the Quick Race Players row. */
        frontend_create_button(SNK_DifficultyButTxt, 120, 337, 224, 32); /* 6: AI difficulty */
        /* [R5] Hide the difficulty row when there are 0 opponents (and still in
         * Quick Race flow). frontend_update_difficulty_button_visibility folds in
         * the old flow_context==2 hide; refreshed in case 4 when opponents change. */
        frontend_update_difficulty_button_visibility(6);
        s_race_difficulty = g_td5.difficulty_tier;
        if (s_race_difficulty < 0) s_race_difficulty = 0;
        if (s_race_difficulty > 2) s_race_difficulty = 2;
        frontend_create_button(SNK_OkButTxt,        120, 377,  96, 32); /* 7: OK */
        /* Quick Race mode: no Back button */
        if (s_flow_context != 2) {
            frontend_create_button(SNK_BackButTxt, 232, 377, 112, 32);  /* 8: Back */
        }
        /* The MP simultaneous car grid (pad-driven) hides the mouse cursor and
         * its all-ready path lands directly here — restore it so this screen's
         * mouse interaction is usable on every entry path. [cursor-fix 2026-06-12] */
        frontend_set_cursor_visible(1);
        /* [#14] RANDOMIZE: dedicated control for the Track selector. Created LAST
         * so the Track/Forwards/Opponents/.../OK/Back indices (0..7) used by the
         * action handlers are unchanged. Activating it picks a random track.
         * [item #7 2026-06-15] NEW default form = a small ICON to the RIGHT of the
         * Track selector (x=348, on its OWN sub-row y=99 — 2px below the Track row
         * y=97). Hit-rect created HIDDEN so the generic button loop skips its
         * frame+label; the icon is painted in frontend_render_trksel_randomize_icon
         * (chip only when focused, #12).
         * [#12 2026-06-15 nav-selectable] As on car-select, the icon's .y (99) is
         * kept DISTINCT from the Track selector's .y (97): the screen navigates by
         * INDEX+ROW, so a distinct row keeps the Track row's LEFT/RIGHT cycling the
         * track VALUE while this (highest-index, different-row) icon is reached by
         * pressing UP from the Track row (the vertical cycler wraps to the highest
         * different-row button). It is `active`, only `hidden`, never `disabled`, so
         * neither frontend_cycle_selected_button_by_row nor frontend_spatial_pick
         * (both ignore `hidden`) exclude it. Do NOT "align" it to the Track row —
         * that would hijack the value-cycle key. TD5RE_RANDOM_ICON=0 = full button. */
        if (frontend_random_button_on()) {
            if (frontend_random_icon_on()) {
                s_trksel_rand_btn = frontend_create_button(NULL, 348, 99,
                                                           FE_RAND_ICON_W, FE_RAND_ICON_H);
                if (s_trksel_rand_btn >= 0) s_buttons[s_trksel_rand_btn].hidden = 1;
            } else {
                s_trksel_rand_btn = frontend_create_button("Randomize", 120, 57, 224, 32);
            }
        }

        /* Network track-pick (entered from the lobby's SELECT TRACK, flow
         * context 4): the global ESC/back handler navigates to s_return_screen,
         * which defaults to CAR_SELECTION (the parent). Point it at the lobby so
         * ESC matches the OK/Back exit dispatch in state 8. [2026-06-07] */
        if (s_flow_context == 4)
            s_return_screen = TD5_SCREEN_NETWORK_LOBBY;

        /* Create 0x128 x 0xB8 info surface */
        frontend_load_tga("Front_End/TrackSelect.tga", "Front_End/FrontEnd.zip");

        s_track_direction = 0;
        s_track_switch_tick = 16; /* holds preview settled during button slide-in (state 3); reset to 0 in state 5 */
        frontend_load_selected_track_preview();
        /* Hide the Direction toggle for forward-only/circuit tracks from the
         * very first frame (cases 1-3 render before case 5 reloads). */
        frontend_update_direction_button_visibility(1, 1);
        /* Hide the Laps row (button 3) on point-to-point tracks (no laps). */
        frontend_update_laps_button_visibility(3);
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present + tick */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        /* (Direction-button visibility is set in case 0 and refreshed on each
         * track cycle in case 4 — see frontend_update_direction_button_visibility.) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            /* Original goes to state 5 (load preview) then state 8 (slide-in) on initial entry.
             * Route through state 5 so the 16-frame preview slide-in plays here too
             * [CONFIRMED @ frontend_screens_decompiled.c line 1222]. */
            TD5_LOG_I(LOG_TAG, "TrackSel: button slide-in complete, starting preview slide-in");
            s_inner_state = 5;
        }
        break;

    case 4: /* Main interaction: track preview + navigation */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int selected_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            /* [R3-3 2026-06-19] Suppress the track CYCLE on the single frame focus
             * first LANDS on the Track selector (button 0). Same class as the R6
             * randomize-on-leave fix: a focus-change event must not be read as a
             * value change. Moving focus onto button 0 (e.g. pressing UP from the
             * Direction row, or a diagonal analog stick whose horizontal bias also
             * sets the LEFT/RIGHT bit) was cycling the track immediately — both via
             * a stale s_arrow_input L/R bit AND via the hold-repeater's stale
             * static timer (frontend_trksel_hold_repeat is only CALLED while
             * selected_button==0, so its prev_held/next_ms freeze when focus is
             * elsewhere and can fire a phantom repeat the instant focus returns).
             * Tracking the previously-focused button lets us drop the delta for
             * exactly that entry frame; an EXPLICIT LEFT/RIGHT pressed on a later
             * frame (when focus is already settled on 0) still cycles normally.
             * s_trksel_prev_focus is a module static reset to -2 at screen init. */
            int focus_entered_track = (selected_button == 0 && s_trksel_prev_focus != 0);
            s_trksel_prev_focus = selected_button;
            /* [#21] Hold-to-scroll: while the TRACK selector (button 0) is focused
             * and no edge fired this frame, fold in the auto-repeat so holding
             * LEFT/RIGHT keeps cycling tracks (first press still comes from the
             * edge delta above). frontend_trksel_hold_repeat() is still CALLED on
             * the entry frame (so its prev_held/next_ms re-sync to the live held
             * state and can't fire a phantom repeat next frame), but its result is
             * discarded there. */
            if (selected_button == 0 && delta == 0) {
                int hb = frontend_trksel_hold_repeat();
                if (!focus_entered_track) {
                    if (hb == 1)      delta = -1;
                    else if (hb == 2) delta = +1;
                }
            }
            if (focus_entered_track) delta = 0;   /* landing focus here is not a track change */
            if (selected_button == 0 && delta != 0) {
                /* Cycle track index, skipping tracks whose level zips are absent */
                if (s_network_active) {
                    frontend_cycle_track(delta, 0, s_track_max + 1);
                } else if (s_two_player_mode && !frontend_random_button_on()) {
                    /* [legacy, TD5RE_RANDOM_BUTTON=0] 2P supports -1 = random as a
                     * list entry, handled separately. With the RANDOMIZE button on
                     * (default) this wrap-to -1 is removed (see the else branch). */
                    s_selected_track += delta;
                    if (s_selected_track < -1) s_selected_track = s_track_max - 1;
                    if (s_selected_track >= s_track_max) s_selected_track = -1;
                    /* Skip unavailable levels AND selector-excluded slots (drag
                     * strip + cups): cups resolve to level001.zip so the level-
                     * exists check alone won't drop them — route through the
                     * exclusion-aware cycler. */
                    if (s_selected_track >= 0 &&
                        (frontend_track_excluded_from_selector(s_selected_track) ||
                         !frontend_track_level_exists(s_selected_track)))
                        frontend_cycle_track(delta, 0, s_track_max);
                } else {
                    /* [#14] Normal cycling (incl. 2P when the RANDOMIZE button
                     * replaces the old "?" random list entry): wrap 0..track_max-1,
                     * never landing on -1. Random is now the dedicated button. */
                    if (s_selected_track < 0) s_selected_track = 0;  /* drop any stale -1 */
                    frontend_cycle_track(delta, 0, s_track_max);
                }
                frontend_play_sfx(2); /* ping2.wav cycle */
                TD5_LOG_I(LOG_TAG, "TrackSel CYCLED: track=%d level=%d name=%s",
                          s_selected_track, td5_asset_level_number(s_selected_track),
                          frontend_get_track_name(s_selected_track));

                /* Hide text+image immediately so render on THIS frame shows neither
                 * (old preview still loaded, new s_selected_track already set).
                 * Case 5 loads new preview next frame; case 9 runs the 16-frame slide-in. */
                s_track_switch_tick = 0;
                /* Re-evaluate the Direction toggle + Laps row for the new track
                 * (hide on forward-only/circuit tracks, restore on reverse-capable). */
                frontend_update_direction_button_visibility(1, 1);
                frontend_update_laps_button_visibility(3);
                s_inner_state = 5;
            }

            /* Direction toggle: 0=Forwards, 1=Backwards. Gated on the track
             * actually having reverse data — forward-only/circuit tracks hide
             * this button (see frontend_update_direction_button_visibility), and
             * the guard here keeps the toggle inert even if reached by another
             * path. Mirrors original 0x00427630 which only flips when the track's
             * reverse flag is set. */
            if (selected_button == 1 && !s_buttons[1].hidden &&
                (delta != 0 || s_button_index == 1)) {
                s_track_direction = !s_track_direction;
                strncpy(s_buttons[1].label,
                        s_track_direction ? "Backwards" : "Forwards",
                        sizeof(s_buttons[1].label) - 1);
                s_buttons[1].label[sizeof(s_buttons[1].label) - 1] = '\0';
            }

            /* [PORT ENHANCEMENT 2026-06] race-option rows (AI/laps/traffic/police/difficulty). */
            if (delta != 0 && selected_button >= 2 && selected_button <= 6) {
                if (selected_button == 2) {            /* AI opponents */
                    s_num_ai_opponents += delta;
                    if (s_num_ai_opponents < 0) s_num_ai_opponents = 0;
                    if (s_num_ai_opponents > TD5_MAX_RACER_SLOTS - 1)
                        s_num_ai_opponents = TD5_MAX_RACER_SLOTS - 1;
                    /* [R5] Show/hide the difficulty row live as opponents cross 0. */
                    frontend_update_difficulty_button_visibility(6);
                } else if (selected_button == 3 && !s_buttons[3].hidden) {  /* laps (value+1) */
                    s_game_option_laps += delta;
                    if (s_game_option_laps < 0) s_game_option_laps = 0;
                    if (s_game_option_laps > 9) s_game_option_laps = 9;
                } else if (selected_button == 4) {     /* traffic volume Off/Light/Normal/Heavy/Very-High */
                    /* [dynamic-traffic] 5-state wrap (delta may be negative). */
                    s_game_option_traffic =
                        ((s_game_option_traffic + delta) % TD5_TRAFFIC_VOLUME_COUNT
                         + TD5_TRAFFIC_VOLUME_COUNT) % TD5_TRAFFIC_VOLUME_COUNT;
                } else if (selected_button == 5) {     /* police on/off */
                    s_game_option_cops = (s_game_option_cops + delta) & 1;
                } else if (selected_button == 6 && !s_buttons[6].hidden) {
                    /* Per-race AI difficulty 0..2, wraps both ways (matches the
                     * Game Options row + orig 0x00420060 wrap behaviour). */
                    s_race_difficulty += delta;
                    if (s_race_difficulty < 0) s_race_difficulty = 2;
                    if (s_race_difficulty > 2) s_race_difficulty = 0;
                }
                frontend_play_sfx(2);
            }

            if (s_button_index == 7) { /* OK */
                /* Lock enforcement */
                int locked = (s_selected_track >= 0 && s_selected_track < 37 &&
                             s_track_lock_table[s_selected_track] != 0 &&
                             !s_network_active && !s_cheat_unlock_all);
                if (locked) {
                    frontend_play_sfx(10);
                } else {
                    g_td5.reverse_direction = s_track_direction;
                    /* [S02 (c) 2026-06-04] Persist the lap choice (re-homed from
                     * Game Options, which no longer owns this setting).
                     * [dynamic-traffic] Persist the traffic volume picked on
                     * this screen too. */
                    g_td5.ini.laps    = s_game_option_laps;
                    /* [dynamic-traffic] Persist the full 0..4 volume (was & 3,
                     * which truncated VERY HIGH to OFF on save). */
                    g_td5.ini.traffic = s_game_option_traffic;
                    if (g_td5.ini.traffic < 0) g_td5.ini.traffic = 0;
                    if (g_td5.ini.traffic > TD5_TRAFFIC_VOLUME_COUNT - 1)
                        g_td5.ini.traffic = TD5_TRAFFIC_VOLUME_COUNT - 1;
                    td5_ini_persist_options();
                    /* [2026-06-12] Per-race AI difficulty: commit the row into
                     * the live tier read by InitializeRaceActorRuntime
                     * (@0x00432E60) + AdjustCheckpointTimersByDifficulty
                     * (@0x0040A530). NOT persisted to the INI — the Game
                     * Options global is untouched, and ConfigureGameTypeFlags
                     * re-derives the tier from it on the next main-menu mode
                     * selection, so Quick Race never inherits this pick. */
                    if (!s_buttons[6].hidden && g_td5.difficulty_tier != s_race_difficulty) {
                        TD5_LOG_I(LOG_TAG, "TrackSel OK: per-race difficulty tier %d -> %d",
                                  g_td5.difficulty_tier, s_race_difficulty);
                        g_td5.difficulty_tier = s_race_difficulty;
                    }
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 6;
                }
            }

            /* Back (button 8 — master's per-race difficulty row shifted it from 7).
             * Guard against the RANDOMIZE button landing on index 8 in Quick Race
             * flow (flow_context==2 skips Back, so RANDOMIZE becomes index 8) —
             * without this, pressing RANDOMIZE there would also trip Back. */
            if (s_button_index == 8 && s_button_index != s_trksel_rand_btn) { /* Back */
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 6;
            }

            /* [#14] RANDOMIZE: pick a random track, then run the SAME change flow as
             * a manual cycle (hide preview this frame, reload + slide-in via 5->9).
             * track_max is exclusive; network caps at 0x13 like frontend_cycle_track.
             * [#22] Also triggered by the keyboard 'R' key (edge-latched below), so
             * randomize works without the mouse. */
            {
                /* 'R' (DIK 0x13) one-shot: latch the rising edge so holding R
                 * randomizes once per press, not every frame. */
                static int s_trksel_r_held = 0;
                int r_now  = td5_plat_input_key_pressed(0x13) ? 1 : 0;
                int r_edge = (r_now && !s_trksel_r_held) ? 1 : 0;
                s_trksel_r_held = r_now;
                /* [R6a 2026-06-19] Fire ONLY on an EXPLICIT activation of the rand
                 * button (A/Enter or a click set s_button_index == rand) or the R
                 * key. Require the activation to AGREE with the live focus
                 * (s_selected_button) so a stray s_button_index that aliases the
                 * rand index (e.g. an overlapping/hidden-rect click while focus is
                 * elsewhere, or an index collision in a flow with fewer buttons)
                 * can't randomize on a focus-change/leave. The R key is its own
                 * unambiguous trigger and bypasses the focus check. */
                int btn_activated = (s_trksel_rand_btn >= 0 &&
                                     s_button_index == s_trksel_rand_btn &&
                                     s_selected_button == s_trksel_rand_btn);
                int do_random = btn_activated || r_edge;
                if (do_random) {
                    int bound = s_track_max;   /* [2026-06-19] net incl. TD6 (s_track_max already full) */
                    if (frontend_pick_random_track(bound)) {
                        frontend_play_sfx(3);
                        TD5_LOG_I(LOG_TAG, "TrackSel RANDOMIZE: track=%d level=%d name=%s",
                                  s_selected_track, td5_asset_level_number(s_selected_track),
                                  frontend_get_track_name(s_selected_track));
                        s_track_switch_tick = 0;
                        frontend_update_direction_button_visibility(1, 1);
                        frontend_update_laps_button_visibility(3);
                        s_inner_state = 5;
                    } else {
                        frontend_play_sfx(10); /* nothing else to pick */
                    }
                }
            }
        }
        break;

    case 5: /* Track change: load new preview, start slide-in animation */
        /* Original: frame 1 clears text surface, frame 2 loads preview → state 8 (slide-in).
         * Source port: load immediately, then run slide-in via state 9.
         * Both text and preview animate together via s_track_switch_tick [CONFIRMED @ 0x427984]. */
        frontend_load_selected_track_preview();
        s_track_switch_tick = 0;
        TD5_LOG_I(LOG_TAG, "TrackSel: track change slide-in start track=%d", s_selected_track);
        s_inner_state = 9;
        break;

    case 6: /* Slide-out prep */
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 7;
        break;

    case 7: /* Slide-out animation (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_inner_state = 8;
        }
        break;

    case 8: /* Exit dispatch */
        /* Quick Race (context 2): go to screen 7 */
        /* Network (context 4): CreateFrontendNetworkSession() */
        /* Normal (return == -1): launch race */
        /* Back (return != -1): nav to screen */
        if (s_flow_context == 2) {
            td5_frontend_set_screen(TD5_SCREEN_QUICK_RACE);
        } else if (s_flow_context == 4) {
            /* Network: create session and go to lobby */
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        } else if (s_return_screen == -1) {
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
        } else {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    case 9: /* Track-switch slide-in: 16 frames [CONFIRMED @ 0x427e96 original state 8].
             * Single tick counter drives both preview (slides from right) and text (slides from above).
             * Original: tick * -0x10 + 0x22e for preview x; (tick-0x10)*0x10 + base for text y. */
        s_track_switch_tick += s_fe_logic_ticks;
        if (s_track_switch_tick >= 16) {
            s_track_switch_tick = 16; /* settled: render offsets become 0 */
            TD5_LOG_I(LOG_TAG, "TrackSel: track change slide-in complete");
            frontend_play_sfx(4); /* completion chime [CONFIRMED @ 0x427e96] */
            s_inner_state = 4;
        }
        break;
    }
}

void Screen_PostRaceHighScore(void) {
    switch (s_inner_state) {
    case 0: /* Init: load BG, create surfaces, buttons */
        frontend_init_return_screen(TD5_SCREEN_HIGH_SCORE);
        TD5_LOG_D(LOG_TAG, "PostRaceHighScore: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;
        /* Create 0x208 x 0x90 score panel surface (black fill) */
        /* Create nav button + OK button */
        /* [FIXED 2026-06-01 v2] Nav-bar selector + OK button — FAITHFUL native positions,
         * read from the original's RESOLVED auto-layout in runtime memory (button table
         * @0x499c78, the -0x208/-0x130 auto-layout sentinels resolve to these):
         *   button 0 (nav bar): origin (0x73,0x61)=(115,97), 0x208x0x20 = 520x32
         *   button 1 (OK)     : origin (115,377),            0x60 x0x20 = 96x32
         * The prior y128 was derived from the widescreen-PATCHED exe, whose ~1.275x vertical
         * frontend stretch renders the nav text at screen-y ~140-158 (= native ~103-118); the
         * previous fix wrongly treated those stretched screen pixels as native coords. With the
         * faithful button y=97, the centered track name (drawn at ty=nav_by+6, see ~line 6175)
         * lands at native top 103 — matching the de-stretched original. OK was likewise off
         * (port 120,416 vs faithful 115,377). The track name itself is button-centered, which
         * mirrors the original: RebuildFrontendButtonSurface(0,0,SNK_TrackNames[pool]) draws
         * the label MeasureOrCenter'd across the 520px button width. */
        frontend_create_button(NULL, 115, 97, 520, 32);    /* nav bar selector */
        frontend_create_button(SNK_OkButTxt, 115, 377, 96, 32);  /* OK button */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        s_score_category_index = 0;
        /* [#2b] The Records browse screen walks the 26 authored TD5 groups by L/R;
         * clear any stale TD6 post-race flag so the overlay shows the TD5 groups
         * (not a leftover TD6 record table from the last race). */
        s_postrace_td6_level = 0;
        /* [FIXED 2026-06-01] Records browsing has no "just-inserted" row, so no row
         * is highlighted here. -1 keeps the shared high-score overlay from golding a
         * stale rank (NAME_ENTRY sets s_score_insert_pos to the real inserted rank). */
        s_score_insert_pos = -1;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present, reset tick, draw score entry 0 */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            frontend_set_cursor_visible(1);
            frontend_play_sfx(4);
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R arrows browse score categories */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            if (delta != 0) {
                s_score_category_index += delta;
                /* High score screen: all 26 tracks+cups accessible regardless of lock state.
                 * Simple wrap [0..0x19]. */
                if (s_score_category_index > 0x19) s_score_category_index = 0;
                if (s_score_category_index < 0)    s_score_category_index = 0x19;
                /* Menu-move nav sound on track change. The original plays sound id 2
                 * (DXSound::Play(2)) centrally for arrow-capable L/R changes in the
                 * shared frontend input handler [CONFIRMED @ 0x0042687c, handler
                 * UpdateFrontendDisplayModeSelection @ 0x00426580]. The port's central
                 * keyboard handler only fires sfx(2) when a horizontal cycle moves the
                 * selected button, which the High Scores nav bar never does, so play it
                 * explicitly here like the other option-cycle screens (e.g. line 10103). */
                frontend_play_sfx(2); /* ping2.wav cycle */
                TD5_LOG_I(LOG_TAG, "PostRaceHighScore: track nav delta=%d -> category=%d (sfx 2)",
                          delta, s_score_category_index);
            }
            if (s_button_index == 1) { /* OK */
                s_inner_state = 7;
            }
        }
        break;

    case 7: /* Prep slide-out */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* Return to caller or init race.
             * When s_return_screen == -1: original calls InitializeFrontendDisplayModeState
             * which unconditionally calls WritePackedConfigTd5 to flush high-score table.
             * [CONFIRMED @ 0x00413b60 / 0x00414aa0] */
            if (s_return_screen == -1) {
                frontend_init_race_schedule();
                TD5_LOG_I(LOG_TAG, "PostRaceHighScore: writing config (high-score flush)");
                td5_save_write_config(NULL);
            } else {
                td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            }
        }
        break;
    }
}

/* [item 17 2026-06-14] Race-results screen fixes: entry slide-in animation +
 * restrict car-cycling to when the selector bar is focused. Gated by
 * TD5RE_RESULTS_FIX (default on; "0" restores the old behavior). */
static int results_fix_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RESULTS_FIX");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "results-screen fix %s (TD5RE_RESULTS_FIX=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* [#1 2026-06-16] Race-results ENTRY slide-in. Every other frontend screen
 * advances its slide-in state via frontend_update_timed_animation(0x27, 650)
 * (650 ms; see Screen_QuickRaceMenu/TrackSelection case 3, Screen_CarSelection
 * case 3, Screen_PostRaceHighScore case 3) — the results screen instead used a
 * bespoke tick counter, and the default all-players SUMMARY view (drawn at fixed
 * column centres) ignored the panel slide entirely, so it "popped" in with NO
 * animation. When ON, state 3 is driven by the SAME timed-animation helper and
 * the summary table slides in horizontally with the panel; "0" restores the old
 * tick-based behaviour (and the summary's static draw). */
static int results_anim_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RESULTS_ANIM");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "results-screen entry slide-in (#1) %s (TD5RE_RESULTS_ANIM=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* ========================================================================
 * [#10] All-players race-end SUMMARY
 * ========================================================================
 *
 * Reworks the post-race results from the original one-car-at-a-time browse
 * (the single-slot stat sheet in frontend_render_race_results_overlay) into a
 * single TABLE that shows EVERY participating racer at once, with per-racer:
 * finishing POSITION, player/car, TOP SPEED, AVERAGE SPEED, plus the new
 * telemetry COLLISIONS / TIME ON AIR / DRIFTS (g_race_metrics, accumulated each
 * sim tick in td5_physics_accumulate_metrics).
 *
 * Knob: TD5RE_RACE_SUMMARY (default ON). When ON, Screen_RaceResults disables
 * the L/R per-car browse (state 6) so the panel stays put and this all-players
 * table is what shows; "0" restores the faithful one-at-a-time results screen.
 *
 * RENDER WIRING (the one cross-file line this needs — see report): the frontend
 * draw happens in td5_frontend.c's render switch, which clears the frame AFTER
 * the screen handler runs, so this table must be drawn from the RENDER path.
 * frontend_render_race_summary_overlay() is exported for that; td5_frontend.c
 * must call it for TD5_SCREEN_RACE_RESULTS when race_summary_on() (one line +
 * an extern decl). Until that line lands the OLD single-slot overlay renders. */
int frontend_race_summary_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RACE_SUMMARY");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "race-end all-players summary %s (TD5RE_RACE_SUMMARY=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Speed: raw internal units -> MPH/KPH, identical to the HUD speedometer and
 * frontend_convert_speed (kept local since that helper is file-static to
 * td5_frontend.c). */
static int frontend_summary_speed_disp(int raw, int kph) {
    if (raw < 0) raw = 0;
    return kph ? (raw * 256 + 389) / 778    /* KPH [CONFIRMED @0x438ebc] */
               : (raw * 256 + 625) / 1252;  /* MPH [CONFIRMED @0x438ed4] */
}

/* [#17 2026-06-15] Knob for the race-results AI car-name fix. Default ON =
 * proper pretty display name (the same name the High-Scores / car-select screens
 * show); TD5RE_SUMMARY_CARNAME=0 restores the legacy raw config.nfo line-1 read
 * with the "CAR %d" / underscore-style fallback. */
static int frontend_summary_carname_pretty(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_SUMMARY_CARNAME");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "race-results AI car names (#17) %s (TD5RE_SUMMARY_CARNAME=%s)",
                  v ? "PRETTY display name" : "legacy raw", e ? e : "default");
    }
    return v;
}

/* Copy `src` into dst[dst_cap], converting '_' -> ' ' and stopping at EOL/NUL
 * (and at the first whitespace when stop_at_space). Mirrors td5_frontend.c's
 * frontend_copy_pretty_text (file-static there, so it can't be shared) so summary
 * names read identically to the rest of the UI — no underscores, no raw IDs.
 * Returns 1 if it wrote a non-empty string. */
static int frontend_summary_copy_pretty(char *dst, size_t dst_cap, const char *src, int stop_at_space) {
    size_t di = 0;
    if (!dst || dst_cap == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    while (*src && di + 1 < dst_cap) {
        char ch = *src++;
        if (ch == '\r' || ch == '\n') break;
        if (stop_at_space && (ch == ' ' || ch == '\t')) break;
        dst[di++] = (ch == '_') ? ' ' : ch;
    }
    dst[di] = '\0';
    return dst[0] != '\0';
}

/* Read line `line_no` (0-based) of `entry` in `archive`, prettified into out.
 * first_token=1 keeps only the first whitespace token (the config.eng display-name
 * convention). Returns 1 on a non-empty result. Used to mirror the High-Scores
 * short-name and the car-select display-name readers without touching the
 * file-static helpers in td5_frontend.c. */
static int frontend_summary_read_pretty_line(const char *entry, const char *archive,
                                             int line_no, int first_token,
                                             char *out, size_t out_cap) {
    int sz = 0, ok = 0;
    char *data;
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!archive) return 0;
    data = (char *)td5_asset_open_and_read(entry, archive, &sz);
    if (data && sz > 0) {
        int i = 0, ln = 0;
        while (ln < line_no && i < sz) {                 /* skip to the target line */
            while (i < sz && data[i] != '\n' && data[i] != '\r') i++;
            while (i < sz && (data[i] == '\r' || data[i] == '\n')) i++;
            ln++;
        }
        /* Copy the raw line slice into a bounded temp, then prettify into out. */
        char raw[64]; int j = 0;
        while (i < sz && j + 1 < (int)sizeof(raw) &&
               data[i] != '\r' && data[i] != '\n' && data[i] != '\0')
            raw[j++] = data[i++];
        raw[j] = '\0';
        ok = frontend_summary_copy_pretty(out, out_cap, raw, first_token);
    }
    if (data) free(data);
    return ok;
}

/* SHORT car name for a RACE SLOT, read lazily from the car's archive and cached.
 * [#17] Returns the PROPER pretty display name — the same name the High-Scores
 * table (frontend_get_car_short_name) and the car-select screens
 * (frontend_get_car_display_name) show — so AI cars no longer render the raw car
 * ID / an underscore-style name. Source order matches those readers: config.nfo
 * line 1 (short name) -> config.eng token 0 -> config.nfo line 0 -> zip basename,
 * all with '_' converted to ' '. Per-slot car index comes from
 * g_td5.ai_car_indices[] (set for every slot incl. slot 0 at schedule build).
 * Those UI readers are file-static in td5_frontend.c (not exported in any header),
 * so the pretty + fallback chain is replicated here rather than extern-called.
 * TD5RE_SUMMARY_CARNAME=0 restores the legacy raw read. */
static const char *frontend_summary_car_name(int slot) {
    static char  s_name[TD5_MAX_RACER_SLOTS][24];
    static int   s_loaded[TD5_MAX_RACER_SLOTS];
    static int   s_for_car[TD5_MAX_RACER_SLOTS];
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return "";
    int car = (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) ? g_td5.ai_car_indices[slot] : 0;
    if (s_loaded[slot] && s_for_car[slot] == car) return s_name[slot];
    s_loaded[slot]  = 1;
    s_for_car[slot] = car;
    s_name[slot][0] = '\0';

    const char *zip = (car >= 0 && car < TD5_CAR_COUNT) ? s_car_zip_paths[car] : NULL;

    if (!frontend_summary_carname_pretty()) {
        /* Legacy path: raw config.nfo line 1, "CAR %d" fallback. */
        if (zip) {
            int sz = 0;
            char *data = (char *)td5_asset_open_and_read("config.nfo", zip, &sz);
            if (data && sz > 0) {
                int i = 0;
                while (i < sz && data[i] != '\n' && data[i] != '\r') i++;   /* skip line 0 */
                while (i < sz && (data[i] == '\r' || data[i] == '\n')) i++;
                int j = 0;
                while (i < sz && j + 1 < (int)sizeof(s_name[slot]) &&
                       data[i] != '\r' && data[i] != '\n' && data[i] != '\0')
                    s_name[slot][j++] = data[i++];
                s_name[slot][j] = '\0';
            }
            if (data) free(data);
        }
        if (!s_name[slot][0])
            snprintf(s_name[slot], sizeof(s_name[slot]), "CAR %d", car);
        return s_name[slot];
    }

    /* Pretty path (default): match the High-Scores / car-select name chain. */
    if (zip) {
        /* 1) config.nfo line 1 = the short name the High-Scores table uses. */
        if (frontend_summary_read_pretty_line("config.nfo", zip, 1, 0,
                                              s_name[slot], sizeof(s_name[slot])))
            return s_name[slot];
        /* 2) config.eng token 0 = the car-select DISPLAY name (first whitespace
         *    token, prettified) — same as frontend_get_car_display_name. */
        if (frontend_summary_read_pretty_line("config.eng", zip, 0, 1,
                                              s_name[slot], sizeof(s_name[slot])))
            return s_name[slot];
        /* 3) config.nfo line 0 = the long display line (whole line). */
        if (frontend_summary_read_pretty_line("config.nfo", zip, 0, 0,
                                              s_name[slot], sizeof(s_name[slot])))
            return s_name[slot];
        /* 4) zip basename minus extension, prettified. */
        {
            const char *base = strrchr(zip, '/');
            base = base ? base + 1 : zip;
            if (frontend_summary_copy_pretty(s_name[slot], sizeof(s_name[slot]), base, 0)) {
                char *dot = strrchr(s_name[slot], '.');
                if (dot) *dot = '\0';
                if (s_name[slot][0]) return s_name[slot];
            }
        }
    }
    /* Absolute last resort (no archive at all): a neutral label, no underscores. */
    snprintf(s_name[slot], sizeof(s_name[slot]), "CAR %d", car);
    return s_name[slot];
}

/* Build the ordered list of racer slots to show: finishers first (by finish
 * position), then any not-yet-finished/DNF racers, capped at the racer field.
 * Returns the count written to order[]. */
static int frontend_summary_build_order(int order[TD5_MAX_RACER_SLOTS]) {
    int total = td5_game_get_total_actor_count();
    int racers = total < TD5_MAX_RACER_SLOTS ? total : TD5_MAX_RACER_SLOTS;
    if (racers < 1) racers = 1;
    char seen[TD5_MAX_RACER_SLOTS]; memset(seen, 0, sizeof(seen));
    int n = 0;
    /* Finishers in race-order (position 0 = 1st). */
    for (int pos = 0; pos < racers; pos++) {
        int slot = td5_game_get_race_order(pos);
        if (slot < 0 || slot >= racers) continue;
        if (td5_game_get_slot_state(slot) == 3) continue;   /* disabled/decoration */
        if (seen[slot]) continue;
        seen[slot] = 1;
        order[n++] = slot;
    }
    /* Any remaining active racers the order table didn't list (e.g. DNF). */
    for (int slot = 0; slot < racers; slot++) {
        if (seen[slot]) continue;
        if (td5_game_get_slot_state(slot) == 3) continue;
        seen[slot] = 1;
        order[n++] = slot;
    }
    if (n < 1) { order[0] = 0; n = 1; }
    return n;
}

/* Exported render-path draw for the all-players summary. Coordinates use the
 * 640x480 virtual canvas (sx/sy = window/canvas), same convention as every
 * other frontend overlay. Text via the public td5_vui_text API (the only
 * cross-module text helper; forwards to the frontend bitmap/MSDF text path). */
/* Small-font results-table helpers shared with the High-Scores overlay
 * (defined in td5_frontend.c) so this table matches that screen's exact
 * format: same proportional small font, centred columns, MM:SS.cc times. */
void  fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_measure_small_text(const char *text);
void  frontend_format_score_time(char *buf, size_t cap, int raw_ticks, int type);

/* [r3 2026-06-15] Race-results table, reworked to the HIGH-SCORES format
 * (frontend_render_high_score_overlay): centred columns in the small
 * proportional font, two-line headers, the local player highlighted, speeds
 * carrying their KPH/MPH unit, and a TIME column. Columns:
 *   NAME | TIME | TOP SPEED | AVG SPEED | COLLISIONS | AIR TIME
 * The "#" position column and the DRIFT column were dropped per feedback. */
void frontend_render_race_summary_overlay(float sx, float sy) {
    if (s_inner_state < 3 || s_inner_state >= 0x0C) return;
    if (s_network_active) return;   /* net flow skips straight to the lobby */

    int order[TD5_MAX_RACER_SLOTS];
    int n = frontend_summary_build_order(order);
    int kph = (td5_save_get_speed_units() != 0);
    const char *unit = kph ? "KPH" : "MPH";
    int humans = s_num_human_players; if (humans < 1) humans = 1;
    const int my_slot = 0;          /* the local player (P1); in SP the sole human */

    /* 4:3-locked glyph scale, exactly like frontend_render_high_score_overlay,
     * so centred columns stay centred at any window aspect. */
    const float gsx = sx < sy ? sx : sy;
    /* [#1] Entry slide-in: states 7-0xB drive s_results_panel_slide_x for the
     * one-car browse; state 3 now drives it too (see Screen_RaceResults), so the
     * whole table glides in/out with the panel instead of popping. The offset is
     * a CANVAS-px delta applied to every column centre. */
    const float sum_slide = results_anim_on() ? (float)s_results_panel_slide_x : 0.0f;
    #define SUM_CTR(CX, S) (((CX) + sum_slide) * sx - fe_measure_small_text(S) * 0.5f * gsx)

    /* Column CENTRES (640x480 canvas px). */
    const float c_name = 168.0f;
    const float c_time = 256.0f;
    const float c_top  = 336.0f;
    const float c_avg  = 410.0f;
    const float c_col  = 486.0f;
    const float c_air  = 560.0f;

    /* Two-line headers: single labels on the middle line, stacked labels on
     * top+bottom — mirrors DrawPostRaceHighScoreEntry's y0/y7/y14 rows. */
    const float h_top = 88.0f, h_mid = 95.0f, h_bot = 102.0f;
    const float row0  = 122.0f, row_h = 20.0f;

    const uint32_t HDR   = 0xFFFFFFFFu;  /* white headers (High-Scores style)  */
    const uint32_t SELF  = 0xFFD9C50Cu;  /* gold/yellow = the local player row  */
    const uint32_t AICOL = 0xFFE0E0E0u;  /* light grey = AI (HS non-highlight)  */

    fe_draw_small_text(SUM_CTR(c_name, "NAME"),       h_mid * sy, "NAME",       HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_time, "TIME"),       h_mid * sy, "TIME",       HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_top,  "TOP"),        h_top * sy, "TOP",        HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_top,  "SPEED"),      h_bot * sy, "SPEED",      HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_avg,  "AVG"),        h_top * sy, "AVG",        HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_avg,  "SPEED"),      h_bot * sy, "SPEED",      HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_col,  "COLLISIONS"), h_mid * sy, "COLLISIONS", HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_air,  "AIR"),        h_top * sy, "AIR",        HDR, sx, sy);
    fe_draw_small_text(SUM_CTR(c_air,  "TIME"),       h_bot * sy, "TIME",       HDR, sx, sy);

    /* Reference time for DNF pace-estimates: the winner's finish time. */
    int32_t winner_time = 0;
    if (n > 0 && td5_game_slot_is_finished(order[0]))
        winner_time = td5_game_get_result_primary(order[0]);

    int span_count = td5_track_get_span_count();
    if (span_count <= 0) span_count = 1;
    int laps_total = (g_track_is_circuit && g_td5.circuit_lap_count > 0)
                     ? g_td5.circuit_lap_count : 1;

    for (int r = 0; r < n; r++) {
        int slot = order[r];
        float y = (row0 + (float)r * row_h) * sy;
        char buf[40];

        int is_human = (slot < humans);
        uint32_t row_color = (slot == my_slot) ? SELF
                           : is_human ? k_mp_player_colors[slot < TD5_MAX_HUMAN_PLAYERS ? slot : 0]
                           : AICOL;

        /* NAME — humans "P1".."P6"; AI the car's short name. */
        if (is_human) snprintf(buf, sizeof(buf), "P%d", slot + 1);
        else          snprintf(buf, sizeof(buf), "%s", frontend_summary_car_name(slot));
        fe_draw_small_text(SUM_CTR(c_name, buf), y, buf, row_color, sx, sy);

        /* TIME — finishers show their finish time; cars still out show a "~"
         * estimate: elapsed scaled by (total spans / spans completed), i.e.
         * holding their average pace over the spans remaining to the end. */
        {
            char tb[28];
            if (td5_game_slot_is_finished(slot)) {
                int32_t ft = td5_game_get_result_primary(slot);
                if (ft <= 0) ft = td5_game_get_race_timer(slot, 0);
                frontend_format_score_time(tb, sizeof(tb), ft, 0);
            } else {
                int cur_lap = td5_game_get_player_lap(slot);
                if (cur_lap < 0) cur_lap = 0;
                if (cur_lap >= laps_total) cur_lap = laps_total - 1;
                int cur_span = td5_game_get_slot_span(slot);
                if (cur_span < 0) cur_span = 0;
                if (cur_span > span_count) cur_span = span_count;
                long long done  = (long long)cur_lap * span_count + cur_span;
                long long total = (long long)laps_total * span_count;
                if (done  < 1)    done  = 1;
                if (total < done) total = done;
                int32_t elapsed = td5_game_get_race_timer(slot, 0);
                if (elapsed <= 0) elapsed = winner_time;
                int32_t est = (int32_t)((long long)elapsed * total / done);
                char inner[24];
                frontend_format_score_time(inner, sizeof(inner), est, 0);
                snprintf(tb, sizeof(tb), "~%s", inner);
            }
            fe_draw_small_text(SUM_CTR(c_time, tb), y, tb, row_color, sx, sy);
        }

        const TD5_RaceMetrics *m = td5_game_get_metrics(slot);
        int top_raw = (int)td5_game_get_result_top_speed(slot);
        if (top_raw == 0 && m) top_raw = m->top_speed;
        int avg_raw = (int)td5_game_get_result_avg_speed(slot);
        if (avg_raw == 0 && m && m->sample_ticks > 0)
            avg_raw = (int)(m->speed_sum / m->sample_ticks);

        snprintf(buf, sizeof(buf), "%d%s", frontend_summary_speed_disp(top_raw, kph), unit);
        fe_draw_small_text(SUM_CTR(c_top, buf), y, buf, row_color, sx, sy);
        snprintf(buf, sizeof(buf), "%d%s", frontend_summary_speed_disp(avg_raw, kph), unit);
        fe_draw_small_text(SUM_CTR(c_avg, buf), y, buf, row_color, sx, sy);

        int collisions = m ? m->collisions : 0;
        int air_tenths = m ? (m->air_ticks * 10 + 15) / 30 : 0;
        snprintf(buf, sizeof(buf), "%d", collisions);
        fe_draw_small_text(SUM_CTR(c_col, buf), y, buf, row_color, sx, sy);
        snprintf(buf, sizeof(buf), "%d.%ds", air_tenths / 10, air_tenths % 10);
        fe_draw_small_text(SUM_CTR(c_air, buf), y, buf, row_color, sx, sy);
    }
    #undef SUM_CTR
}

void Screen_RaceResults(void) {
    switch (s_inner_state) {
    case 0: /* Init & routing: sort results, create panel */
        frontend_init_return_screen(TD5_SCREEN_RACE_RESULTS);
        frontend_reset_buttons();
        TD5_LOG_I(LOG_TAG, "RaceResults: state 0 - init, game_type=%d",
                  s_selected_game_type);

        /* [#2a] If we land on the results screen with VALID live results (a genuine
         * finished race — incl. a replay that completed normally and re-finished),
         * the live data supersedes any pre-replay snapshot, so drop it. Only an
         * ESC-aborted replay (slot 0 left unfinished) keeps the snapshot alive for
         * the post-race name-entry fallback. */
        if (replay_quit_flow_on() && s_pr_snap_valid && td5_game_slot_is_finished(0)) {
            s_pr_snap_valid = 0;
            TD5_LOG_D(LOG_TAG, "RaceResults: live results valid — cleared pre-replay snapshot");
        }

        /* P8 — DXSound::Play(5) on screen entry.
         * [CONFIRMED @ 0x00422480 case 0] Original calls DXSound::Play(5) near
         * the end of state 0 init, before g_frontendInnerState advances. */
        frontend_play_sfx(5);

        /* P2 (plan_screen24) — MainMenu.tga backdrop. Original case 0 calls
         * LoadTgaToFrontendSurface16bpp("MainMenu.tga", "FrontEnd.zip") then
         * CopyPrimaryFrontendBufferToSecondary so the post-race UI overlays
         * the main-menu artwork. Without it the bg-gallery slideshow floats
         * on a black clear — visually empty. Every other screen handler
         * loads this same TGA as its s_background_surface (see e.g.
         * Screen_MainMenu state 0); we mirror that to give screen 24 a
         * faithful backdrop. */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* [CONFIRMED @ 0x004224B6 RunRaceResultsScreen case 0] Early-route to
         * Screen_CupFailed (TD5_SCREEN_CUP_FAILED, 0x1a) when the player has
         * been eliminated mid-cup. The original has two parallel paths:
         *
         *   1) game_type == 4 + slot_state[0]+0x383 != 0 (game-type-4-only)
         *   2) !network && results_skip_display != 1 && 1 <= game_type < 7
         *      && (slot_state[0].companion_state_2 == 2 ||
         *          actor_state.slot[0]+0x328 == 0)
         *
         * Both jump straight to TD5_SCREEN_CUP_FAILED with return_screen = 5
         * (TD5_SCREEN_MAIN_MENU). Without this, exiting a cup mid-progression
         * skips the failure dialog entirely and dumps the player back to the
         * main menu. The port reaches TD5_SCREEN_CUP_FAILED only via the
         * explicit Quit button at case 0x10 — which doesn't fire when the
         * cup-failed condition is detected automatically. */
        if (!s_results_view_data_request &&
            !s_network_active &&
            !s_results_skip_display &&
            s_selected_game_type >= 1 && s_selected_game_type < 7 &&
            (td5_game_get_slot_companion_2(0) == 2 ||
             !td5_game_slot_is_finished(0))) {
            TD5_LOG_I(LOG_TAG, "RaceResults: cup-fail early-route "
                      "(game_type=%d companion_2=%d finished=%d) -> CUP_FAILED",
                      s_selected_game_type,
                      td5_game_get_slot_companion_2(0),
                      td5_game_slot_is_finished(0));
            s_return_screen = TD5_SCREEN_MAIN_MENU;
            td5_frontend_set_screen(TD5_SCREEN_CUP_FAILED);
            return;
        }

        /* Sort results by game type:
         * Types 1/6: by secondary metric desc (SortRaceResultsBySecondaryMetricDesc @ 0x40AB80)
         * Types 2-5: by primary metric asc    (SortRaceResultsByPrimaryMetricAsc   @ 0x40AAD0)
         * [CONFIRMED @ 0x00422480 case 0] */
        td5_game_sort_results();

        /* Save race snapshot on first entry */
        if (!s_results_rerace_flag) {
            s_snap_car = s_selected_car;
            s_snap_paint = s_selected_paint;
            s_snap_trans = s_selected_transmission;
            s_snap_config = s_selected_config;
            s_snap_num_ai_opponents = s_num_ai_opponents;  /* [FIX 2026-06-05] */
            s_results_rerace_flag = 1;
            TD5_LOG_I(LOG_TAG,
                      "RaceResults: snapshot saved car=%d opponents=%d (for Race Again)",
                      s_snap_car, s_snap_num_ai_opponents);
        }

        /* [CONFIRMED @ 0x00422480 case 0 tail] Build the click-catcher + OK
         * button pair, then advance to state 1. The original places them at
         * (-0x208, 0, 0x208, 0x20) and (-0x208, 0, 0x60, 0x20) — both off the
         * left edge of the canvas — and slides them in via MoveFrontendSpriteRect
         * during state 3 (P7 of plan_screen24, not in this slice). Until the
         * sprite-rect animation lands we plant them at their on-screen rest
         * positions: 520x32 invisible click-catcher centred at y=400 with the
         * 96x32 OK button overlaid in the centre. Both indices satisfy the
         * state-6 exit gate (s_button_index >= 0 && < 2), so clicking either
         * the OK button or anywhere along the catcher row returns to the
         * post-race menu. The state-3 gate below short-circuits to state 0xC
         * when no race data is present (fresh StartScreen=24 land on menu;
         * post-race natural flow runs the table sub-flow). */
        s_results_cup_complete = 0;
        s_results_skip_display = 0;
        s_anim_tick = 0;
        /* [FIXED 2026-06-02] Faithful rest positions (orig RunRaceResultsScreen case 3
         * MoveFrontendSpriteRect at counter==0x27, halfW=320/halfH=240):
         *   btn0 selector nav bar (520x32) slides to (115,97) — shows the car name +
         *        ◄► arrows (browse racers); orig creates it NULL-labelled at -0x208.
         *   btn1 OK (96x32) slides to (115,377) — bottom-LEFT.
         * The prior placeholder planted BOTH at y=400, which read as a double OK. */
        /* [item #3 (a) 2026-06-15] When the all-players SUMMARY replaces the old
         * one-car-at-a-time browse, the selector nav bar at (115,97) is vestigial
         * (cycling is disabled — see case 6) and its blue 9-slice frame + centred
         * car-name/◄► overlay sit right on top of the summary's column headers
         * (hdr_y=96). Hide it: hidden=1 makes the button loop skip the 9-slice,
         * and pushing its rect off-canvas makes the car-name overlay — which is
         * NOT hidden-gated and keys off button 0's render rect — draw off-screen
         * too. Also mark it disabled so nav/mouse skip it (no invisible focus
         * target); the OK button (index 1) + ESC still exit via the state-6 gate,
         * which only ever sees a non-disabled confirm (index 1). Default focus is
         * moved to OK. Create at the normal (115,97) first so the negative-x
         * auto-layout path isn't triggered, then relocate. Knob OFF / summary OFF
         * keeps the faithful on-screen selector bar. */
        {
            int hide_selector_bar = (results_fix_on() && frontend_race_summary_on());
            int bi = frontend_create_button(NULL, 115, 97, 0x208, 0x20);
            if (bi >= 0) {
                if (hide_selector_bar) {
                    s_buttons[bi].hidden   = 1;
                    s_buttons[bi].disabled = 1;
                    s_buttons[bi].x = 4000; /* off-canvas: no 9-slice, no nav-bar overlay */
                    s_buttons[bi].y = 4000;
                } else {
                    s_buttons[bi].is_selector = 1;
                }
            }
            frontend_create_button(SNK_OkButTxt, 115, 377, 0x60, 0x20);
            if (hide_selector_bar) s_selected_button = 1;   /* focus the visible OK */
        }
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present buffer, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        /* [#1] Seed the shared timed-animation clock so state 3 can drive the
         * slide-in the same way every other screen does (begin resets
         * s_anim_start_ms; matches Screen_TrackSelection state 0). */
        if (results_anim_on()) frontend_begin_timed_animation();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames.
             * [CONFIRMED @ 0x004228EC case 3 head] First three statements
             * gate the slide-in itself. The original short-circuits to
             * state 0xC (cleanup -> 0xD menu) when ANY of:
             *   - DAT_00497a74 != 0    (s_results_skip_display)
             *   - slot[0].companion_state_2 == 2  (player disqualified)
             *   - actor.slot._808_4_ == 0         (no race finished)
             * The `slot._808_4_ == 0` clause is what makes a fresh
             * SetFrontendScreen(0x18) — i.e. --StartScreen=24 or a Frida
             * frontend_screen=24 hop — land directly on the post-race menu:
             * no actor data has been written, so the table sub-flow is
             * skipped. After a real race the finish flag is set and the
             * gate falls through, so the table animates in normally. */
        if (!s_results_view_data_request &&
            (s_results_skip_display ||
             td5_game_get_slot_companion_2(0) == 2 ||
             !td5_game_slot_is_finished(0))) {
            TD5_LOG_I(LOG_TAG,
                      "RaceResults: state 3 early-exit gate fired "
                      "(skip=%d companion_2=%d finished=%d) -> 0xC",
                      s_results_skip_display,
                      td5_game_get_slot_companion_2(0),
                      td5_game_slot_is_finished(0));
            s_results_skip_display = 0;
            s_inner_state = 0x0C;
            break;
        }
        /* [#1] Entry slide-in. When ON, drive the SAME 650 ms timed animation
         * every other screen's case-3 uses (frontend_update_timed_animation
         * sets s_anim_tick 0..0x27 and returns the 0..1 progress) and derive the
         * panel offset from it so the slide GLIDES in instead of popping — the
         * old tick path only moved s_results_panel_slide_x, which the default
         * summary view ignores. Advance when the helper reports >= 1.0f, exactly
         * like Screen_QuickRaceMenu/PostRaceHighScore. */
        int slide_done;
        if (results_anim_on()) {
            float t = frontend_update_timed_animation(0x27, 650);
            /* Enter from the right edge (+0x220) and settle to rest (0), matching
             * the L/R browse slide magnitude used by states 7-10. */
            s_results_panel_slide_x = (int)((1.0f - t) * (float)0x220 + 0.5f);
            if (s_results_panel_slide_x < 0) s_results_panel_slide_x = 0;
            slide_done = (t >= 1.0f);
        } else {
            s_anim_tick += 2 * s_fe_logic_ticks;
            if (results_fix_on()) {
                /* [item 17b legacy] panel enters from the right, settles at rest. */
                s_results_panel_slide_x = (0x12 - s_anim_tick) * 0x20;
                if (s_results_panel_slide_x < 0) s_results_panel_slide_x = 0;
            }
            slide_done = (s_anim_tick >= 0x12);
        }
        if (slide_done) {
            /* P8 — DXSound::Play(4) on slide-in completion.
             * [CONFIRMED @ 0x00422480 case 3] Original fires Play(4) inside
             * the AdvanceFrontendTickAndCheckReady ready-branch immediately
             * before g_frontendInnerState++. */
            frontend_play_sfx(4);
            /* Sentinel served its purpose — clear now that state 3's full
             * 9-tick animation has run. Clearing earlier (e.g. immediately
             * after the gate check) would let the gate fire on tick 2 since
             * !finished is still true; the flag must survive across the
             * entire state-3 anim window. */
            s_results_view_data_request = 0;
            /* Mark the intro slide complete (mirrors High Scores case 3). This both
             * enables the shared ESC handler and gates the selector nav-bar car-name +
             * ◄► render — without it the top selector bar drew empty. [FIXED 2026-06-02] */
            s_anim_complete = 1;
            s_results_panel_slide_x = 0;   /* [#1] snap to rest (clear any rounding residue) */
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R browse racer slots (0-5), confirm exits.
             * Original @ 0x004229DA: button_index >= 0 && < 2 -> state 0x0B.
             * [CONFIRMED @ 0x004229DA] DAT_00497a68 cycles by DAT_0049b690 (arrow delta),
             * skips slots with state == 3 (disabled). Drag: masked & 1 for 2-slot only.
             * P7 PANEL fix: arrow input now triggers state 7 (right) or 9 (left)
             * for the slide-out animation; the actual slot cycle happens
             * between out- and in-slide so the new slot's data is visible
             * only as the panel re-enters from the opposite side. */
        s_results_panel_slide_x = 0;  /* clean rest position while in interactive */
        if (s_input_ready) {
            /* [item 17a 2026-06-14] Only cycle racer cards when the SELECTOR BAR
             * (button 0, is_selector) is focused — not when the OK button is.
             * Knob-gated: TD5RE_RESULTS_FIX=0 restores cycling from any focus. */
            int car_focused = (s_selected_button >= 0 &&
                               s_selected_button < s_button_count &&
                               s_buttons[s_selected_button].is_selector);
            int allow_cycle = (!results_fix_on()) || car_focused;
            /* [#10] All-players summary shows every racer at once, so the
             * per-car L/R browse is pointless — disable cycling when the
             * summary is on (TD5RE_RACE_SUMMARY). OK/confirm still exits. */
            if (frontend_race_summary_on()) allow_cycle = 0;
            /* [FIX 2026-06-05 results-nav] Only LEFT/RIGHT browse racer slots
             * (the horizontal panel slide). s_arrow_input is a BITMASK
             * (1=LEFT,2=RIGHT,4=UP,8=DOWN), so the old `!= 0` test fired on
             * UP/DOWN too, and `> 0` is true for any non-zero mask — so every
             * arrow (incl. LEFT) slid the panel RIGHT. UP/DOWN are already
             * consumed by the shared row-nav handler (frontend_update_input
             * moves the selection between the selector bar and OK vertically),
             * so here we react to the horizontal bits only and honour their
             * direction: RIGHT -> state 7 (exit right), LEFT -> state 9. */
            if (allow_cycle && (s_arrow_input & 2)) { /* RIGHT */
                s_results_panel_slide_dir = +1;
                s_anim_tick = 0;
                s_inner_state = 7;
                TD5_LOG_D(LOG_TAG,
                          "RaceResults state 6: RIGHT -> slide-out state 7");
                break;
            }
            if (allow_cycle && (s_arrow_input & 1)) { /* LEFT */
                s_results_panel_slide_dir = -1;
                s_anim_tick = 0;
                s_inner_state = 9;
                TD5_LOG_D(LOG_TAG,
                          "RaceResults state 6: LEFT -> slide-out state 9");
                break;
            }
            /* [CONFIRMED @ 0x004229DA] Original wraps the button-press exit
             * in `if (DAT_0049b690 == 0)` — i.e. only honor the confirm when
             * no arrow input is also queued. Without the gate a paired
             * arrow-and-click exits the browser before s_score_category_index
             * has updated, which the original avoids. */
            if (s_button_index >= 0 && s_button_index < 2) { /* confirm -> exit */
                TD5_LOG_I(LOG_TAG, "RaceResults: state 6 -> 0x0B (confirm, btn=%d)",
                          s_button_index);
                s_anim_tick = 0;
                s_inner_state = 0x0B;
            }
        }
        break;

    case 7: /* Right slide-OUT: panel exits right edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 7] Original formula:
             *   panel_x = g_frontendAnimFrameCounter * 0x20 + 0x2a + iVar4
             * Step +0x20 (32 px/frame), end counter == 0x11 (17). At end:
             *   DrawRaceDataSummaryPanel(DAT_00497a68);  // re-fill with new slot
             *   counter = 0; state++;                    // -> state 8
             * Port uses s_anim_tick stepping +2 from 0..0x11. */
        s_anim_tick += 2 * s_fe_logic_ticks;
        s_results_panel_slide_x = s_anim_tick * 0x20;  /* +0..+0x220 */
        if (s_anim_tick >= 0x11) {
            /* Cycle slot index now (mid-slide) — new data visible on slide-in.
             * [CONFIRMED @ 0x00422A22] Wrap: [0..5] with 6 -> 0 and -1 -> 5. */
            s_score_category_index += s_results_panel_slide_dir;
            if (s_selected_game_type == 7) {
                s_score_category_index &= 1;  /* drag 2-slot mask */
            } else {
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            /* Skip disabled slots */
            for (int _skip = 0;
                 _skip < 6 && td5_game_get_slot_state(s_score_category_index) == 3;
                 _skip++) {
                s_score_category_index += s_results_panel_slide_dir;
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            TD5_LOG_D(LOG_TAG, "RaceResults: slid out right, now slot=%d",
                      s_score_category_index);
            s_anim_tick = 0;
            s_inner_state = 8;
        }
        break;

    case 8: /* Left slide-IN: panel enters from left edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 8] Original formula:
             *   panel_x = counter * 0x20 + -0x1f6 + iVar4
             * At counter=0 panel is off-screen left (-0x1f6 + iVar4 ~ -382);
             * at counter=0x11 it reaches rest x. Port: offset progresses
             * from -0x220 → 0. */
        s_anim_tick += 2 * s_fe_logic_ticks;
        s_results_panel_slide_x = -((0x11 - s_anim_tick) * 0x20);  /* -0x220..0 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_inner_state = 6;
        }
        break;

    case 9: /* Left slide-OUT: panel exits left edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 9] Original formula:
             *   panel_x = iVar4 + counter * -0x20 + 0x2a
             * Step -0x20 per frame; same DrawRaceDataSummaryPanel re-fill
             * trigger at counter==0x11. */
        s_anim_tick += 2 * s_fe_logic_ticks;
        s_results_panel_slide_x = -(s_anim_tick * 0x20);  /* 0..-0x220 */
        if (s_anim_tick >= 0x11) {
            s_score_category_index += s_results_panel_slide_dir;
            if (s_selected_game_type == 7) {
                s_score_category_index &= 1;
            } else {
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            for (int _skip = 0;
                 _skip < 6 && td5_game_get_slot_state(s_score_category_index) == 3;
                 _skip++) {
                s_score_category_index += s_results_panel_slide_dir;
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            TD5_LOG_D(LOG_TAG, "RaceResults: slid out left, now slot=%d",
                      s_score_category_index);
            s_anim_tick = 0;
            s_inner_state = 10;
        }
        break;

    case 10: /* Right slide-IN: panel enters from right edge. 17 frames.
              * [CONFIRMED @ 0x00422480 case 10] Original formula:
              *   panel_x = iVar4 + counter * -0x20 + 0x24a
              * At counter=0 panel is off-screen right (+0x220 from rest);
              * at counter=0x11 reaches rest. */
        s_anim_tick += 2 * s_fe_logic_ticks;
        s_results_panel_slide_x = (0x11 - s_anim_tick) * 0x20;  /* +0x220..0 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_inner_state = 6;
        }
        break;

    case 0x0B: /* Exit slide-out: 17 frames.
                * [CONFIRMED @ 0x00422480 case 0xB] Original simultaneously
                * slides panel right (step +0x30) and title left (step -0x20).
                * Port animates the panel via slide_x; the title strip is
                * gated to states 3..0xB by the P1 fix and disappears at
                * state 0xC, so its X slide is approximated by the existing
                * Y-slide-out path that fires when the FSM exits the
                * table-browse window. */
        s_anim_tick += 2 * s_fe_logic_ticks;
        s_results_panel_slide_x = s_anim_tick * 0x30;  /* +0..+0x330 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_anim_tick = 0;
            s_inner_state = 0x0C;
        }
        break;

    case 0x0C: /* Cleanup: release tracked surfaces + clear button table.
                * P9 — [CONFIRMED @ 0x00422CEE] original body:
                *   DAT_0049628c = ReleaseTrackedFrontendSurface(DAT_0049628c);
                *   DAT_00496358 = ReleaseTrackedFrontendSurface(DAT_00496358);
                *   ReleaseFrontendDisplayModeButtons();
                *   g_frontendInnerState++;
                * Port has no analog of those tracked surfaces — the panel
                * (DAT_0049628c, 408×392) is rendered fresh each frame from
                * frontend_render_race_results_overlay via fe_draw_text + the
                * MainMenu.tga backdrop (P2), not allocated through
                * CreateTrackedFrontendSurface, so there is nothing to release.
                * The title (DAT_00496358) maps to the RESULTS header, which is
                * now drawn procedurally via the title.ttf face each frame
                * (frontend_draw_screen_title) — no baked surface to release.
                * So state 0xC collapses to button-table reset.
                * State 0x14 transitions to 0xD (NOT 0xC per the [Ghidra
                * 2026-05-01 RE: agent corrected the plan]) — no double-free. */
        frontend_reset_buttons();
        s_inner_state = 0x0D;
        break;

    case 0x0D: /* Post-results menu: create context-dependent buttons.
                * Button dims 0x120 x 0x20 (288x32), not port's prior 0xE0.
                * Y offsets -0x8F/-0x5F/-0x2F/+1/+0x31 from canvas center
                * (step 0x30 = 48). [CONFIRMED @ 0x004231D6-0x0042323C] */
        TD5_LOG_I(LOG_TAG, "RaceResults: state 0xD - building menu (game_type=%d)",
                  s_selected_game_type);

        if (s_network_active) {
            /* Network: skip to lobby or main menu */
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
            return;
        }

        {
            /* [FIXED 2026-06-02, runtime @0x499c78] menu buttons LEFT-align at x=120 (halfW-200),
             * NOT centered (port had FE_CENTER_X-0x90=176). Y0=97 step 48 + 288-wide confirmed. */
            const int RR_BX = FE_CENTER_X - 200;      /* 120 */
            const int RR_BW = 0x120;                  /* 288 */
            const int RR_BH = 0x20;                   /* 32 */
            const int RR_Y0 = FE_CENTER_Y - 0x8F;     /* 97 */
            const int RR_Y1 = FE_CENTER_Y - 0x5F;     /* 145 */
            const int RR_Y2 = FE_CENTER_Y - 0x2F;     /* 193 */
            const int RR_Y3 = FE_CENTER_Y + 0x01;     /* 241 */
            const int RR_Y4 = FE_CENTER_Y + 0x31;     /* 289 */

            /* Cup races are game_type 1..6; types 0, 7, 9 are single-race
             * variants (Quick Race / Time Trial / Drag). Slot 1 (View Replay)
             * and slot 2 (View Race Data) are identical across both branches;
             * only slots 0/3/4 differ. ConfigureGameTypeFlags has runtime
             * side effects (mode globals) so it stays gated to cup races. */
            const int is_cup = (s_selected_game_type >= 1 &&
                                s_selected_game_type != 7 &&
                                s_selected_game_type != 9);
            const int next_valid = is_cup ? ConfigureGameTypeFlags() : 1;
            /* [FIXED 2026-06-01] byte-exact SNK_ labels (results action menu, state 0xD):
             * SNK_NextCupRace/RaceAgain/SaveRaceStatus/SelectNewCar/Quit, OK = SNK_OkButTxt. */
            const char *btn0 = is_cup ? SNK_NextCupRace    : SNK_RaceAgain;
            const char *btn3 = is_cup ? SNK_SaveRaceStatus : SNK_SelectNewCar;
            /* Cup-complete path @ 0x00422FD8: slot 4 swaps Quit→OK and the
             * Quit dispatch in state 0x10 routes to CUP_WON/CUP_FAILED. */
            const char *btn4 = (is_cup && !next_valid) ? SNK_OkButTxt : SNK_Quit;

            frontend_create_button(btn0,              RR_BX, RR_Y0, RR_BW, RR_BH);
            /* NOTE (2026-06-01): orig greys View Replay / View Race Data as disabled
             * preview buttons when g_replayFileAvailable==0 / no race-data. The port has
             * no replay subsystem, so there is no availability flag to gate on; leaving
             * them live (they no-op) rather than greying them ALWAYS (which would be the
             * literal result of "no replay ever available" and is arguably worse UX).
             * Revisit if/when a replay system is added. [fix_20.md S24] */
            frontend_create_button(SNK_ViewReplay,     RR_BX, RR_Y1, RR_BW, RR_BH);
            frontend_create_button(SNK_ViewRaceData,  RR_BX, RR_Y2, RR_BW, RR_BH);
            frontend_create_button(btn3,              RR_BX, RR_Y3, RR_BW, RR_BH);
            frontend_create_button(btn4,              RR_BX, RR_Y4, RR_BW, RR_BH);

            if (is_cup && !next_valid) {
                s_results_cup_complete = 1;
            }

            /* Masters (type 5): special progression (PRE-EXISTING branch
             * reused verbatim; flag inversion from original is tracked in
             * prior memory and not touched by this fix) */
            if (is_cup && s_selected_game_type == 5 && !s_results_rerace_flag &&
                s_race_within_series != 9) {
                s_inner_state = 0x15;
                return;
            }
        }

        s_anim_tick = 0;
        s_inner_state = 0x0E;
        break;

    case 0x0E: /* Menu slide-in: 5 buttons animate in, 32 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x0F;
        }
        break;

    case 0x0F: /* Menu interaction */
        if (s_input_ready && s_button_index >= 0) {
            s_results_button = s_button_index;
            s_anim_tick = 0;
            s_inner_state = 0x10;
        }
        break;

    case 0x10: /* Menu slide-out: 32 frames, then dispatch */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            switch (s_results_button) {
            case 0: /* Race Again / Next Cup Race */
                if (s_results_rerace_flag) {
                    /* Restore snapshot */
                    s_selected_car = s_snap_car;
                    s_selected_paint = s_snap_paint;
                    s_selected_transmission = s_snap_trans;
                    s_selected_config = s_snap_config;
                    /* [FIX 2026-06-05 race-again-opponent-count] Restore the
                     * opponent count so the rerun keeps the same field size.
                     * frontend_init_race_schedule reads s_num_ai_opponents for
                     * the RACE_RESULTS re-race path (game_type 0); cups override
                     * the count downstream so this is a no-op for them. */
                    if (s_snap_num_ai_opponents >= 0)
                        s_num_ai_opponents = s_snap_num_ai_opponents;
                    TD5_LOG_I(LOG_TAG,
                              "RaceResults: Race Again -> restored car=%d opponents=%d "
                              "(game_type=%d)",
                              s_selected_car, s_num_ai_opponents, s_selected_game_type);
                }
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_race_within_series++;
                }
                frontend_init_race_schedule();
                break;

            case 1: /* View Replay */
                /* [CONFIRMED @ 0x00422F2C case 1 in case 0xF]:
                 *   g_attractModeControlEnabled = 2
                 *   g_inputPlaybackActive = 1
                 *   g_frontendInnerState++ (fall into slide-out then InitializeFrontendDisplayModeState)
                 * Port: set s_replay_mode via td5_input_set_replay_mode(1),
                 *       enable playback via td5_input_set_playback_active(1),
                 *       then re-init race (replay file was saved at race start).
                 * [DA-M1 fix 2026-05-22] Also set the game-side s_replay_mode static
                 * via td5_game_set_replay_mode(1). Without this, td5_game_init_race_session
                 * at td5_game.c:1902 hits the WriteOpen branch (memsets recording buffer)
                 * → playback reads zero input forever → race appears to restart blank.
                 * Closes todo-view-replay-restarts-race-2026-05-19. */
                TD5_LOG_I(LOG_TAG, "RaceResults: View Replay selected "
                          "(reuse schedule + restore seed; track=%d reverse=%d "
                          "ai=[%d,%d,%d,%d,%d])",
                          g_td5.track_index, g_td5.reverse_direction,
                          g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
                          g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
                          g_td5.ai_car_indices[5]);
                /* [#2a] Snapshot the genuine qualifying result NOW, before the
                 * replay's td5_game_init_race_session wipes the live race results.
                 * On return from the replay, the post-race high-score NAME-ENTRY
                 * falls back to this so QUIT still leads to the name-entry / record
                 * flow instead of an empty frontend -> main menu. */
                if (replay_quit_flow_on()) frontend_postrace_snapshot_capture();
                td5_game_set_demo_mode(0);            /* a replay, not a demo */
                td5_input_set_replay_mode(1);
                td5_input_set_playback_active(1);
                td5_game_set_replay_mode(1);
                /* Determinism: re-enter the SAME race WITHOUT rebuilding the AI
                 * schedule. Keep the recorded race's track/direction/opponents
                 * (g_td5.track_index, reverse_direction, ai_car_indices[]) and let
                 * td5_game_init_race_session restore the saved RNG seed (step 0).
                 * Mirrors the original, which re-enters via the unchanged
                 * g_selectedScheduleIndex and does NOT call
                 * InitializeRaceSeriesSchedule for View Replay [CONFIRMED
                 * @0x422F2C]. Calling frontend_init_race_schedule() here (as the
                 * port previously did) re-picks AI cars from a fresh time-seed →
                 * different opponents → "not the race I just drove". Just request
                 * the race; the state machine fires init on the next tick. */
                g_td5.race_requested = 1;
                break;

            case 2: /* View Race Data — re-enter screen 24 from state 0.
                     * [CONFIRMED @ 0x00423110 case 0x10 dispatch on
                     * DAT_00497a64 == 2] Original simply calls
                     * SetFrontendScreen(0x18) — which IS this screen,
                     * 0x18 == 24 == TD5_SCREEN_RACE_RESULTS. Re-entry runs
                     * state 0 again: snapshot restore, sort, button rebuild,
                     * then states 1..3 take the table path because the
                     * state-3 gate above does not fire (race actor data
                     * is still populated). The previous port "invention"
                     * (manual button reset + state=1 seed) skipped the
                     * sort/snapshot and could leak stale buttons across
                     * cycles; faithful re-entry replaces all of that. */
                TD5_LOG_I(LOG_TAG,
                          "RaceResults: View Race Data button — self-jump "
                          "(companion_2[0]=%d finished[0]=%d skip=%d)",
                          td5_game_get_slot_companion_2(0),
                          td5_game_slot_is_finished(0),
                          s_results_skip_display);
                /* Bypass state 0 cup-fail and state 3 table-skip gates so the
                 * table always shows when the user explicitly asks for it
                 * (even after a DNF / early quit where slot_is_finished == 0). */
                s_results_view_data_request = 1;
                td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS);
                return;

            case 3: /* Save Race Status / Select New Car */
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_inner_state = 0x11; /* save cup data */
                } else {
                    /* [#4 2026-06-15] "Select New Car" (non-cup) re-enters
                     * CAR_SELECTION, but a local simultaneous-MP flow is still mid-
                     * setup (s_mp_simul==1, s_mp_phase==1), so Screen_CarSelection's
                     * 0->1 intercept is skipped, frontend_mp_flow_reset() never runs,
                     * the s_mp_pos_shown_this_flow latch stays set, and the new race
                     * jumps STRAIGHT to the car grid (CHOOSE YOUR SCREEN + name/colour
                     * bypassed). Force the canonical fresh-flow path: clear s_mp_simul
                     * / s_mp_phase so the intercept fires, resets the flow, re-offers
                     * the position picker, and shows the picker again. Names/colours
                     * restore from the saved session. "Race Again" (case 0, identical
                     * rematch) is intentionally left untouched. Gate TD5RE_MP_POS_REASK
                     * (default on; "0" = legacy jump-straight-to-grid). */
                    if (mp_pos_reask_enabled() && s_mp_flow && s_num_human_players >= 2) {
                        s_mp_simul = 0;
                        s_mp_phase = 0;
                        TD5_LOG_I(LOG_TAG, "RaceResults: Select New Car -> reset MP "
                                  "simul flow (re-ask position screen for new race)");
                    }
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                }
                break;

            case 4: /* Quit
                     * P6 — Quit branch fidelity. [CONFIRMED @ 0x004233E0
                     * dispatch on DAT_00497a64 == 4] Original dispatches FOUR
                     * branches; the prior port collapsed them to TWO and got
                     * the cup-won/failed inversion wrong:
                     *
                     *   game_type < 1 (single):
                     *     AwardCupCompletionUnlocks(); SetFrontendScreen(0x19)
                     *   game_type >= 1, DAT_00497a70 == 0  (cup mid-progress):
                     *     SetFrontendScreen(5)            // back to MainMenu
                     *   cup, DAT_00497a70 != 0, DAT_0048d988._2_2_ == 0  (won):
                     *     g_returnToScreenIndex = 0x19;   SetFrontendScreen(0x1B)
                     *   cup, DAT_00497a70 != 0, DAT_0048d988._2_2_ != 0  (failed):
                     *     g_returnToScreenIndex = 0x19;   SetFrontendScreen(0x1A)
                     *
                     * DAT_0048d988._2_2_ is the int16 at +2 of s_results[0]
                     * — i.e. final_position. 0 = 1st place (won), nonzero =
                     * not 1st (failed). DAT_00497a70 is s_results_cup_complete,
                     * set in state 0xD when ConfigureGameTypeFlags()==0.
                     * AwardCupCompletionUnlocks @ 0x00421DA0 grants unlock flags;
                     * Screen_CupWon already invokes the port equivalent
                     * (td5_save_apply_cup_unlocks_ex) on its own entry, so the
                     * single-race quit path here only needs the screen jump.
                     * Game types 7 (Drag) and 9 (Drag Race) fall under "cup"
                     * here because the dispatch only special-cases <1, NOT 7/9. */
                    if (s_selected_game_type < 1) {
                        td5_frontend_set_screen(TD5_SCREEN_NAME_ENTRY);  /* 0x19 */
                    } else if (!s_results_cup_complete) {
                        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);   /* 5 */
                    } else {
                        s_return_screen = TD5_SCREEN_NAME_ENTRY;         /* 0x19 */
                        if (td5_game_get_finish_position(0) == 0) {
                            td5_frontend_set_screen(TD5_SCREEN_CUP_WON);    /* 0x1B */
                        } else {
                            td5_frontend_set_screen(TD5_SCREEN_CUP_FAILED); /* 0x1A */
                        }
                    }
                break;
            }
        }
        break;

    case 0x11: /* Save cup data */
        /* [CONFIRMED @ 0x0042332E]: WriteCupData() result picks SNK_BlockSavedOK or
         * SNK_FailedToSave string; creates 2 buttons (message + OK).
         * Port maps to localised strings via static labels. */
        {
            const char *save_msg;
            if (frontend_write_cup_data()) {
                TD5_LOG_I(LOG_TAG, "RaceResults: cup data saved (Block Saved OK)");
                save_msg = "Block Saved OK";
            } else {
                TD5_LOG_W(LOG_TAG, "RaceResults: failed to save cup data (Failed to Save)");
                save_msg = "Failed to Save";
            }
            /* Button 0: message label (288x32); Button 1: OK (96x32).
             * [CONFIRMED @ 0x00423342/0x0042335C]: offset -0x120 from canvas, width 0x120/0x60 */
            frontend_create_button(save_msg, FE_CENTER_X - 0x90, FE_CENTER_Y - 0x5F, 0x120, 0x20);
            frontend_create_button(SNK_OkButTxt,     FE_CENTER_X - 0x30, FE_CENTER_Y + 0x31, 0x60,  0x20);
        }
        s_anim_tick = 0;
        s_inner_state = 0x12;
        break;

    case 0x12: /* Save confirmation slide-in: 32 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x13;
        }
        break;

    case 0x13: /* Save confirmation wait */
        if (s_input_ready && s_button_index >= 0) {
            s_inner_state = 0x14;
        }
        break;

    case 0x14: /* Save confirmation slide-out: 32 frames -> back to menu */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x0D; /* return to post-results menu */
        }
        break;

    case 0x15: /* Masters series progression */
        TD5_LOG_D(LOG_TAG, "RaceResults: Masters progression");
        s_results_rerace_flag = 1;
        s_results_skip_display = 1;
        /* Go to car selection for next Masters race */
        td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
        break;
    }
}

void Screen_PostRaceNameEntry(void) {
    switch (s_inner_state) {
    case 0: /* Qualification check */
        /* [CONFIRMED @ 0x00413BC0 case 0] */
        frontend_init_return_screen(TD5_SCREEN_NAME_ENTRY);
        TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: qualification check, game_type=%d",
                  s_selected_game_type);

        /* Snapshot speeds NOW while the actor pool is still alive — by the
         * time case 4 fires (after name entry slide-in/typing/slide-out OR
         * the direct !qualifies jump) the race teardown may have invalidated
         * td5_game_get_actor(0), which would zero the avg/top columns in
         * the inserted entry (user-reported 2026-05-26). */
        s_post_race_top_speed = td5_game_get_result_top_speed(0);
        s_post_race_avg_speed = td5_game_get_result_avg_speed(0);

        /* [#2b] Detect a TD6 track up front. A single race on a TD6 track has no
         * authored NPC group; the score belongs in the genuine TD6 record store,
         * and the high-score table renders that (or "NO RECORDS YET"), never fake
         * names. Only single-race types reach a TD6 track (cups are TD5-only), so
         * this is gated to the non-cup branch below. 0 = normal TD5 path. */
        s_postrace_td6_level = (s_selected_game_type < 1 || s_selected_game_type == 7)
                               ? frontend_postrace_td6_level() : 0;

        /* Compute group index for the high-score table.
         * Cup types 1-6: group = game_type + 0x13 (mirroring original case 0 at 0x413BCF).
         * Drag (type 7):  group = 0x13.
         * Others:         group = s_selected_track (direct schedule index). */
        {
            /* [#2a] Effective qualifying result. Normally the LIVE slot-0 result;
             * but if we got here AFTER a View Replay (which re-inits the race and
             * wipes slot 0's result — see frontend_postrace_snapshot_capture) and
             * the live result is now stale (slot 0 not finished), fall back to the
             * snapshot taken before the replay so QUIT still reaches the high-score
             * / name-entry flow with the GENUINE achieved time, not an empty table
             * that collapses to the main menu. Consumed here (one shot). Gated by
             * TD5RE_REPLAY_QUIT_FLOW. */
            int     eff_finished  = td5_game_slot_is_finished(0);
            int32_t eff_primary   = td5_game_get_result_primary(0);
            int32_t eff_secondary = td5_game_get_result_secondary(0);
            int32_t eff_best_lap  = td5_game_get_best_lap_time(0);
            if (replay_quit_flow_on() && s_pr_snap_valid && !eff_finished && s_pr_snap_finished) {
                TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: live result stale after replay — "
                          "using pre-replay snapshot (primary=%d secondary=%d best_lap=%d)",
                          (int)s_pr_snap_primary, (int)s_pr_snap_secondary, (int)s_pr_snap_best_lap);
                eff_finished  = s_pr_snap_finished;
                eff_primary   = s_pr_snap_primary;
                eff_secondary = s_pr_snap_secondary;
                eff_best_lap  = s_pr_snap_best_lap;
                if (s_post_race_top_speed == 0) s_post_race_top_speed = s_pr_snap_top;
                if (s_post_race_avg_speed == 0) s_post_race_avg_speed = s_pr_snap_avg;
            }
            s_pr_snap_valid = 0;   /* consumed (or not needed) — one shot per replay */

            int group_idx;
            if (s_selected_game_type == 7) {
                group_idx = 0x13;
            } else if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                group_idx = s_selected_game_type + 0x13;
            } else {
                group_idx = s_selected_track;
            }
            group_idx = (group_idx < 0) ? 0 : (group_idx >= 26 ? 25 : group_idx);

            /* [#2b] For a TD6 track, derive the score type from its genuine record
             * group (if any) instead of the clamped TD5 group: TIME for point-to-
             * point, LAP for circuit; the TD5 group_idx/grp is ignored. */
            const TD5_NpcGroup *grp;
            int group_type;
            if (s_postrace_td6_level > 0) {
                const TD5_NpcGroup *td6grp = td5_save_get_td6_record_group(s_postrace_td6_level);
                if (td6grp) group_type = td6grp->header & 3;
                else        group_type = frontend_track_is_circuit(s_selected_track) ? 1 : 0;
                s_postrace_td6_score_type = group_type;
                grp = NULL;                 /* never qualify against a TD5 group */
            } else {
                grp = td5_save_get_npc_group(group_idx);
                group_type = grp ? (grp->header & 3) : 0;
            }

            /* Derive player's score for this group type:
             * 0 = TIME  (primary metric = finish time ticks, lower is better)
             * 1 = LAP   (best lap time ticks, lower is better)
             * 2 = PTS   (secondary metric = points, higher is better)
             * [CONFIRMED @ 0x00413BCF-0x00413C5C] */
            s_post_race_score = 0;
            if (group_type == 0) {
                /* Time (primary finish metric) */
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    /* Cup: use s_results secondary lap time field
                     * [CONFIRMED @ 0x00413C0B]: DAT_0048d990 for cup types */
                    s_post_race_score = eff_secondary;
                } else {
                    s_post_race_score = eff_primary;
                }
            } else if (group_type == 1) {
                /* Lap time: best lap across all slots */
                s_post_race_score = eff_best_lap;
            } else if (group_type == 2) {
                /* Points: secondary metric */
                s_post_race_score = eff_secondary;
            }

            /* Qualification check: compare against worst entry (entries[4].score).
             * Time-based (types 0/1): score of 0 = not finished = disqualified.
             *   Qualifies if player_time < worst_time.
             * Points-based (type 2): qualifies if player_pts > worst_pts.
             * [CONFIRMED @ 0x00413C5E-0x00413C7E] */
            int qualifies = 0;
            if (s_postrace_td6_level > 0) {
                /* [#2b] TD6: qualify against the GENUINE record table — an empty/
                 * partial table always has room (so the very first run prompts for
                 * a name and becomes record #1), otherwise beat the worst of 5. No
                 * authored entries exist, so a TD6 run can never be blocked by fake
                 * names. */
                const TD5_NpcGroup *td6grp = td5_save_get_td6_record_group(s_postrace_td6_level);
                if (s_post_race_score != 0) {
                    if (!td6grp) {
                        qualifies = 1;                         /* first ever record */
                    } else if (td6grp->entries[4].name[0] == '\0') {
                        qualifies = 1;                         /* a slot is still free */
                    } else if (group_type < 2) {
                        qualifies = (s_post_race_score < td6grp->entries[4].score);
                    } else {
                        qualifies = (s_post_race_score > td6grp->entries[4].score);
                    }
                }
            } else if (s_post_race_score != 0 && grp != NULL) {
                int32_t worst = grp->entries[4].score;
                if (group_type < 2) {
                    /* time: lower is better; qualify if player < worst */
                    qualifies = (s_post_race_score < worst);
                } else {
                    /* points: higher is better; qualify if player > worst */
                    qualifies = (s_post_race_score > worst);
                }
            }

            /* 2P mode: player 2 result doesn't go into high score.
             * DQ (slot state not finished): no entry.
             * [CONFIRMED @ 0x00413C80-0x00413CA4]
             * [#2a] eff_finished is the live finish state, or the pre-replay
             * snapshot when a replay wiped the live result. */
            if (s_two_player_mode) qualifies = 0;
            if (!eff_finished) qualifies = 0;

            TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: group=%d type=%d score=%d qualifies=%d",
                      group_idx, group_type, (int)s_post_race_score, qualifies);

            /* DEV harness: force the name-prompt phase so the name-input widget
             * and the resulting high-score table can be viewed without finishing a
             * qualifying race. Two triggers:
             *   TD5RE_DEMO_NAMEENTRY=1  — frame-dump the name widget (score=1 stub).
             *   TD5RE_INJECT_POSTRACE=1 — the post-race preview harness; the result
             *     data was fabricated by td5_game_inject_demo_results, so
             *     s_post_race_score is already a realistic value (~1:00) here and the
             *     full name-entry -> table flow renders real-looking columns.
             * Inert unless one of the env vars is set. */
            {
                static int s_demo_ne_init = 0, s_demo_ne = 0;
                if (!s_demo_ne_init) { s_demo_ne_init = 1;
                    const char *e = getenv("TD5RE_DEMO_NAMEENTRY");
                    const char *p = getenv("TD5RE_INJECT_POSTRACE");
                    s_demo_ne = ((e && e[0] && e[0] != '0') || (p && p[0] && p[0] != '0')); }
                if (s_demo_ne) { qualifies = 1; if (s_post_race_score == 0) s_post_race_score = 1; }
            }

            if (!qualifies) {
                /* Skip name entry — go straight to table insert (no name prompt) */
                s_post_race_score = 0;
                s_inner_state = 4;
                break;
            }
        }

        /* Player qualifies: prompt for name */
        memset(s_post_race_name, 0, sizeof(s_post_race_name));
        strcpy(s_post_race_name, "PLAYER");
        frontend_begin_text_input(s_post_race_name, (int)sizeof(s_post_race_name));
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            s_anim_tick = 0;
            s_inner_state = 2;
        }
        break;

    case 2: /* Text input active — process keys here; widget DRAWN in render path
             * (td5_frontend_render_ui_rects NAME_ENTRY dispatch). */
        frontend_handle_text_input_key();
        if (frontend_text_input_confirmed()) {
            /* Copy entered name, or fallback to default */
            frontend_play_sfx(5);
            s_inner_state = 3;
        }
        break;

    case 3: /* Slide-out of input: 32 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x10) {
            s_anim_tick = 0;
            s_inner_state = 4;
        }
        break;

    case 4: /* Insert score into table */
        /* [FIX 2026-06-07] Close the name-entry text field now that the input
         * phase (cases 1-3) is over. frontend_commit_text_input left
         * s_text_input_state = 2, and it otherwise stays non-zero until the next
         * set_screen — which suppresses the keyboard WM_KEYDOWN nav FIFO drain
         * (frontend_poll_input: `if (s_text_input_state == 0)`). With the field
         * still "open", the OK button's keyboard Enter in the table phase (case
         * 10) was flushed instead of processed, so Enter never set s_input_ready
         * and the user could not leave the screen (gamepad Enter worked because it
         * uses the un-gated edge path). Clearing it here re-enables keyboard nav
         * for cases 5-12. Harmless on the no-qualify path (already 0). */
        s_text_input_state = 0;
        /* [CONFIRMED @ 0x00413CB0 case 4] ScreenPostRaceNameEntry:
         * 1. Scan entries[0..4].score to find insert position (uVar8).
         *    - Types 0/1: find first entry where player_score <= entry.score (insert before)
         *    - Type 2:    find first entry where player_score >= entry.score (insert before)
         * 2. Shift entries[uVar8..3] down one slot (memmove-style).
         * 3. Write entry at position uVar8: name, score, car_id, avg_speed, top_speed.
         * 4. s_score_insert_pos = uVar8.
         */
        if (s_postrace_td6_level > 0 && s_post_race_score != 0) {
            /* [#2b] TD6 track: insert into the GENUINE TD6 record store (td5_save),
             * NOT a clamped TD5 NPC group — so no fake names are ever written and
             * the record persists per TD6 level. The store handles sorting +
             * qualification + persistence. The high-score overlay reads the records
             * via s_postrace_td6_level; s_score_category_index only drives the
             * nav-bar TITLE, so point it at the raced TD6 track for the right name. */
            int avg = s_post_race_avg_speed, top = s_post_race_top_speed;
            int ins_pos = td5_save_td6_record_insert(
                s_postrace_td6_level, s_postrace_td6_score_type,
                s_post_race_name, s_post_race_score,
                (int)(uint8_t)s_selected_car, avg, top);
            s_score_insert_pos = ins_pos;            /* -1 if it didn't fit */
            s_score_category_index = s_selected_track;  /* nav-bar title only */
            s_anim_complete = 1;                     /* unblock the table render */
            TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: TD6 record insert '%s' score=%d "
                      "level=%d -> rank=%d", s_post_race_name, (int)s_post_race_score,
                      s_postrace_td6_level, ins_pos);
        } else if (s_post_race_score != 0) {
            /* Determine group and type — mirrors case 0 logic */
            int ins_group;
            if (s_selected_game_type == 7) {
                ins_group = 0x13;
            } else if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                ins_group = s_selected_game_type + 0x13;
            } else {
                ins_group = s_selected_track;
            }
            ins_group = (ins_group < 0) ? 0 : (ins_group >= 26 ? 25 : ins_group);

            TD5_NpcGroup *grp = td5_save_get_npc_group_mutable(ins_group);
            if (grp != NULL) {
                int group_type = grp->header & 3;

                /* Find insert position [CONFIRMED @ 0x00413CB5-0x00413CDA] */
                int ins_pos = 5; /* default: past end (discard) */
                for (int k = 0; k < 5; k++) {
                    if (group_type < 2) {
                        /* Time: lower is better; insert where player < entry */
                        if (s_post_race_score <= grp->entries[k].score) {
                            ins_pos = k; break;
                        }
                    } else {
                        /* Points: higher is better; insert where player >= entry */
                        if (s_post_race_score >= grp->entries[k].score) {
                            ins_pos = k; break;
                        }
                    }
                }

                if (ins_pos < 5) {
                    /* Shift entries[ins_pos..3] down one slot [CONFIRMED @ 0x00413CDB-0x00413D1B] */
                    for (int k = 3; k >= ins_pos; k--) {
                        grp->entries[k + 1] = grp->entries[k];
                    }

                    /* Write new entry [CONFIRMED @ 0x00413D1C-0x00413D71]:
                     * name (13 bytes), score, car_id, avg_speed, top_speed */
                    TD5_NpcEntry *e = &grp->entries[ins_pos];
                    memset(e, 0, sizeof(*e));
                    strncpy(e->name, s_post_race_name, sizeof(e->name) - 1);
                    e->score = s_post_race_score;
                    e->car_id = (int32_t)(uint8_t)s_selected_car;

                    /* Average and top speed.
                     * Non-cup: direct from slot 0 metrics.
                     * Cup: avg_speed = total / race count [CONFIRMED @ 0x00413D55-0x00413D70] */
                    if (s_selected_game_type < 1 || s_selected_game_type == 7) {
                        e->avg_speed = s_post_race_avg_speed;
                        e->top_speed = s_post_race_top_speed;
                    } else {
                        int race_count = (s_race_within_series > 0) ? s_race_within_series : 1;
                        e->avg_speed = s_post_race_avg_speed / race_count;
                        e->top_speed = s_post_race_top_speed;
                    }
                    s_score_insert_pos = ins_pos;
                    /* Point the shared score-overlay at the just-inserted
                     * group so the post-submit display (cases 5-12) renders
                     * the user's new entry. Without this the overlay would
                     * show whatever group was last viewed in the Records
                     * screen (initialized to 0 at HIGH_SCORE entry). */
                    s_score_category_index = ins_group;
                    /* Unblock the overlay's `!s_anim_complete` early-return so
                     * NAME_ENTRY cases 6+ can render the high-score table.
                     * Without this the screen stays blank between insert and
                     * slide-out (user-reported 2026-05-26). */
                    s_anim_complete = 1;

                    TD5_LOG_I(LOG_TAG,
                              "PostRaceNameEntry: inserted '%s' score=%d at pos=%d in group=%d",
                              e->name, (int)e->score, ins_pos, ins_group);
                } else {
                    TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: score=%d doesn't fit (insert_pos=%d)",
                              (int)s_post_race_score, ins_pos);
                }
            }
        } else {
            TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: case 4 skip (score=0, no qualification)");
        }

        /* [#2b] TD6 tracks always render the records table (genuine records or the
         * "NO RECORDS YET" empty state), even when the player set no qualifying
         * time — so unblock the overlay and clear any stale highlight. The overlay
         * reads the records via s_postrace_td6_level; s_score_category_index only
         * drives the nav-bar title (the raced TD6 track's name). */
        if (s_postrace_td6_level > 0) {
            if (s_post_race_score == 0) s_score_insert_pos = -1;
            s_score_category_index = s_selected_track;   /* nav-bar title only */
            s_anim_complete = 1;
        }

        /* [FIX 2026-06-07] Create the table-phase buttons. The original
         * ScreenPostRaceNameEntry @0x00413BC0 case 4 creates the SAME two
         * buttons as ScreenPostRaceHighScoreTable @0x00413580 immediately after
         * inserting the score, then forces g_frontendButtonIndex =
         * g_frontendEscKeyButtonIndex = 1 [CONFIRMED @ 0x00413BC0]:
         *   CreateFrontendDisplayModeButton(NULL,         -0x208,0, 0x208,0x20, 0)  // nav bar, NO label
         *   CreateFrontendDisplayModeButton(SNK_OkButTxt, -0x130,0, 0x60, 0x20, 0)  // OK
         * The port previously created NO button in this table-display phase, so
         * the post-name-entry High Scores table rendered with no visible button
         * to leave the screen (user-reported 2026-06-07). Positions mirror the
         * resolved sentinels used by Screen_PostRaceHighScore: (115,97) 520x32
         * and (115,377) 96x32. The nav bar is a label-less empty 9-slice frame
         * (RebuildFrontendButtonSurface @0x00426120 draws NO text for a NULL
         * caption [CONFIRMED]) and this table is STATIC — unlike the Records
         * screen [23] it does NOT browse categories with L/R [CONFIRMED @
         * 0x00413BC0], so no nav-bar text/arrows are drawn (see the
         * HIGH_SCORE-gated nav-text block in the render path). Buttons survive
         * cases 5-12 (no set_screen until case 12); the list is empty here
         * (set_screen reset it on entry; text input creates no button), so the
         * nav bar lands at index 0 and OK at index 1. */
        frontend_create_button(NULL, 115, 97, 520, 32);          /* nav bar (empty frame) */
        frontend_create_button(SNK_OkButTxt, 115, 377, 96, 32);  /* OK button */
        s_selected_button = 1;   /* pre-select OK (orig forces g_frontendButtonIndex=1) */
        TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: created nav-bar + OK buttons for table phase (OK preselected)");
        s_inner_state = 5;
        break;

    case 5: case 6: /* Present, draw score table */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 7: /* Score table slide-in: 39 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x12) {
            s_inner_state = 8;
        }
        break;

    case 8: case 9: /* Static display (2 frames) */
        s_inner_state = 10;
        break;

    case 10: /* Interactive: auto-select OK, wait for confirm */
        if (s_input_ready) {
            s_inner_state = 11;
        }
        break;

    case 11: /* Prep slide-out */
        s_anim_tick = 0;
        s_inner_state = 12;
        break;

    case 12: /* Slide-out: 16 frames */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 16) {
            /* Persist high-score table to Config.td5.
             * [CONFIRMED @ 0x413BC0 case 4]: original writes into g_npcRacerGroupTable
             * (part of the Config block serialized by WritePackedConfigTd5 @ 0x40F8D0).
             * Port's case 4 already updated the in-memory NpcGroup; we flush here so
             * the entry survives across sessions. */
            TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: persisting high score to Config.td5");
            td5_save_write_config(NULL);

            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

void Screen_CupFailed(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        /* Only for cup types 1-6 */
        if (s_selected_game_type < 1 || s_selected_game_type > 6) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }

        frontend_init_return_screen(TD5_SCREEN_CUP_FAILED);
        TD5_LOG_D(LOG_TAG, "CupFailed: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198x0x70 (408x112) rendered live in
         * frontend_render_cup_failed_overlay (called from render_ui_rects).
         * Original: CreateTrackedFrontendSurface(0x198,0x70) @ DAT_0049628c,
         * then DrawFrontendLocalizedStringToSurface x4 for:
         *   SNK_SorryTxt     y=0x00 ("SORRY")           [CONFIRMED Language.dll]
         *   SNK_YouFailedTxt y=0x1c ("YOU FAILED")       [CONFIRMED Language.dll]
         *   SNK_ToWinTxt     y=0x38 ("TO WIN")           [CONFIRMED Language.dll]
         *   SNK_RaceTypeText y=0x54 ([cup type name])    [CONFIRMED @ 0x4237F0]
         * [CONFIRMED @ ScreenCupFailedDialog 0x004237F0] */
        /* [FIXED 2026-06-02, decomp @0x4237F0] OK rests at (296,289) — slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x31) = (296, 289). 408x112 panel @(152,97). */
        frontend_create_button(SNK_OkButTxt, 296, 289, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x4237F0 case 4: anim==0x20 exit] */
        s_anim_tick += s_fe_logic_ticks;
        /* Dialog slides from right (24px/frame), button from left */
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm */
        if (s_input_ready) {
            /* Release surfaces, go to return screen (typically main menu) */
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

void Screen_CupWon(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        if (s_selected_game_type < 1 || s_selected_game_type > 6) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }

        frontend_init_return_screen(TD5_SCREEN_CUP_WON);
        TD5_LOG_D(LOG_TAG, "CupWon: init -- deleting CupData.td5");
        frontend_delete_cup_data();

        /* Apply cup unlock progression and save to Config.td5.
         * Original AwardCupCompletionUnlocks checks companion_state_2 == 1 (player
         * finished, not DNF) before proceeding. [CONFIRMED @ 0x00421da0]
         *
         * Lifetime contract: s_cup_won_car_count / s_cup_won_track_count are
         * latched HERE in state 0 (or left at zero on DNF) and read by
         * frontend_render_cup_won_overlay during states 4-5 (slide-in + wait).
         * They persist via static-zero-init until the next Screen_CupWon
         * entry; td5_frontend_set_screen(MAIN_MENU) in state 5 leaves them
         * stale, but no other screen reads them so the staleness is inert.
         * [CONFIRMED @ 0x00423A80]: DAT_00494bb0 = car count, DAT_00494bb4 = track count. */
        s_cup_won_car_count   = 0;
        s_cup_won_track_count = 0;
        {
            int new_unlocks = 0;
            if (td5_game_slot_is_finished(0)) {
                new_unlocks = td5_save_apply_cup_unlocks_ex((int)s_selected_game_type,
                                                            &s_cup_won_car_count,
                                                            &s_cup_won_track_count);
            }
            TD5_LOG_I(LOG_TAG, "CupWon: game_type=%d finished=%d new_unlocks=%d cars=%d tracks=%d",
                      (int)s_selected_game_type, td5_game_slot_is_finished(0),
                      new_unlocks, s_cup_won_car_count, s_cup_won_track_count);

            /* Persist updated unlock state */
            td5_save_write_config(NULL);

            /* Refresh frontend lock tables from save system */
            td5_save_get_car_lock_table(s_car_lock_table, TD5_BASE_CAR_COUNT);
            td5_save_get_track_lock_table(s_track_lock_table, 26);
            { int td6s; for (td6s = 26; td6s <= 36; td6s++) s_track_lock_table[td6s] = 0; } /* TD6 tracks always available */
            if (td5_save_get_all_cars_unlocked()) {
                s_total_unlocked_cars = 37;
            } else {
                s_total_unlocked_cars = td5_save_get_max_unlocked_car();
                if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21;
            }
            {
                int t;
                s_total_unlocked_tracks = 20;
                for (t = 20; t < 37; t++) {
                    if (s_track_lock_table[t] == 0)
                        s_total_unlocked_tracks = t + 1;
                }
            }
        }

        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198×0xC4 (408×196) rendered live in frontend_render_cup_won_overlay.
         * [CONFIRMED @ 0x00423AEB/AF0]: CreateTrackedFrontendSurface(0x198, 0xC4) */
        /* [FIXED 2026-06-02, decomp @0x423A80] OK rests at (296,337) — it slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x61) = (296, 337); the taller
         * 408x196 panel pushes it below the 408x112 dialogs' (296,289). Port auto-layout didn't match. */
        frontend_create_button(SNK_OkButTxt, 296, 337, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x00423D35: anim==0x20 exit, +1/frame] */
        s_anim_tick += s_fe_logic_ticks;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm */
        if (s_input_ready) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}
