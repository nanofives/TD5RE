# TD5 Frontend — Master Faithfulness Fix Plan (2026-06-01)

Source: the corrected 3-layer audit `re/analysis/frontend_fixlist/fix_*.md` (all 30 screens,
each element checked at L1 creation / L2 rendering / L3 behavior — not the old creation-only
diff). Verification: build + frame-dump (`TD5RE_FRAMEDUMP=<path>`) per screen; never ship blind.

## Method per fix (the thought process, generalized)
For each element: confirm (L1) it's created with the real SNK_ label/pos/size/gating,
(L2) it's wired into the render dispatch that draws it (overlay switch ~:5560, arrow
switch ~:5784, or the flush), and (L3) the input handler reacts with the
`active_button = (s_button_index>=0)?s_button_index:s_selected_button` fallback + correct
global/value/nav. A fix is done only when all three hold AND the frame-dump confirms it.

## Already faithful — DO NOT TOUCH (audit confirmed)
S0, S2, S7 (exemplar), S12, S16, S19 (Music Test, just fixed). Also: the old diff's
"Config→STATS", "Reverse→BACKWARDS", and "missing title strips" claims were FALSE — port
already correct; do not "fix" them.

## PHASE A — L3 input/behavior bugs (highest value; the Music-Test bug class)
1. **S23 + S25 high-score highlight** — `:4784` golds row 0; must bold the inserted rank:
   gate on `i == s_score_insert_pos` (computed at :10280/10208 but never read); reset to -1 at
   each screen's case 0. (Port lacks SmallTextb bold atlas → render as the gold/color highlight
   on the correct row.) ONE fix file, fixes BOTH screens (shared overlay).
2. **S9 SessionPicker** — delete the spurious "Create" button (`:7049`), renumber OK→slot1 /
   Back→slot2 + ESC index; fix arrow gate `:7069` to active_button fallback.
3. **S14 ControlOptions** — wire device-source ◄► cycling in case 6 (write g_player1/2InputSource
   via td5_input_set_input_source); currently case 6 only handles Configure/OK.
4. **S20 CarSelection** — Stats slot-2 ◄► must cycle wheel-scheme 0..3 (case7) — dead in input.
5. **S11 Lobby** — buttons use raw s_button_index (`:7249`) → keyboard nav dead; active_button fallback. (netplay-adjacent; lower urgency)

## PHASE B — L2 rendering (created but not drawn / wrong value shown)
6. **S14** device-NAME panel (`:4557` draws icons only) + add CONTROL_OPTIONS title; add Stats arrows to dispatch for S20 (`:5801`).
7. **S15 Sound** — render SFX-mode NAME text "MONAURAL/STEREO/3D SOUND" at (394,97) (icon-only now).
8. **S17 TwoPlayer** — value text: split = "LEFT/RIGHT"/"UP/DOWN" (SNK_Split_Modes), catchup = numeric "%d" 0..9 — port wrongly shows ON/OFF (hides the level).
9. **S20** Stats arrows render (pair with #6); **S24** grey ViewReplay/ViewRaceData when absent.
10. **S3 LanguageSelect** — add overlay case: "LANGUAGE SELECT" header (literal @0x4667c0, NOT SNK_) + 4 corner flag images from Language.tga (currently 4 text buttons, no overlay).
11. **S4 Legal + S2 Attract** — wire `frontend_render_fade()` (state1/3 are no-op counters → screen pops); shared 64-row dither-wipe primitive.

## PHASE C — L1 labels / correctness
12. **S13 GameOptions Dynamics array INVERTED** (`:4002` {"SIMULATION","ARCADE"} → {"ARCADE","SIMULATION"}; 0=arcade) — real correctness bug, shows wrong word per state.
13. **S6 RaceType** hover description → SNK_RaceTypeText via the state-4 button→SNK remap (port hardcodes English; verify s_cup_unlock_tier is integer not bitfield first).
14. **S27 CupWon** label text (CONGRATULATIONS!, YOU HAVE WON THE, SNK_CarsUnlocked/TracksUnlocked); **S10** session-name title; **S5** "ARE YOU SURE?" exit prompt; batch SNK_* label swaps across menus.
15. **S26/S27** ESC→parent vs OK→MAIN_MENU mismatch; dialog panels opaque vs translucent.

## PHASE D — Music Test residual (from fix_15)
16. **S19** cover art keys on live cursor `s_music_test_track_idx`; orig keys on `g_attractCdTrackCandidate` (set on SELECT confirm only) → cover should change on confirm, not while cycling. (Small, verify against original intent.)

## ARCH-DIV — out of scope (note, don't fix)
Netplay rendering/peer layer (S8/S9/S10/S11/S29 DXPTYPE), name-entry DXInput vs Win32 input
model (S25 — but caret/default/box details ARE in scope), S18 wheel/pedal class + live joystick
(keyboard surrogate; flag), S22 credits scroll-reel vs paged slideshow, decoupled-flush→live-render
model, no-bold-atlas (S23/25 render highlight as color instead).

## Sequencing
Do A → B → C → D. Build + frame-dump after each screen (or small batch). Commit per phase to
the worktree branch (no merge until user approves). Self-verify every screen via frame-dump
BEFORE reporting; relaunch no-auto-close for user confirmation at phase boundaries.
