/* ========================================================================
 * td5_fe_mp_setup.c -- Multiplayer setup: split layouts, car-select grid,
 *                      player-setup window
 *
 * Split out of td5_frontend.c (2026-07-02). Contents, in original order:
 *   - MP split-screen layout tables + layout resolve/commit (PORT ENH 2026-06)
 *   - Race schedule init (frontend_init_race_schedule)
 *   - Simultaneous multiplayer car select grid (panes, stats, buttons)
 *   - Player-setup window: per-pane NAME entry (QWERTY) + colour grid,
 *     position picker + setup render
 * The FE_MP_* layout literals moved to td5_frontend_internal.h (they were
 * duplicated in td5_fe_race.c with 'keep in sync' comments - now one copy).
 * Cross-TU seam: td5_frontend_internal.h.
 * ======================================================================== */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_track_registry.h"  /* custom-track registry: name/slot lookups + slot headroom */
#include "td5_game.h"
#include "td5_profile.h"
#include "td5_input.h"
#include "td5_physics.h"
#include "td5_carparam.h"      /* shared carparam field map (rebuilt MORE STATS panel) */
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_config.h"      /* shared TD5RE_* env-knob accessors */
#include "td5_sound.h"
#include "td5_hud.h"           /* per-viewport player-identity overlay (race) */
#include "td5re.h"
#include "td5_snk_strings.h"   /* byte-exact SNK_ labels baked from Language.dll */
#include "td5_credits.h"       /* SNK_CreditsText array + dev mugshot map (Extras scroll) */
#include "td5_vectorui.h"      /* public VectorUI surface (HUD reuses these primitives) */
#include "td5_font.h"          /* [S13] runtime TTF glyph cache (native menu text) */
#include "td5_version.h"       /* build identity (version / channel / date / git rev) */
#include "td5_changelog.h"     /* CHANGELOG screen content table (file-static here) */
#include "td5_pending.h"       /* PENDING TO TEST checklist (list/state/overlay) */
#include "deps/cjson/cJSON.h"  /* track-marker JSON (retired trak_markers*.dat) */
#include "td5_frontend_internal.h"

#define LOG_TAG "frontend"

/* ======== [split] QWERTY + MP_KBD layout constants (moved from td5_frontend.c) ======== */
/* On-screen QWERTY (pad name entry): 4 letter rows + a special row (SPACE/DEL/DONE). */
const char *const k_mp_kbd_rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
#define MP_KBD_SPECIAL     MP_KBD_LETTER_ROWS
#define MP_KBD_ROW_H       14.4f                       /* compact: ~20% over the cap height (9) */
#define MP_KBD_BLOCK_H     (MP_KBD_ROWS * MP_KBD_ROW_H)

/* ======== [split] MP layout tables + race schedule (moved verbatim) ======== */
/* ========================================================================
 * Multiplayer Options — split-screen layout tables (PORT ENHANCEMENT 2026-06)
 *
 * For N local human players, the selectable split layouts. The N players fill
 * the first N cells of a cols x rows grid (row-major); any remaining cells are
 * "missing" and get a (deferred) content selector. Per the design:
 *   1: single   2: L|R or U/D   3: L|R / U/D / 2x2(+1 empty)   4: 2x2
 *   5: 3x2 or 2x3 (+1 empty)    6: 3x2 or 2x3    7: 3x3 (+2 empty)
 *   8: 3x3 (+1 empty)           9: 3x3
 * ======================================================================== */

const MpSplitLayout *mp_split_layouts(int n, int *count)
{
    static const MpSplitLayout L1[]   = { {"SINGLE", 1, 1} };
    static const MpSplitLayout L2[]   = { {"LEFT / RIGHT", 2, 1}, {"UP / DOWN", 1, 2} };
    static const MpSplitLayout L3[]   = { {"LEFT / RIGHT", 3, 1}, {"UP / DOWN", 1, 3}, {"2X2 GRID", 2, 2} };
    static const MpSplitLayout L4[]   = { {"2X2 GRID", 2, 2} };
    static const MpSplitLayout L56[]  = { {"3X2 GRID", 3, 2}, {"2X3 GRID", 2, 3} };
    static const MpSplitLayout L789[] = { {"3X3 GRID", 3, 3} };
    switch (n) {
        case 1:  if (count) *count = 1; return L1;
        case 2:  if (count) *count = 2; return L2;
        case 3:  if (count) *count = 3; return L3;
        case 4:  if (count) *count = 1; return L4;
        case 5:
        case 6:  if (count) *count = 2; return L56;
        default: if (count) *count = 1; return L789;  /* 7,8,9 (callers clamp n<=9) */
    }
}

/* Resolve the active layout for (n, sel) → cols/rows + missing-cell count. */
void mp_resolve_layout(int n, int sel, int *cols, int *rows, int *missing)
{
    int cnt = 1;
    const MpSplitLayout *opts = mp_split_layouts(n, &cnt);
    int c, r, m;
    if (sel < 0 || sel >= cnt) sel = 0;
    c = opts[sel].cols;
    r = opts[sel].rows;
    m = c * r - n;
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    if (cols)    *cols = c;
    if (rows)    *rows = r;
    if (missing) *missing = m;
}

/* What to display in an empty split-screen cell. MAP and STANDINGS are rendered
 * into the empty pane by td5_fe_race.c (the split-screen filler-map / standings
 * paths); "EMPTY" leaves the pane blank. */
const char *const k_mp_missing_content[3] = { "EMPTY", "MAP", "STANDINGS" };

/* Commit the AI-spectate pane count + split grid for a race launch: clamps
 * the requested spectate count against the live AI field and the viewport
 * cap, stores it, and (re)resolves the split grid for humans+spectate panes.
 * Single source for the Quick Race menu path and the AutoRace SpectateScreens
 * override so the clamp/grid logic cannot drift. */
void frontend_commit_pane_layout(int eff_humans, int requested_spectate)
{
    int spectate = requested_spectate;
    if (spectate < 0) spectate = 0;
    if (spectate > g_td5.num_ai_opponents) spectate = g_td5.num_ai_opponents;
    if (spectate > TD5_MAX_VIEWPORTS - eff_humans) spectate = TD5_MAX_VIEWPORTS - eff_humans;
    if (spectate < 0) spectate = 0;
    g_td5.num_spectate_screens = spectate;

    int eff_panes = eff_humans + spectate;
    if (eff_panes < 1) eff_panes = 1;
    if (eff_panes > TD5_MAX_VIEWPORTS) eff_panes = TD5_MAX_VIEWPORTS;

    /* For >=2 panes split is on and the layout grid (cols x rows) overrides
     * the automatic ladder in td5_game_init_viewport_layout. split_screen_mode
     * keeps its legacy meaning for HUD / minimap / sound consumers: 0=single,
     * 2=two-player left|right, 1=any other split "on". */
    if (eff_panes >= 2) {
        int cols = 0, rows = 0, missing = 0;
        mp_resolve_layout(eff_panes, s_mp_layout_sel, &cols, &rows, &missing);
        g_td5.split_grid_cols = cols;
        g_td5.split_grid_rows = rows;
        g_td5.split_screen_mode = (eff_panes == 2 && cols == 2) ? 2 : 1;
        g_td5.split_missing_content[0] = (missing > 0) ? s_mp_missing_content[0] : 0;
        g_td5.split_missing_content[1] = (missing > 1) ? s_mp_missing_content[1] : 0;
    } else {
        g_td5.split_grid_cols = 0;
        g_td5.split_grid_rows = 0;
        g_td5.split_screen_mode = 0;
        g_td5.split_missing_content[0] = 0;
        g_td5.split_missing_content[1] = 0;
    }
}

void frontend_init_race_schedule(void) {
    int i;
    int slot_active[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_ext_id[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_variant[TD5_MAX_RACER_SLOTS] = {0};
    int start_slot = 1;
    int eff_humans = 1;   /* human-driven slots actually rendered/controlled (<=2) */

    /* A normal race entry (new race / Race Again / AutoRace) always RECORDS
     * fresh input. Clear any replay/playback/demo state left over from a prior
     * View Replay or attract demo. View Replay bypasses this function entirely
     * (it re-enters the same race without rebuilding the schedule), so its flags
     * survive; the attract-demo path re-sets the demo flag after this returns. */
    td5_input_set_replay_mode(0);
    td5_input_set_playback_active(0);
    td5_game_set_replay_mode(0);
    td5_game_set_demo_mode(0);

    /* [ATTRACT DEMO 2026-06-25] Force a clean single-player single-race context
     * for the attract demo BEFORE the cup / cop-chase / MP branches below read it.
     * The demo fires from the main menu, but a stale MP mode (e.g. backing out of a
     * cop-chase lobby and idling) would otherwise drag the demo into that mode. The
     * per-race randomisation (car/opponents/traffic) + forced no-police are applied
     * once the slot table is built (search "ATTRACT demo" below). */
    if (s_current_screen == TD5_SCREEN_ATTRACT_MODE) {
        g_td5.mp_mode_config.mode = TD5_MP_MODE_RACE;  /* regular single race            */
        g_td5.wanted_mode_enabled = 0;                 /* never a cop chase              */
        g_td5.reverse_direction   = 0;                 /* forwards                       */
        s_cup_user_active         = 0;                 /* not a user-cup series          */
        s_selected_game_type      = 0;                 /* single race -> default AI fill  */
        s_mp_flow                 = 0;
        s_two_player_mode         = 0;
        s_launching_net_race      = 0;
    }

#ifndef TD5RE_RELEASE
    /* [item #4] When launching from Quick Race, log the (dev) span-offset field's
     * committed value once. td5_game.c InitRace applies g_td5.ini.start_span_offset
     * per-slot (16-bit wrap) to both TD5 and TD6 tracks. */
    if (s_current_screen == TD5_SCREEN_QUICK_RACE)
        TD5_LOG_I(LOG_TAG, "QuickRace launch: start_span_offset=%d", g_td5.ini.start_span_offset);
#endif

    g_td5.race_requested = 1;
    g_td5.car_index   = frontend_current_car_index();
    g_td5.track_index = (s_current_screen == TD5_SCREEN_ATTRACT_MODE)
                        ? s_attract_track
                        : s_selected_track;

    /* [MP CUP 2026-06-22] Begin the series on the first cup race (seeds its track
     * list from the picked track) and force the current cup race's track. */
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_CUP) {
        if (!td5_game_mp_cup_active()) td5_game_mp_cup_begin();
        if (s_launching_net_race) {
            /* [NET GAME MODES 2026-07-04] The host drives cup progression: adopt
             * the broadcast cup race index (keeps standings + "race X of Y" in
             * lockstep across peers) and use the broadcast track for this race. */
            TD5_NetRaceConfig nc;
            if (td5_net_get_race_config(&nc)) {
                if (nc.cup_race_index >= 0)
                    td5_game_mp_cup_set_current(nc.cup_race_index);
                if (nc.track_index >= 0) { s_selected_track = nc.track_index; g_td5.track_index = nc.track_index; }
            }
        } else {
            int ct = td5_game_mp_cup_track();
            if (ct >= 0) { s_selected_track = ct; g_td5.track_index = ct; }
        }
    }

    /* [CUP TRACK SELECT 2026-06-25] Single-player cup with player-chosen tracks:
     * force the current race's chosen track + direction. Gated on s_cup_user_active
     * so the faithful auto-schedule cup path (picker off) is byte-unchanged. Covers
     * race 1 (from the picker commit) AND races 2..N (from the Next-Cup-Race advance,
     * which only bumps s_race_within_series and re-enters this function). */
    if (s_cup_user_active &&
        s_selected_game_type >= 1 && s_selected_game_type <= 6 &&
        g_td5.mp_mode_config.mode != TD5_MP_MODE_CUP) {
        int r = s_race_within_series;
        if (r >= 0 && r < s_cup_user_count) {
            int t = s_cup_user_tracks[r];
            if (t >= 0 && t != 99) {
                s_selected_track        = t;
                g_td5.track_index       = t;
                g_td5.reverse_direction = s_cup_user_dirs[r];
                TD5_LOG_I(LOG_TAG, "Cup race %d/%d -> chosen track=%d dir=%d",
                          r + 1, s_cup_user_count, t, s_cup_user_dirs[r]);
            }
        }
    }

    /* --- Local human count + split-screen layout (PORT ENHANCEMENT 2026-06) ---
     * s_num_human_players is the single source of truth, set by EITHER the Quick
     * Race "Players" selector OR the rebuilt Multiplayer Options "PLAYERS" button
     * (both edit the same static). The chosen layout (s_mp_layout_sel) resolves
     * to a cols x rows grid consumed by td5_game_init_viewport_layout. The
     * untouched default (1 human + 5 AI) stays byte-faithful to the legacy grid.
     * g_td5.network_active=0 ensures the local split-screen path is taken. */
    {
        int humans, ai;
        /* Human count: only the multiplayer lobby flow (s_mp_flow, sets
         * s_two_player_mode) runs >1 local human. Quick Race is single-player
         * (driven by the active controller); everything else is single-player. */
        if (s_launching_net_race) {
            /* S10: each synced network player occupies an input-reading racer
             * slot (slots 0..N-1 read s_control_bits, which the lockstep fills
             * with the host-merged input each frame); the remaining slots are AI
             * fill. NOTE: this currently produces N split-screen viewports
             * because it reuses the local split-screen slot model; a single
             * local-follow viewport for net play is a documented follow-up. */
            int np = td5_net_get_player_count();
            humans = (np > 0) ? np : 1;
        } else if (s_mp_flow && s_two_player_mode != 0)
            humans = (s_num_human_players > 1) ? s_num_human_players : 2;
        else
            humans = 1;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;

        /* [PORT ENHANCEMENT 2026-06] Single-player: the active menu controller
         * (whoever navigated here) becomes the driver = player 0's device. */
        if (!s_mp_flow) {
            td5_input_set_input_source(0, s_active_menu_device);
            td5_save_set_player_device_index(0, (uint32_t)s_active_menu_device);
        }

        /* AI count: the screens that expose an opponents selector (Quick Race +
         * the track selector race-option row) drive s_num_ai_opponents; other
         * flows use the legacy fill (cup game-types override it anyway).
         * [FIX 2026-06-05 race-again-opponent-count] "Race Again" re-enters this
         * from the RACE_RESULTS screen, which previously fell through to the
         * legacy 5-opponent fill regardless of the original field size. For a
         * Single/Quick Race (game_type 0) honour the snapshotted opponent count
         * (restored into s_num_ai_opponents above); cups (game_type != 0) keep
         * the legacy fill and override the count downstream. */
        if (s_current_screen == TD5_SCREEN_QUICK_RACE ||
            s_current_screen == TD5_SCREEN_TRACK_SELECTION ||
            (s_current_screen == TD5_SCREEN_RACE_RESULTS && s_selected_game_type == 0))
            ai = s_num_ai_opponents;
        else
            ai = TD5_LEGACY_RACE_SLOTS - humans;
        if (ai < 0) ai = 0;
        /* [MP COP CHASE AI COP 2026-06-23] An AI cop drives the first non-human
         * slot (td5_game_cop_chase_cop_slot), so guarantee at least one AI
         * opponent exists to be that cop — otherwise the cop slot would be empty
         * and player 1 would end up the cop. */
        if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE &&
            g_td5.mp_mode_config.cop_is_ai && ai < 1)
            ai = 1;
        if (ai > TD5_MAX_RACER_SLOTS - humans) ai = TD5_MAX_RACER_SLOTS - humans;

        /* [MP AI TEST PLAYERS 2026-06-25] Build the simulated-player mask from the
         * lobby's per-slot AI flags (only meaningful on the local split-screen MP
         * flow). When any AI test players exist they ARE the field — suppress the
         * legacy AI-opponent fill so the race is exactly the lobby roster
         * (1 human at the wheel + N AI-driven players, each in its own pane). */
        g_td5.mp_ai_player_mask = 0;
        if (s_mp_flow && s_two_player_mode != 0) {
            int p;
            for (p = 0; p < humans && p < TD5_MAX_HUMAN_PLAYERS; p++)
                if (s_mp_slot_is_ai[p]) g_td5.mp_ai_player_mask |= (1u << p);
        }
        if (g_td5.mp_ai_player_mask) ai = 0;

        /* [CUP TRACK SELECT 2026-06-25] MP cup: the AI opponent count comes from
         * the cup options (OPPONENTS), overriding the legacy fill / AI-test-player
         * suppression. These AI cars earn championship points alongside the humans
         * (td5_game_mp_cup_award tallies every active slot). */
        if (s_cup_user_active && g_td5.mp_mode_config.mode == TD5_MP_MODE_CUP) {
            int opp = g_td5.mp_mode_config.cup_ai_opponents;
            if (opp < 0) opp = 0;
            if (opp > TD5_MAX_RACER_SLOTS - humans) opp = TD5_MAX_RACER_SLOTS - humans;
            ai = opp;
            TD5_LOG_I(LOG_TAG, "MP cup: AI opponents=%d (humans=%d)", ai, humans);
        }

        g_td5.num_human_players = humans;
        g_td5.num_ai_opponents  = ai;
    }

    eff_humans = g_td5.num_human_players;
    if (eff_humans < 1) eff_humans = 1;
    if (eff_humans > TD5_MAX_VIEWPORTS) eff_humans = TD5_MAX_VIEWPORTS;

    /* [PORT ENHANCEMENT 2026-06-08] AI spectator split-screens (dev/profiling).
     * Quick Race may render the first N AI cars (slots 1..N) each in its own
     * viewport pane on top of the human pane(s); only the humans read input.
     * Inert outside Quick Race. Clamping + split-grid resolution live in
     * frontend_commit_pane_layout (shared with the AutoRace SpectateScreens
     * override so the two paths cannot drift). */
    /* [S31 net] A network race renders ONE full-screen view per machine
     * (each pinned to that machine's own car in InitRace step 17) — the
     * net players are racer SLOTS, not local split panes. */
    frontend_commit_pane_layout(s_launching_net_race ? 1 : eff_humans,
                                (s_current_screen == TD5_SCREEN_QUICK_RACE)
                                    ? s_num_spectate_screens : 0);
    int eff_panes = (s_launching_net_race ? 1 : eff_humans) + g_td5.num_spectate_screens;
    g_td5.network_active    = s_launching_net_race;   /* S10: net race engages lockstep */
    s_launching_net_race    = 0;                      /* one-shot intent */

    /* [FIX 2026-06-04 S05] Re-commit the Traffic / Police toggles at launch.
     * The Quick Race + Multiplayer track-select screen edits s_game_option_traffic
     * / s_game_option_cops AFTER ConfigureGameTypeFlags already ran (it's invoked
     * at game-type selection on the Main Menu), and the MULTIPLAYER lobby flow
     * never calls ConfigureGameTypeFlags at all — so the toggles were stale (or
     * ignored entirely) at race start. For the modes that expose the rows
     * (s_selected_game_type == 0 = Single Race / Quick Race / Multiplayer; cup
     * game-types 1..6 and the special modes 7/8/9 force their own values, so they
     * are left untouched) push the latest toggle state into the runtime gates
     * read by InitRace / td5_ai. Mirrors ConfigureGameTypeFlags @ 0x00410CA0
     * case 0. */
    if (s_selected_game_type == 0) {
        /* [dynamic-traffic] 5-state volume row: 0=Off 1=Low 2=Medium 3=High
         * 4=Very High. Clamp to 0..4 (NOT & 3, which would swallow Very High). */
        int tv = s_game_option_traffic;
        if (tv < 0) tv = 0;
        if (tv > TD5_TRAFFIC_VOLUME_COUNT - 1) tv = TD5_TRAFFIC_VOLUME_COUNT - 1;
        g_td5.traffic_volume            = tv;
        g_td5.traffic_enabled           = (g_td5.traffic_volume != 0);
        g_td5.special_encounter_enabled = s_game_option_cops;
    }

    /* [COP-CHASE 2026-06-21] Cop Chase / wanted mode never spawns the separate
     * traffic-police "encounter" cop — the player is already the pursuer. Force it
     * off at race launch regardless of any stale POLICE toggle (the row is hidden in
     * this mode; ConfigureGameTypeFlags case 8 already zeroes it — this is the
     * belt-and-suspenders chokepoint so no path can re-enable it). */
    if (g_td5.wanted_mode_enabled && g_td5.special_encounter_enabled) {
        g_td5.special_encounter_enabled = 0;
        TD5_LOG_I(LOG_TAG, "InitRaceSchedule: cop chase -> special-encounter (traffic police) forced OFF");
    }

    /* [CUP TRACK SELECT 2026-06-25] Per-race cup traffic + police: the picker
     * chose a traffic level and a police on/off for THIS race. Apply them now
     * (overriding the generic commit above) using the current cup race index
     * (MP cup: td5_game_mp_cup_current; SP cup: s_race_within_series). */
    if (s_cup_user_active) {
        int is_mpcup = (g_td5.mp_mode_config.mode == TD5_MP_MODE_CUP);
        int idx = is_mpcup ? td5_game_mp_cup_current() : s_race_within_series;
        if (idx < 0) idx = 0;
        if (idx < s_cup_user_count && idx <= TD5_CUP_MAX_RACES) {
            int tv = s_cup_user_traffic[idx];
            if (tv < 0) tv = 0;
            if (tv > TD5_TRAFFIC_VOLUME_COUNT - 1) tv = TD5_TRAFFIC_VOLUME_COUNT - 1;
            g_td5.traffic_volume            = tv;
            g_td5.traffic_enabled           = (tv != 0);
            g_td5.special_encounter_enabled = s_cup_user_cops[idx] ? 1 : 0;
            /* Per-race direction (forwards/backwards) and lap count (circuit). */
            g_td5.reverse_direction         = s_cup_user_dirs[idx] ? 1 : 0;
            g_td5.ini.laps                  = s_cup_user_laps[idx];
            TD5_LOG_I(LOG_TAG, "Cup race %d: per-race dir=%d laps=%d traffic=%d police=%d",
                      idx + 1, s_cup_user_dirs[idx], s_cup_user_laps[idx],
                      tv, s_cup_user_cops[idx]);
        }
    }

    TD5_LOG_I(LOG_TAG,
              "InitRaceSchedule: split=%d grid=%dx%d humans=%d opp=%d eff=%d spectate=%d panes=%d 2p=%d layout_sel=%d",
              g_td5.split_screen_mode, g_td5.split_grid_cols, g_td5.split_grid_rows,
              g_td5.num_human_players, g_td5.num_ai_opponents, eff_humans,
              g_td5.num_spectate_screens, eff_panes,
              s_two_player_mode, s_mp_layout_sel);

    /* Slot 0 = player, always active */
    slot_active[0]  = 1;
    slot_ext_id[0]  = s_selected_car;
    slot_variant[0] = s_selected_paint;

    /* [S31 NET 2026-06-10] Network race: identical grids everywhere. Fill the
     * net-player slots from the host-broadcast config and reseed the CRT with
     * the shared seed so the rand()-driven AI fill below picks the SAME cars
     * on every machine. Lockstep has no state correction -- a different
     * carparam on any slot is a permanent desync. */
    TD5_NetRaceConfig net_cfg;
    int net_cfg_valid = 0;
    if (g_td5.network_active) {
        if (td5_net_get_race_config(&net_cfg)) {
            int np = td5_net_get_player_count();
            net_cfg_valid = 1;
            if (np > TD5_MAX_RACER_SLOTS) np = TD5_MAX_RACER_SLOTS;
            for (i = 0; i < np && i < 6; i++) {
                slot_active[i]  = 1;
                slot_ext_id[i]  = (net_cfg.car_index[i] >= 0) ? net_cfg.car_index[i] : 0;
                slot_variant[i] = net_cfg.paint_index[i];
            }
            /* The AI fill below must not overwrite the net players' slots
             * (it used to start at slot 1 and stomp every client's car). */
            if (np > start_slot) start_slot = np;
            /* [S31] TD6 body colours ride the config too: the asset painter
             * otherwise colours slot 0 with the LOCAL machine's INI choice
             * and other humans with the AI hash -- every machine rendered
             * its own idea of the field ("client sees the same car twice,
             * host sees correctly"). */
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++)
                td5_asset_set_human_td6_color(
                    i, (i < np && i < 6) ? net_cfg.td6_color[i] : -1);
            /* [NET GAME MODES 2026-07-04] Colour each net player's results /
             * standings / podium row by their chosen body colour (falls back to
             * a distinct per-slot colour). mp_slot_color() reads
             * s_mp_player_accent[]; that array is indexed by LOCAL setup slot, so
             * over the net the local machine's own accent would otherwise mis-
             * colour net slot 0. Set it per net slot on every peer so the field
             * shows the same, distinct colour for each player everywhere. */
            for (i = 0; i < TD5_MAX_HUMAN_PLAYERS; i++) {
                uint32_t col = (i < np && i < 6 && net_cfg.td6_color[i] > 0 &&
                                (uint32_t)net_cfg.td6_color[i] != 0x00FFFFFFu)
                                   ? (uint32_t)net_cfg.td6_color[i] & 0x00FFFFFFu
                                   : k_mp_player_colors[i % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu;
                s_mp_player_accent[i] = (int)col;
            }
            for (i = 0; i < np && i < 6; i++)
                TD5_LOG_I(LOG_TAG,
                          "InitRaceSchedule: net slot%d car=%d paint=%d color=%06X",
                          i, net_cfg.car_index[i], net_cfg.paint_index[i],
                          (unsigned)net_cfg.td6_color[i]);
            /* Opponent count is host-authoritative: it decides how many racer
             * slots InitRace enables -- a mismatch is a different grid.
             * InitRace computes the field as humans(1) + num_ai_opponents, so
             * fold the EXTRA net players into the opponent count: the field
             * is np humans + the host-chosen AI cars. */
            /* [2026-06-19] The grid is sized num_human_players + num_ai_opponents.
             * For net, THIS machine has exactly ONE local human viewport; every
             * OTHER player is folded into the opponent count via (np-1) below. But
             * g_td5.num_human_players was set to `humans` (= the joined count, e.g.
             * 2) above, so the other humans were counted TWICE -> an extra phantom
             * AI car ("0 opponents selected but I see 1"; 3 cars for a 2-player
             * race). Force the local human count to 1 so the field is exactly
             * np humans + the host-chosen AI cars. */
            g_td5.num_human_players = 1;
            g_td5.num_ai_opponents = net_cfg.num_opponents + (np - 1);
            if (g_td5.num_ai_opponents > TD5_MAX_RACER_SLOTS - 1)
                g_td5.num_ai_opponents = TD5_MAX_RACER_SLOTS - 1;
            TD5_LOG_I(LOG_TAG,
                      "InitRaceSchedule: net config applied (np=%d opp=%d seed=0x%08X)",
                      np, net_cfg.num_opponents, net_cfg.rng_seed);
        } else {
            TD5_LOG_W(LOG_TAG,
                      "InitRaceSchedule: network race WITHOUT a host config");
        }
    }

    /* In-race per-viewport identity (coloured frame + name plate): cleared for
     * every race, populated below only for the multiplayer flow. */
    td5_hud_clear_player_identities();

    /* [PORT ENHANCEMENT 2026-06] Multiplayer lobby flow: each human slot uses the
     * car that player chose in the sequential car select. ([S31] skipped for
     * net races: a stale local flag would overwrite the replicated grid.) */
    if (s_mp_flow && !g_td5.network_active) {
        slot_ext_id[0]  = s_mp_player_car[0];
        slot_variant[0] = s_mp_player_paint[0];
        /* [MP CUP CAR CARRYOVER FIX 2026-06-28] Re-anchor slot 0's ACTUAL driven
         * car to player 1's MP pick. For a non-network race td5_game InitRace
         * (td5_game.c:2440) loads slot 0 from g_td5.car_index, NOT from
         * ai_car_indices[0]/slot_ext_id[0]. g_td5.car_index was set above (~3236)
         * from s_selected_car, but the MP-cup post-race "NEXT RACE" path restores
         * s_selected_car from the once-only SP Race-Again snapshot (s_snap_car,
         * td5_fe_race.c). If that snapshot was captured on a race BEFORE the cup
         * (s_results_rerace_flag is never re-armed on MP-cup entry — only
         * frontend_reset_sp_race_carryover does that, for SP), player 1 reverts to
         * the pre-cup car on cup race 2+ while slots 1+ (read from ai_car_indices)
         * stay correct. Pin both back to the live MP selection so the SP snapshot
         * can never leak into the local MP slot 0. Inert for the normal flow where
         * s_selected_car already equals s_mp_player_car[0]. */
        if (s_mp_player_car[0] >= 0 && s_mp_player_car[0] < TD5_CAR_COUNT) {
            if (g_td5.car_index != s_mp_player_car[0])
                TD5_LOG_I(LOG_TAG,
                          "MP slot0 car re-anchored to player1 pick %d (was car_index=%d) "
                          "[pre-cup snapshot leak guard]",
                          s_mp_player_car[0], g_td5.car_index);
            s_selected_car  = s_mp_player_car[0];
            g_td5.car_index = s_mp_player_car[0];
        }
        /* Each human slot is painted with that player's chosen TD6 colour (no-op
         * for TD5 cars, which have no carmask). -1 = leave the default. */
        td5_asset_set_human_td6_color(0, s_mp_player_color[0]);
        td5_hud_set_player_identity(0, s_mp_player_name[0], (uint32_t)s_mp_player_accent[0]);
        for (i = 1; i < eff_humans && i < TD5_MAX_RACER_SLOTS; i++) {
            slot_active[i]  = 1;
            slot_ext_id[i]  = s_mp_player_car[i];
            slot_variant[i] = s_mp_player_paint[i];
            td5_asset_set_human_td6_color(i, s_mp_player_color[i]);
            td5_hud_set_player_identity(i, s_mp_player_name[i], (uint32_t)s_mp_player_accent[i]);
            if (i + 1 > start_slot) start_slot = i + 1;
        }
        /* AI / unused slots get the hashed AI palette (clear any stale override). */
        for (; i < TD5_MAX_RACER_SLOTS; i++)
            td5_asset_set_human_td6_color(i, -1);

        /* [MP SESSION PERSISTENCE 2026-06] Bulk-snapshot the just-committed local
         * roster into the process-lifetime store so re-entering the MP menu in the
         * same session restores each player's name/accent/car/paint/color/trans.
         * Local flow only (net rosters are host-replicated and must not be mixed
         * with this frontend store). Gated by TD5RE_MP_SESSION. */
        if (mp_session_enabled()) {
            int humans = g_td5.num_human_players;
            int p;
            if (humans < 0) humans = 0;
            if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;
            for (p = 0; p < humans; p++)
                mp_session_save_player(p);
            s_mp_session.valid = 1;
            s_mp_session.count = humans;
            TD5_LOG_I(LOG_TAG, "MP session: saved roster (%d players)", humans);
        }
    }

    /* Two-player setup [CONFIRMED @ 0x0040daf0]:
     * Original gate: g_twoPlayerModeEnabled != 0 || g_selectedGameType == 7.
     * In the original, game_type 7 is DRAG RACE (user picks a 2nd car via the
     * 2-pass CarSelect loop). The port's convention uses game_type 9 for drag
     * race, so the constant is swapped here. Time Trials is solo and must NOT
     * fall into this branch. (Skipped for the N-way multiplayer lobby flow,
     * which already populated the human slots above.) */
    if ((s_two_player_mode || s_selected_game_type == 9) && !s_mp_flow &&
        !g_td5.network_active) {
        slot_active[1]  = 1;
        slot_ext_id[1]  = s_p2_car;
        slot_variant[1] = 0;
        start_slot = 2;
        TD5_LOG_I(LOG_TAG, "InitRaceSchedule: P2 slot1 ext_id=%d", s_p2_car);
    }

    /* Quick Race extra humans: slots 1..eff_humans-1 are human-driven. Until a
     * per-player car-select UI exists (the next infra step), the extra humans
     * default to the player's selected car. AI then fills from start_slot; the
     * remaining slots beyond (humans+opponents) are disabled in td5_game InitRace. */
    if (s_current_screen == TD5_SCREEN_QUICK_RACE) {
        for (i = 1; i < eff_humans && i < TD5_MAX_RACER_SLOTS; i++) {
            slot_active[i]  = 1;
            slot_ext_id[i]  = s_selected_car;
            slot_variant[i] = 0;
            if (i + 1 > start_slot) start_slot = i + 1;
        }
    }

    /* [ATTRACT DEMO 2026-06-25] Apply the random demo card for an attract-mode
     * launch. Gated on ATTRACT_MODE so EVERY other race path is byte-identical.
     * The track was already applied from s_attract_track above; here we override the
     * player car (slot 0), opponent count, traffic and force police OFF WITHOUT
     * touching the player's saved menu selections. The context was forced to a clean
     * single race at the top of this function, so the AI car-fill below takes the
     * default speed-pool path and InitRace sizes the field to 1 human + N AI.
     * [PORT-ONLY — the original demo randomised only track + AI cars.] */
    if (s_current_screen == TD5_SCREEN_ATTRACT_MODE) {
        int dcar = (s_attract_car >= 0 && s_attract_car < TD5_CAR_COUNT) ? s_attract_car : 0;
        int dopp = s_attract_opponents;
        int dtv  = s_attract_traffic;
        if (dopp < 1) dopp = 1;
        if (dopp > TD5_LEGACY_RACE_SLOTS - 1) dopp = TD5_LEGACY_RACE_SLOTS - 1;
        if (dtv < 0) dtv = 0;
        if (dtv > TD5_TRAFFIC_VOLUME_COUNT - 1) dtv = TD5_TRAFFIC_VOLUME_COUNT - 1;

        slot_ext_id[0]  = dcar;            /* random player car (deduped by the AI fill) */
        slot_variant[0] = 0;
        g_td5.car_index = dcar;

        g_td5.num_ai_opponents          = dopp;
        g_td5.traffic_volume            = dtv;
        g_td5.traffic_enabled           = (dtv != 0);
        g_td5.special_encounter_enabled = 0;   /* NO POLICE EVER */

        TD5_LOG_I(LOG_TAG,
                  "InitRaceSchedule: ATTRACT demo -> car=%d opponents=%d traffic=%d(en=%d) police=OFF",
                  dcar, dopp, dtv, g_td5.traffic_enabled);
    }

    /* RNG state for AI ext_id picks.
     *
     * Original path (non-trace / live frontend):
     *   - InitializeFrontendResourcesAndState @ 0x00414740 calls srand(timeGetTime())
     *     TWICE, then burns 1+ rand() for the CD-track pick loop.
     *   - InitializeRaceSeriesSchedule @ 0x0040dac0 does NOT call srand —
     *     it only stores timeGetTime() into g_randomSeedForRace.
     *   - AI car picks consume _holdrand from that time-dependent state.
     *
     * Trace path (race_trace_enabled=1 + Frida --trace):
     *   - Frida hook (td5_quickrace_hook.js) calls _srand(0x1A2B3C4D) with
     *     ZERO preamble rand() calls before InitializeRaceSeriesSchedule().
     *   - Port calls srand(0x1A2B3C4D) with ZERO preamble burns.
     *   - Both sides start AI-car selection from rand #1 → identical picks.
     * [CONFIRMED @ td5_quickrace_hook.js:180-186, td5_quickrace.py:261] */
    if (net_cfg_valid) {
        /* [S31 NET] Every machine must run the AI car fill from the SAME
         * rand() stream: seed with the host-broadcast seed, no preamble burn
         * (every machine runs this exact path). The wall-clock srand below
         * used to run AFTER the net seed was applied and silently wiped it --
         * each machine then picked its own AI grid ("cars are different
         * between client and server"). The fixed-trace seed masked this in
         * the headless A/B runs by overriding both sides identically. */
        srand(net_cfg.rng_seed);
    } else if (g_td5.ini.race_trace_enabled) {
        /* Under race_trace_enabled the Frida quickrace hook calls
         * _srand(0x1A2B3C4D) IMMEDIATELY before InitializeRaceSeriesSchedule()
         * with zero preamble rand() calls between them (hook bypasses
         * InitializeFrontendResourcesAndState entirely).
         * [CONFIRMED @ re/tools/quickrace/td5_quickrace_hook.js:180-186]
         * [CONFIRMED @ re/tools/quickrace/td5_quickrace.py:261 — seed_crt=True
         *  is auto-set when --trace is passed to the Python launcher]
         *
         * The port must therefore start from rand #1 (no preamble burns) to
         * match the original's AI-car selection sequence. The 1-burn below
         * that approximates the CD-track-pick loop must be SKIPPED here —
         * it belonged to the non-trace path where InitializeFrontendResourcesAndState
         * fires and consumes 1+ rand() calls before InitializeRaceSeriesSchedule.
         *
         * Skipping 1 burn under race_trace_enabled aligns the rand() index
         * sequence for slots 1-5 with the Frida-captured original sequence,
         * enabling the same AI cars to be selected on both sides and thus
         * zero-delta spawn world_y at tick=0.
         * [CONFIRMED via disassembly of InitializeRaceSession @ 0x0042aa5f-0x0042aa80:
         *  srand(seed) at 0x42aa52 → 12 seed-table rand()s → 1 extra rand()
         *  → loading screen pick — all happen AFTER AI car selection] */
        srand(0x1A2B3C4D);
        /* No burn: Frida hook has no preamble rand() before schedule call */
    } else {
        srand(timeGetTime());
        /* Approximate the original's CD-track-pick rand() burn from
         * InitializeFrontendResourcesAndState @ 0x00414a78:
         *   do { rand() % 7; } while (== g_selectedCdTrackIndex);
         * With a fresh seed both sides start with cdTrack = -1 (default),
         * so the first rand() always exits the loop. One rand() consumed.
         * [CONFIRMED @ 0x00414740, 0x0040dac0, 0x0042aa33 (the real srand via
         *  mislabeled __set_new_handler).] */
        (void)rand();
    }

    if (s_selected_game_type == 2 && !g_td5.network_active) {
        /* === Path 1: Quick Race (gameType == 2, Era) [CONFIRMED @ 0x0040dac0] ===
         * Original loop body consumes THREE rand() calls per iteration:
         *   rand #1 -> ext_id (& 7, optionally +8 if player car > 7)
         *   rand #2 -> variant (& 3)
         *   rand #3 -> discard
         * On dedup collision, the whole block re-runs and consumes 3 more
         * rands. Port previously had variant outside the loop — see the
         * default path comment below for why that matters. */
        for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
            int ext_id;
            int variant;
            int attempts = 0;
            do {
                ext_id  = rand() & 7;                   /* rand #1 */
                variant = rand() & 3;                   /* rand #2 (variant) */
                (void)rand();                           /* rand #3 (discard) */
                if (s_selected_car > 7)
                    ext_id += 8;
                if (++attempts > 100) break; /* safety */
            } while (frontend_ai_ext_id_taken(ext_id, slot_ext_id, slot_active,
                                               TD5_MAX_RACER_SLOTS));
            slot_active[i]  = 1;
            slot_ext_id[i]  = ext_id;
            slot_variant[i] = variant;
            TD5_LOG_I(LOG_TAG, "InitRaceSchedule: quick-race slot%d ext_id=%d var=%d attempts=%d",
                      i, ext_id, slot_variant[i], attempts);
        }
    } else if (s_selected_game_type == 5 && !g_td5.network_active) {
        /* === Path 2: Cup/Masters (gameType == 5) [CONFIRMED @ 0x0040dac0] ===
         * Scans s_masters_roster_flags[] for state==1 entries, claims them (sets to 2),
         * reads ext car id from s_masters_roster[]. */
        for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
            int found = 0;
            for (int j = 0; j < 15; j++) {
                if (s_masters_roster_flags[j] == 1) {
                    s_masters_roster_flags[j] = 2; /* claimed */
                    slot_active[i]  = 1;
                    slot_ext_id[i]  = s_masters_roster[j];
                    slot_variant[i] = rand() % 3;
                    found = 1;
                    TD5_LOG_I(LOG_TAG, "InitRaceSchedule: cup slot%d roster[%d] ext_id=%d var=%d",
                              i, j, slot_ext_id[i], slot_variant[i]);
                    break;
                }
            }
            if (!found) {
                TD5_LOG_W(LOG_TAG, "InitRaceSchedule: cup slot%d no roster entry available", i);
            }
        }
    } else {
        /* === Path 3: Default (single race, all other types) [CONFIRMED @ 0x0040dac0] ===
         * Faithful port of the original's loop structure. Each iteration of
         * the outer do/while body consumes THREE rand() calls in a specific
         * order:
         *   rand #1 -> tier_idx (mod 6 into row[gRaceDifficultyTier])
         *   rand #2 -> variant  (& 3)
         *   rand #3 -> discard  (return value thrown away)
         * When the chosen ext_id collides with an already-claimed slot, the
         * outer do/while RE-RUNS for the same slot — consuming ANOTHER 3
         * rand() calls. Only when the dedup check passes are the slot fields
         * written and the loop advances to the next slot.
         *
         * This is structurally distinct from the previous port version which
         * placed `slot_variant[i] = rand() & 3` OUTSIDE the do/while — on
         * collision retry, the previous port consumed 2 rands where the
         * original consumes 3, desyncing the rand() sequence.
         *
         * [CONFIRMED @ 0x0040dac0 body structure — decomp shows iVar6=rand();
         * uVar1=rand()&3; rand(); {dedup scan}; retry-on-collision path]. */
        int tier = g_td5.difficulty_tier;
        if (tier < 0 || tier > 2) tier = 2; /* clamp to valid tiers */

        /* S21: dynamic opponent pool bucketed by top speed. Build the sorted
         * pool (cached), take the band matching the difficulty tier, shuffle it
         * and walk it so opponents are distinct until the band is exhausted. */
        frontend_build_speed_pool();
        if (s_speed_pool_count > 0) {
            int lo, hi, band, seq;
            int band_ids[TD5_CAR_COUNT];
            frontend_speed_band_for_tier(tier, &lo, &hi);
            band = hi - lo;
            for (int k = 0; k < band; k++)
                band_ids[k] = s_speed_pool_ids[lo + k];
            /* Fisher-Yates shuffle so cars vary race-to-race (and which cars
             * within the band lead are not always the same). */
            for (int k = band - 1; k > 0; k--) {
                int r = rand() % (k + 1);
                int t = band_ids[k]; band_ids[k] = band_ids[r]; band_ids[r] = t;
            }
            seq = 0;
            for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
                int ext_id  = band_ids[seq % band];   /* distinct until wrap */
                int variant = rand() & 3;
                seq++;
                slot_active[i]  = 1;
                slot_ext_id[i]  = ext_id;
                slot_variant[i] = variant;
                TD5_LOG_I(LOG_TAG,
                          "InitRaceSchedule: speedpool slot%d tier=%d band=[%d,%d) ext_id=%d var=%d",
                          i, tier, lo, hi, ext_id, slot_variant[i]);
            }
        } else {
            /* Fallback (carparam unreadable): original hardcoded tier roster.
             * Faithful 3-rand-per-slot loop preserved for this path. */
            for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
                int ext_id;
                int variant;
                int attempts = 0;
                do {
                    int tier_idx = rand() % 6;              /* rand #1 */
                    variant      = rand() & 3;              /* rand #2 (variant) */
                    (void)rand();                           /* rand #3 (discard) */
                    ext_id = s_difficulty_tier_cars[tier][tier_idx];
                    if (++attempts > 100) break;
                } while (frontend_ai_ext_id_taken(ext_id, slot_ext_id, slot_active,
                                                   TD5_MAX_RACER_SLOTS));
                slot_active[i]  = 1;
                slot_ext_id[i]  = ext_id;
                slot_variant[i] = variant;
                TD5_LOG_I(LOG_TAG, "InitRaceSchedule: default slot%d tier=%d ext_id=%d var=%d attempts=%d",
                          i, tier, ext_id, slot_variant[i], attempts);
            }
        }
    }

    /* [MP COP CHASE AI COP 2026-06-23] Give the AI cop a POLICE car so it reads
     * unmistakably as the cop (not "just another racer"). The AI cop occupies the
     * first non-human slot (= num_human_players); TD5 police liveries are ext_id
     * 33..36 (loadable as racer cars — the original lets the player drive one in
     * cop chase). */
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE && !g_td5.network_active &&
        g_td5.mp_mode_config.cop_is_ai) {
        int cop = g_td5.num_human_players;
        if (cop >= 1 && cop < TD5_MAX_RACER_SLOTS && 33 < TD5_CAR_COUNT) {
            slot_active[cop]  = 1;
            slot_ext_id[cop]  = 33;   /* TD5 police car */
            slot_variant[cop] = 0;
            TD5_LOG_I(LOG_TAG, "InitRaceSchedule: AI cop slot %d -> police car (ext_id 33)", cop);
        }
    }

    /* Store ext_ids directly as car indices.
     * s_car_zip_paths is indexed by ext_id (display order), NOT by the original
     * binary's gCarZipPathTable type_index. The s_ext_car_to_type_index conversion
     * is NOT applied here — it maps to the original binary's table ordering which
     * doesn't match the source port's reordered table. */
    for (i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
        /* Accept any valid s_car_zip_paths index (TD5 0..32 + TD6 37..75). The
         * S21 speed pool can assign TD6 cars (ext_id >= 37) as opponents; the
         * old `< 37` bound clamped those to VIPER. The cup/masters/quick-race
         * paths only ever produce TD5 ext_ids, so widening is a no-op for them. */
        if (slot_active[i] && slot_ext_id[i] >= 0 && slot_ext_id[i] < TD5_CAR_COUNT) {
            g_td5.ai_car_indices[i]  = slot_ext_id[i];
            g_td5.ai_car_variants[i] = slot_variant[i];
        } else {
            g_td5.ai_car_indices[i]  = 0; /* fallback: VIPER (ext_id 0) */
            g_td5.ai_car_variants[i] = 0;
        }
    }

    /* Commit the PLAYER's selected paint (slot 0). The AI loop above starts at
     * slot 1, so without this the player's chosen colour is dropped and the car
     * always loads carskin0 (the default). The per-slot variant table mirrors
     * the original's gSlotCarIdSelectionTable[0] = g_player1SelectedPaintScheme
     * [CONFIRMED @ 0x0040DADC → built into "CARSKIN%d.TGA" @ 0x00442949]. */
    g_td5.ai_car_variants[0] = (slot_variant[0] >= 0 && slot_variant[0] <= 3)
                               ? slot_variant[0] : 0;
    /* [S31] Slot 0's CAR index too: net races load every slot from
     * ai_car_indices (g_td5.car_index is the LOCAL player's pick, which on a
     * client is not slot 0). Non-net keeps loading slot 0 from car_index. */
    g_td5.ai_car_indices[0] = (slot_ext_id[0] >= 0 && slot_ext_id[0] < TD5_CAR_COUNT)
                              ? slot_ext_id[0] : 0;

    TD5_LOG_I(LOG_TAG, "InitializeRaceSeriesSchedule: car=%d (resolved=%d) track=%d level=%d screen=%d type=%d ai=[%d,%d,%d,%d,%d]",
              s_selected_car, g_td5.car_index, g_td5.track_index,
              td5_asset_level_number(g_td5.track_index),
              s_current_screen, s_selected_game_type,
              g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
              g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
              g_td5.ai_car_indices[5]);
}

/* ======== [split] simultaneous car-select grid + player setup (moved verbatim) ======== */
/* ========================================================================
 * Simultaneous multiplayer car select (grid)  [PORT ENHANCEMENT 2026-06-07]
 *
 * Every joined player picks their car at the same time, each driven by their
 * OWN controller, in panes laid out by the chosen split-screen grid (the same
 * mp_resolve_layout the race viewports use) so each player can identify their
 * own forked screen by its coloured PLAYER N banner/border. Each pane carries a
 * compact copy of the single-player car-select buttons (CAR / PAINT / STATS /
 * AUTO-MANUAL / OK), navigated by that player's own pad:
 *   up / down  move the button cursor   left / right  change the focused value
 *   A          activate (STATS opens the spec sheet; OK locks the pick in)
 *   B / ESC    back to the MULTIPLAYER LOBBY
 * When ALL players have pressed OK the screen auto-advances to track selection
 * after a short beat. Per-player input is read through the still-alive lobby
 * scan handles (td5_plat_input_device_nav); the per-player EXCLUSIVE devices are
 * bound only at the commit, since binding them releases the shared scan handles.
 * Inner states: 0x20 = lobby->grid slide-in animation, 0x21 = interactive. */

static float mp_simul_clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }





/* Parse a car's config.nfo into this pane's spec cache (only on car change). */
void mp_simul_load_pane_spec(int p, int car) {
    int sz = 0, field; size_t i; char *data;
    if (s_mp_pane_spec_car[p] == car) return;
    s_mp_pane_spec_car[p] = car;
    for (field = 0; field < 17; field++) s_mp_pane_spec[p][field][0] = '\0';
    if (car < 0 || car >= td5_car_total_count()) return;
    data = (char *)td5_asset_open_and_read("config.nfo", s_car_zip_paths[car], &sz);
    if (!data || sz <= 0) return;
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(s_mp_pane_spec[p][0])) s_mp_pane_spec[p][field][j++] = data[i];
            i++;
        }
        s_mp_pane_spec[p][field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
}





/* Centred small-font helper (px in, native cap size scaled by lsx/lsy). */
static void mp_simul_small_centered(float cx_px, float y_px, const char *t,
                                    uint32_t c, float lsx, float lsy) {
    float w = fe_measure_small_text(t) * fe_glyph_sx(lsx, lsy);
    fe_draw_small_text(cx_px - w * 0.5f, y_px, t, c, lsx, lsy);
}

/* [responsive 2026-06-21] Largest scale multiplier (<=1) at which the small-font
 * string `t` fits within max_w_px at base scale (lsx,lsy). 1.0 if it already
 * fits / is empty. The MP split-screen cards shrink to as small as ~172x127 (9
 * players, 3x3 grid); names + button labels drawn at the fixed small-font scale
 * used to overflow those panes. This lets every label shrink to stay inside its
 * box so all UI elements remain fully visible regardless of pane size. */
static float mp_fit_scale(const char *t, float max_w_px, float lsx, float lsy) {
    float w;
    if (!t || !t[0] || max_w_px <= 0.0f) return 1.0f;
    w = fe_measure_small_text(t) * fe_glyph_sx(lsx, lsy);
    return (w <= max_w_px) ? 1.0f : (max_w_px / w);
}

/* Centred small text auto-shrunk to fit max_w_px (see mp_fit_scale). */
static void mp_simul_small_centered_fit(float cx_px, float y_px, const char *t,
                                        uint32_t c, float lsx, float lsy, float max_w_px) {
    float s  = mp_fit_scale(t, max_w_px, lsx, lsy);
    float w  = fe_measure_small_text(t) * fe_glyph_sx(lsx * s, lsy * s);
    fe_draw_small_text(cx_px - w * 0.5f, y_px, t, c, lsx * s, lsy * s);
}

/* ◄ ► selector arrows (procedural triangle-SDF, ps_arrow) at the left/right
 * edges of a button. Native. [2026-06-16] The ArrowButtonz.tga sprite bitmap
 * fallback was retired. */
static void mp_simul_draw_arrows(float bx, float by, float bw, float bh, float sx, float sy) {
    if (!s_ps_arrow) return;
    /* [responsive 2026-06-21] Cap the glyph to the button height so it never
     * sticks out of a short button in a small split-screen pane. */
    float a2 = 12.0f;
    if (a2 > bh - 2.0f) a2 = bh - 2.0f;
    if (a2 < 5.0f)      a2 = 5.0f;
    float ay2 = by + (bh - a2) * 0.5f;
    fe_draw_arrow_proc((bx + 3.0f) * sx,           ay2 * sy, a2 * sx, a2 * sy, 0, 0xFF7995FFu);
    fe_draw_arrow_proc((bx + bw - 3.0f - a2) * sx, ay2 * sy, a2 * sx, a2 * sy, 1, 0xFF7995FFu);
}

/* One compact pane button — the REGULAR TD5 button frame (the same 9-slice / neon
 * design the rest of the menus use), with a TRANSPARENT interior when unselected.
 * The FOCUSED button uses the SELECTED frame (the golden ring) but with the
 * player's accent colour as the INTERIOR fill in place of the default purple.
 * Selector buttons draw the original ◄/► arrow sprites at the edges; `val`/
 * `swatch_rgb` (right, kept clear of the right arrow) are optional. */
void mp_simul_draw_btn(float x, float y, float w, float h, const char *label,
                       int focused, uint32_t pcol, int arrows,
                       const char *val, int swatch_rgb, float sx, float sy) {
    uint32_t rgb = pcol & 0x00FFFFFFu;
    uint32_t tc  = 0xFFFFFFFFu;
    float ty = (y + (h - SMALLFONT_TTF_CAP) * 0.5f) * sy;   /* vertically centred */
    float redge = arrows ? (x + w - 18.0f) : (x + w - 6.0f);
    /* Transparent background: unselected frame has no interior fill; the focused
     * one fills with the player's accent colour.
     * [2026-06-24] Shrink the neon rim + corner radius proportionally to the
     * button HEIGHT so the small split-screen pane buttons (a 6-player 2x3 grid
     * shrinks these to ~11-17px tall) don't carry the fat 6px side rim / 20px
     * corner sized for the full 32px menu buttons. Clamped so a full-height pane
     * button (>=32px, e.g. the 2-player big-car panes) keeps the normal look and
     * a tiny one keeps a thin-but-visible rim (>=~2px). */
    float bscale = h / 32.0f;
    if (bscale > 1.0f) bscale = 1.0f;
    if (bscale < 0.34f) bscale = 0.34f;
    fe_draw_button_frame_fill_scaled(x * sx, y * sy, w * sx, h * sy,
                                     focused ? 0 : 1, rgb | 0xFF000000u, bscale, sx, sy);
    if (focused) {
        int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        int lum = (r * 30 + g * 59 + b * 11) / 100;     /* readable label over the accent */
        tc = (lum > 150) ? 0xFF101010u : 0xFFFFFFFFu;
    }
    if (arrows) mp_simul_draw_arrows(x, y, w, h, sx, sy);

    /* [responsive 2026-06-21] Fit the label between the left arrow and the
     * value/swatch on the right, shrinking it for narrow panes so long labels
     * (AUTO / MANUAL) never overflow the button. */
    float val_w  = (val && swatch_rgb < 0) ? fe_measure_small_text(val) * fe_glyph_sx(sx, sy) : 0.0f;
    float l_right;
    if (swatch_rgb >= 0)   l_right = (redge - 17.0f) * sx;
    else if (val)          l_right = redge * sx - val_w - 4.0f * sx;
    else                   l_right = (arrows ? (x + w - 18.0f) : (x + w - 4.0f)) * sx;
    /* [MP PROFILE SELECTION centring 2026-07-04] Mirror the right-side reserve
     * (value readout, colour swatch, or arrow gutter) onto the LEFT so the label
     * is centred on the BUTTON centre — matching the plain PROFILE/AUTOMATIC/
     * ASSIST/OK buttons. Previously the value/swatch buttons (NAME, COLOUR) kept a
     * flush-left l_left (x+4) and only pulled l_right in, so their labels centred
     * in a left-biased span and sat visibly LEFT of the others in the vertical
     * stack. This is a no-op for already-symmetric plain/arrowed buttons (their
     * left inset already equals the right reserve). */
    float l_left = (x * sx) + ((x + w) * sx - l_right);
    if (l_left >= l_right)   /* degenerate: value wider than half the button — keep it visible */
        l_left = (arrows ? (x + 17.0f) : (x + 4.0f)) * sx;
    {
        /* [2026-06-29] CENTRE the (fit-scaled) label within the available span
         * [l_left, l_right] for EVERY button — arrowed CAR/PAINT now read centred
         * between the ◄► instead of jammed against the left arrow. mp_fit_scale has
         * already shrunk the text to fit that span, so a centred label never spills
         * past the arrows / outside the button. */
        float ls   = mp_fit_scale(label, l_right - l_left, sx, sy);
        float lsx2, lsy2, lty, lw;
        /* [2026-06-29] Also cap the label to the button HEIGHT so a very short pane
         * button (the 7-9p 3x3 grid shrinks these well below the 9px font) shrinks
         * its text vertically instead of overflowing into the neighbouring row. */
        float h_cap = (h - 2.0f) / SMALLFONT_TTF_CAP;
        if (h_cap < 0.34f) h_cap = 0.34f;
        if (ls > h_cap) ls = h_cap;
        /* [7+ players 2026-06-28] Crowded grids (7-9 split panes) get a touch
         * smaller button font so the labels stay comfortable in the tiny rows. */
        if (s_num_human_players >= 7) ls *= 0.85f;
        lsx2 = sx * ls; lsy2 = sy * ls;
        lty  = (y + (h - SMALLFONT_TTF_CAP * ls) * 0.5f) * sy;
        lw   = fe_measure_small_text(label) * fe_glyph_sx(lsx2, lsy2);
        fe_draw_small_text((l_left + l_right) * 0.5f - lw * 0.5f, lty, label, tc, lsx2, lsy2);
    }

    if (swatch_rgb >= 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((redge - 15.0f) * sx, (y + h * 0.5f - 5.0f) * sy, 15.0f * sx, 10.0f * sy,
                     frontend_rgb_to_bgra((uint32_t)swatch_rgb), -1, 0, 0, 1, 1);
    } else if (val) {
        fe_draw_small_text(redge * sx - val_w, ty, val, tc, sx, sy);
    }
}

/* Per-pane STATS spec sheet, drawn as an OVERLAY on top of the normal pane (car
 * stays at the top, the button menu stays semi-visible underneath) — the same
 * "processing" the original uses (dimmed content, spec rows over it). Text is
 * scaled to fit the pane. */
static void mp_simul_render_stats(int p, float px, float py, float pw, float ph, float sx, float sy) {
    /* [2026-06-25] Rebuilt physics MORE STATS (frontend_render_physics_stats):
     * real carparam.dat parameters drawn in the player's accent colour,
     * replacing the cosmetic config.nfo spec sheet. A translucent scrim keeps
     * the car + menu semi-visible underneath; a back-hint sits at the bottom. */
    float ax = px + 6.0f, ay = py + 28.0f, aw = pw - 12.0f, ah = ph - 34.0f;
    uint32_t pcol = ((uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu) | 0xFF000000u;
    float rh  = ah / 11.0f;                        /* 10 stat rows + a back-hint line */
    float lsy = sy * mp_simul_clamp01(rh / 11.0f), lsx;
    if (lsy < sy * 0.42f) lsy = sy * 0.42f;
    lsx = sx * (lsy / sy);

    /* Translucent dark scrim — dims the car + menu underneath so they stay
     * SEMI-VISIBLE behind the spec text (no opaque takeover). */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(ax * sx, ay * sy, aw * sx, ah * sy, 0xC2000810u, -1, 0, 0, 1, 1);

    frontend_render_physics_stats(s_mp_pane_spec_car[p], ax + 2.0f, ay + 2.0f,
                                  aw - 4.0f, ah - rh - 2.0f, pcol, 1, sx, sy);
    mp_simul_small_centered((px + pw * 0.5f) * sx, (ay + ah - rh) * sy, "A / B = BACK",
                            0xFFFFE060u, lsx, lsy);
}

/* [#3 2026-06-16] MP per-player panel MAX-WIDTH cap, matching the IDENTICAL
 * formula the sibling td5_fe_race.c applies to its in-file overlays (the
 * position-picker cells + PROFILE chip) so the main name/colour panes
 * (frontend_mp_setup_render) and the simultaneous car-select panes
 * (frontend_mp_simul_carsel_render) drawn HERE line up underneath them.
 *
 * EXACT FORMULA (canvas width W = 640, cols from mp_resolve_layout):
 *   pane_w  = min(W / cols, W / 3)          (cap at the 3x3-grid pane = 213.33)
 *   row_x0  = (W - cols * pane_w) / 2        (centre the capped row)
 *   px[col] = row_x0 + col * pane_w
 * With 2 players each pane is capped at 213.33 px and the row is centred;
 * cols >= 3 is unchanged (pane_w already <= cap, row_x0 == 0). Height stays
 * W/rows (only WIDTH is capped). Same env var the fe_race helper used. */
static int frontend_mp_panel_cap_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PANEL_CAP");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP main-pane width cap (#3) %s (TD5RE_MP_PANEL_CAP=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Writes the capped per-pane width to *pane_w and the centred row's col-0 left
 * edge to *row_x0 for `cols` columns across a 640px canvas. No-op (full width,
 * x0=0) when the cap knob is off. */
void frontend_mp_panel_capped(int cols, float *pane_w, float *row_x0) {
    /* [layout 2026-06-19] Lay the columns across the usable band
     * x[FE_MP_LEFT_MARGIN..FE_MP_RIGHT_EDGE] (clears the art's left black bar,
     * reaches the right edge) instead of the full 640 starting at x=0. The cap is
     * the 3-column-equivalent of the BAND (usable/3) so a 3x3 grid fills the band
     * edge-to-edge and 1-2 cols stay centred in it without a wide right gap. */
    float c = (float)(cols < 1 ? 1 : cols);
    float full = FE_MP_USABLE_W / c;
    float w = full;
    if (frontend_mp_panel_cap_on()) {
        /* [layout 2026-06-19] Cap at HALF the usable band (was a third). This lets a
         * 2-column grid fill the band edge-to-edge (bigger 2-player cards, no wide
         * right gap) and a 3-column grid fills it anyway (516/3 < 516/2). The cap
         * only restrains the degenerate 1-column case to a half-band centred card. */
        float cap = FE_MP_USABLE_W / 2.0f;
        if (w > cap) w = cap;
    }
    if (pane_w) *pane_w = w;
    if (row_x0) *row_x0 = FE_MP_LEFT_MARGIN + (FE_MP_USABLE_W - c * w) * 0.5f;
}

/* [adaptive 2-col 2026-06-20] Decide the INTERNAL card layout for a car-select
 * pane of the given size. A TWO-COLUMN card — car image + at-a-glance stat bars
 * on the LEFT, the CAR/PAINT/MORE STATS/transmission/OK stack on the RIGHT (the
 * player-name banner + car name still span the full width up top) — needs the
 * pane to be both WIDE enough to split into two readable columns AND TALL enough
 * to stack the 5 buttons / car / stats per column (the user's "if the boxes are
 * tall enough"). So the trigger is a MINIMUM width AND a MINIMUM height: the
 * common 2-up (258x381), up/down (258x190) and 2x2 (258x190) panes all qualify;
 * narrow 3-column grids (172-wide) and the very short stacked rows (127-tall)
 * stay single-column. The two-column body then fills the free space (buttons
 * distributed down the right column, stats stretched to the bottom of the left).
 *
 * Knob TD5RE_MP_CARSEL_2COL: unset/other = adaptive; "1" = force two-column on
 * every pane; "0" = force the pre-2026-06-20 single-column stack everywhere. */
#define MP_2COL_MIN_W 200.0f   /* pane must be at least this wide to split into two columns */
/* [2026-06-24] Lowered 150->120 so the 6-player 2x3 grid pane (258x127) — and the
 * other wide-but-short panes it shares the threshold with: 3-player UP/DOWN
 * (258x127) and 5-player 2x3 (258x127) — use the TWO-COLUMN card (car + at-a-glance
 * stat bars on the LEFT, the CAR/PAINT/MORE STATS/transmission/OK stack on the
 * RIGHT) instead of the single-column stack. At 127px tall the single column
 * overflowed the pane and pushed AUTOMATIC/OK off the bottom (clipped text); the
 * two-column card packs the 5 buttons into a parallel column so they all fit.
 * Narrow 3-column grids (172px wide) still stay single-column via MP_2COL_MIN_W,
 * and very short 1xN stacks (<120px) also stay single. */
#define MP_2COL_MIN_H 120.0f   /* ...and at least this tall to be worth it ("tall enough") */
/* [big-car 2026-06-23] A WIDE pane that is also this TALL (the default 2-player
 * side-by-side 2-up pane, ~258x381) has enough vertical room to stack a FULL-WIDTH
 * car preview over a compact menu. There the cramped two-column card (car squeezed
 * into a ~46%-wide column, stat bars stretched hundreds of px tall beside it) is the
 * wrong choice — we route those panes to the single-column stack with an enlarged
 * car band + shorter buttons so the car picture is as big as possible. Short/wide
 * up-down + 2x2 panes (~258x190) and narrow 3-col grids fall below this and keep
 * their existing two-column / classic single-column layout untouched. */
#define MP_BIGCAR_MIN_H 330.0f
static int mp_carsel_two_col(float pane_w, float pane_h) {
    static int mode = -1;   /* 0 = force off, 1 = adaptive, 2 = force on */
    if (mode < 0) {
        const char *e = getenv("TD5RE_MP_CARSEL_2COL");
        mode = (e && e[0] == '1' && e[1] == '\0') ? 2
             : (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP car-select 2-column card layout %s (TD5RE_MP_CARSEL_2COL=%s)",
                  mode == 2 ? "FORCED ON" : mode == 0 ? "FORCED OFF" : "adaptive",
                  e ? e : "default");
    }
    if (mode != 1) return mode == 2;
    return (pane_w >= MP_2COL_MIN_W && pane_h >= MP_2COL_MIN_H) ? 1 : 0;
}

/* Draw player p's car preview (+ TD6 body-paint overlay) fitted into the rect,
 * keeping the 408:280 car-pic aspect and centred both ways. Shared by the single-
 * and two-column card layouts. */
static void mp_simul_draw_pane_car(int p, float ax, float ay, float aw, float ah,
                                   float sx, float sy) {
    int car = s_mp_player_car[p];
    int td6 = frontend_car_is_td6(car);
    float ar = 408.0f / 280.0f;
    float dw = aw, dh = aw / ar, dx, dy;
    if (dh > ah) { dh = ah; dw = ah * ar; }
    dx = ax + (aw - dw) * 0.5f;
    dy = ay + (ah - dh) * 0.5f;
    if (s_mp_pane_preview[p] > 0)
        fe_draw_surface_rect(s_mp_pane_preview[p], dx * sx, dy * sy, dw * sx, dh * sy, 0xFFFFFFFF);
    if (td6 && frontend_car_paintable(car) && s_mp_pane_overlay[p] > 0)
        fe_draw_surface_rect(s_mp_pane_overlay[p], dx * sx, dy * sy, dw * sx, dh * sy,
                             frontend_rgb_to_bgra((uint32_t)s_mp_player_color[p]));
}

/* Draw one car-select pane button by MP_BTN_* index — keeps the PAINT variants
 * (TD6 swatch / paintless "-"), the ◄► arrows on the selector rows, and the focus
 * styling in ONE place so the single- and two-column layouts stay in sync.
 * [2026-06-29] Labels shortened for the narrow split panes: MORE STATS -> STATS,
 * AUTOMATIC -> AUTO (MANUAL unchanged), and the TD5 paint "n/4" value dropped — so
 * the button column can be narrower without the text spilling, leaving more width
 * for the stat bars + a bigger car. */
static void mp_simul_draw_pane_button(int p, int which, float bx, float by,
                                      float bw, float bh, float sx, float sy) {
    int car = s_mp_player_car[p];
    int td6 = frontend_car_is_td6(car);
    int focus = (s_mp_pane_btn[p] == which);
    uint32_t pcol = ((uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu) | 0xFF000000u;
    switch (which) {
    case MP_BTN_CAR:
        mp_simul_draw_btn(bx, by, bw, bh, "CAR", focus, pcol, 1, NULL, -1, sx, sy);
        break;
    case MP_BTN_PAINT:
        if (td6 && frontend_car_paintable(car))
            mp_simul_draw_btn(bx, by, bw, bh, "PAINT", focus, pcol, 1, NULL,
                              s_mp_player_color[p], sx, sy);
        else if (!td6 && frontend_car_has_paint(car))
            mp_simul_draw_btn(bx, by, bw, bh, "PAINT", focus, pcol, 1, NULL, -1, sx, sy);
        else
            mp_simul_draw_btn(bx, by, bw, bh, "PAINT", focus, pcol, 0, "-", -1, sx, sy);
        break;
    case MP_BTN_OK:
        mp_simul_draw_btn(bx, by, bw, bh, "OK", focus, pcol, 0, NULL, -1, sx, sy);
        break;
    }
}

/* [MP HOST INDICATOR 2026-06-28] Draw an MP pane's coloured name banner: the
 * chosen profile name (or "PLAYER N"), and — for slot 0, the HOST — a gold "HOST"
 * pill badge at the banner's LEFT with the name re-centred to its right so badge
 * + name never overlap. Player slot 0 is always the host (slot-based; slot 0's
 * pick is the binding one — see the comments at Screen_MpPosition / the mode
 * vote). This single helper is shared by the PROFILE SELECTION and SELECT CAR
 * split-screen panes so the host marker is pixel-identical on both. (px,pyr,
 * pane_w,cx) are the pane's virtual-px layout already computed by the caller. */
static void mp_draw_pane_name_banner(int p, float px, float pyr, float pane_w,
                                     float cx, float sx, float sy) {
    char buf[64];
    uint32_t rgb = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;
    if (s_mp_player_name[p][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[p]);
    else                        snprintf(buf, sizeof buf, "PLAYER %d", p + 1);
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, 16.0f * sy,
                 rgb | 0xD0000000u, -1, 0, 0, 1, 1);
    if (p == 0) {
        float badge_w = td5_vui_host_badge(px + 6.0f, pyr + 4.5f, 13.0f, sx, sy);
        float name_l = px + 6.0f + badge_w + 5.0f;   /* reserve the badge column */
        float name_r = px + pane_w - 3.0f;
        mp_simul_small_centered_fit((name_l + name_r) * 0.5f * sx, (pyr + 6) * sy, buf,
                                    0xFF000000u, sx, sy, (name_r - name_l) * sx);
        { static int s_logged_host_badge = 0;
          if (!s_logged_host_badge) { s_logged_host_badge = 1;
              TD5_LOG_I(LOG_TAG, "MP setup/carsel: drew HOST badge on slot 0 (name='%s')", buf); } }
    } else {
        mp_simul_small_centered_fit(cx * sx, (pyr + 6) * sy, buf, 0xFF000000u, sx, sy,
                                    (pane_w - 8.0f) * sx);
    }
}

void frontend_mp_simul_carsel_render(float sx, float sy) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    float anim_t = 1.0f;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    (void)missing;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    if (s_inner_state == 0x20)
        anim_t = mp_simul_clamp01((float)(now - s_mp_simul_anim_ms) / (float)MP_SIMUL_ANIM_MS);

    /* [#3] Cap pane WIDTH at the 3x3-grid equivalent (640/3) and centre the row,
     * matching the fe_race overlays so the position-picker/profile chips line up. */
    float pane_w, row_x0 = 0.0f;
    frontend_mp_panel_capped(cols, &pane_w, &row_x0);
    /* [R1] Reserve a top band for the "SELECT CAR" title so the panes start BELOW
     * it instead of overlapping the title text.
     * [R4 2026-06-19] Raise to the shared FE_MP_TOP_BAND (85) to match every other
     * MP screen — the old 40px let the panes overlap the title + the background
     * art's upper decoration line. Reserve a matching bottom band so the panes
     * occupy the same comfortable middle band as the profile-setup screen. */
    const float mp_title_band  = FE_MP_TOP_BAND;
    const float mp_bottom_band = FE_MP_BOTTOM_BAND;
    float pane_h = (480.0f - mp_title_band - mp_bottom_band) / (float)rows;

    /* [layout 2026-06-19] Full-screen darkening scrim REMOVED at user request so
     * the MainMenu background art shows through at full brightness (matches the
     * profile-setup + choose-your-screen screens). The per-pane translucent
     * backdrops below still give the cards enough contrast. */

    /* [#19] Standard top title. The global title-strip path suppresses titles
     * while s_mp_simul is set (the grid draws its own per-pane headers), so the
     * screen header is drawn here directly. */
    if (td5_titlefont_ready())
        frontend_draw_screen_title("SELECT CAR", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                                   0xFFE3D708u, sx, sy);

    for (p = 0; p < n; p++) {
        /* [#6 2026-06-15] Place each pane at the player's CHOSEN position cell
         * (from the split-screen position picker) instead of identity p, so a
         * player parked bottom-right while choosing a screen also picks their car
         * bottom-right. Unclaimed cells draw nothing (this loop only iterates the
         * human players). Identity when the positions feature is off. */
        extern int frontend_mp_player_pane_cell(int);  /* defined in td5_fe_race.c */
        int cell = frontend_mp_player_pane_cell(p);
        int col = cell % cols, row = cell / cols;
        float px = row_x0 + (float)col * pane_w, py = mp_title_band + (float)row * pane_h;
        float cx, pyr, pt, pe, rise;
        uint32_t rgb  = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;  /* chosen identity colour */
        uint32_t pcol = rgb | 0xFF000000u;
        int car   = s_mp_player_car[p];
        int ready = s_mp_player_ready[p];
        int stats = (s_mp_pane_substate[p] == 1);
        char buf[64];

        /* Staggered rise-in: each pane eases up into place from below. */
        pt = mp_simul_clamp01(anim_t * (1.0f + 0.12f * (float)n) - 0.10f * (float)p);
        pe = 1.0f - (1.0f - pt) * (1.0f - pt);
        rise = (1.0f - pe) * (pane_h * 0.45f);
        pyr = py + rise;
        cx  = px + pane_w * 0.5f;

        /* Pane backdrop + coloured border (brighter/thicker when READY). */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, (pane_h - 6) * sy,
                     ready ? 0xC0102818u : 0xB0141420u, -1, 0, 0, 1, 1);
        {
            float bt = ready ? 4.0f : 2.0f;
            uint32_t bc = rgb | (ready ? 0xFF000000u : 0xD0000000u);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + pane_h - 3 - bt) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + pane_w - 3 - bt) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
        }

        /* Header banner: the player's chosen NAME (host slot 0 gets the crown). */
        mp_draw_pane_name_banner(p, px, pyr, pane_w, cx, sx, sy);

        /* Car NAME above the image (request). */
        snprintf(buf, sizeof buf, "%s", frontend_get_car_display_name(car));
        mp_simul_small_centered_fit(cx * sx, (pyr + 21) * sy, buf, 0xFFFFFFFFu, sx, sy, (pane_w - 10.0f) * sx);

        /* READY panes: chosen car centred with a READY badge, no menu. */
        if (ready) {
            float avail_w = pane_w - 20.0f;
            mp_simul_draw_pane_car(p, cx - avail_w * 0.5f, pyr + 32.0f, avail_w, pane_h * 0.38f, sx, sy);
            mp_simul_small_centered_fit(cx * sx, (pyr + pane_h * 0.66f) * sy, "READY", 0xFF40FF40u, sx, sy, (pane_w - 12.0f) * sx);
            mp_simul_small_centered_fit(cx * sx, (pyr + pane_h - 16.0f) * sy, "A = CHANGE   B = BACK",
                                    0xFFB0B0B0u, sx, sy, (pane_w - 8.0f) * sx);
            continue;
        }

        /* Stat bars (both layouts) need the cached spec sheet (loaded even before
         * MORE STATS is opened). */
        mp_simul_load_pane_spec(p, car);

        /* [big-car 2026-06-23] A tall + wide 2-player 2-up pane (~258x381) skips
         * the cramped two-column card and uses the enlarged single-column car
         * stack below so the preview is full-width instead of squeezed into a
         * ~46%-wide column. Short/2x2/narrow panes are unaffected. */
        int big_car = (pane_w >= MP_2COL_MIN_W && pane_h >= MP_BIGCAR_MIN_H);
        {   /* one-shot: confirm the chosen pane layout without per-frame spam */
            static int s_logged_bigcar = 0;
            if (!s_logged_bigcar) {
                s_logged_bigcar = 1;
                TD5_LOG_I(LOG_TAG, "MP carsel pane layout: n=%d pane=%.0fx%.0f big_car=%d two_col=%d",
                          n, pane_w, pane_h, big_car,
                          (!big_car && mp_carsel_two_col(pane_w, pane_h)) ? 1 : 0);
            }
        }

        if (!big_car && mp_carsel_two_col(pane_w, pane_h)) {
            /* TWO-COLUMN card: car image + at-a-glance stat bars on the LEFT, the
             * CAR / PAINT / MORE STATS / transmission / OK stack on the RIGHT. The
             * name banner + car name above still span the full width. Every element
             * is sized from the pane's ACTUAL free space (no fixed pixel sizes): the
             * column widths are fractions of the pane width, the car takes its snug
             * fit, the stat bars stretch to the bottom of the left column, and the 5
             * buttons are spread evenly down the full right column. So the card
             * grows/shrinks to fill whatever cell the split-screen grid gives it. */
            float gap = 6.0f;
            float col_top = pyr + 31.0f;                 /* just below the car name */
            float col_bot = pyr + pane_h - 8.0f;
            float col_h   = col_bot - col_top;
            float lx = px + 8.0f;
            float usable = pane_w - 16.0f - gap;
            float lcw = usable * 0.46f;                    /* car/stats column (narrower) */
            float rcw = usable - lcw;                      /* button column (wider, fits AUTOMATIC) */
            float rx = lx + lcw + gap;
            /* LEFT: car snug at the top (no dead band above/below a width-limited
             * landscape car-pic), then the stat bars STRETCHED to fill the rest of
             * the column down to col_bot. */
            float car_fit = lcw / (408.0f / 280.0f) + 18.0f;   /* natural height at this width + margin */
            float car_h   = col_h * 0.52f;
            float bars_y, bars_h;
            if (car_h > car_fit) car_h = car_fit;
            bars_y = col_top + car_h + 4.0f;
            bars_h = col_bot - bars_y;
            mp_simul_draw_pane_car(p, lx, col_top, lcw, car_h, sx, sy);
            if (bars_h > 8.0f)
                frontend_draw_car_stat_bars(lx, bars_y, lcw, bars_h,
                                            s_mp_pane_spec[p][7], s_mp_pane_spec[p][8],
                                            s_mp_pane_spec_car[p], pcol, 1, 0.0f, sx, sy);
            /* RIGHT: the 5 buttons spread evenly down the WHOLE column (one row each
             * = col_h / MP_BTN_COUNT) so they always fill the height instead of
             * clumping at a fixed size — capped so they don't get comically tall in
             * a big 2-up pane. */
            {
                float slot = col_h / (float)MP_BTN_COUNT;
                float bh = slot - 5.0f;
                int b;
                if (bh > 44.0f) bh = 44.0f;
                if (bh < 10.0f) bh = 10.0f;
                for (b = 0; b < MP_BTN_COUNT; b++) {
                    float yy = col_top + (float)b * slot + (slot - bh) * 0.5f;
                    mp_simul_draw_pane_button(p, b, rx, yy, rcw, bh, sx, sy);
                }
            }
        } else if (pane_h < 250.0f) {
            /* SHORT narrow pane: the default 5-6p 3x2 grid (172x190) and the 7-9p 3x3
             * grid (172x127). [layout 2026-06-29] Lay the CAR full-width across the TOP
             * (as big as the pane allows) and split the space BELOW it into TWO columns
             * — the 5 buttons (CAR / PAINT / STATS / AUTO / OK) on the LEFT, the SPEED /
             * ACCEL / HANDLING stat bars on the RIGHT. Side by side, neither is starved
             * vertically, so the car gets the full pane width instead of the ~16px
             * sliver the old single-column stack left it. Short button labels keep the
             * button column narrow, freeing width for a wide stat column (full words +
             * a usable bar); the stat panel falls back to single letters only if a
             * label still won't fit. One shared button height -> one border scale,
             * passed to the stat panel too, so every frame has the SAME border. */
            float bx = px + 8.0f, bw = pane_w - 16.0f;
            float top0   = pyr + 32.0f;                  /* below the car-name banner */
            float bottom = pyr + pane_h - 8.0f;
            const float GAP = 6.0f;
            float content_h = bottom - top0;
            float car_natural = bw / (408.0f / 280.0f);
            float zone_y, zone_h, zusable, lcw, rcw, rx, bh, frame_scale, slot;
            float car_h;
            int b;
            /* Prioritise the CAR. The two-column ZONE (buttons + stats) scales with the
             * pane HEIGHT — shorter panes get a SMALLER zone so the car grows — then the
             * full-width car takes the rest (capped to its natural landscape height so
             * there's no letterbox). On the 5-6p 3x2 pane (190) the zone is ~52px
             * (comfortable buttons/stats); on the cramped 7-9p 3x3 pane (127) it drops to
             * ~30px, giving a much BIGGER car with tighter buttons + stats (their text
             * auto-shrinks to the row height, so smaller stays legible, never overlaps). */
            zone_h = content_h * 0.35f;
            if (zone_h < 28.0f) zone_h = 28.0f;
            car_h  = content_h - zone_h - GAP;
            if (car_h > car_natural) {                    /* car would letterbox: cap, grow zone */
                car_h = car_natural;
                zone_h = content_h - car_h - GAP;
            }
            if (car_h < 18.0f) {                          /* pane too short: clamp car, shrink zone to fit */
                car_h = 18.0f;
                zone_h = content_h - car_h - GAP;
                if (zone_h < 24.0f) zone_h = 24.0f;
            }
            zone_y  = top0 + car_h + GAP;
            zusable = bw - GAP;
            /* Short button labels (CAR/PAINT/STATS/AUTO/OK) let the button column be
             * narrow; the stat column takes the wider share for full words + a longer
             * bar. CAR/PAINT still carry ◄► arrows, so the button column keeps enough
             * width that those labels don't shrink to nothing. */
            lcw     = zusable * 0.44f;                    /* narrow buttons column */
            rcw     = zusable - lcw;                      /* WIDE stats column */
            rx      = bx + lcw + GAP;
            slot    = zone_h / (float)MP_BTN_COUNT;
            bh      = slot - 1.5f;
            if (bh < 6.0f)  bh = 6.0f;
            if (bh > 28.0f) bh = 28.0f;
            frame_scale = bh / 32.0f;
            if (frame_scale > 1.0f) frame_scale = 1.0f;
            if (frame_scale < 0.34f) frame_scale = 0.34f;
            {   /* one-shot: confirm the two-column split without per-frame spam */
                static int s_logged_2col = 0;
                if (!s_logged_2col) {
                    s_logged_2col = 1;
                    TD5_LOG_I(LOG_TAG, "MP carsel short-narrow 2col: pane=%.0fx%.0f car=%.0f "
                              "zone=%.0f btn=%.1f stats_w=%.0f", pane_w, pane_h, car_h,
                              zone_h, bh, rcw);
                }
            }
            mp_simul_draw_pane_car(p, bx, top0, bw, car_h, sx, sy);    /* full-width car */
            for (b = 0; b < MP_BTN_COUNT; b++) {
                float yy = zone_y + (float)b * slot + (slot - bh) * 0.5f;
                mp_simul_draw_pane_button(p, b, bx, yy, lcw, bh, sx, sy);
            }
            frontend_draw_car_stat_bars(rx, zone_y, rcw, zone_h,
                                        s_mp_pane_spec[p][7], s_mp_pane_spec[p][8],
                                        s_mp_pane_spec_car[p], pcol, 2, frame_scale, sx, sy);
        } else {
            /* TALL single-column stack: the 3p LEFT/RIGHT narrow pane (172x381) AND the
             * 2-player big-car pane (258x381, which bypasses the two-column card). Both
             * have ample height, so reserve the readable stat panel first, then buttons,
             * and let the car take the (large) remainder. [big-car 2026-06-23] the 2-up
             * pane keeps an enlarged car (0.44 of pane_h) and a lower button cap (26px).
             * [rebalance 2026-06-28] Priority squeeze car -> buttons -> panel with a
             * proportional fit so nothing clips; here there is slack, so the car grows. */
            float btn_cap  = big_car ? 26.0f : 28.0f;
            float bx = px + 8.0f, bw = pane_w - 16.0f;
            float top0   = pyr + 32.0f;                  /* below the car-name banner */
            float bottom = pyr + pane_h - 18.0f;
            const float CAR_GAP = 4.0f, ROW_GAP = 2.0f;
            /* 3 buttons (CAR/PAINT/OK) + 1 stat panel = 4 stacked rows below the car. */
            float avail  = (bottom - top0) - (CAR_GAP + 4.0f * ROW_GAP);
            const float PANEL_WANT = 31.0f, PANEL_FLOOR = 12.0f;
            const float BTN_MIN = 11.0f, CAR_MIN = 16.0f;
            float car_h  = pane_h * (big_car ? 0.44f : 0.32f);
            float bh     = btn_cap;
            float panel_h = PANEL_WANT;
            float over   = (car_h + 3.0f * bh + panel_h) - avail;
            float frame_scale, yy;
            if (over > 0.0f) { float g = car_h - CAR_MIN; if (g > over) { car_h -= over; over = 0.0f; } else { car_h = CAR_MIN; over -= g; } }
            if (over > 0.0f) { float g = 3.0f * (bh - BTN_MIN); if (g > over) { bh -= over / 3.0f; over = 0.0f; } else { bh = BTN_MIN; over -= g; } }
            if (over > 0.0f) { panel_h -= over; if (panel_h < PANEL_FLOOR) panel_h = PANEL_FLOOR; }
            { float total = car_h + 3.0f * bh + panel_h; if (total > avail && total > 0.0f) { float k = avail / total; car_h *= k; bh *= k; panel_h *= k; } }
            { float slack = avail - (car_h + 3.0f * bh + panel_h); if (slack > 0.0f) car_h += slack; }
            frame_scale = bh / 32.0f;
            if (frame_scale > 1.0f) frame_scale = 1.0f;
            if (frame_scale < 0.34f) frame_scale = 0.34f;
            yy = top0 + car_h + CAR_GAP;
            mp_simul_draw_pane_car(p, cx - (pane_w - 20.0f) * 0.5f, top0,
                                   pane_w - 20.0f, car_h, sx, sy);
            mp_simul_draw_pane_button(p, MP_BTN_CAR,   bx, yy, bw, bh, sx, sy); yy += bh + ROW_GAP;
            mp_simul_draw_pane_button(p, MP_BTN_PAINT, bx, yy, bw, bh, sx, sy); yy += bh + ROW_GAP;
            frontend_draw_car_stat_bars(bx, yy, bw, panel_h,
                                        s_mp_pane_spec[p][7], s_mp_pane_spec[p][8],
                                        s_mp_pane_spec_car[p], pcol, 1, frame_scale, sx, sy);
            yy += panel_h + ROW_GAP;
            mp_simul_draw_pane_button(p, MP_BTN_OK,    bx, yy, bw, bh, sx, sy);
        }

        /* STATS spec sheet overlays the car + menu (both kept semi-visible). */
        if (stats) mp_simul_render_stats(p, px, pyr, pane_w, pane_h, sx, sy);
    }

    /* All-ready beat banner. */
    if (s_mp_simul_ready_ms != 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad(0.0f, 227.0f * sy, 640.0f * sx, 26.0f * sy, 0xC0103018u, -1, 0, 0, 1, 1);
        fe_draw_text_centered(320.0f * sx, 232.0f * sy, "ALL READY - STARTING...", 0xFFFFFF80u, sx, sy);
    }

    /* [HOST CAR OPTIONS 2026-06-28] Bottom hint telling the host (slot 0) how to
     * raise the set-all-cars menu — only while no modal is up and the grid isn't
     * already counting down to the race. */
    if (!s_mp_host_menu_open && s_mp_simul_ready_ms == 0) {
        mp_simul_small_centered_fit(320.0f * sx, 470.0f * sy,
                                    "HOST: press X / TAB to set everyone's car",
                                    0xFFFFE060u, sx, sy, 560.0f * sx);
    }

    /* [HOST CAR OPTIONS 2026-06-28] Host-only modal: a full-screen scrim + centred
     * panel listing the four set-all-cars actions, the highlighted one drawn in a
     * translucent bar of the host's accent colour. Only the host drives it (input
     * is gated in frontend_mp_simul_carsel_update); everyone else is frozen. */
    if (s_mp_host_menu_open) {
        static const char *const k_host_opt[MP_HOST_OPT_COUNT] = {
            "GIVE EVERYONE MY CAR",
            "EVERYONE: RANDOM SLOW CAR",
            "EVERYONE: RANDOM AVERAGE CAR",
            "EVERYONE: RANDOM FAST CAR",
        };
        const float px0 = 130.0f, py0 = 135.0f, pw = 380.0f, ph = 210.0f;
        uint32_t argb = (uint32_t)s_mp_player_accent[0] & 0x00FFFFFFu;
        uint32_t acc  = argb | 0xFF000000u;
        int r;
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        /* Full-screen darken. */
        fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xD8000810u, -1, 0, 0, 1, 1);
        /* Panel body + accent border (top/bottom/left/right, 3px). */
        fe_draw_quad(px0 * sx, py0 * sy, pw * sx, ph * sy, 0xF00A1018u, -1, 0, 0, 1, 1);
        fe_draw_quad(px0 * sx, py0 * sy, pw * sx, 3.0f * sy, acc, -1, 0, 0, 1, 1);
        fe_draw_quad(px0 * sx, (py0 + ph - 3.0f) * sy, pw * sx, 3.0f * sy, acc, -1, 0, 0, 1, 1);
        fe_draw_quad(px0 * sx, py0 * sy, 3.0f * sx, ph * sy, acc, -1, 0, 0, 1, 1);
        fe_draw_quad((px0 + pw - 3.0f) * sx, py0 * sy, 3.0f * sx, ph * sy, acc, -1, 0, 0, 1, 1);
        /* Title. */
        mp_simul_small_centered_fit(320.0f * sx, (py0 + 8.0f) * sy, "SET EVERYONE'S CAR",
                                    acc, sx, sy, (pw - 24.0f) * sx);
        /* Option rows. */
        for (r = 0; r < MP_HOST_OPT_COUNT; r++) {
            float ry = py0 + 31.0f + (float)r * 32.0f;
            if (r == s_mp_host_menu_sel) {
                td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
                fe_draw_quad((px0 + 8.0f) * sx, ry * sy, (pw - 16.0f) * sx, 24.0f * sy,
                             argb | 0xC0000000u, -1, 0, 0, 1, 1);
            }
            mp_simul_small_centered_fit(320.0f * sx, (ry + 5.0f) * sy, k_host_opt[r],
                                        r == s_mp_host_menu_sel ? 0xFFFFFFFFu : 0xFFB8C2D0u,
                                        sx, sy, (pw - 28.0f) * sx);
        }
        /* Footer hints. */
        mp_simul_small_centered_fit(320.0f * sx, (py0 + ph - 38.0f) * sy, "UP / DOWN = CHOOSE",
                                    0xFF90A0B0u, sx, sy, (pw - 24.0f) * sx);
        mp_simul_small_centered_fit(320.0f * sx, (py0 + ph - 22.0f) * sy, "A = APPLY     B / X = CANCEL",
                                    0xFFFFE060u, sx, sy, (pw - 24.0f) * sx);
    }

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * Player-setup window (phase 0): per-pane NAME + background COLOUR entry.
 * Same grid layout as the car-select window. Keyboard players type directly;
 * pad players use an on-screen QWERTY. Colour uses the TD6 colour picker grid.
 * ======================================================================== */


/* Colour at a cell of the compact 16x16 background-colour palette (0xRRGGBB).
 * Hue runs across the columns; rows run light tint -> pure -> dark shade — with
 * NO fully-white top row (min saturation 0.35) and no pure-black bottom. */
uint32_t mp_setup_grid_color(int col, int row) {
    float hue = (float)col / (float)MP_COL_COLS;
    float t   = (float)row / (float)(MP_COL_ROWS - 1);   /* 0..1 */
    float sat, val;
    if (t < 0.5f) { sat = 0.35f + 1.30f * t; if (sat > 1.0f) sat = 1.0f; val = 1.0f; }
    else          { sat = 1.0f; val = 1.0f - 1.6f * (t - 0.5f); if (val < 0.20f) val = 0.20f; }
    return td6_hsv_to_rgb(hue, sat, val);
}




/* [2026-06-15 BUG #5] TD5RE_KBD_GRID (default ON; "0" reverts). The on-screen
 * QWERTY's pad nav (td5_fe_race.c) moves the cursor by COLUMN INDEX preserved
 * across rows (UP/DOWN keep `col`, clamped to the new row's length) — a pure grid
 * model. But the render USED to center each row by its own length
 * (rowx = ax + (aw - kw*len)*0.5f), so a given column index sat at a DIFFERENT
 * screen-x in each row (rows are 10/10/9/7 keys). The highlight (drawn at that
 * same index) therefore landed on a key that was NOT geometrically above/below
 * the previous selection — e.g. UP from 'C' (row3 col2) selected 'D' (row2 col2)
 * while the key visually above 'C' was 'F', so the move looked wrong AND the box
 * appeared over the "wrong" letter. Fix: align every row to a common left origin
 * with one fixed cell width, so column index -> identical x in every row. Now the
 * index-based nav steps to the key directly above/below and the highlight sits
 * exactly on the selected key. "0" restores the old centered (mismatched) rows. */
static int frontend_kbd_grid_align_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_KBD_GRID");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "On-screen QWERTY column-aligned grid (#5) %s (TD5RE_KBD_GRID=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* On-screen QWERTY for pad name entry — compact: each row is just over the
 * uppercase cap height (MP_KBD_ROW_H), width spans the pane content. */
static void mp_setup_render_kbd(int p, float ax, float ay, float aw, float ah,
                                uint32_t accent, float sx, float sy) {
    int row, col;
    int grid_align = frontend_kbd_grid_align_on();
    float rh = MP_KBD_ROW_H;
    (void)ah;
    for (row = 0; row < MP_KBD_LETTER_ROWS; row++) {
        const char *r = k_mp_kbd_rows[row];
        int len = (int)strlen(r);
        float kw = aw / 10.0f;
        /* [BUG #5] Left-align every row to a common origin so column index N is at
         * the SAME x in every row (matches the index-preserving pad nav). The old
         * per-row centering (kept under TD5RE_KBD_GRID=0) put the same col index at
         * a different x per row, desyncing nav target vs highlight. */
        float rowx = grid_align ? ax : ax + (aw - kw * (float)len) * 0.5f;
        float ry = ay + (float)row * rh;
        for (col = 0; col < len; col++) {
            float kx = rowx + (float)col * kw;
            int foc = (s_mp_kbd_row[p] == row && s_mp_kbd_col[p] == col);
            char ch[2]; ch[0] = r[col]; ch[1] = '\0';
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(kx * sx, ry * sy, (kw - 1.0f) * sx, (rh - 1.0f) * sy,
                         foc ? (accent | 0xFF000000u) : 0xB0203040u, -1, 0, 0, 1, 1);
            mp_simul_small_centered((kx + kw * 0.5f) * sx, (ry + (rh - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                                    ch, foc ? 0xFF101010u : 0xFFFFFFFFu, sx, sy);
        }
    }
    {
        static const char *const sp[3] = { "SPACE", "DEL", "DONE" };
        float kw = aw / 3.0f, ry = ay + (float)MP_KBD_SPECIAL * rh;
        for (col = 0; col < 3; col++) {
            float kx = ax + (float)col * kw;
            int foc = (s_mp_kbd_row[p] == MP_KBD_SPECIAL && s_mp_kbd_col[p] == col);
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(kx * sx, ry * sy, (kw - 1.0f) * sx, (rh - 1.0f) * sy,
                         foc ? (accent | 0xFF000000u) : 0xB0203040u, -1, 0, 0, 1, 1);
            mp_simul_small_centered((kx + kw * 0.5f) * sx, (ry + (rh - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                                    sp[col], foc ? 0xFF101010u : 0xFFFFFFFFu, sx, sy);
        }
    }
}

/* Compact 16x16 HSV background-colour palette. Height matches the keyboard. */
static void mp_setup_render_colorgrid(int p, float ax, float ay, float aw, float ah,
                                      float sx, float sy) {
    int r, c;
    float cw = aw / (float)MP_COL_COLS, ch = ah / (float)MP_COL_ROWS;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (r = 0; r < MP_COL_ROWS; r++) {
        for (c = 0; c < MP_COL_COLS; c++) {
            uint32_t col = mp_setup_grid_color(c, r);
            float kx = ax + (float)c * cw, ky = ay + (float)r * ch;
            fe_draw_quad(kx * sx, ky * sy, cw * sx + 0.5f, ch * sy + 0.5f,
                         frontend_rgb_to_bgra(col), -1, 0, 0, 1, 1);
        }
    }
    {
        float kx = ax + (float)s_mp_col_col[p] * cw, ky = ay + (float)s_mp_col_row[p] * ch;
        uint32_t hl = 0xFFFFFFFFu;
        float t = 1.5f;
        fe_draw_quad((kx - t) * sx, (ky - t) * sy, (cw + 2 * t) * sx, t * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx - t) * sx, (ky + ch) * sy, (cw + 2 * t) * sx, t * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx - t) * sx, (ky - t) * sy, t * sx, (ch + 2 * t) * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx + cw) * sx, (ky - t) * sy, t * sx, (ch + 2 * t) * sy, hl, -1, 0, 0, 1, 1);
    }
}

/* [#8] Render the split-screen POSITION picker: the cols x rows layout grid with
 * each occupied cell tinted in its player's accent + name, the empty cells shown
 * as "EMPTY", and a footer with controls + the host's layout selector. Each
 * player's OWN cell gets a thicker pulsing border so they can see where they are.
 * Reads s_mp_player_cell[] / accent / name / ready (no per-pane surfaces). */
void frontend_mp_position_render(float sx, float sy) {
    int p, c, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    int ncells, all_ready = 1;
    int owner[TD5_MAX_VIEWPORTS];
    float pulse = 0.55f + 0.45f * (float)((now / 60u) % 16u) / 15.0f; /* 0.55..1.0 */
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

    /* [layout 2026-06-19] DEAD PATH: the active CHOOSE YOUR SCREEN render is now
     * frontend_mp_position_render2 (td5_fe_race.c, wired at the TD5_SCREEN_MP_POSITION
     * case). The full-screen 0xC0101018 darkening scrim is removed here too so this
     * fallback can't reintroduce the dim if ever re-pointed. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_text_centered(320.0f * sx, 10.0f * sy, "CHOOSE YOUR SCREEN", 0xFFFFE060u, sx, sy);

    /* Layout grid occupies a centred area below the title, above the footer. */
    {
        const float gx = 40.0f, gy = 40.0f, gw = 560.0f, gh = 372.0f;
        float cw = gw / (float)cols, ch = gh / (float)rows;
        for (c = 0; c < ncells; c++) {
            int col = c % cols, row = c / cols;
            float px = gx + (float)col * cw, py = gy + (float)row * ch;
            float ccx = px + cw * 0.5f;
            int occ = owner[c];
            uint32_t rgb = (occ >= 0) ? ((uint32_t)s_mp_player_accent[occ] & 0x00FFFFFFu) : 0x303040u;
            int ready = (occ >= 0) && s_mp_player_ready[occ];
            char buf[40];

            /* cell fill (faint tint of the owner's colour). */
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, (ch - 6) * sy,
                         (occ >= 0) ? (rgb | 0x40000000u) : 0x40181820u, -1, 0, 0, 1, 1);

            /* border: this player's own cell pulses + is thick; others steady. */
            {
                float bt = (occ >= 0) ? 3.0f : 1.5f;
                uint32_t a = (occ >= 0)
                             ? ((uint32_t)(0x60 + (ready ? 0x9F : (int)(0x9F * pulse))) << 24)
                             : 0x80000000u;
                uint32_t bc = (rgb | a);
                fe_draw_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + 3) * sx, (py + ch - 3 - bt) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + 3) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + cw - 3 - bt) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
            }

            /* big cell number (1-based) so players can call out "I'm on 3". */
            snprintf(buf, sizeof buf, "%d", c + 1);
            fe_draw_text_centered(ccx * sx, (py + ch * 0.30f) * sy, buf,
                                  (occ >= 0) ? 0xFFFFFFFFu : 0xFF707080u, sx, sy);

            if (occ >= 0) {
                if (s_mp_player_name[occ][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[occ]);
                else                          snprintf(buf, sizeof buf, "PLAYER %d", occ + 1);
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy, buf,
                                        rgb | 0xFF000000u, sx, sy);
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 40.0f) * sy,
                                        ready ? "READY" : "MOVE: D-PAD", ready ? 0xFF40FF40u : 0xFFB0B0B0u,
                                        sx, sy);
            } else {
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy, "EMPTY",
                                        0xFF707080u, sx, sy);
            }
        }
    }

    /* Footer: controls + host layout selector + all-ready hint. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(0.0f, 420.0f * sy, 640.0f * sx, 60.0f * sy, 0xB0080810u, -1, 0, 0, 1, 1);
    {
        int lcnt = 1;
        const MpSplitLayout *opts = mp_split_layouts(n, &lcnt);
        char lbuf[64];
        const char *lname = (opts && s_mp_layout_sel >= 0 && s_mp_layout_sel < lcnt)
                            ? opts[s_mp_layout_sel].label : "SINGLE";
        if (lcnt > 1)
            snprintf(lbuf, sizeof lbuf, "P1 L/R: LAYOUT  [%s]", lname);
        else
            snprintf(lbuf, sizeof lbuf, "LAYOUT: %s", lname);
        fe_draw_text_centered(320.0f * sx, 426.0f * sy,
                              "D-PAD: MOVE   A: READY   B: BACK", 0xFFFFFFFFu, sx, sy);
        mp_simul_small_centered(320.0f * sx, 450.0f * sy, lbuf, 0xFFFFE060u, sx, sy);
        if (all_ready)
            mp_simul_small_centered(320.0f * sx, 464.0f * sy, "ALL READY - STARTING CARS...",
                                    0xFF80FF80u, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

void frontend_mp_setup_render(float sx, float sy) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    int caret = ((now / 400u) & 1u) != 0;
    float anim_t = 1.0f;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    (void)missing;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (s_inner_state == 0x20)
        anim_t = mp_simul_clamp01((float)(now - s_mp_simul_anim_ms) / (float)MP_SIMUL_ANIM_MS);

    /* [#3] Same capped/centred pane width as the car-select panes + fe_race
     * overlays (640/3 cap), so the name/colour row lines up underneath them. */
    float pane_w, row_x0 = 0.0f;
    frontend_mp_panel_capped(cols, &pane_w, &row_x0);
    /* [R1] Reserve a top band for the "PROFILE SELECTION" title so the panes start
     * BELOW it instead of overlapping the title text.
     * [R3-2 2026-06-19] The panes were still spanning all the way to y=480, so they
     * reached into the background art's lower text lines and sat tight under the
     * title. Grow the top band (more air under the title) AND reserve a matching
     * BOTTOM band so the boxes occupy a comfortable MIDDLE band — title clear above,
     * the art's 3 text lines clear below. Both bands MUST match the companion
     * fe_race.c profile-chip overlay (which positions PROFILE relative to the same
     * py/pane_h), or the chip drifts off the pane.
     * [R4 2026-06-19] Boxes were still too high (overlapping the title + the
     * background art's upper decoration bar). Use the shared FE_MP_TOP_BAND (85)
     * so the panes start below that line, applied consistently across every MP
     * screen. The companion fe_race.c profile-chip overlay uses the SAME literals. */
    const float mp_title_band  = FE_MP_TOP_BAND;
    const float mp_bottom_band = FE_MP_BOTTOM_BAND;
    float pane_h = (480.0f - mp_title_band - mp_bottom_band) / (float)rows;

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    /* [#18a] Profile/name-colour setup: by default DON'T darken the background so
     * the MainMenu art stays visible behind the panes. Set TD5RE_MP_SETUP_DIM=1 to
     * restore the old full-screen scrim. */
    {
        static int s_draw_dim = -1;
        if (s_draw_dim < 0)
            s_draw_dim = td5_env_flag_off("TD5RE_MP_SETUP_DIM");
        if (s_draw_dim)
            fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy,
                         ((uint32_t)(0xB0 * anim_t) << 24) | 0x101018u, -1, 0, 0, 1, 1);
    }

    /* [#18a] Standard top title (Lunatica face) so the setup step matches every
     * other menu's header. */
    if (td5_titlefont_ready())
        frontend_draw_screen_title("PROFILE SELECTION", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                                   0xFFE3D708u, sx, sy);

    for (p = 0; p < n; p++) {
        int col = p % cols, row = p / cols;
        float px = row_x0 + (float)col * pane_w, py = mp_title_band + (float)row * pane_h;
        float cx, pyr, pt, pe, rise, ax, ay, aw;
        uint32_t rgb = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;
        uint32_t pcol = rgb | 0xFF000000u;
        int ready = s_mp_player_ready[p];
        int sub = s_mp_setup_sub[p];
        int isk = (s_mp_join_device[p] == 0);
        char buf[64];

        pt = mp_simul_clamp01(anim_t * (1.0f + 0.12f * (float)n) - 0.10f * (float)p);
        pe = 1.0f - (1.0f - pt) * (1.0f - pt);
        rise = (1.0f - pe) * (pane_h * 0.45f);
        pyr = py + rise;
        cx  = px + pane_w * 0.5f;

        /* Backdrop + accent border. */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, (pane_h - 6) * sy,
                     ready ? 0xC0102818u : 0xB0141420u, -1, 0, 0, 1, 1);
        {
            float bt = ready ? 4.0f : 2.0f;
            uint32_t bc = rgb | (ready ? 0xFF000000u : 0xD0000000u);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + pane_h - 3 - bt) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + pane_w - 3 - bt) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
        }

        /* Header banner: chosen name or PLAYER N (host slot 0 gets the crown). */
        mp_draw_pane_name_banner(p, px, pyr, pane_w, cx, sx, sy);

        ax = px + 6.0f; ay = pyr + 22.0f; aw = pane_w - 12.0f;

        if (ready) {
            mp_simul_small_centered_fit(cx * sx, (pyr + pane_h * 0.42f) * sy, "READY", 0xFF40FF40u, sx, sy, (pane_w - 12.0f) * sx);
            snprintf(buf, sizeof buf, "%s", s_mp_player_name[p][0] ? s_mp_player_name[p] : "(no name)");
            mp_simul_small_centered_fit(cx * sx, (pyr + pane_h * 0.42f + 14.0f) * sy, buf, 0xFFFFFFFFu, sx, sy, (pane_w - 10.0f) * sx);
            /* [#16] Nudged up ~10px so the profile-management hint clears the pane
             * bottom edge / footer band. */
            mp_simul_small_centered_fit(cx * sx, (pyr + pane_h - 26.0f) * sy, "A = CHANGE   B = LOBBY",
                                    0xFFB0B0B0u, sx, sy, (pane_w - 8.0f) * sx);
            continue;
        }

        if (sub == 1) {                 /* NAME entry */
            /* name field box */
            float fy = ay;
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(ax * sx, fy * sy, aw * sx, 16.0f * sy, 0xC0000814u, -1, 0, 0, 1, 1);
            snprintf(buf, sizeof buf, "%s%s", s_mp_player_name[p], caret ? "_" : " ");
            fe_draw_small_text((ax + 4.0f) * sx, (fy + (16.0f - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                               buf, 0xFFFFFFFFu, sx, sy);
            if (isk)
                mp_simul_small_centered_fit(cx * sx, (fy + 24.0f) * sy,
                                        "TYPE NAME - ENTER=DONE  ESC=BACK", 0xFFFFE060u, sx, sy, (pane_w - 8.0f) * sx);
            else {
                mp_setup_render_kbd(p, ax, fy + 19.0f, aw, MP_KBD_BLOCK_H, pcol, sx, sy);
                /* [#15c] Pad hints below the on-screen keyboard (inside the pane). */
                mp_simul_small_centered_fit(cx * sx, (fy + 19.0f + MP_KBD_BLOCK_H + 3.0f) * sy,
                                        "X = DELETE   START = DONE", 0xFFB0B0B0u, sx, sy, (pane_w - 8.0f) * sx);
            }
            continue;
        }

        if (sub == 2) {                 /* COLOUR picker (compact 16x16) */
            mp_setup_render_colorgrid(p, ax, ay + 2.0f, aw, MP_KBD_BLOCK_H, sx, sy);
            mp_simul_small_centered_fit(cx * sx, (ay + 2.0f + MP_KBD_BLOCK_H + 3.0f) * sy,
                                    "A=OK  B=BACK", 0xFFFFE060u, sx, sy, (pane_w - 8.0f) * sx);
            continue;
        }

        /* idle: NAME / COLOUR / OK buttons.
         * [#3 2026-06-15] When profile management is enabled the band has a 4th
         * slot for the PROFILE button (drawn by fe_race's
         * frontend_mp_setup_profile_render); reserve its row here and pin OK to
         * the LAST slot so the nav band lines up with the profile overlay. */
        {
            extern int mp_profiles_enabled(void);   /* defined in td5_fe_race.c */
            int pon = mp_profiles_enabled();
            /* [LANE ASSIST 2026-06-28] Band now carries 6 rows with profiles on
             * (NAME, COLOUR, PROFILE, AUTO/MANUAL, ASSIST, OK) or 5 without (PROFILE
             * absent). PROFILE (slot 2) is drawn by frontend_mp_setup_profile_render;
             * its slot math (room/6) must match this band. */
            int slots = pon ? 6 : 5;
            int trans_slot = pon ? 3 : 2;           /* TRANS row index in the band */
            float bx = px + 8.0f, bw = pane_w - 16.0f;
            float bsy = ay + 4.0f;
            float room = (pyr + pane_h - 12.0f) - bsy;
            float bh = room / (float)slots - 3.0f;
            int focus = s_mp_setup_btn[p];
            float yy = bsy;
            if (bh < 12.0f) bh = 12.0f;
            if (bh > 26.0f) bh = 26.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "NAME", focus == MP_SET_NAME, pcol, 0,
                              s_mp_player_name[p][0] ? s_mp_player_name[p] : "-", -1, sx, sy);
            yy += bh + 3.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "COLOUR", focus == MP_SET_COLOUR, pcol, 0,
                              NULL, s_mp_player_accent[p], sx, sy);
            /* PROFILE (slot 2, profiles-on only) is drawn by frontend_mp_setup_profile_render. */
            yy = bsy + (float)trans_slot * (bh + 3.0f);
            mp_simul_draw_btn(bx, yy, bw, bh, s_mp_player_trans[p] ? "MANUAL" : "AUTOMATIC",
                              focus == MP_SET_TRANS, pcol, 0, NULL, -1, sx, sy);
            yy += bh + 3.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, s_mp_player_laneassist[p] ? "ASSIST ON" : "ASSIST OFF",
                              focus == MP_SET_LANEASSIST, pcol, 0, NULL, -1, sx, sy);
            yy = bsy + (float)(slots - 1) * (bh + 3.0f);
            mp_simul_draw_btn(bx, yy, bw, bh, "OK", focus == MP_SET_OK, pcol, 0, NULL, -1, sx, sy);
        }
    }

    if (s_mp_simul_ready_ms != 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad(0.0f, 227.0f * sy, 640.0f * sx, 26.0f * sy, 0xC0103018u, -1, 0, 0, 1, 1);
        fe_draw_text_centered(320.0f * sx, 232.0f * sy, "ALL READY - CHOOSE CARS...", 0xFFFFFF80u, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}
