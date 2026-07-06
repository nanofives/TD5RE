/* ========================================================================
 * td5_selftest.c — in-session automated test suite (PORT-ONLY, DEV-ONLY)
 *
 * A frame-ticked "test director" hooked at the top of td5_game_tick().
 * In ONE process run it:
 *
 *   Phase A (screens): jumps through a curated list of frontend screens
 *     (td5_frontend_set_screen — the StartScreen harness path), waits for
 *     each to settle, and records the nav-reachability selftest result
 *     (frontend_nav_selftest_maybe, enabled via TD5RE_NAV_SELFTEST),
 *     active button count, and WARN/ERROR log deltas. Post-race screens
 *     (23..27) get fabricated results via td5_game_inject_demo_results().
 *
 *   Phase B (races): runs a matrix of scripted races by mutating the same
 *     g_td5.ini fields td5_frontend_auto_race_setup() consumes and
 *     re-arming ini.auto_race — the real MENU-state launch path. Each race
 *     runs N sim ticks under AutoThrottle (+ trace fast-forward), then
 *     exits via td5_game_selftest_end_race() (the pause-menu QUIT TO MENU
 *     sequence). Repeated identical Moscow races early and late in the
 *     matrix form the DEGRADATION reference series: working set, private
 *     bytes, GDI/USER objects, handle count, game-heap alloc balance and
 *     frame times are compared across repeats and drift beyond the
 *     (env-tunable) thresholds fails the suite.
 *
 *   Report: log/selftest_report.csv (one row per step, all metrics) +
 *     log/selftest_report.md (PASS/WARN/FAIL table + degradation
 *     verdicts). WinMain returns td5_selftest_exit_code() — nonzero on
 *     any FAIL — so scripts/CI can gate on the process exit code.
 *
 * Watchdogs at every wait state plus a global suite deadline guarantee
 * the process always terminates (CI can never hang on a wedged race).
 *
 * Enable: [SelfTest] Enabled=1 or --SelfTest=1 (+ --SelfTestSuite=1 for
 * the full matrix). Whole module is compiled out under TD5RE_RELEASE.
 * ======================================================================== */
#ifndef TD5RE_RELEASE

#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5re.h"
#include "td5_types.h"
#include "td5_selftest.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_game.h"
#include "td5_frontend.h"
#include "td5_trace.h"

#define LOG_TAG "selftest"

/* ------------------------------------------------------------------------
 * Step records
 * ---------------------------------------------------------------------- */

enum { ST_PASS = 0, ST_WARN = 1, ST_FAIL = 2 };

typedef struct {
    char     name[48];
    char     kind;            /* 'S' = screen, 'R' = race, 'D' = degradation verdict */
    int      status;          /* ST_PASS / ST_WARN / ST_FAIL */
    char     note[120];
    unsigned load_ms;         /* race: menu->race load; screen: ms to settle */
    unsigned run_ms;          /* total step wall time */
    unsigned frames;          /* frames sampled for the histogram */
    float    avg_ms, p95_ms;
    unsigned max_ms;
    int      sim_ticks;       /* race: sim ticks executed */
    int      buttons;         /* screen: active+enabled buttons */
    int      nav_reached, nav_navigable;   /* screen: nav-selftest result (-1 = n/a) */
    /* resource snapshot at step end + log deltas across the step */
    unsigned long long ws, ws_peak, priv_b;
    unsigned gdi, user_obj, handles, heap_allocs, heap_frees;
    unsigned d_warns, d_errs;
    int      repeat_group;    /* >0: member of a degradation series */
    int      finished;        /* row closed — a later recovery path must not
                               * overwrite a FAIL verdict (exit code already
                               * latched; a PASS rewrite would contradict it) */
} StepRow;

#define ST_MAX_ROWS 96
static StepRow  s_rows[ST_MAX_ROWS];
static int      s_row_count;

/* ------------------------------------------------------------------------
 * Scenario tables
 * ---------------------------------------------------------------------- */

/* -1 (or -2 where -1 is meaningful) = keep the boot-time base value. */
typedef struct {
    const char *name;
    int track;            /* frontend track index (TD6 conversions live at 26+) */
    int car;              /* -1 = base */
    int reverse;          /* -1 = base, 0/1 */
    int dynamics;         /* -1 = base, 0 = arcade, 1 = simulation */
    int traffic, cops;    /* -1 = base */
    int laps;             /* -1 = base */
    int opponents;        /* -2 = base (-1 itself means "full grid") */
    int spectate;         /* -1 = base, N = AI spectator panes */
    int player_is_ai;     /* -1 = base */
    int auto_throttle;    /* -1 = base (base forces 1) */
    int natural_finish;   /* 1 = run to the real finish (results screen) */
    int repeat_group;     /* >0 = degradation series id */
    int trace_golden;     /* 1 = golden-trace scenario: fixed-seed RaceTrace on,
                           * per-tick CSVs hashed vs trace_goldens.txt (table
                           * rows omitting the field default to 0) */
} RaceScenario;

/* Order matters: smoke = first ST_SMOKE_RACES entries; the late Moscow
 * repeats close the degradation loop AFTER the varied middle of the matrix;
 * the spectate scenario runs LAST so its split-screen pane layout can't
 * leak into any row another verdict depends on. */
static const RaceScenario k_races[] = {
    /* name                 trk car rev dyn  tr cop lap  opp spec pia at nat grp */
    { "race-moscow-base",     0, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 1 },
    { "race-moscow-rep2",     0, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 1 },
    { "race-moscow-rep3",     0, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 1 },
    { "race-newcastle-circ",  5, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 0 },
    { "race-moscow-reverse",  0, -1,  1, -1, -1, -1, -1, -2, -1, -1, -1, 0, 0 },
    /* ---- end of smoke tier ---- */
    { "race-td6-pelton",     26, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 0 },
    { "race-td6-paris",      32, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 0 },
    { "race-keswick",        10, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 0 },
    /* Drag runs SOLO (opponents=0): the real drag lobby never fields AI, and
     * a full AutoRace grid parks 5 cars against the extended-strip walls
     * (bogus wall-contact storm — see the wall_reject sampler in td5_track).
     * KNOWN ISSUE (selftest finding 2026-07-02): a natural drag finish never
     * fires under AutoRace (300 s timeout; the lobby-config path presumably
     * seeds the finish/extension globals the INI path doesn't) — so this
     * runs on the tick budget like the other scenarios until that's fixed. */
    { "race-drag-solo",      19, -1,  0, -1, -1, -1, -1,  0, -1, -1, -1, 0, 0 },
    { "race-arcade-tr-cops",  2, -1,  0,  0,  1,  1, -1, -2, -1, -1, -1, 0, 0 },
    { "race-ai-slot0",        0, -1,  0, -1, -1, -1, -1, -2, -1,  1,  0, 0, 0 },
    { "race-moscow-late1",    0, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 1 },
    { "race-moscow-late2",    0, -1,  0, -1, -1, -1, -1, -2, -1, -1, -1, 0, 1 },
    /* [TRACE GOLDEN 2026-07-06] Deterministic sim-regression net: fixed
     * RaceTrace seed (0x1A2B3C4D) + every sim-relevant knob pinned (the
     * columns here + st_golden_begin for ini knobs the columns don't cover),
     * AI-driven player, per-tick trace CSVs hashed vs trace_goldens.txt.
     * One TD5 track + one TD6 conversion. Kept BEFORE spectate3 (its pane
     * layout must stay last / leak-free). */
    { "race-golden-moscow",   0,  0,  0,  1,  2,  0,  2,  5, -1,  1,  0, 0, 0, 1 },
    { "race-golden-pelton",  26,  0,  0,  1,  2,  0,  2,  5, -1,  1,  0, 0, 0, 1 },
    { "race-spectate3",       0, -1,  0, -1, -1, -1, -1, -2,  3, -1, -1, 0, 0 },
};
#define ST_RACE_COUNT  ((int)(sizeof(k_races) / sizeof(k_races[0])))
#define ST_SMOKE_RACES 5

typedef struct {
    const char *name;
    int screen;
    int inject_results;   /* 1 = fabricate finished-race data first (23..27) */
    int allow_redirect;   /* 1 = bouncing to another screen is expected
                           * (screens whose case-0 requires context the
                           * injection can't fabricate, e.g. a live cup) */
} ScreenStep;

static const ScreenStep k_screens_full[] = {
    { "scr-main-menu",       TD5_SCREEN_MAIN_MENU,          0 },
    /* scr-language / scr-legal removed 2026-07-03 — screens retired (table
     * slots NULL, set_screen redirects to MAIN_MENU). */
    { "scr-race-type",       TD5_SCREEN_RACE_TYPE_MENU,     0 },
    { "scr-quick-race",      TD5_SCREEN_QUICK_RACE,         0 },
    { "scr-options-hub",     TD5_SCREEN_OPTIONS_HUB,        0 },
    { "scr-game-options",    TD5_SCREEN_GAME_OPTIONS,       0 },
    { "scr-control-options", TD5_SCREEN_CONTROL_OPTIONS,    0 },
    { "scr-sound-options",   TD5_SCREEN_SOUND_OPTIONS,      0 },
    { "scr-display-options", TD5_SCREEN_DISPLAY_OPTIONS,    0 },
    { "scr-two-player-opts", TD5_SCREEN_TWO_PLAYER_OPTIONS, 0 },
    { "scr-ctrl-binding",    TD5_SCREEN_CONTROLLER_BINDING, 0 },
    { "scr-music-test",      TD5_SCREEN_MUSIC_TEST,         0 },
    { "scr-car-select",      TD5_SCREEN_CAR_SELECTION,      0 },
    { "scr-track-select",    TD5_SCREEN_TRACK_SELECTION,    0 },
    { "scr-extras-gallery",  TD5_SCREEN_EXTRAS_GALLERY,     0 },
    { "scr-high-score",      TD5_SCREEN_HIGH_SCORE,         1 },
    { "scr-race-results",    TD5_SCREEN_RACE_RESULTS,       1 },
    { "scr-name-entry",      TD5_SCREEN_NAME_ENTRY,         1 },
    { "scr-cup-failed",      TD5_SCREEN_CUP_FAILED,         1, 1 },
    { "scr-cup-won",         TD5_SCREEN_CUP_WON,            1, 1 },
    { "scr-mp-lobby",        TD5_SCREEN_MP_LOBBY,           0 },
    { "scr-changelog",       TD5_SCREEN_CHANGELOG,          0 },
    { "scr-pending-test",    TD5_SCREEN_PENDING_TEST,       0 },
    { "scr-ui-guide",        TD5_SCREEN_UI_GUIDE,           0 },
    { "scr-mp-guide",        TD5_SCREEN_MP_GUIDE,           0 },
};
#define ST_SCREEN_COUNT_FULL ((int)(sizeof(k_screens_full) / sizeof(k_screens_full[0])))
#define ST_SMOKE_SCREENS 3   /* main menu, language, legal — plus races cover results */

/* ------------------------------------------------------------------------
 * Director state
 * ---------------------------------------------------------------------- */

typedef enum {
    PH_OFF = 0,
    PH_WAIT_FIRST_MENU,
    PH_SCREENS,
    PH_RACES,
    PH_REPORT,
    PH_DONE
} Phase;

typedef enum {              /* sub-state within one step */
    SS_ENTER = 0,           /* apply the step's setup on the next eligible frame */
    SS_SCREEN_SETTLE,       /* wait for the jumped-to screen to settle */
    SS_SCREEN_DWELL,        /* fixed dwell so the nav selftest fires + renders */
    SS_RACE_WAIT_START,     /* auto_race armed; wait for GAMESTATE_RACE */
    SS_RACE_RUNNING,        /* sample frames until the tick budget is spent */
    SS_RACE_WAIT_MENU,      /* fade-out in flight; wait for GAMESTATE_MENU */
    SS_RACE_POST_MENU       /* teardown settle frames, then record the row */
} SubState;

static int       s_active;
static int       s_exit_code;
static Phase     s_phase;
static SubState  s_sub;
static int       s_step;             /* index within the current phase's table */
static int       s_n_screens, s_n_races;

static uint32_t  s_suite_start_ms, s_suite_deadline_ms;
static uint32_t  s_step_start_ms, s_step_deadline_ms;
static uint32_t  s_race_launch_ms, s_race_start_ms;
static int       s_race_start_tick, s_last_sim_tick;
static int       s_settle_frames, s_dwell_frames;
static unsigned  s_base_warns, s_base_errs;   /* log counters at step start */

/* frame-time histogram (ms, clamped to 255) for the RUNNING window */
static unsigned  s_histo[256];
static unsigned  s_histo_n, s_histo_max;
static unsigned long long s_histo_sum;
static uint32_t  s_last_frame_ms;

/* base (boot-time) copies of every INI field a scenario may touch */
static struct {
    int car, game_type, dynamics, traffic, cops, laps;
    int opponents, players, spectate, player_is_ai, auto_throttle;
    float fast_forward;
} s_base;

/* env-tunable knobs (read once at boot) */
static float s_ff;                 /* race fast-forward multiplier */
static int   s_ws_mb_per_rep;      /* allowed WS growth per repeat, MB */
static int   s_frame_drift_pct;    /* allowed avg-frame-ms drift, % */
static int   s_gdi_growth;         /* allowed net GDI-object growth over a series */
static int   s_handle_growth;      /* allowed net handle growth over a series */
static int   s_allow_errors;       /* 1 = ERR log lines don't fail a step */
static int   s_race_ticks;         /* sim ticks per scripted race */

/* ------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void st_sample_resources(StepRow *r)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        r->ws      = (unsigned long long)pmc.WorkingSetSize;
        r->ws_peak = (unsigned long long)pmc.PeakWorkingSetSize;
        r->priv_b  = (unsigned long long)pmc.PrivateUsage;
    }
    r->gdi      = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    r->user_obj = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    {
        DWORD hc = 0;
        GetProcessHandleCount(GetCurrentProcess(), &hc);
        r->handles = (unsigned)hc;
    }
    td5_plat_heap_stats(&r->heap_allocs, &r->heap_frees);
}

static void st_histo_reset(void)
{
    memset(s_histo, 0, sizeof(s_histo));
    s_histo_n = 0;
    s_histo_max = 0;
    s_histo_sum = 0;
    s_last_frame_ms = 0;
}

static void st_histo_add(uint32_t now)
{
    if (s_last_frame_ms) {
        uint32_t dt = now - s_last_frame_ms;
        unsigned bucket = (dt > 255) ? 255 : (unsigned)dt;
        s_histo[bucket]++;
        s_histo_n++;
        s_histo_sum += dt;
        if (dt > s_histo_max) s_histo_max = dt;
    }
    s_last_frame_ms = now;
}

static void st_histo_stats(float *avg, float *p95, unsigned *mx)
{
    *avg = s_histo_n ? (float)((double)s_histo_sum / (double)s_histo_n) : 0.0f;
    *mx  = s_histo_max;
    *p95 = 0.0f;
    if (s_histo_n) {
        unsigned target = (unsigned)((double)s_histo_n * 0.95);
        unsigned cum = 0;
        int i;
        for (i = 0; i < 256; i++) {
            cum += s_histo[i];
            if (cum >= target) { *p95 = (float)i; break; }
        }
    }
}

static StepRow *st_new_row(const char *name, char kind)
{
    StepRow *r;
    if (s_row_count >= ST_MAX_ROWS) return NULL;
    r = &s_rows[s_row_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->kind = kind;
    r->nav_reached = r->nav_navigable = -1;
    return r;
}

static void st_finish_row(StepRow *r, int status, const char *note)
{
    unsigned w = 0, e = 0;
    uint32_t now = td5_plat_time_ms();
    if (!r || r->finished) return;
    r->finished = 1;
    td5_plat_log_counts(&w, &e);
    r->d_warns = w - s_base_warns;
    r->d_errs  = e - s_base_errs;
    r->run_ms  = now - s_step_start_ms;
    /* New ERR lines during the step upgrade the verdict unless allowed.
     * WARN lines are recorded (d_warns column) but do NOT drive status —
     * several port subsystems have chatty per-frame warns (e.g. the minimap
     * span tracker) that would otherwise mark every race WARN. */
    if (r->d_errs > 0 && !s_allow_errors && status < ST_FAIL) {
        status = ST_FAIL;
        if (!note || !note[0]) note = "log errors during step";
    }
    r->status = status;
    if (note) snprintf(r->note, sizeof(r->note), "%s", note);
    st_sample_resources(r);
    if (status == ST_FAIL) s_exit_code = 1;
    TD5_LOG_I(LOG_TAG, "step %-24s %s  (%ums, dWARN=%u dERR=%u)",
              r->name, status == ST_PASS ? "PASS" : status == ST_WARN ? "WARN" : "FAIL",
              r->run_ms, r->d_warns, r->d_errs);
}

static void st_begin_step(const char *label, uint32_t timeout_ms)
{
    unsigned w = 0, e = 0;
    char title[192];
    uint32_t now = td5_plat_time_ms();
    s_step_start_ms = now;
    s_step_deadline_ms = now + timeout_ms;
    td5_plat_log_counts(&w, &e);
    s_base_warns = w;
    s_base_errs  = e;
    snprintf(title, sizeof(title), "TD5RE SELFTEST %d/%d: %s",
             s_row_count + 1, s_n_screens + s_n_races, label);
    td5_plat_set_window_title(title);
    TD5_LOG_I(LOG_TAG, "=== step %d/%d: %s ===",
              s_row_count + 1, s_n_screens + s_n_races, label);
}

/* Restore every scenario-touched INI field to its boot-time base value. */
static void st_reset_scenario_fields(void)
{
    g_td5.ini.default_car       = s_base.car;
    g_td5.ini.default_game_type = s_base.game_type;
    g_td5.ini.default_reverse   = 0;
    g_td5.ini.dynamics          = s_base.dynamics;
    g_td5.ini.traffic           = s_base.traffic;
    g_td5.ini.cops              = s_base.cops;
    g_td5.ini.laps              = s_base.laps;
    g_td5.ini.default_opponents = s_base.opponents;
    g_td5.ini.default_players   = s_base.players;
    g_td5.ini.spectate_screens  = s_base.spectate;
    g_td5.ini.player_is_ai      = s_base.player_is_ai;
    g_td5.ini.auto_throttle     = s_base.auto_throttle;
    g_td5.ini.trace_fast_forward = s_base.fast_forward;
}

/* ------------------------------------------------------------------------
 * [TRACE GOLDEN 2026-07-06] Golden-trace regression net
 *
 * A trace_golden scenario runs with the RaceTrace harness on (fixed CRT seed
 * 0x1A2B3C4D — see InitRace step 0) and every sim-relevant knob pinned, so
 * the per-tick CSVs are byte-deterministic. At race end each canonical CSV
 * is hashed (FNV-1a 64, skipping the render-dependent leading `frame`
 * column, capped at ST_GOLDEN_TICKS sim ticks) and compared against the
 * tracked goldens file. A mismatch means the SIM CHANGED — either a real
 * regression, or an intentional physics/AI change that must re-record via
 * TD5RE_TRACE_GOLDEN_UPDATE=1 (rewrites the file; commit it with the change).
 * ---------------------------------------------------------------------- */

#define ST_GOLDEN_MODULES (TD5_TRACE_MOD_POSE | TD5_TRACE_MOD_MOTION | \
                           TD5_TRACE_MOD_TRACK | TD5_TRACE_MOD_CONTROLS | \
                           TD5_TRACE_MOD_PROGRESS)
#define ST_GOLDEN_TICKS   300
#define ST_GOLDEN_FILE    "td5mod/src/td5re/trace_goldens.txt"

static const struct { unsigned mask; const char *suffix; } k_golden_mods[] = {
    { TD5_TRACE_MOD_POSE,     "pose"     },
    { TD5_TRACE_MOD_MOTION,   "motion"   },
    { TD5_TRACE_MOD_TRACK,    "track"    },
    { TD5_TRACE_MOD_CONTROLS, "controls" },
    { TD5_TRACE_MOD_PROGRESS, "progress" },
};
#define ST_GOLDEN_MOD_COUNT ((int)(sizeof(k_golden_mods) / sizeof(k_golden_mods[0])))

static struct {
    int car_damage, toughness, deform, difficulty, lane_assist;
} s_golden_saved;
static char s_golden_lines[32][96];   /* recorded "<scenario> <mod> <hash>" */
static int  s_golden_line_count;
static int  s_golden_ran, s_golden_active;

/* Pin the sim-relevant ini knobs the scenario columns don't cover, then
 * restart the trace harness so its CSVs reopen fresh for THIS race only. */
static void st_golden_begin(void)
{
    s_golden_saved.car_damage  = g_td5.ini.car_damage;
    s_golden_saved.toughness   = g_td5.ini.car_damage_toughness;
    s_golden_saved.deform      = g_td5.ini.car_damage_deform;
    s_golden_saved.difficulty  = g_td5.ini.difficulty;
    s_golden_saved.lane_assist = g_td5.ini.lane_assist;
    g_td5.ini.car_damage           = 1;
    g_td5.ini.car_damage_toughness = 1;
    g_td5.ini.car_damage_deform    = 1;
    g_td5.ini.difficulty           = 1;
    g_td5.ini.lane_assist          = 0;

    g_td5.ini.race_trace_enabled       = 1;
    g_td5.ini.trace_module_mask        = ST_GOLDEN_MODULES;
    /* Tick-paced stages ONLY: FRAME_BEGIN/FRAME_END fire once per RENDER
     * frame (several per sim tick, count varying with fps run-to-run), which
     * made the progress module's row count nondeterministic across identical
     * sim runs. PAUSE_MENU never fires under AutoRace but is real-time-paced
     * too, so it's excluded on principle. */
    g_td5.ini.trace_stage_mask         = TD5_TRACE_STG_ALL &
        ~(TD5_TRACE_STG_FRAME_BEGIN | TD5_TRACE_STG_FRAME_END |
          TD5_TRACE_STG_PAUSE_MENU);
    g_td5.ini.race_trace_slot          = -1;
    g_td5.ini.race_trace_max_frames    = 1 << 30;
    g_td5.ini.race_trace_max_sim_ticks = 0;   /* never the quit-request path */
    td5_trace_shutdown();
    td5_trace_init();                         /* reopen ("w") = clean CSVs */
    s_golden_active = 1;
}

/* Undo st_golden_begin (also the abort path when the race never started). */
static void st_golden_end(void)
{
    if (!s_golden_active) return;
    td5_trace_shutdown();                     /* flush + close the CSVs */
    g_td5.ini.race_trace_enabled = 0;
    td5_trace_init();                         /* trace off again (no-op) */
    g_td5.ini.car_damage           = s_golden_saved.car_damage;
    g_td5.ini.car_damage_toughness = s_golden_saved.toughness;
    g_td5.ini.car_damage_deform    = s_golden_saved.deform;
    g_td5.ini.difficulty           = s_golden_saved.difficulty;
    g_td5.ini.lane_assist          = s_golden_saved.lane_assist;
    s_golden_active = 0;
}

/* FNV-1a 64 over the data rows of one trace CSV, minus each row's leading
 * `frame` column (render-cadence-dependent), capped at ST_GOLDEN_TICKS sim
 * ticks past the first row's tick (tick numbering is absolute-per-race but
 * starts wherever countdown left it). Returns 0 if the file is missing. */
static int st_golden_hash_csv(const char *path, unsigned long long *out)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    unsigned long long h = 1469598103934665603ULL;
    int first_tick = -1, rows = 0;
    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }   /* header */
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ',');
        int tick;
        if (!p) continue;
        tick = atoi(p + 1);
        if (first_tick < 0) first_tick = tick;
        if (tick - first_tick >= ST_GOLDEN_TICKS) break;
        for (p = p + 1; *p && *p != '\n' && *p != '\r'; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= (unsigned char)'\n';
        h *= 1099511628211ULL;
        rows++;
    }
    fclose(f);
    if (!rows) return 0;
    *out = h;
    return 1;
}

/* Look up "<scenario> <suffix> <hex>" in the tracked goldens file. */
static int st_golden_lookup(const char *scenario, const char *suffix,
                            unsigned long long *out)
{
    FILE *f = fopen(ST_GOLDEN_FILE, "r");
    char line[160], sc[64], mod[32];
    unsigned long long h;
    int found = 0;
    if (!f) return -1;   /* no goldens file at all */
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%63s %31s %llx", sc, mod, &h) == 3 &&
            strcmp(sc, scenario) == 0 && strcmp(mod, suffix) == 0) {
            *out = h;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Hash the CSVs of a finished golden race, judge vs the goldens file, and
 * append the 'G' verdict row. In update mode collect lines and rewrite the
 * file once the LAST golden scenario has run. */
static void st_golden_finish(const RaceScenario *sc)
{
    const char *env = getenv("TD5RE_TRACE_GOLDEN_UPDATE");
    int update = (env && *env && *env != '0');
    int golden_total = 0, i;
    int status = ST_PASS;
    char note[120] = "";
    size_t noff = 0;
    StepRow *r;

    st_golden_end();   /* flush CSVs + restore knobs BEFORE hashing */
    s_golden_ran++;
    for (i = 0; i < ST_RACE_COUNT; i++) golden_total += k_races[i].trace_golden;

    r = st_new_row(sc->name, 'G');
    for (i = 0; i < ST_GOLDEN_MOD_COUNT; i++) {
        char path[640];
        unsigned long long have = 0, want = 0;
        int lk;
        snprintf(path, sizeof(path), "%srace_trace_%s.csv",
                 td5_plat_log_dir(), k_golden_mods[i].suffix);
        if (!st_golden_hash_csv(path, &have)) {
            status = ST_FAIL;
            noff += snprintf(note + noff, sizeof(note) - noff, "%s: no rows; ",
                             k_golden_mods[i].suffix);
            continue;
        }
        if (update) {
            if (s_golden_line_count < 32) {
                snprintf(s_golden_lines[s_golden_line_count++],
                         sizeof(s_golden_lines[0]), "%s %s %016llx",
                         sc->name, k_golden_mods[i].suffix, have);
            }
            continue;
        }
        lk = st_golden_lookup(sc->name, k_golden_mods[i].suffix, &want);
        if (lk <= 0) {
            if (status < ST_WARN) status = ST_WARN;
            noff += snprintf(note + noff, sizeof(note) - noff,
                             "%s: no golden; ", k_golden_mods[i].suffix);
        } else if (have != want) {
            status = ST_FAIL;
            noff += snprintf(note + noff, sizeof(note) - noff,
                             "%s: %016llx != golden %016llx; ",
                             k_golden_mods[i].suffix, have, want);
        }
        if (noff >= sizeof(note)) noff = sizeof(note) - 1;
    }

    if (update) {
        snprintf(note, sizeof(note), "goldens recorded (%d/%d scenarios)",
                 s_golden_ran, golden_total);
        if (s_golden_ran >= golden_total) {
            FILE *f = fopen(ST_GOLDEN_FILE, "w");
            if (f) {
                fputs("# Golden trace hashes -- regenerate with:\n"
                      "#   TD5RE_TRACE_GOLDEN_UPDATE=1 pwsh scripts/selftest.ps1 -Suite full\n"
                      "# Commit this file together with any INTENTIONAL sim change.\n"
                      "# <scenario> <module> <fnv1a64 of trace rows, frame column stripped>\n", f);
                for (i = 0; i < s_golden_line_count; i++)
                    fprintf(f, "%s\n", s_golden_lines[i]);
                fclose(f);
            } else {
                snprintf(note, sizeof(note), "cannot write %s", ST_GOLDEN_FILE);
                status = ST_FAIL;
            }
        }
    } else if (status == ST_PASS) {
        snprintf(note, sizeof(note), "%d module hashes match goldens",
                 ST_GOLDEN_MOD_COUNT);
    }
    st_finish_row(r, status, note);
}

static void st_apply_scenario(const RaceScenario *sc)
{
    st_reset_scenario_fields();
    g_td5.ini.default_track   = sc->track;
    g_td5.ini.default_reverse = (sc->reverse > 0) ? 1 : 0;
    if (sc->car        >= 0) g_td5.ini.default_car       = sc->car;
    if (sc->dynamics   >= 0) g_td5.ini.dynamics          = sc->dynamics;
    if (sc->traffic    >= 0) g_td5.ini.traffic           = sc->traffic;
    if (sc->cops       >= 0) g_td5.ini.cops              = sc->cops;
    if (sc->laps       >= 0) g_td5.ini.laps              = sc->laps;
    if (sc->opponents  >= -1) g_td5.ini.default_opponents = sc->opponents;
    if (sc->spectate   >= 0) g_td5.ini.spectate_screens  = sc->spectate;
    if (sc->player_is_ai >= 0) g_td5.ini.player_is_ai    = sc->player_is_ai;
    if (sc->auto_throttle >= 0) g_td5.ini.auto_throttle  = sc->auto_throttle;
    g_td5.ini.trace_fast_forward = s_ff;
    if (sc->trace_golden) st_golden_begin();
}

/* ------------------------------------------------------------------------
 * Report
 * ---------------------------------------------------------------------- */

static const char *st_status_str(int s)
{
    return s == ST_PASS ? "PASS" : s == ST_WARN ? "WARN" : "FAIL";
}

/* Compare the repeat-group race rows and append 'D' verdict rows. */
static void st_degradation_verdicts(void)
{
    int gi;
    for (gi = 1; gi <= 4; gi++) {
        StepRow *series[ST_MAX_ROWS];
        int n = 0, i;
        for (i = 0; i < s_row_count; i++)
            if (s_rows[i].kind == 'R' && s_rows[i].repeat_group == gi &&
                s_rows[i].status != ST_FAIL)
                series[n++] = &s_rows[i];
        if (n < 2) continue;

        /* Memory: private (committed) bytes, early-half vs late-half group
         * averages. Identical races legitimately swing ±20 MB run-to-run
         * (traffic RNG, texture cache churn, OS trim), so single-repeat
         * deltas are noise — a real leak shows as the LATE average sitting
         * above the EARLY average beyond the allowance. WorkingSet is
         * reported in the CSV but deliberately not judged (paging noise). */
        {
            StepRow *r = st_new_row("degrade-private-bytes", 'D');
            int half = n / 2;
            double early = 0.0, late = 0.0;
            long long delta;
            for (i = 0; i < half; i++)      early += (double)series[i]->priv_b;
            for (i = n - half; i < n; i++)  late  += (double)series[i]->priv_b;
            early /= half;
            late  /= half;
            delta = (long long)((late - early) / 1024.0);   /* KB */
            if (r) {
                snprintf(r->note, sizeof(r->note),
                         "private bytes early-avg %.1f MB -> late-avg %.1f MB "
                         "(%+lld KB, limit %d MB)",
                         early / (1024.0 * 1024.0), late / (1024.0 * 1024.0),
                         delta, s_ws_mb_per_rep);
                r->status = (delta > (long long)s_ws_mb_per_rep * 1024)
                            ? ST_FAIL : ST_PASS;
                if (r->status == ST_FAIL) s_exit_code = 1;
            }
        }

        /* Frame time: last repeat's average vs the first. */
        {
            StepRow *r = st_new_row("degrade-frame-time", 'D');
            float first = series[0]->avg_ms, last = series[n - 1]->avg_ms;
            float drift = (first > 0.01f) ? (last - first) * 100.0f / first : 0.0f;
            if (r) {
                snprintf(r->note, sizeof(r->note),
                         "avg frame ms %.2f -> %.2f (%+.1f%%, limit %d%%)",
                         first, last, drift, s_frame_drift_pct);
                r->status = (drift > (float)s_frame_drift_pct) ? ST_FAIL : ST_PASS;
                if (r->status == ST_FAIL) s_exit_code = 1;
            }
        }

        /* GDI / handles: monotonic growth across every repeat AND net growth
         * beyond the allowance = leak. */
        {
            StepRow *r = st_new_row("degrade-gdi-handles", 'D');
            int gdi_mono = 1, hnd_mono = 1;
            long long gdi_net, hnd_net;
            for (i = 1; i < n; i++) {
                if (series[i]->gdi     <= series[i - 1]->gdi)     gdi_mono = 0;
                if (series[i]->handles <= series[i - 1]->handles) hnd_mono = 0;
            }
            gdi_net = (long long)series[n - 1]->gdi     - (long long)series[0]->gdi;
            hnd_net = (long long)series[n - 1]->handles - (long long)series[0]->handles;
            if (r) {
                snprintf(r->note, sizeof(r->note),
                         "GDI %+lld (mono=%d, limit %d), handles %+lld (mono=%d, limit %d)",
                         gdi_net, gdi_mono, s_gdi_growth,
                         hnd_net, hnd_mono, s_handle_growth);
                r->status = ((gdi_mono && gdi_net > s_gdi_growth) ||
                             (hnd_mono && hnd_net > s_handle_growth))
                            ? ST_FAIL : ST_PASS;
                if (r->status == ST_FAIL) s_exit_code = 1;
            }
        }

        /* Heap alloc/free balance: net outstanding allocations added per
         * repeat. Caches legitimately retain, so this is a WARN signal. */
        {
            StepRow *r = st_new_row("degrade-heap-balance", 'D');
            long long first_out = (long long)series[0]->heap_allocs - series[0]->heap_frees;
            long long last_out  = (long long)series[n - 1]->heap_allocs - series[n - 1]->heap_frees;
            long long per_rep   = (last_out - first_out) / (n - 1);
            if (r) {
                snprintf(r->note, sizeof(r->note),
                         "net outstanding game-heap allocs %+lld per repeat", per_rep);
                r->status = (per_rep > 200) ? ST_WARN : ST_PASS;
            }
        }
    }
}

static void st_write_report(void)
{
    FILE *f;
    int i, n_pass = 0, n_warn = 0, n_fail = 0;

    CreateDirectoryA("log", NULL);

    st_degradation_verdicts();

    for (i = 0; i < s_row_count; i++) {
        if (s_rows[i].status == ST_PASS) n_pass++;
        else if (s_rows[i].status == ST_WARN) n_warn++;
        else n_fail++;
    }

    f = fopen("log/selftest_report.csv", "w");
    if (f) {
        fprintf(f, "idx,kind,name,status,note,load_ms,run_ms,frames,avg_ms,"
                   "p95_ms,max_ms,sim_ticks,buttons,nav_reached,nav_navigable,"
                   "ws_kb,ws_peak_kb,priv_kb,gdi,user,handles,heap_allocs,"
                   "heap_frees,d_warns,d_errs\n");
        for (i = 0; i < s_row_count; i++) {
            StepRow *r = &s_rows[i];
            fprintf(f, "%d,%c,%s,%s,\"%s\",%u,%u,%u,%.2f,%.1f,%u,%d,%d,%d,%d,"
                       "%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u\n",
                    i, r->kind, r->name, st_status_str(r->status), r->note,
                    r->load_ms, r->run_ms, r->frames, r->avg_ms, r->p95_ms,
                    r->max_ms, r->sim_ticks, r->buttons, r->nav_reached,
                    r->nav_navigable, r->ws / 1024, r->ws_peak / 1024,
                    r->priv_b / 1024, r->gdi, r->user_obj, r->handles,
                    r->heap_allocs, r->heap_frees, r->d_warns, r->d_errs);
        }
        fclose(f);
    }

    f = fopen("log/selftest_report.md", "w");
    if (f) {
        fprintf(f, "# TD5RE self-test report\n\n");
        fprintf(f, "Suite: **%s** — %d steps: **%d PASS / %d WARN / %d FAIL** "
                   "— exit code %d — total %u s\n\n",
                g_td5.ini.selftest_suite ? "full" : "smoke",
                s_row_count, n_pass, n_warn, n_fail, s_exit_code,
                (td5_plat_time_ms() - s_suite_start_ms) / 1000);
        fprintf(f, "| # | step | status | note | load ms | avg/p95/max frame ms "
                   "| WS MB | GDI | handles | dWARN/dERR |\n");
        fprintf(f, "|---|------|--------|------|---------|----------------------"
                   "|-------|-----|---------|------------|\n");
        for (i = 0; i < s_row_count; i++) {
            StepRow *r = &s_rows[i];
            fprintf(f, "| %d | %s | %s | %s | %u | %.1f / %.0f / %u | %.1f | %u | %u | %u/%u |\n",
                    i, r->name, st_status_str(r->status), r->note, r->load_ms,
                    r->avg_ms, r->p95_ms, r->max_ms,
                    (double)r->ws / (1024.0 * 1024.0), r->gdi, r->handles,
                    r->d_warns, r->d_errs);
        }
        fprintf(f, "\nThresholds: private bytes +%d MB early-vs-late, frame "
                   "drift %d%%, GDI +%d, handles +%d (env TD5RE_SELFTEST_*).\n",
                s_ws_mb_per_rep, s_frame_drift_pct, s_gdi_growth, s_handle_growth);
        fclose(f);
    }

    TD5_LOG_I(LOG_TAG, "REPORT: %d steps, %d PASS / %d WARN / %d FAIL -> exit %d "
              "(log/selftest_report.csv|.md)",
              s_row_count, n_pass, n_warn, n_fail, s_exit_code);
}

/* ------------------------------------------------------------------------
 * Boot
 * ---------------------------------------------------------------------- */

void td5_selftest_boot(void)
{
    if (!g_td5.ini.selftest_enabled) return;

    /* Harness baseline — a bare --SelfTest=1 must be a complete invocation. */
    g_td5.ini.skip_intro     = 1;
    g_td5.ini.log_enabled    = 1;
    /* Pin to INFO exactly: the suite needs its own INF lines, and DEBUG
     * (MinLevel=0) floods gigabytes per run — the render page_blend spam
     * alone wrote 2.3 GB in one full-suite session. */
    g_td5.ini.log_min_level  = 1;
    g_td5.ini.log_frontend   = 1;
    g_td5.ini.log_race       = 1;
    g_td5.ini.log_engine     = 1;
    g_td5.ini.sfx_volume     = 0;      /* parallel-session audio hygiene */
    g_td5.ini.debug_overlay  = 1;
    g_td5.ini.auto_throttle  = 1;      /* slot-0 synthetic driver */
    g_td5.ini.tutorial_overlay = 0;    /* would block on "press to start" w/ a pad */
    g_td5.ini.auto_race      = 0;      /* the director owns race launches */
    g_td5.ini.player_is_ai   = 0;
    g_td5.ini.race_trace_enabled = 0;  /* no CSV trace; the suite has its own report */
    g_td5.ini.start_screen   = TD5_SCREEN_MAIN_MENU;  /* boot straight to the menu */
    _putenv("TD5RE_NAV_SELFTEST=1");   /* per-screen nav reachability check */

    /* Base snapshot for scenario resets. */
    s_base.car           = g_td5.ini.default_car;
    s_base.game_type     = g_td5.ini.default_game_type;
    s_base.dynamics      = g_td5.ini.dynamics;
    s_base.traffic       = g_td5.ini.traffic;
    s_base.cops          = g_td5.ini.cops;
    s_base.laps          = g_td5.ini.laps;
    s_base.opponents     = g_td5.ini.default_opponents;
    s_base.players       = g_td5.ini.default_players;
    s_base.spectate      = 0;
    s_base.player_is_ai  = 0;
    s_base.auto_throttle = 1;
    s_base.fast_forward  = 1.0f;

    /* Knobs. */
    /* Races run at 8x by default: physics is fixed-timestep, so higher FF
     * just drains more sim ticks per render frame (the trace fast-forward
     * path bypasses the 4-tick spiral-of-death cap); the ticks themselves
     * are identical. Frame-time metrics stay comparable because every
     * scenario in a session runs at the same FF. Override: TD5RE_SELFTEST_FF. */
    s_ff              = td5_env_float("TD5RE_SELFTEST_FF", 8.0f, 1.0f, 16.0f);
    s_ws_mb_per_rep   = td5_env_int("TD5RE_SELFTEST_LEAK_MB", 24, 1, 1024);
    s_frame_drift_pct = td5_env_int("TD5RE_SELFTEST_FRAME_DRIFT_PCT", 15, 1, 500);
    s_gdi_growth      = td5_env_int("TD5RE_SELFTEST_GDI_GROWTH", 100, 1, 100000);
    s_handle_growth   = td5_env_int("TD5RE_SELFTEST_HANDLE_GROWTH", 200, 1, 100000);
    s_allow_errors    = td5_env_flag_off("TD5RE_SELFTEST_ALLOW_ERRORS");
    s_race_ticks      = (g_td5.ini.selftest_race_ticks > 0)
                        ? g_td5.ini.selftest_race_ticks : 450;

    s_n_races   = g_td5.ini.selftest_suite ? ST_RACE_COUNT : ST_SMOKE_RACES;
    s_n_screens = g_td5.ini.selftest_suite ? ST_SCREEN_COUNT_FULL : ST_SMOKE_SCREENS;

    s_active = 1;
    s_phase  = PH_WAIT_FIRST_MENU;
    s_sub    = SS_ENTER;
    s_step   = 0;
    s_suite_start_ms    = td5_plat_time_ms();
    s_suite_deadline_ms = s_suite_start_ms +
        (uint32_t)td5_env_int("TD5RE_SELFTEST_TIMEOUT_S", 900, 60, 7200) * 1000u;

    TD5_LOG_I(LOG_TAG, "suite armed: %s (%d screens + %d races), race_ticks=%d "
              "ff=%.1f leak_limit=%dMB drift=%d%%",
              g_td5.ini.selftest_suite ? "full" : "smoke",
              s_n_screens, s_n_races, s_race_ticks, s_ff, s_ws_mb_per_rep,
              s_frame_drift_pct);
}

int td5_selftest_active(void)   { return s_active; }
int td5_selftest_exit_code(void) { return s_active ? s_exit_code : 0; }

/* ------------------------------------------------------------------------
 * Per-frame director
 * ---------------------------------------------------------------------- */

static void st_suite_finish(void)
{
    st_write_report();
    td5_plat_set_window_title(s_exit_code ? "TD5RE SELFTEST: FAIL"
                                          : "TD5RE SELFTEST: PASS");
    s_phase = PH_DONE;
    g_td5.quit_requested = 1;
}

/* Advance to the next step of the current phase (or the next phase). */
static void st_next_step(void)
{
    s_step++;
    s_sub = SS_ENTER;
    if (s_phase == PH_SCREENS && s_step >= s_n_screens) {
        s_phase = PH_RACES;
        s_step = 0;
    }
    if (s_phase == PH_RACES && s_step >= s_n_races) {
        s_phase = PH_REPORT;
    }
}

static void st_tick_screens(uint32_t now)
{
    const ScreenStep *sc = &k_screens_full[s_step];
    StepRow *row = (s_row_count > 0 &&
                    s_rows[s_row_count - 1].kind == 'S' &&
                    strcmp(s_rows[s_row_count - 1].name, sc->name) == 0)
                   ? &s_rows[s_row_count - 1] : NULL;

    /* A screen step must never leave the MENU state; if something (attract
     * timer, stray input) launched a race, fail the step and recover. */
    if (g_td5.game_state == TD5_GAMESTATE_RACE) {
        td5_game_selftest_end_race();
        if (row) st_finish_row(row, ST_FAIL, "unexpected race launch");
        st_next_step();
        return;
    }
    if (g_td5.game_state != TD5_GAMESTATE_MENU) return;  /* intro etc. */

    switch (s_sub) {
    case SS_ENTER:
        st_begin_step(sc->name, 8000);
        row = st_new_row(sc->name, 'S');
        if (row) row->repeat_group = 0;
        if (sc->inject_results)
            td5_game_inject_demo_results();
        td5_frontend_set_screen((TD5_ScreenIndex)sc->screen);
        s_settle_frames = 0;
        s_sub = SS_SCREEN_SETTLE;
        break;

    case SS_SCREEN_SETTLE:
        s_settle_frames++;
        /* Settled: we are on the target screen and its slide-in finished —
         * or the 90-frame fallback for screens that never set the flag
         * (same rule frontend_nav_selftest_maybe uses, with margin). */
        if ((td5_frontend_get_screen() == (TD5_ScreenIndex)sc->screen &&
             td5_frontend_selftest_settled()) ||
            s_settle_frames >= 90 ||
            (int32_t)(now - s_step_deadline_ms) >= 0) {
            if (row) row->load_ms = now - s_step_start_ms;
            s_dwell_frames = 0;
            s_sub = SS_SCREEN_DWELL;
        }
        break;

    case SS_SCREEN_DWELL:
        s_dwell_frames++;
        if (s_dwell_frames >= 45 || (int32_t)(now - s_step_deadline_ms) >= 0) {
            int cur = (int)td5_frontend_get_screen();
            int nscr = -1, nreach = -1, nnav = -1;
            int status = ST_PASS;
            char note[120] = "";
            if (row) {
                row->buttons = td5_frontend_selftest_button_count();
                td5_frontend_selftest_nav_result(&nscr, &nreach, &nnav);
                if (nscr == sc->screen) {
                    row->nav_reached   = nreach;
                    row->nav_navigable = nnav;
                    if (nnav > 0 && nreach < nnav) {
                        status = ST_WARN;
                        snprintf(note, sizeof(note),
                                 "nav reached %d/%d buttons", nreach, nnav);
                    }
                }
                if (cur != sc->screen) {
                    /* Screens like LEGAL auto-advance; table-flagged entries
                     * (cup screens without a live cup) bounce by design. */
                    if (!sc->allow_redirect && status < ST_WARN) status = ST_WARN;
                    snprintf(note, sizeof(note), "redirected to screen %d%s",
                             cur, sc->allow_redirect ? " (expected)" : "");
                }
                st_finish_row(row, status, note);
            }
            st_next_step();
        }
        break;

    default:
        s_sub = SS_ENTER;
        break;
    }
}

static void st_tick_races(uint32_t now)
{
    const RaceScenario *sc = &k_races[s_step];
    StepRow *row = (s_row_count > 0 &&
                    s_rows[s_row_count - 1].kind == 'R' &&
                    strcmp(s_rows[s_row_count - 1].name, sc->name) == 0)
                   ? &s_rows[s_row_count - 1] : NULL;

    switch (s_sub) {
    case SS_ENTER:
        if (g_td5.game_state != TD5_GAMESTATE_MENU) return;
        /* One race can take a while on first asset load; generous budget. */
        st_begin_step(sc->name, 60000);
        row = st_new_row(sc->name, 'R');
        if (row) row->repeat_group = sc->repeat_group;
        st_apply_scenario(sc);
        g_td5.ini.auto_race = 1;      /* the MENU state consumes this next frame */
        s_race_launch_ms = now;
        s_sub = SS_RACE_WAIT_START;
        break;

    case SS_RACE_WAIT_START:
        if (g_td5.game_state == TD5_GAMESTATE_RACE) {
            if (row) row->load_ms = now - s_race_launch_ms;
            s_race_start_ms   = now;
            s_race_start_tick = g_td5.simulation_tick_counter;
            s_last_sim_tick   = s_race_start_tick;
            st_histo_reset();
            /* Running budget: tick target at 30 Hz / ff, plus countdown and
             * fade margins. Natural finishes get a much longer leash. */
            s_step_deadline_ms = now + (sc->natural_finish
                ? 300000u
                : (uint32_t)((float)s_race_ticks / 30.0f / s_ff) * 1000u + 90000u);
        } else if ((int32_t)(now - s_step_deadline_ms) >= 0) {
            st_finish_row(row, ST_FAIL, "race never started (timeout)");
            g_td5.ini.auto_race = 0;
            st_golden_end();   /* no-op unless this was a golden scenario */
            st_reset_scenario_fields();
            st_next_step();
        }
        break;

    case SS_RACE_RUNNING:   /* fallthrough guard — reached via WAIT_START below */
        break;

    case SS_RACE_WAIT_MENU:
        if (g_td5.game_state == TD5_GAMESTATE_MENU) {
            s_settle_frames = 0;
            s_sub = SS_RACE_POST_MENU;
        } else if ((int32_t)(now - s_step_deadline_ms) >= 0) {
            st_finish_row(row, ST_FAIL, "race exit never completed (timeout)");
            st_reset_scenario_fields();
            st_next_step();
        }
        break;

    case SS_RACE_POST_MENU:
        s_settle_frames++;
        if (s_settle_frames >= 30) {
            char note[120] = "";
            int status = ST_PASS;
            if (row) {
                float avg, p95; unsigned mx;
                st_histo_stats(&avg, &p95, &mx);
                row->frames    = s_histo_n;
                row->avg_ms    = avg;
                row->p95_ms    = p95;
                row->max_ms    = mx;
                row->sim_ticks = s_last_sim_tick - s_race_start_tick;
                if (sc->natural_finish &&
                    td5_frontend_get_screen() == TD5_SCREEN_RACE_RESULTS) {
                    snprintf(note, sizeof(note), "finished -> results screen ok");
                    td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                }
                st_finish_row(row, status, note);
            }
            /* Golden scenario: hash the trace CSVs + append the 'G' verdict
             * row (also restores the pinned knobs + closes the trace). */
            if (sc->trace_golden) st_golden_finish(sc);
            st_reset_scenario_fields();
            st_next_step();
        }
        break;

    default:
        s_sub = SS_ENTER;
        break;
    }

    /* RUNNING is handled outside the switch so the histogram samples the
     * frame cadence on every frame the race is live. */
    if (s_sub == SS_RACE_WAIT_START && g_td5.game_state == TD5_GAMESTATE_RACE)
        s_sub = SS_RACE_RUNNING;

    if (s_sub == SS_RACE_RUNNING) {
        int tick = g_td5.simulation_tick_counter;
        if (tick != s_last_sim_tick) {          /* sim is advancing */
            st_histo_add(now);
            s_last_sim_tick = tick;
        }
        if (g_td5.game_state != TD5_GAMESTATE_RACE) {
            /* Natural finish (or knockout): the fade already returned us. */
            s_sub = SS_RACE_WAIT_MENU;
        } else if (!sc->natural_finish &&
                   tick - s_race_start_tick >= s_race_ticks) {
            td5_game_selftest_end_race();
            s_step_deadline_ms = now + 30000u;
            s_sub = SS_RACE_WAIT_MENU;
        } else if ((int32_t)(now - s_step_deadline_ms) >= 0) {
            st_finish_row(row, ST_FAIL, "race never reached tick budget (timeout)");
            td5_game_selftest_end_race();
            s_step_deadline_ms = now + 30000u;
            s_sub = SS_RACE_WAIT_MENU;
        }
    }
}

void td5_selftest_tick(void)
{
    uint32_t now;
    if (!s_active || s_phase == PH_DONE) return;
    now = td5_plat_time_ms();

    /* Global suite watchdog — the process must always terminate. */
    if ((int32_t)(now - s_suite_deadline_ms) >= 0 && s_phase != PH_REPORT) {
        TD5_LOG_E(LOG_TAG, "GLOBAL TIMEOUT — aborting suite");
        s_exit_code = 1;
        {
            StepRow *r = st_new_row("suite-global-timeout", 'D');
            if (r) { r->status = ST_FAIL;
                     snprintf(r->note, sizeof(r->note), "suite deadline exceeded"); }
        }
        st_suite_finish();
        return;
    }

    switch (s_phase) {
    case PH_WAIT_FIRST_MENU:
        /* StartScreen=MAIN_MENU boot: wait until the frontend is live. */
        if (g_td5.game_state == TD5_GAMESTATE_MENU &&
            td5_frontend_get_screen() == TD5_SCREEN_MAIN_MENU) {
            TD5_LOG_I(LOG_TAG, "frontend live after %u ms — starting phases",
                      now - s_suite_start_ms);
            s_phase = (s_n_screens > 0) ? PH_SCREENS : PH_RACES;
            s_step = 0;
            s_sub = SS_ENTER;
        }
        break;

    case PH_SCREENS:
        st_tick_screens(now);
        break;

    case PH_RACES:
        st_tick_races(now);
        break;

    case PH_REPORT:
        st_suite_finish();
        break;

    default:
        break;
    }
}

#endif /* !TD5RE_RELEASE */
