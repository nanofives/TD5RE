# DA-T4: RaceTypeCategoryMenuStateMachine (0x004168B0) Deep Audit

**Date:** 2026-05-22
**Target:** TD5_d3d.exe `RaceTypeCategoryMenuStateMachine` @ 0x004168B0 (body 0x004168B0..0x00417CCD = 5149 bytes).
**Port mirror:** `td5mod/src/td5re/td5_frontend.c::Screen_RaceTypeCategory()` @ line 6360.
**Method:** Ghidra (TD5_pool12) full decomp + line-by-line port comparison.
**Pool slot:** read-only; cleaned up.

State count clarification: orig uses **14 distinct states**: 0,1,2,3,4,5,6,7,8,9,0xA,0xB,0xC,0x14. States 0xD–0x13 fall through `switchD_004168e2_caseD_d` (return). Task said "21 states (0x00 - 0x14)" but only 14 are implemented; the 0x14 jump-table simply has dead arms.

---

## Section A — State-by-state orig vs port comparison

| State | Orig behavior | Port behavior (Screen_RaceTypeCategory) | Match? |
|-------|---------------|------------------------------------------|--------|
| **0** Init | Clears g_carSelectChampionshipReturnFlag, g_wantedModeEnabled, g_selectedGameType=0 then -1; resets g_frontendScreenTransitionFlag; creates header surface (CreateMenuStringLabelSurface(1)); allocates 0x110×0xB4 description surface (g_lobbyErrorDialogSurface, BltColorFill 0); LoadTga MainMenu.tga; CopyPrimaryFrontendBufferToSecondary; creates 7 buttons (SingleRace/CupRace/[ContCup OR ContCupPreview based on ValidateCupDataChecksum]/TimeTrials/DragRace/CopChase/Back); sets g_frontendEscKeyButtonIndex=6; SetFrontendInlineStringTable(SNK_RaceMenu_MT); renders smallTextSurface; SetSurfaceColorKey 0; PresentPrimaryFrontendBufferViaCopy; ActivateFrontendCursorOverlay; g_frontendAnimFrameCounter=-1; DXSound::Play(5); state++. | frontend_init_return_screen(TD5_SCREEN_RACE_TYPE_MENU); reset buttons; load MainMenu.tga; **does NOT explicitly clear g_carSelectChampionshipReturnFlag, g_wantedModeEnabled, g_screenTransitionFlag, g_smallTextSurface color-key, run CopyPrimary→Secondary, or call ActivateFrontendCursorOverlay**; creates same 7 buttons (cup-checksum-gated 3rd one); s_selected_game_type=-1; frontend_begin_timed_animation. No DXSound::Play(5). | Partial — see §C.1 |
| **1** Slide-in (32 frames) | Per-frame MoveFrontendSpriteRect(0..6) with extensive trig-curve formulas; QueueFrontendOverlayRect for header + description surface; at frame 0x20: state++ AND **DXSound::Play(4)**. | frontend_update_timed_animation(0x20, 533); when >=1.0 → state=2. No sprite-move per-frame; no SFX 4 at completion. | Cosmetic OK (smoothed). **Missing DXSound::Play(4)** — see §C.2 |
| **2** Tick-and-wait | QueueFrontendOverlayRect header; AdvanceFrontendTickAndCheckReady — when true: DeactivateFrontendCursorOverlay, g_previousButtonIndex=-1, state++. | frontend_advance_tick → s_anim_complete=1, state=3. **No DeactivateFrontendCursorOverlay**; no explicit previous-button reset. | Functional partial; cursor stays active. See §C.3 |
| **3** Main interaction | Re-queues header + description rect. Hover change → state=4 (preview), previousButtonIndex updated. Button press: button 0→type=0; button 1→state=6 (cup); button 2→LoadContinueCupData unconditional + replayFile=0 + return=0x18 + state=5; button 3→type=9 (orig DragRace=9? no, Time Trials, but enum value 9); button 4→type=7 (Drag Race=7 orig); button 5→type=8 (Cop Chase=8 orig); button 6→Back: returnIndex = (DAT_004962c0==1)?0xA:5. All non-cup, non-back paths call ConfigureGameTypeFlags + set returnIndex=0x14 + state=5. | Same hover→4. Button 0→type=0; button 1→state=6; button 2→**checksum-guarded** LoadContinueCupData then state=5 with returnIndex=24; button 3→type=7 (port enum TIME_TRIAL); button 4→type=9 (port enum DRAG_RACE) + drag_carselect_pass=0; button 5→type=8 (COP_CHASE); button 6→return = (s_network_active) ? CREATE_SESSION : MAIN_MENU. Calls ConfigureGameTypeFlags on cases 0,3,4,5. | REG-1 remap confirmed correct. **Two new divergences:** §C.4 button-2 checksum guard + §C.5 Back-condition uses wrong global |
| **4** Preview redraw | If g_frontendAnimFrameCounter==1: BltColorFill description surface to black; derive g_selectedGameType from button index (button 0→0, 4→7, 5→8, 3→9, 1→10, 2→0xB); fetch SNK_RaceTypeText[g_selectedGameType*4]; draw title (localized large font) + body lines (12px spacing, small font) into description surface. Queue overlay. state=3. | Stub: `s_inner_state = 3;` no actual draw. | **Description text never rendered** — see §C.6 |
| **5** Slide-out prep + animate | Queues header at sliding-X position + description sliding-Y position; at frame 1: DXSound::Play(5); frames 0..2: PresentSecondary + BlitSecondary (no movement); frames 3+: MoveFrontendSpriteRect(0..6) per-frame; at frame 0x23: ResetFrontendInlineStringTable, RenderTgaToFrontendSurface, SetSurfaceColorKeyFromRGB(0xffffff), **release all 3 surfaces** (g_lobbyErrorDialogSurface, DAT_004962dc, DAT_00496358), ReleaseFrontendDisplayModeButtons, SetFrontendScreen(g_returnToScreenIndex). | frontend_begin_timed_animation(); s_inner_state=0x14. No animation, no SFX at frame 1, **no surface releases**, no button release, no string-table reset. State 0x14 finishes navigation. | Significant cleanup gap — see §C.7 (surface leak) |
| **6** Cup enter slide-out (top buttons) | Queue header + description; at frame 1: DXSound::Play(5); frames 0..2: PresentSecondary + Blit (no anim); frames 3+: MoveFrontendSpriteRect(0..5); at frame 0x23: ReleaseFrontendDisplayModeButtons; ResetFrontendInlineStringTable; create 7 cup buttons (Championship + Era unconditional, Challenge/Pitbull/Masters/Ultimate gated on g_cheatFlagBitfieldGameModes==0/1/else); create Back button at fixed position; g_frontendEscKeyButtonIndex=6; AdvanceFrontendInlineStringTableState(0,0); return. | reset_buttons immediately (no animation); creates 7 cup buttons gated on s_cup_unlock_tier (>=1 / >=2); Back button at -0xE0,0 (no fixed position); frontend_begin_timed_animation; state=7. | Cup gating logic equivalent (see §B). **Animation skipped, Back position differs, header EscKey not set, InlineStringTable not reset.** See §C.8 |
| **7** Cup slide-in | Per-frame MoveFrontendSpriteRect(0..5) curves; at 0x20: DXSound::Play(4); state++. | frontend_update_timed_animation(0x20, 533) → state=8. No SFX at completion. | Same as state 1 issue — §C.2 |
| **8** Cup interactive | Hover change → previousButton update, state=9. Button press (idx≥0): if idx<6: g_selectedGameType=idx+1, returnIndex=0x14, raceWithinSeriesIndex=0, ConfigureGameTypeFlags, selectedScheduleIndex=g_attractModeTrackIndex, **anim=0**, state=10. If idx==6: anim=0, state=0xB. | Hover not explicit (no state-9 transition on hover; preview is dead-code stub). Press: case 0,1→cup_type=1,2 directly; case 2,3,4,5→tier-gated (`if (tier>=N) cup_type=X; else play_sfx(10)`); case 6→state=11. If cup_type≥0: selected_game_type=cup_type, race_within_series=0, ConfigureGameTypeFlags, returnIndex=CAR_SELECTION, state=10. | **3 divergences:** §C.9 (no hover→9), §C.10 (port adds tier-recheck on press redundant with greyed buttons but actually a NEW SFX 10 reject — useful), §C.11 (port misses **selected_schedule_index = attract_mode_track_index**) |
| **9** Cup preview draw | If anim==1: BltColorFill description; if idx<6: g_selectedGameType=idx+1; if game_type≥0: SNK_RaceTypeText[idx*4], same title+body draw as case 4. Queue rect. state=8. | Stub: `s_inner_state = 8;` | Same as §C.6 — cup description never rendered |
| **0xA** Cup slide-out → car select | Header slide + description slide; at frame 1: SFX 5; frames 0..2: PresentSecondary+Blit (LAB_00417793); frames 3+: MoveFrontendSpriteRect(0..5); at frame 0x23: ResetFrontendInlineStringTable, RenderTga, SetSurfaceColorKey(0xffffff), **release 3 surfaces**, release buttons, SetFrontendScreen(g_returnToScreenIndex). | s_anim_tick=0; s_inner_state=0x14. No animation, no cleanup. | Same surface leak as §C.7 |
| **0xB** Cup back (sub-menu→top) slide-out | Header + description queued; at frame 1: SFX 5; frames 0..2: PresentSecondary+Blit; frames 3+: MoveFrontendSpriteRect(0..5); at frame 0x23: ReleaseFrontendDisplayModeButtons, ResetFrontendInlineStringTable, recreate top-7 (SingleRace..CopChase + Back fixed-position), g_frontendEscKeyButtonIndex=6, SetFrontendInlineStringTable(SNK_RaceMenu_MT), DXSound::Play(5), anim=0, state++. | reset_buttons, state=0. **Re-enters state 0 → fully re-inits everything including reloading MainMenu.tga and recreating header/description surfaces.** Surfaces from state-0 first entry already exist → orphan leak. | Major divergence — §C.12 |
| **0xC** Cup-back final slide-in (top buttons) | MoveFrontendSpriteRect(0..5) per-frame; at frame 0x20: DXSound::Play(4); state=3 (NOT state++). | Stub: state=9. **Wrong target state** (port stub is unreachable since state 11 jumps to 0). Effectively dead code. | Stub harmless because port re-enters state 0 (which slides in via state 1) instead of using 0xC for the slide-in. |
| **0x14** Reset | state=1; g_frontendAnimFrameCounter=-1. | frontend_update_timed_animation(16,267); navigate when ≥1.0. | **Semantic mismatch.** Orig 0x14 is a *re-arm* used after various sub-sequences to bounce back to state 1; port 0x14 is repurposed as the final slide-out + dispatch. Internally consistent only because port never reaches 0x14 except from state-5 dispatch path. — see §C.13 |

---

## Section B — REG-1 enum remap (re-confirmed)

The TD5_GameType enum value differences between orig (Drag=7, CopChase=8, TimeTrial=9) and port (TimeTrial=7, CopChase=8, DragRace=9) are deliberate and consistently propagated:

- Port `s_selected_game_type` assignments at td5_frontend.c:6427 (Time Trials → 7), :6434 (Drag Race → 9), :6442 (Cop Chase → 8) match the port-side enum.
- All consumers reference `TD5_GAMETYPE_*` names (td5_game.c dispatch, td5_hud.c, td5_frontend.c selection flows), not raw integers.
- `s_cup_unlock_tier` (DAT_004962a8 in orig = `g_cheatFlagBitfieldGameModes`) gating:
  - orig `== 0`: all cup tiers below Era are preview (locked).
  - orig `== 1`: Challenge + Pitbull unlock; Masters + Ultimate stay preview.
  - orig `else` (≥2): all unlock.
  - port gates Challenge/Pitbull on `tier >= 1`, Masters/Ultimate on `tier >= 2` — semantically equivalent for tier ∈ {0,1,2,…}.

**REG-1 verdict 2026-05-22 stands: false alarm, deliberate enum remap.**

---

## Section C — NEW divergences found

### C.1 — State 0 missing init writes (LOW)

Port omits these global resets present in orig:
- `g_carSelectChampionshipReturnFlag = 0`
- `g_wantedModeEnabled = 0`
- `g_frontendScreenTransitionFlag = 0`
- Initial `g_selectedGameType = 0` (then immediately =-1; could be a Ghidra artifact but the 0 is observable for one tick)
- `CopyPrimaryFrontendBufferToSecondary()` after TGA load
- `ActivateFrontendCursorOverlay()` before state++
- `DXSound::Play(5)` at end of state 0

`g_wantedModeEnabled=0` is the most semantically meaningful: if the prior screen left wanted-mode active (cop chase HUD), entering RaceTypeCategory does not clear it in port. **Real bug if cop-chase HUD persists when navigating back to race-type menu.**

### C.2 — Missing DXSound::Play(4) on slide-in completion (LOW, cosmetic)

States 1 and 7 (slide-in animations) in orig play SFX 4 (typically a "thunk/lock-in" sound) at frame 0x20 when the slide completes. Port omits both. Audio polish gap.

### C.3 — State 2 missing DeactivateFrontendCursorOverlay (LOW)

Orig deactivates cursor overlay before entering the interactive state 3 (since hover is now button-driven). Port leaves cursor overlay active throughout state 3 → mouse cursor remains visible during button interaction. Likely a visible bug if the cursor was visible in state 0/1.

### C.4 — State 3 button 2 (Continue Cup) — port adds checksum guard

Orig case 3 button 2 unconditionally:
```c
g_frontendAnimFrameCounter = 0;
LoadContinueCupData();
g_replayFileAvailable = 0;
g_returnToScreenIndex = 0x18;  // RACE_RESULTS
g_frontendInnerState = 5;
return;
```

The button is **already greyed (preview) in state 0** if `ValidateCupDataChecksum()` failed — so the button technically should not be clickable, and orig relies on that filtering. Port adds a *runtime* `frontend_validate_cup_checksum()` re-check and plays SFX 10 (reject) on failure. Defensive but **also misses setting `g_replayFileAvailable = 0`**. If `s_replay_file_available` is still set from a prior race, post-Continue-Cup flow may incorrectly think a replay is available.

### C.5 — State 3 button 6 (Back) uses wrong gate variable

Orig:
```c
g_returnToScreenIndex = (-(uint)(DAT_004962c0 != 1) & 0xfffffffb) + 10;
// == (DAT_004962c0 == 1) ? 10 : 5
// 10 = CREATE_SESSION, 5 = MAIN_MENU
```

`DAT_004962c0` = **g_networkLobbyEntryPhase** (a *lobby-entered* gate set to 1 by CreateSession/0x0041a990 et al, cleared by NetworkLobby destroy paths). It is **NOT** the same as `g_networkSessionActive` (DAT_004962bc which port mirrors as `s_network_active`).

Port:
```c
s_return_screen = (s_network_active) ? TD5_SCREEN_CREATE_SESSION : TD5_SCREEN_MAIN_MENU;
```

`s_network_active` is mapped to `DAT_004962bc` per the port comment at td5_frontend.c:264. In orig, `DAT_004962bc` is set to 1 at NetworkLobby state-0 entry (post-init) — it tracks "network session has reached lobby" rather than "we came in via CreateSession". The gates **do agree on the common path** (both are 1 when navigating from network flow, both are 0 in single-player), but the orig gate flips earlier (during CreateSession state-0) than the port gate. There may be an intermediate moment (CreateSession entered → before lobby state 0 runs) where the port returns the user to MAIN_MENU instead of CREATE_SESSION. Whether that window is reachable from the race-type-category Back button depends on the screen graph — verifying requires runtime trace.

### C.6 — State 4 and state 9 description preview are stubs (MEDIUM cosmetic)

Port stubs both preview-text states. The orig draws a localized title plus body text (12px line spacing) into the 0x110×0xB4 description surface, showing race-type descriptions when hovering over each button. Port shows an empty (or stale-from-init-black) preview pane. **User-visible UX gap**: no race-type descriptions in the menu.

The orig draws from `SNK_RaceTypeText[selected_game_type * 4]` — a per-game-type string table.

### C.7 — State 5 missing cleanup before navigate (MEDIUM, surface leak)

Orig state 5 at frame 0x23 releases:
- `g_lobbyErrorDialogSurface` (0x110×0xB4 description surface)
- `DAT_004962dc` (unclear; another tracked surface)
- `DAT_00496358` (CreateMenuStringLabelSurface result — the header label)
- `ReleaseFrontendDisplayModeButtons` (frees the 7 button objects)
- `ResetFrontendInlineStringTable`

Port state 5 just begins a timed animation and jumps to 0x14, which calls `td5_frontend_set_screen` without these cleanups. **If the port's framework does not implicitly release tracked surfaces / buttons on screen change, this leaks 3 surfaces + buttons every time the user enters and exits RaceTypeCategory.**

### C.8 — State 6 Back button position diverges (LOW)

Orig state 6 cup-button creation places the Back button at:
```c
CreateFrontendDisplayModeButton((byte *)SNK_BackButTxt_exref, uVar9 - 0x90, uVar7 + 0x89, 0x70, 0x20, 0);
// x = halfCanvasW-0x90, y = halfCanvasH+0x89, w=0x70, h=0x20
```
Port:
```c
frontend_create_button("Back", -0xE0, 0, 0xE0, 0x20);
```
Port uses the "slide-in template" position (-0xE0, 0, 0xE0, 0x20) — the slide-in animation should land it, but **port has no per-frame anim in state 7 either**. Net result: the Back button may end up at an unintended position relative to orig pixel layout. Also misses setting `g_frontendEscKeyButtonIndex = 6` and `AdvanceFrontendInlineStringTableState(0,0)`.

### C.9 — State 8 hover transition missing (LOW)

Orig state 8 (cup interactive) transitions to state 9 (cup preview) on hover-change *just like state 3→4*. Port state 8 has no hover→9 branch; preview state 9 is unreachable. (Stubbed-out preview is moot since C.6 already covers content.)

### C.10 — State 8 (port) adds redundant tier re-check (NEUTRAL)

Port re-checks `s_cup_unlock_tier >= N` inside the case-2..5 handlers and plays SFX 10 (reject). Orig doesn't because greyed-out preview buttons are filtered upstream by the press validator. Defensive — not strictly wrong, but inconsistent with orig.

### C.11 — State 8 (port) missing `selected_schedule_index = attract_mode_track_index` (MEDIUM)

Orig state 8 cup-press path:
```c
g_selectedScheduleIndex = g_attractModeTrackIndex;
```
This seeds the schedule index from the attract-mode preview track. Port's path skips this entirely. **Cup races may start on the wrong track**, or `s_selected_schedule_index` may carry stale data from a prior session.

### C.12 — State 0xB returning to state 0 vs port's reset (MEDIUM — surface re-create)

Orig state 0xB at frame 0x23:
- Releases display-mode buttons (✓ would-be in port too if state 0 ran reset_buttons before re-creating)
- Resets inline string table
- Creates the 7 top buttons + Back (fixed-position)
- Sets EscKey=6
- Sets InlineStringTable(SNK_RaceMenu_MT)
- DXSound::Play(5)
- Sets anim=0, state++ (=0xC, which then slides in via animation)

Port state 11:
- `frontend_reset_buttons()`
- `s_inner_state = 0` — **fully re-enters state 0**

State 0 in port re-creates `g_lobbyErrorDialogSurface` and `DAT_00496358` via the LoadTga path and original allocation calls. Combined with C.7 (state 5/0xA not releasing), and the fact that port state-0 doesn't release the prior instance, this is the surface-leak amplifier: every cup-back → top transition allocates new surfaces without releasing the previous pair.

### C.13 — State 0x14 semantic re-purpose (NEUTRAL, internally consistent)

Orig 0x14 = "rearm to state 1 with counter=-1" — a tiny re-init helper. Used as a target by code paths that need to bounce. The port has no such jump; instead 0x14 is used as the "final slide-out + dispatch" state. This works because port doesn't reuse 0x14 for the rearm — but anyone reading port code expecting orig's 0x14 semantics will be confused. Pure naming issue.

---

## Section D — Actionable fixes

Prioritized by user-visible impact, highest first.

### D.1 (HIGH) — Add race-type description preview rendering (case 4 and 9)
Implements C.6 — port state 4 and 9 are currently stubs.
- Locate `SNK_RaceTypeText` table reference in port (likely already imported as a localized-string array).
- In case 4: clear description surface, fetch `RaceTypeText[selected_game_type * 4]`, draw title in localized font + body in small font with 12px spacing into description surface.
- Same logic for case 9 (cup variants).

### D.2 (MEDIUM) — Add explicit cleanup in state 5 and 0xA (C.7, C.12)
Before transitioning to state 0x14, release `g_lobbyErrorDialogSurface` (port's equivalent description surface ptr) and the header label surface. Also `ReleaseFrontendDisplayModeButtons`. Without these, every entry-exit of RaceTypeCategory leaks surfaces.

### D.3 (MEDIUM) — Seed `s_selected_schedule_index = s_attract_mode_track_index` in cup press path (C.11)
Add the assignment to td5_frontend.c case 8 cup-type-confirmed branch, before `s_inner_state = 10`.

### D.4 (MEDIUM) — Clear `g_wantedModeEnabled` (and `g_carSelectChampionshipReturnFlag`, `g_frontendScreenTransitionFlag`) in state 0 (C.1)
Mirror orig 0x4168B0 init. The wanted-mode clear specifically fixes "cop-chase HUD persists after backing out of cop-chase race-type."

### D.5 (LOW) — Add DXSound::Play(4) at slide-in completion (states 1 and 7) (C.2)
Single-line addition where `s_inner_state` transitions to interactive states.

### D.6 (LOW) — DeactivateFrontendCursorOverlay in state 2 before transitioning to 3 (C.3)
Hides cursor during button-driven interaction.

### D.7 (LOW) — Drop `frontend_validate_cup_checksum()` re-check in state 3 button 2; ALSO set `s_replay_file_available = 0` (C.4)
Match orig semantics — the button greying is the gate; runtime re-check is redundant. Also add the missed `s_replay_file_available = 0` write.

### D.8 (LOW) — Verify state 3 Back gate (C.5)
Confirm at runtime that `s_network_active` flips exactly when `DAT_004962c0` does. If not, add a separate `s_network_lobby_entered` (=DAT_004962c0 mirror) and use that for the Back gate.

### D.9 (COSMETIC) — Restore state 6 Back button fixed position + EscKey set + AdvanceFrontendInlineStringTableState (C.8)

### D.10 (DEFER) — Reinstate state-by-state per-frame sprite movement (animation polish)
Currently the timed-animation utility provides smooth interpolation but does not match orig's exact frame curves. Visually similar; revisit only if pixel-parity is required.

---

## Audit summary

- **REG-1 enum remap**: confirmed false alarm; remap is consistent end-to-end. Section B.
- **No critical semantic bugs found** that would prevent any race type from launching.
- **One real bug**: state-0 missing `g_wantedModeEnabled = 0` (D.4).
- **One real bug**: state-8 missing `s_selected_schedule_index = s_attract_mode_track_index` (D.3) — likely cup-track seed bug.
- **Cosmetic gaps**: missing description preview (D.1), missing slide-in SFX (D.5), cursor not deactivated (D.6), Back button position drift (D.9).
- **Resource hygiene**: surface release missing in state 5 / 0xA / 0xB (D.2). Likely contributes to memory growth on repeated screen navigation.
- **Architectural choices** (single timed-animation vs per-frame curves, state-0x14 repurposed as slide-out): internally consistent but documentation drift from orig comments could be improved.

Total fixes recommended: 9 (1 HIGH, 3 MEDIUM, 4 LOW, 1 cosmetic). Surface-leak (D.2) and wanted-mode clear (D.4) and cup-schedule-seed (D.3) are the only items with semantic / resource consequences; everything else is polish.
