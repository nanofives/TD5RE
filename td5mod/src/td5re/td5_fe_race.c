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
static void mp_simul_cycle_paint(int p, int step);
static void frontend_mp_simul_carsel_init(void);
static void mp_simul_back_to_lobby(int n);
static void frontend_mp_simul_carsel_update(void);
static void mp_setup_name_append(int p, char c);
static void mp_setup_name_backspace(int p);
static int mp_repeat_fire(int p, uint32_t held, uint32_t edge, uint32_t now);
static void frontend_mp_setup_init(void);
static void frontend_mp_setup_update(void);
static int frontend_track_is_circuit(int track_slot);
static void frontend_update_laps_button_visibility(int laps_btn_idx);
static void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label);

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
    int track_max = s_network_active ? 0x13 : s_total_unlocked_tracks; /* exclusive bound */
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
    case 0: /* Init: validate indices, create the 7-row improved layout */
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
        frontend_create_button(SNK_OkButTxt,           QR_COL_X,       QR_ROW_Y(6),  96, 32); /* QR_BTN_OK */
        frontend_create_button(SNK_BackButTxt,         QR_COL_X + 108, QR_ROW_Y(6), 112, 32); /* QR_BTN_BACK */

        /* Reset direction to Forwards on entry (matches TrackSelection); hide the
         * toggle on forward-only/circuit tracks (caption stays "Direction" —
         * manage_label=0). Clamp the player/opponent counts. */
        s_track_direction = 0;
        frontend_update_direction_button_visibility(QR_BTN_DIRECTION, 0);
        /* Hide the Laps row on point-to-point tracks (no laps); show on circuits. */
        frontend_update_laps_button_visibility(QR_BTN_LAPS);
        frontend_quickrace_clamp_counts();

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
 * palette; TD5 cars cycle their 4 paint schemes (no-op for paintless cars). */
static void mp_simul_cycle_paint(int p, int step) {
    int car = s_mp_player_car[p];
    if (frontend_car_paintable(car)) {
        int idx = s_mp_player_color_idx[p] + step;
        if (idx < 0) idx = TD6_PALETTE_N - 1;
        if (idx >= TD6_PALETTE_N) idx = 0;
        s_mp_player_color_idx[p] = idx;
        s_mp_player_color[p]     = (int)s_td6_palette[idx];
        frontend_play_sfx(2);
    } else if (!frontend_car_is_td6(car) && frontend_car_has_paint(car)) {
        int pa = s_mp_player_paint[p] + step;
        if (pa < 0) pa = 3;
        if (pa > 3) pa = 0;
        s_mp_player_paint[p] = pa;
        mp_simul_refresh_pane(p);
        frontend_play_sfx(2);
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
        int car = s_mp_player_car[p];
        if (car < s_car_roster_min) car = s_car_roster_min;
        if (car > s_car_roster_max) car = s_car_roster_max;
        if (!frontend_car_selectable(car))
            car = frontend_car_cycle_step(car, 1, s_car_roster_min, s_car_roster_max);
        s_mp_player_car[p]       = car;
        s_mp_player_ready[p]     = 0;
        s_mp_player_color_idx[p] = p % TD6_PALETTE_N;          /* distinct default colour */
        s_mp_player_color[p]     = (int)s_td6_palette[s_mp_player_color_idx[p]];
        s_mp_player_trans[p]     = 0;                          /* Automatic by default */
        s_mp_pane_btn[p]         = MP_BTN_CAR;
        s_mp_pane_substate[p]    = 0;
        s_mp_pane_spec_car[p]    = -1;
        s_mp_pane_preview[p]     = 0;
        s_mp_pane_overlay[p]     = 0;
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
            int left = (edge & 1) != 0, right = (edge & 2) != 0, act = (edge & 0x10) != 0;
            switch (s_mp_pane_btn[p]) {
            case MP_BTN_CAR:
                if (left || right) {
                    car = frontend_car_cycle_step(car, right ? +1 : -1, s_car_roster_min, s_car_roster_max);
                    s_mp_player_car[p]   = car;
                    s_mp_player_paint[p] = 0;   /* reset paint on car change (matches single-player) */
                    mp_simul_refresh_pane(p);
                    frontend_play_sfx(5);
                }
                break;
            case MP_BTN_PAINT:
                if (left || right) mp_simul_cycle_paint(p, right ? +1 : -1);
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

    for (p = 0; p < n; p++) {
        if (s_mp_player_accent[p] == 0)
            s_mp_player_accent[p] = (int)(k_mp_player_colors[p % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu);
        s_mp_player_ready[p]  = 0;
        s_mp_setup_sub[p]     = 0;
        s_mp_setup_btn[p]     = MP_SET_NAME;
        s_mp_kbd_col[p]       = 0;
        s_mp_kbd_row[p]       = 0;
        s_mp_col_col[p]       = 0;
        s_mp_col_row[p]       = 0;
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

static void frontend_mp_setup_update(void) {
    int p, n = s_num_human_players;
    int all_ready = 1, want_back = 0;
    uint32_t now = td5_plat_time_ms();
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;

    if (s_inner_state == 0x20) {   /* slide-in animation */
        for (p = 0; p < n; p++) s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
        frontend_check_escape();
        if (now - s_mp_simul_anim_ms >= MP_SIMUL_ANIM_MS) s_inner_state = 0x21;
        return;
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
                if (edge & 1) { if (col > 0) col--; frontend_play_sfx(2); }
                if (edge & 2) { if (col < rowlen - 1) col++; frontend_play_sfx(2); }
                if (edge & 4) { if (row > 0) row--; frontend_play_sfx(2); }
                if (edge & 8) { if (row < MP_KBD_ROWS - 1) row++; frontend_play_sfx(2); }
                rowlen = (row < MP_KBD_LETTER_ROWS) ? (int)strlen(k_mp_kbd_rows[row]) : 3;
                if (col > rowlen - 1) col = rowlen - 1;
                s_mp_kbd_row[p] = row; s_mp_kbd_col[p] = col;
                if (edge & 0x10) {
                    if (row < MP_KBD_LETTER_ROWS) { mp_setup_name_append(p, k_mp_kbd_rows[row][col]); frontend_play_sfx(2); }
                    else if (col == 0) { mp_setup_name_append(p, ' '); frontend_play_sfx(2); }
                    else if (col == 1) { mp_setup_name_backspace(p); frontend_play_sfx(2); }
                    else { s_mp_setup_sub[p] = 0; frontend_play_sfx(3); }   /* DONE */
                }
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

        if (s_mp_player_ready[p]) {
            if (edge & 0x10) { s_mp_player_ready[p] = 0; s_mp_simul_ready_ms = 0; frontend_play_sfx(5); }
            if (edge & 0x20) want_back = 1;
            continue;
        }

        /* idle: navigate NAME / COLOUR / OK */
        if (edge & 4) { s_mp_setup_btn[p] = (s_mp_setup_btn[p] + MP_SET_COUNT - 1) % MP_SET_COUNT; frontend_play_sfx(2); }
        if (edge & 8) { s_mp_setup_btn[p] = (s_mp_setup_btn[p] + 1) % MP_SET_COUNT; frontend_play_sfx(2); }
        if (edge & 0x10) {
            if (s_mp_setup_btn[p] == MP_SET_NAME)   { s_mp_setup_sub[p] = 1; if (isk) td5_plat_input_flush_chars(); frontend_play_sfx(3); }
            else if (s_mp_setup_btn[p] == MP_SET_COLOUR) { s_mp_setup_sub[p] = 2; frontend_play_sfx(3); }
            else { s_mp_player_ready[p] = 1; frontend_play_sfx(3); }
        }
        if (edge & 0x20) want_back = 1;
    }

    if (frontend_check_escape()) {
        /* A keyboard player mid name-entry: ESC just leaves the field. Otherwise
         * ESC backs the whole setup out to the lobby. */
        int handled = 0;
        for (p = 0; p < n; p++)
            if (s_mp_join_device[p] == 0 && s_mp_setup_sub[p] == 1) { s_mp_setup_sub[p] = 0; handled = 1; }
        if (!handled) want_back = 1;
    }
    if (want_back) { mp_simul_back_to_lobby(n); return; }

    for (p = 0; p < n; p++)
        if (!s_mp_player_ready[p]) { all_ready = 0; break; }

    if (all_ready) {
        if (s_mp_simul_ready_ms == 0) s_mp_simul_ready_ms = now;
        if (now - s_mp_simul_ready_ms >= 500u) {
            /* Advance to the car-select grid (phase 1). */
            s_mp_phase = 1;
            s_mp_simul_ready_ms = 0;
            s_inner_state = 0;        /* intercept will run carsel_init next frame */
            TD5_LOG_I(LOG_TAG, "MP setup: all %d ready -> car select", n);
            return;
        }
    } else {
        s_mp_simul_ready_ms = 0;
    }
}

void Screen_CarSelection(void) {
    /* Multiplayer simultaneous flow (2+ humans) takes over this screen with its
     * own per-pane panes + per-device input: phase 0 = name/colour setup window,
     * phase 1 = the car-select grid. Single-player / classic-2P / network fall
     * through to the original state machine below. */
    if (s_mp_flow && s_num_human_players >= 2) {
        if (!s_mp_simul) { s_mp_simul = 1; s_mp_phase = 0; s_inner_state = 0; }
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
            case 0: /* Car: L/R arrows cycle car index */
                if (delta != 0) {
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
                     * scheme to 0 on EVERY car change. The color panel stays open
                     * across TD6->TD6 changes; it auto-hides after the switch if the
                     * new car is a TD5 car (see the post-switch check). */
                    s_selected_paint = 0;
                    s_selected_config = 0;
                    /* s_paint_active is NOT cleared on car change: the chosen
                     * paint colour carries over to the next car (and survives a
                     * race) — it's a single remembered colour for all TD6 cars. */
                    s_inner_state = 10; /* trigger new car image load */
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
                } else if (delta != 0) {
                    /* TD5: cycle paint 0-3 (disabled for cop cars 0x1C-0x24). */
                    int actual_car = (s_selected_game_type == 5) ?
                                     s_masters_roster[s_selected_car] : s_selected_car;
                    if (actual_car < 0x1C || actual_car > 0x24) {
                        s_selected_paint += delta;
                        if (s_selected_paint < 0) s_selected_paint = 3;
                        if (s_selected_paint > 3) s_selected_paint = 0;
                        s_inner_state = 10; /* re-render */
                    }
                }
                break;

            case 2: /* Stats: ◄► cycles wheel/config scheme 0..3; press opens spec sheet.
                     * [CONFIRMED @ 0x40DFC0 case 7 g_frontendButtonIndex==2: arrow cycles
                     * g_carSelectWheelSchemeTransient 0..3 wrap; press enters state 0xf.] */
                if (delta != 0) {
                    s_selected_config += delta;
                    if (s_selected_config < 0) s_selected_config = 3;
                    if (s_selected_config > 3) s_selected_config = 0;
                    s_inner_state = 10; /* re-render preview with new scheme */
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

        /* Validate track index for cup modes: skip locked/invalid NPC groups */
        /* Determine track max for current mode */
        if (s_network_active) {
            s_track_max = 18; /* 19 tracks total */
        } else if (s_two_player_mode) {
            s_track_max = s_total_unlocked_tracks;
        } else {
            s_track_max = s_total_unlocked_tracks;
        }
        if (s_selected_track >= s_track_max) s_selected_track = (s_two_player_mode ? -1 : 0);
        if (!s_two_player_mode && s_selected_track < 0) s_selected_track = 0;
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
        frontend_create_button(SNK_OkButTxt,        120, 377,  96, 32); /* 6: OK */
        /* Quick Race mode: no Back button */
        if (s_flow_context != 2) {
            frontend_create_button(SNK_BackButTxt, 232, 377, 112, 32);  /* 7: Back */
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
            if (selected_button == 0 && delta != 0) {
                /* Cycle track index, skipping tracks whose level zips are absent */
                if (s_network_active) {
                    frontend_cycle_track(delta, 0, s_track_max + 1);
                } else if (s_two_player_mode) {
                    /* 2P supports -1 = random, handled separately */
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

            /* [PORT ENHANCEMENT 2026-06] race-option rows (AI/laps/traffic/police). */
            if (delta != 0 && selected_button >= 2 && selected_button <= 5) {
                if (selected_button == 2) {            /* AI opponents */
                    s_num_ai_opponents += delta;
                    if (s_num_ai_opponents < 0) s_num_ai_opponents = 0;
                    if (s_num_ai_opponents > TD5_MAX_RACER_SLOTS - 1)
                        s_num_ai_opponents = TD5_MAX_RACER_SLOTS - 1;
                } else if (selected_button == 3 && !s_buttons[3].hidden) {  /* laps (value+1) */
                    s_game_option_laps += delta;
                    if (s_game_option_laps < 0) s_game_option_laps = 0;
                    if (s_game_option_laps > 9) s_game_option_laps = 9;
                } else if (selected_button == 4) {     /* traffic volume Off/Light/Normal/Heavy */
                    s_game_option_traffic = (s_game_option_traffic + delta) & 3;
                } else if (selected_button == 5) {     /* police on/off */
                    s_game_option_cops = (s_game_option_cops + delta) & 1;
                }
                frontend_play_sfx(2);
            }

            if (s_button_index == 6) { /* OK */
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
                    g_td5.ini.traffic = s_game_option_traffic & 3;
                    td5_ini_persist_options();
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 6;
                }
            }

            if (s_button_index == 7) { /* Back */
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 6;
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

void Screen_RaceResults(void) {
    switch (s_inner_state) {
    case 0: /* Init & routing: sort results, create panel */
        frontend_init_return_screen(TD5_SCREEN_RACE_RESULTS);
        frontend_reset_buttons();
        TD5_LOG_I(LOG_TAG, "RaceResults: state 0 - init, game_type=%d",
                  s_selected_game_type);

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
        { int bi = frontend_create_button(NULL, 115, 97, 0x208, 0x20);
          if (bi >= 0) s_buttons[bi].is_selector = 1; }
        frontend_create_button(SNK_OkButTxt, 115, 377, 0x60, 0x20);
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present buffer, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
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
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x12) {
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
            /* [FIX 2026-06-05 results-nav] Only LEFT/RIGHT browse racer slots
             * (the horizontal panel slide). s_arrow_input is a BITMASK
             * (1=LEFT,2=RIGHT,4=UP,8=DOWN), so the old `!= 0` test fired on
             * UP/DOWN too, and `> 0` is true for any non-zero mask — so every
             * arrow (incl. LEFT) slid the panel RIGHT. UP/DOWN are already
             * consumed by the shared row-nav handler (frontend_update_input
             * moves the selection between the selector bar and OK vertically),
             * so here we react to the horizontal bits only and honour their
             * direction: RIGHT -> state 7 (exit right), LEFT -> state 9. */
            if (s_arrow_input & 2) {          /* RIGHT */
                s_results_panel_slide_dir = +1;
                s_anim_tick = 0;
                s_inner_state = 7;
                TD5_LOG_D(LOG_TAG,
                          "RaceResults state 6: RIGHT -> slide-out state 7");
                break;
            }
            if (s_arrow_input & 1) {          /* LEFT */
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
                * The title (DAT_00496358) maps to ResultsText.tga which is
                * cached persistently in s_title_tex_page[] and reused on
                * re-entry; releasing it here would force a re-decode on every
                * round-trip. So state 0xC collapses to button-table reset.
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

        /* Compute group index for the high-score table.
         * Cup types 1-6: group = game_type + 0x13 (mirroring original case 0 at 0x413BCF).
         * Drag (type 7):  group = 0x13.
         * Others:         group = s_selected_track (direct schedule index). */
        {
            int group_idx;
            if (s_selected_game_type == 7) {
                group_idx = 0x13;
            } else if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                group_idx = s_selected_game_type + 0x13;
            } else {
                group_idx = s_selected_track;
            }
            group_idx = (group_idx < 0) ? 0 : (group_idx >= 26 ? 25 : group_idx);

            const TD5_NpcGroup *grp = td5_save_get_npc_group(group_idx);
            int group_type = grp ? (grp->header & 3) : 0;

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
                    s_post_race_score = td5_game_get_result_secondary(0);
                } else {
                    s_post_race_score = td5_game_get_result_primary(0);
                }
            } else if (group_type == 1) {
                /* Lap time: best lap across all slots */
                s_post_race_score = td5_game_get_best_lap_time(0);
            } else if (group_type == 2) {
                /* Points: secondary metric */
                s_post_race_score = td5_game_get_result_secondary(0);
            }

            /* Qualification check: compare against worst entry (entries[4].score).
             * Time-based (types 0/1): score of 0 = not finished = disqualified.
             *   Qualifies if player_time < worst_time.
             * Points-based (type 2): qualifies if player_pts > worst_pts.
             * [CONFIRMED @ 0x00413C5E-0x00413C7E] */
            int qualifies = 0;
            if (s_post_race_score != 0 && grp != NULL) {
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
             * [CONFIRMED @ 0x00413C80-0x00413CA4] */
            if (s_two_player_mode) qualifies = 0;
            if (!td5_game_slot_is_finished(0)) qualifies = 0;

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
        if (s_post_race_score != 0) {
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
