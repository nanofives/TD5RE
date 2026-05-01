# Plan — Screen [24] RaceResults parity with original

Authoritative reference: Ghidra decomp of `RunRaceResultsScreen` @ `0x00422480` (24-state machine, body 0x00422480..0x0042374f). Comparison performed 2026-05-01 with both binaries running side-by-side at `--StartScreen=24 --DefaultGameType=0` (port) vs `frontend_screen=24` (original via td5_quickrace.py Frida hook).

This plan executes in a separate `/fix` session. Phases are independent unless noted; acceptance gates apply per phase.

---

## Already addressed (commit 76a83a0)

These shipped in the current branch and removed visible port-only artifacts:

- **A1** Removed gold-bordered translucent panel + duplicate "RACE RESULTS" / "CUP CHALLENGE RESULTS" inner header from `frontend_render_race_results_overlay`. Original draws no decorative box; the title bar surface (P1 below) is the canonical label.
- **A2** `Screen_RaceResults` state 0 → state 0xD skip-to-menu jump. **REVERT THIS** in P0 below — it is incorrect vs original.
- **A3** "View Race Data" button (case 2 of state 0x10 dispatch) tears down menu and seeds state 1. Mostly correct; original calls `SetFrontendScreen(0x18)` which re-enters and re-runs state 0 setup. P5 below replaces the seed with a faithful re-entry.
- **A4** `td5_frontend_init_resources()` now runs before the StartScreen jump in `td5_game.c` so font + bg-gallery load on direct screen jumps.

---

## P0 — Revert state 0 → 0xD skip; install state-3 early-exit

**Why:** Ghidra confirms the original natural flow is state 0 → 1 → 2 → 3 → 4 → 5 → 6 (table-browse) → 0xB → 0xC → 0xD (menu). With a populated race the user sees the positions table first. The user's "menu first" observation in the original comes from a guard inside **state 3** that fires when there is no race data:

```c
case 3:  // 0x004228EC
    if (DAT_00497a74 != 0 ||
        gRaceSlotStateTable.slot[0].companion_state_2 == 2 ||
        g_actorRuntimeState.slot._808_4_ == 0) {
        DAT_00497a74 = 0;
        g_frontendInnerState = 0xc;   // → cleanup → state 0xD (menu)
        return;
    }
    // ...slide-in animation...
```

For a fresh `SetFrontendScreen(0x18)` (Frida `frontend_screen=24` or port `--StartScreen=24`), `actor_state.slot._808_4_ == 0` (no race ran), so the original short-circuits to the menu. The port currently reproduces "menu first" via my state 0 → 0xD jump (commit 76a83a0), but that breaks the post-race flow where data IS populated.

**Implementation:**
1. In `Screen_RaceResults` state 0, restore the original assignment: `s_inner_state = 1` instead of `0x0D`.
2. Restore the state-0 OK click-catcher creation (`frontend_create_button("OK", FE_CENTER_X - 48, 400, 0x60, 0x20)`) before `s_inner_state = 1`.
3. In state 3, before the slide-in animation, add the gate:
   ```c
   if (s_results_skip_display ||
       td5_game_get_slot_companion_2(0) == 2 ||
       !td5_game_slot_is_finished(0)) {
       s_results_skip_display = 0;
       s_inner_state = 0xC;
       return;
   }
   ```
   `s_results_skip_display` mirrors `DAT_00497a74` (already declared in port). Gate condition matches `DAT_00497a74 != 0 || slot[0].companion_state_2 == 2 || actor.slot._808_4_ == 0`.

**Acceptance:**
- Port `--StartScreen=24 --DefaultGameType=0` lands on the menu (no race → early-exit fires).
- Port post-race natural flow shows positions table first (race data populated, gate doesn't fire).
- Ghidra match: state-3 first three branches identical to 0x004228EC.

---

## P1 — Title bar surface (`DAT_00496358` / `CreateMenuStringLabelSurface(0xe)`)

State 0 of the original creates a 14-row menu-string label surface (`CreateMenuStringLabelSurface(0xE)`) — this is the "RACE RESULTS" / "CUP CHALLENGE RESULTS" / "DRAG RESULTS" / "TIME TRIAL RESULTS" banner that floats above the panel. Index 14 is the screen-banner string slot used by the frontend menu engine.

States 3-0xB queue the surface via `QueueFrontendOverlayRect(uVar3-200, (iVar6-DAT_004962cc)-0x40, 0,0, DAT_004962c4, DAT_004962c8, 0, DAT_00496358)` — y is one panel-step above the panel.

Port currently has nothing analogous. The bg-gallery slideshow is unrelated (gallery is per-frame slideshow at frontend_render_bg_gallery).

**Implementation:**
- Add `frontend_create_label_surface(int label_id)` (or extend the existing menu-string system) to render the localized "RACE RESULTS" string into a small surface, sized DAT_004962c4 × DAT_004962c8 (likely 400×40 or similar — Ghidra constants).
- Render it in the existing `frontend_render_race_results_overlay` ABOVE the table panel, gated on the same state range (3..0xB).
- Localized string source: `SNK_MusicTest_MT_exref` (per state-0 line `SetFrontendInlineStringTable(SNK_MusicTest_MT, 0, 0)`) — re-cite via `search_text` since "MusicTest_MT" looks wrong; verify via Ghidra string XRef.

**Acceptance:** Banner visible above panel during table-browse; matches original capture pixel-position within ±4px.

---

## P2 — MainMenu.tga backdrop

State 0 calls `LoadTgaToFrontendSurface16bpp("MainMenu.tga", "FrontEnd.zip")` then `CopyPrimaryFrontendBufferToSecondary()` so the post-race UI overlays the main-menu artwork. Without this the port sees the bg-gallery slideshow (per A4) — which is wrong for screen 24, screen 24 should use MainMenu.tga.

**Implementation:**
- Load `Front End/MainMenu.tga` via `td5_asset_load_tga`/`frontend_load_tga_colorkey` with no color key (it's an opaque background) into a dedicated shared page.
- During screen 24 render, draw the MainMenu page as a full-screen backdrop BEFORE `frontend_render_bg_gallery` (or instead of it for this screen).
- Update `td5_frontend_render_ui_rects` to skip `frontend_render_bg_gallery` when `s_current_screen == TD5_SCREEN_RACE_RESULTS` (existing skip list at line ~4942).

**Acceptance:** Same backdrop on screen 24 in port + original. Visual diff in same canvas region.

---

## P3 — Wide click-catcher button (button 0)

State 0 creates **two** buttons in the original:
1. Index 0: `CreateFrontendDisplayModeButton(NULL, -0x208, 0, 0x208, 0x20, 0)` — 520×32 invisible click-catcher
2. Index 1: `CreateFrontendDisplayModeButton(SNK_OkButTxt, -0x208, 0, 0x60, 0x20, 0)` — 96×32 OK button

State 6's exit gate is `g_frontendButtonIndex >= 0 && g_frontendButtonIndex < 2` — matches either click-catcher OR OK. Port now creates only the OK button (index 0), so anywhere-on-screen click during browse is a no-op.

**Implementation:**
- In state 0 (post P0 revert), recreate both buttons in order: NULL-label wide click-catcher, then "OK" narrow.
- State 6 exit gate updated to `s_button_index >= 0 && s_button_index < 2` (already correct in port).

**Acceptance:** Click anywhere on screen during table-browse → returns to menu (matches original UX).

---

## P4 — DrawRaceDataSummaryPanel + per-game-type column headers

State 0 fills the 408×392 panel surface (`DAT_0049628c`) with localized text columns based on `g_selectedGameType`:

| game_type | Source (offset) | Y range | Step |
|-----------|-----------------|---------|------|
| <1 (Single) | `SNK_ResultsTxt+0x40` | 0x60..0xf0 | 0x18 (24px) |
| 1, 6 (Cup, Ultimate) | `SNK_ResultsTxt` | 0x30..0xf0 | 0x18 |
| 2, 3, 4, 5 (Cups) | `SNK_ResultsTxt`@0x30 + `+0x100`@0x48 + `+0x40`@0x60..0xf0 | mixed | 0x18 |
| 7 (Drag) | `SNK_DRResultsTxt` | 0x60..0xa8 | 0x18 |
| 8 (Cop Chase) | `SNK_CCResultsTxt` | 0x48..0xa8 | 0x18 |
| 9 (Drag Race) | `SNK_ResultsTxt+0x40` | 0x60..0xf0 except 0x90,0xa8 | 0x18 |

Then `DrawRaceDataSummaryPanel(slot)` blits per-slot result rows for the currently-browsed slot at `DAT_00497a68`.

Port currently renders a generic "POS / DRIVER / CAR / TIME" header + slot list with no per-game-type variant and no slot-focus indicator.

**Implementation:**
- New helper `frontend_panel_draw_results_for_game_type(game_type, panel_x, panel_y)` mirroring the table above.
- New helper `frontend_panel_draw_per_slot_data(slot)` blitting per-slot timing/lap/position fields per `DrawRaceDataSummaryPanel` at `0x004?????` — locate via `function_by_name` on "DrawRaceDataSummary" and decompile.
- Wire into `frontend_render_race_results_overlay` at the existing state gate (state 3..0xB).

**Acceptance:** Table contents match original for each of game_type 0/7/8/9 + Cup variant. Verify by direct visual capture and CSV-diff per slot.

---

## P5 — View Race Data: faithful self-re-entry

Original case 2 in state 0x10 dispatch: `SetFrontendScreen(0x18)` — re-enters screen 24 from state 0, re-running setup (sort, snapshot, re-walk slot table).

Port now seeds state 1 directly with manually-recreated buttons. That skips the state-0 setup (snapshot, sort, panel re-fill) which is observable on cup mode where slot data may have changed.

**Implementation:**
- Replace the case-2 body in `Screen_RaceResults` state 0x10 with `td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS); return;`.
- Verify state-3 gate (P0) does NOT fire on re-entry (it only fires when no actor data; if data is present the table flow runs normally).

**Acceptance:** View Race Data → table re-renders with current sort applied. Multiple cycles menu→table→menu→table preserve state correctly.

---

## P6 — Quit/Race Again branch fidelity in state 0x10

Original state 0x10 dispatch on `DAT_00497a64` (saved button index from state 0xF):

| Btn | game_type | Action |
|-----|-----------|--------|
| 0 (Race Again / Next Cup) | 1..6 | `g_selectedScheduleIndex = g_attractModeTrackIndex`, restore snapshot, masters-special, → state 0x11 |
| 0 | other | restore snapshot, → state 0x11 |
| 1 (View Replay) | * | `g_attractModeControlEnabled=2; g_inputPlaybackActive=1; → state 0x11` |
| 2 (View Race Data) | * | `SetFrontendScreen(0x18)` |
| 3 (Save / Select New Car) | 1..6 (cup) | → state 0x12 (save flow) |
| 3 | other | → state 0x12 (also leads to car-select) |
| 4 (Quit) | <1 (single) | `AwardCupCompletionUnlocks(); SetFrontendScreen(0x19=NameEntry)` |
| 4 | cup, `DAT_00497a70==0` | `SetFrontendScreen(5=MainMenu)` |
| 4 | cup, `DAT_00497a70!=0`, `DAT_0048d988+2==0` | returnScr=0x19, `SetFrontendScreen(0x1B=CupWon)` |
| 4 | cup, `DAT_00497a70!=0`, `DAT_0048d988+2!=0` | returnScr=0x19, `SetFrontendScreen(0x1A=CupFailed)` |

Port branches with similar intent but without `DAT_00497a70` cup-completion gating or `DAT_0048d988+2` cup-won test. `DAT_00497a70` is set in state 0xD (line `DAT_00497a70 = 1` when `ConfigureGameTypeFlags()==0`).

**Implementation:**
- Mirror these branches verbatim in port case-4 dispatch.
- Add port state for `s_results_cup_complete` (already present as `DAT_00497a70` mirror) and decode `DAT_0048d988+2` (likely a cup-state field — verify in Ghidra).

**Acceptance:** Quit from single race → NameEntry. Quit from completed cup → CupWon. Quit from failed cup → CupFailed. Quit mid-cup → MainMenu.

---

## P7 — Slide-in / slide-out sprite animations (states 3, 0xE, 0x10)

Original animates panel and buttons via `MoveFrontendSpriteRect(idx, x, y)` per frame:

- State 3: panel slides in from left (0x28-step), title bar slides in from right
- State 0xE: 5 menu buttons slide in from alternating sides (0x18 step, 32-frame anim)
- State 0x10: 5 menu buttons slide out (mirror animation)
- States 7-10: panel slide L/R during table-browse arrow input
- State 0xB: panel + title slide out simultaneously

Port has frame counters (`s_anim_tick`) and state advancement matching durations, but does NOT animate sprite positions.

**Implementation:**
- Track per-button x/y override in port's button table.
- Apply per-state animation curves matching original `MoveFrontendSpriteRect` formulas: `cx + s_anim_tick * step + offset`.
- Render buttons at animated positions during transition states.

**Acceptance:** Frame-time match within ±2 frames of original on all 5 transitions.

---

## P8 — DXSound::Play(5)/Play(4) entry/transition cues

State 0 plays sound 5 on entry. State 3 plays sound 4 when slide-in completes (`AdvanceFrontendTickAndCheckReady` returns nonzero). Port has no audio cues for screen 24.

**Implementation:** Add `td5_sound_play_ui(5)` in state 0 init, `td5_sound_play_ui(4)` at end of state 3.

**Acceptance:** Audible cue match — manual confirmation via parallel launch of original + port at `frontend_screen=24` / `--StartScreen=24`.

---

## P9 — Surface lifecycle in state 0xC

Original state 0xC:
```c
DAT_0049628c = ReleaseTrackedFrontendSurface(DAT_0049628c);
DAT_00496358 = ReleaseTrackedFrontendSurface(DAT_00496358);
ReleaseFrontendDisplayModeButtons();
g_frontendInnerState++;  // → 0xD
```

Port state 0xC just calls `frontend_reset_buttons()`. Once P1 (title surface) and P4 (panel surface) land, port needs symmetric release in state 0xC (and state 0x14 which `goto`s state 0xC).

**Implementation:** Add panel + title surface release calls before `frontend_reset_buttons()`.

**Acceptance:** No texture-page leak on cycle-and-back.

---

## Execution notes for the /fix session

- Phases are mostly independent. Suggested order: **P0 (revert) → P3 (click-catcher) → P5 (re-entry) → P2 (backdrop) → P1 (title) → P4 (panel content) → P6 (quit branches) → P9 (surface release) → P7 (animations) → P8 (audio)**.
- Use `--StartScreen=24` on the port and `[frontend] frontend_only=false; frontend_screen=24` on the original (td5_quickrace.ini) for side-by-side launch.
- Game-type axis to test per phase: 0 (single), 1 (cup), 5 (Masters), 7 (drag), 8 (cop chase), 9 (drag race). Each affects panel content and Quit-button branching.
- Reopen `todo_screen24_results_flow.md` (already RESOLVED-marked) and re-status it as IN PROGRESS until all phases land.

---

## Acceptance gate (overall)

- Visual diff: side-by-side capture at every state transition (0 → 3 → 6 → 0xD → 0xE → 0xF → 0x10) shows pixel-level parity within ±4px positional, ±10% alpha tolerance.
- Functional: all 5 button paths (Race Again, View Replay, View Race Data, Select New Car/Save, Quit) navigate to identical destinations as original for each game_type.
- Audio: entry+transition cues fire at matching states.
- No regressions: post-race natural flow (race finishes → screen 24) still shows positions table when actor data is present.
