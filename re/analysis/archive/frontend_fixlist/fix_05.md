# TD5 Frontend 3-Layer Faithfulness Fix List — Screens 5/6/7/8/9

RESEARCH ONLY. Per-element verification across **L1 CREATION / L2 RENDERING / L3 BEHAVIOR**.
Port = `td5mod/src/td5re/td5_frontend.c` (worktree `fix-1780170781-137846-13319`).
Spec = `re/analysis/frontend_screens/screens_05.md`. SNK strings = `re/analysis/frontend_snk_strings.md`.

## Port input model (grounds every L3 verdict)
- `s_selected_button` = cursor/focus. Moved by kb up/down (`frontend_cycle_selected_button_vertical` :2245-2258), or mouse-select (:2322).
- `s_button_index` = **confirm** index. Reset to -1 every poll (:2163). Set ONLY on Enter/center-click → `=s_selected_button` (:2262, :2311). During a ◄► **cycle it stays -1**.
- `s_arrow_input` = ◄►/▲▼ edge bits (:2194-2197 kb LEFT=1/RIGHT=2; :2306-2307 mouse arrow-zone). `frontend_option_delta()` :1974 → -1/+1 from bits 1/2.
- **KEY PATTERN**: arrow-cycle handlers MUST resolve `int active_button=(s_button_index>=0)?s_button_index:s_selected_button;` and gate the cycle on `active_button`. Gating a cycle on raw `s_button_index==N` is the BUG: keyboard ◄► never sets `s_button_index`, so the cycle only fires on a mouse click in the edge-zone of the already-selected button. Correct exemplar: Screen 7 :6890.
- Overlay render dispatch: `switch(s_current_screen)` :5588-5658 (per-screen `frontend_render_<x>_overlay`). Arrow dispatch: `switch` :5784-5827 (`fe_draw_option_arrows`). A screen absent from a needed dispatch = **L2 BROKEN** (CREATED-but-not-RENDERED), per `frontend_diff_blindspot_postmortem.md`.

Cross-cutting (recorded once): all 5 screens use **hardcoded English literals** not LANGUAGE.DLL `SNK_*` (port has no runtime SNK lookup; only literals + a few SNK-derived static tables). Headers are TGA strips via `frontend_get_title_tga_for_screen` :725, not `CreateMenuStringLabelSurface` menu-font bakes — visually close, **ARCH-DIV [FONT]**, excluded from fix counts. No FLUSH compositor / no 0xc000 edge-bar highlight / no `UpdateExtrasGalleryDisplay` (inert here) — **ARCH-DIV [DECOUPLED]**.

---

### Screen 5 — ScreenMainMenuAnd1PRaceFlow  [interactive Y]
Flow: states 0 build(7 btns)+load MainMenu.tga / 1-2 present / 3 slide-in(0x27) / 4 MAIN interactive (7-btn dispatch) / 5 build Yes/No / 6 confirm interactive / 7 →EXTRAS_GALLERY / 8-9 slide-out→set_screen. Primes: 7 buttons, return/flow_context. Render: buttons via main loop :5661; NO overlay-switch case (none needed — no value text/selectors). Input: confirm-only (no arrows on this screen).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Background MainMenu.tga | OK :6345 | drawn :5568 | n/a | MATCH | — |
| Header "MAIN MENU" | MainMenuText.TGA :727 | title strip :5858 | n/a | DIVERGENCE [FONT] ARCH-DIV | none (orig=`CreateMenuStringLabelSurface(0)`=`SNK_MenuStrings[0]`="MAIN MENU"; image OK) |
| 7 menu buttons | created :6372-6378 hardcoded EN | drawn :5661 loop | dispatch OK :6416-6498 | DIVERGENCE [LABEL] | :6372-6378 use SNK: RaceMenu/QuickRace/TwoPlayer/NetPlay/Options(="OPTIONS")/HiScore(="HIGH SCORES")/Exit ButTxt. Note port "Options"→SNK_OptionsButTxt="OPTIONS"; "High Scores"→SNK_HiScoreButTxt="HIGH SCORES" |
| Btn2 label conditional (app+0x170) | always "Two Player" :6374 | drawn | benchmark via INI :6461 | MATCH | field never written in orig → "Two Player" always correct (SNK_TwoPlayerButTxt). INI benchmark = benign EXTRA |
| Exit-confirm **prompt** "ARE YOU SURE?" | **NOT created** (only Yes/No :6506-6507) | **NOT rendered** | n/a | **MISSING [LABEL]** | Orig `CreateMenuStringLabelSurface(8)`=`SNK_MenuStrings[8]`="ARE YOU SURE?" queued each frame above Yes/No. Add a live `fe_draw_text_centered` of "ARE YOU SURE?" in state 5/6, centered above the Yes/No row (orig label at cx-200, dialog band; port can center over the two buttons). Render in a state-5/6 branch (e.g. small overlay block in render path or directly draw in the buttons-already path). |
| Exit Yes/No buttons | created :6506-6507 "Yes"/"No", 96×32, gap 100 | drawn loop | confirm by label :6516-6529 | DIVERGENCE [LABEL] | :6506-6507 use SNK_YesButTxt="YES" / SNK_NoxButTxt="NO!". Sizes 0x60=96 OK; orig gap 0x80=128 vs port 100 (cosmetic, optional) |
| Exit "Yes" → EXTRAS_GALLERY(22) | :6535 direct set_screen | n/a | reached via state7 | DIVERGENCE [ANIM] | faithful target; orig adds slide-out states 10/0xb/0xc. Optional polish only |
| Controller-required dialog (0x14-0x17) | dropped | — | keyboard-first | **ARCH-DIV** (excluded) | keyboard always present → device gate can't fire. Documented :6545 |
| Selection highlight (0xc000 bars) | per-button ramp :5670 | — | — | **ARCH-DIV** (excluded) | no FLUSH layer |

PORT-WIRING CHECK: render-overlay dispatch? N — not in :5588 switch, but **none required** (no value-text/selector elements; buttons drawn by main loop). arrow dispatch? N — correct (no selectors). input uses active_button fallback? N/A (confirm-only screen, no ◄► cycle).
SCREEN VERDICT: largely faithful. Real gaps: (1) missing "ARE YOU SURE?" prompt; (2) hardcoded labels.
Ordered fixes: **1.** Add "ARE YOU SURE?" prompt render in state 5/6 (MISSING, L2). **2.** Swap 7 menu labels + Yes/No to SNK_* (DIVERGENCE, L1). **3.** (opt) tighten Yes/No gap 100→128.
Fix counts S5 — L1: 2 (menu labels; Yes/No labels), L2: 1 (prompt), L3: 0. ARCH-DIV: 2 (controller dialog, highlight).

---

### Screen 6 — RaceTypeCategoryMenuStateMachine  [interactive Y]
Flow: TOP states 0 build(7) /1 slide(0x20) /2 settle /3 TOP interactive+hover /5 slide-out / CUP states 6 build /7 slide /8 settle /9 CUP interactive /10 slide-out /11 →TOP /0x14 slide-out→set_screen. Primes: 7 buttons, game_type. Hover-desc render: `frontend_render_race_type_description` :4226 via overlay-switch :5589-5591. NO arrow selectors. Input: confirm-only; hover-desc keyed on `s_selected_button` (focus, correct since hover follows focus).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Background MainMenu.tga | OK :6614 | :5568 | n/a | MATCH | — |
| Header "RACE MENU" | RaceMenuText.TGA :728 | :5858 | n/a | DIVERGENCE [FONT] ARCH-DIV | orig `SNK_MenuStrings[1]`="RACE MENU" |
| **Hover-description text** | hardcoded EN arrays `k_race_type_lines`/`k_cup_lines` :4227-4244 | rendered :5590 (IS wired) | indexed by `s_selected_button` :4246 | **DIVERGENCE [LABEL] + index-namespace** | Orig draws `SNK_RaceTypeText[idx]` (single title line, 12-entry table) where idx is a **state-4 button→SNK remap**: TOP btn0→0,btn3→9,btn4→7,btn5→8,btn1→10(="CUP RACE"),btn2→0xb(="CONTINUE CUP"); CUP btn(0..5)→tier+1 (1..6). Port's 4-line port-authored copy is invented and indexed by raw button row. FIX :4226-4267: replace bodies with `SNK_RaceTypeText` single-line title via the remap LUT; drop the 3 invented body lines (orig has title only for this panel) OR keep wrapped body only if orig small-font body confirmed (spec says title + wrapped body from SNK_RaceTypeText[gameType]; SNK_RaceTypeText entries are single strings, so body wrap is of that one string). Index: build `top_remap[7]={0,10,11,9,7,8,-1}` (btn6 Back=no desc) and `cup_remap`=tier+1; pick by `s_inner_state` TOP vs CUP. |
| Hover-desc panel pos (358,145) | :4247-4248 | drawn | n/a | MATCH | orig (cx+0x26,cy-0x5f)=(358,145) ✓ |
| Hover-rebake states 4/9 | folded into live render | n/a | per-frame | ARCH-DIV (excluded) | live render makes rebake unnecessary |
| TOP 7 buttons | :6624-6634 EN; ContCup preview-gated `frontend_validate_cup_checksum` :6627 | drawn loop | dispatch :6659-6707 | DIVERGENCE [LABEL] | use SNK: SingleRace/CupRace/ContCup/TimeTrials/DragRace/CopChase/Back ButTxt. Back width: orig 0x70, port 0xE0 (:6634) → cosmetic width divergence |
| TOP dispatch gameType+ConfigureGameTypeFlags+return | — | — | :6659-6707 (enum remap ARCH-DIV, internally consistent) | MATCH | — |
| Back(btn6) return CREATE_SESSION vs MAIN_MENU | — | — | `s_network_active?...` :6704 | MATCH | — |
| CUP 7 buttons | :6722-6749 EN; preview-gated `s_cup_unlock_tier` | drawn loop | dispatch :6770-6792 | DIVERGENCE [LABEL] | use SNK: Championship/Era/Challenge/Pitbull/Masters/Ultimate/Back ButTxt |
| Cup-tier gating thresholds | port: Challenge/Pitbull `>=1`, Masters/Ultimate `>=2` :6726-6747 | — | matches state9 :6774-6787 | DIVERGENCE [UNCERTAIN] | orig `g_cheatFlagBitfieldGameModes&7`: ==0→Champ+Era only; ==1→+Challenge+Pitbull; >=2→all. Port `s_cup_unlock_tier` integer maps ==0/>=1/>=2 to same sets → **equivalent IF s_cup_unlock_tier is the integer cheat value**. [UNCERTAIN] verify `s_cup_unlock_tier` derivation (memory note warns bitfield |=1/|=2 encoding could differ for bit1-only). If it's a bitfield, value 2 (bit1) would wrongly unlock Masters without Challenge. Verify source of `s_cup_unlock_tier`; align to integer `&7` semantics if needed. |
| Cup press → gameType=tier, schedule=attract, ConfigureGameTypeFlags | — | — | :6793-6805 (`s_selected_track=s_attract_track`) | MATCH | — |
| Slide-in commit anim==0x20 + sfx4 | :6642/:6756 (0x20) | — | sfx4 not explicitly called on TOP/CUP settle | MATCH / minor | orig plays SoundEffect 4 on settle; verify chime. Minor |
| Slide-out commit | port `(16,...)` :6821 | — | — | DIVERGENCE [ANIM] | orig 0x23(35); port 16. Cosmetic timing |

PORT-WIRING CHECK: render-overlay dispatch? **Y** :5589-5591 (`frontend_render_race_type_description`). arrow dispatch? N — correct (no ◄► selectors on this screen). input uses active_button fallback? N/A (confirm-only; hover-desc uses `s_selected_button` which is correct for hover).
SCREEN VERDICT: structurally faithful (desc IS rendered). Top problem is the hover-description content + index namespace.
Ordered fixes: **1.** Replace hover-desc with `SNK_RaceTypeText` + state-4 button→SNK remap LUT, indexed by focus (DIVERGENCE, L1/content). **2.** Verify `s_cup_unlock_tier` integer-vs-bitfield gating (L3 [UNCERTAIN]). **3.** Swap TOP+CUP button labels to SNK_*; Back width 0xE0→0x70 (L1). **4.** (opt) slide-out 16→0x23.
Fix counts S6 — L1: 2 (button labels both menus; hover-desc content), L3: 1 (cup gating verify, conditional). ARCH-DIV: 2 (rebake states, highlight).

---

### Screen 7 — ScreenQuickRaceMenu  [interactive Y]
Flow: 0 build(4 btns)+panel /1-2 present /3 slide-in(0x27) /4 interactive (◄► car/track + OK/Back) /5 prep /6 slide-out→start race or set_screen. Primes: car/track ids, 4 buttons (0/1 selectors). Overlay render: `frontend_render_quick_race_overlay` :3955 via :5592-5594 (IS wired). Arrows: dispatch :5785-5788 draws arrows 0+1 (IS wired).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Background MainMenu.tga | OK :6844 | :5568 | n/a | MATCH | — |
| Header "QUICK RACE" | QuickRaceText.tga :729 | :5858 | n/a | DIVERGENCE [FONT] ARCH-DIV | orig `SNK_MenuStrings[3]`="QUICK RACE" |
| Car+Track info panel | no backing fill; text only | car/track names rendered :3969-3970 (wired :5593) | rebake implicit (live) | DIVERGENCE | panel frame/background MISSING (orig clears 0x208×200 surface). Cosmetic; text present |
| Car name / Track name pos | port (140,106)/(140,226) :3969-3970 | drawn | n/a | DIVERGENCE | orig panel origin (cx-200,cy-0x8f)=(120,97); verify alignment vs orig screenshot |
| "LOCKED" overlay | hardcoded "LOCKED" :3971-3972 | drawn | car/track lock + !cheat gate :3963-3968 | DIVERGENCE [LABEL] | use SNK_LockedTxt="LOCKED" (same text; localize) |
| Btn0 "CHANGE CAR" + ◄► | created :6861 (120,137,256,32), `is_selector=1` :6862 | label drawn; **arrows drawn** :5786 | cycle OK :6891 (active_button used :6890) | MATCH / DIVERGENCE [LABEL] | SNK_ChangeCarButTxt="CHANGE CAR" |
| Btn1 "CHANGE TRACK" + ◄► | created :6863 (120,257), selector :6864 | arrows :5787 | cycle OK :6902 | MATCH / DIVERGENCE [LABEL] | SNK_ChangeTrackButTxt="CHANGE TRACK" |
| Btn2 "OK" | :6865 (120,377,96,32) | drawn | OK dispatch :6915 raw `s_button_index==2` (confirm → correct) | MATCH / [LABEL] | SNK_OkButTxt="OK" |
| Btn3 "BACK" | :6866 (232,377,112,32) | drawn | :6930 confirm | MATCH / [LABEL] | SNK_BackButTxt="BACK" |
| ◄► cycle dispatch | — | — | **uses `selected_button` fallback :6890** ✓ | **MATCH (L3 correct)** | exemplar of right pattern; wrap bounds match cheat/network :6895-6908 |
| OK locked-block (sfx10) | — | — | :6917-6924 | MATCH | — |
| OK→start race (return=-1 sentinel) / Back→MAIN_MENU | — | — | :6926/:6931, state6 :6946-6951 | MATCH | sentinel = -1 |
| Slide-in 0x27 / slide-out 16 | :6881 / :6945 | — | — | MATCH / [ANIM] | slide-out orig 0x10=16 ✓ |

PORT-WIRING CHECK: render-overlay dispatch? **Y** :5592-5594. arrow dispatch? **Y** :5785-5788 (arrows 0+1). input uses active_button fallback? **Y** :6890 (correct).
SCREEN VERDICT: behaviorally faithful and correctly wired (the model L3 example). Only L1 [LABEL] swaps + minor panel/position cosmetics remain.
Ordered fixes: **1.** Swap labels to SNK_* (Change Car/Change Track/OK/Back, LOCKED) (L1). **2.** (opt) panel backing fill + car/track text origin to (120,97)-relative. **3.** (none behavioral).
Fix counts S7 — L1: 1 (label swap batch; positions optional). L2: 0 (all wired). L3: 0. ARCH-DIV: 1 (highlight).

---

### Screen 8 — RunFrontendConnectionBrowser  [interactive Y] — LIST BROWSER
Flow: 0 enumerate+3 btns /1-2 slide /3-4 present /5 interactive (OK/Back) /6-7 no-op→5 /8-9 slide-out→set_screen. Primes: 3 buttons ("Provider"/OK/Back). The peer-list panel, rows, scroll indicators, selection bar = orig DXPTYPE list = **ARCH-DIV** (note, do not implement the net layer). **In scope:** button-set fidelity + input-handler correctness.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Background MainMenu.tga | OK :6969 | :5568 | n/a | MATCH | — |
| Header "CHOOSE CONNECTION" | NetPlayText.TGA :739 (generic "NET PLAY") | :5858 | n/a | DIVERGENCE [LABEL] ARCH-DIV | orig `SNK_MenuStrings[5]`="NET PLAY" baked menu-font; shared net title is acceptable. SNK_ChooseConnectionButTxt="CHOOSE CONNECTION" is the list-panel button label, not header |
| List-panel button (slot0) | "Provider" 120,160,256,32 :6971 | drawn as plain button | btn0 press → sfx2 only :7001 | DIVERGENCE [LABEL] | label→SNK_ChooseConnectionButTxt="CHOOSE CONNECTION"; orig size 0x1f0×0x80=496×128 (list container). Container/rows = ARCH-DIV |
| **List rows / scroll ▲▼ / selection bar** | not created | not rendered (no overlay case) | no list-nav | **MISSING — ARCH-DIV** (excluded) | DXPTYPE peer enumeration; out of scope |
| OK button (slot1) | "OK" -100,0,100,0x20 :6972 | drawn | :7003 →SESSION_PICKER(9) | DIVERGENCE [LABEL] | SNK_OkButTxt; orig width 0x60=96 (port 100); seed -0x1f0 vs -100 cosmetic |
| BACK button (slot2) | "Back" -100,... :6973 | drawn | :7006 →MAIN_MENU(5) | DIVERGENCE [LABEL] | SNK_BackButTxt; orig width 0x70=112 |
| State5 btn0 list-enter | — | — | :7001 `s_button_index==0 && s_arrow_input!=0` → sfx2 | **BUG (latent, ARCH-DIV-masked)** | Pattern bug: gates on raw `s_button_index==0 && arrow`. Since no peer list exists this is inert, but it's the **same broken gate** as S9. If any LAN list is ever added, fix to `active_button` + enter-list-nav (state6). For now: ARCH-DIV (no list) — note only |
| State6/7 list-nav | no-op→5 :7019-7022 | — | — | MISSING — ARCH-DIV (excluded) | net layer |
| Computer-name seed | `frontend_net_enumerate` :6968, no name | — | — | MISSING (minor) | orig GetComputerNameA / "Clint Eastwood" fallback; cosmetic, low priority |
| Slide-in 0x10 / slide-out 16 | :6983 / :7030 | — | — | DIVERGENCE [ANIM] | orig in 0x14(20), out 0x18(24); cosmetic |

PORT-WIRING CHECK: render-overlay dispatch? N — none (list = ARCH-DIV). arrow dispatch? N — none (no selectors). input uses active_button fallback? **N** :7001 (raw `s_button_index==0`) — but inert (no list).
SCREEN VERDICT: net list = ARCH-DIV (excluded). In-scope real fixes are L1 label/width swaps; the OK/Back **navigation targets are correct** (SESSION_PICKER / MAIN_MENU). The btn0 arrow gate is the broken pattern but inert here.
Ordered fixes: **1.** Swap 3 button labels to SNK_* (ChooseConnection/OK/Back) + widths 96/112 (L1). **2.** (opt) computer-name seed. **3.** (opt) anim timings 20/24.
Fix counts S8 — L1: 1 (label/width batch). L2: 0. L3: 0 in-scope (btn0 gate inert). ARCH-DIV: list rows/scroll/selection-bar/list-nav (4), highlight (1).

---

### Screen 9 — RunFrontendSessionPicker  [interactive Y] — LIST BROWSER
Flow: 0 build /1 list /2 slide-in /3 interactive (OK=join / Back) /4 list-nav / 6/7 slide-out→set_screen. Orig button set = **3 buttons**: slot0 list-panel (SNK_ChooseSessionButTxt), slot1 OK, slot2 Back(ESC=2). Session rows/scroll/selection-bar/live-refresh = DXPTYPE **ARCH-DIV**. **In scope:** spurious 4th button, OK-doesn't-join behavior, suspected arrow-gate bug.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Background MainMenu.tga | reloads :7047 | :5568 | n/a | DIVERGENCE (benign) | orig screen9 reuses screen8 primary (no reload); port reload harmless |
| Header "CHOOSE SESSION" | NetPlayText.TGA :739 | :5858 | n/a | MATCH (inherit) [LABEL] | orig inherits g_currentScreenIndex (not rebuilt) → port reusing net title matches behavior. SNK_ChooseSessionButTxt is the panel button label not header |
| List-panel button (slot0) | "Session" 120,160,256,32 :7048 | drawn plain | btn0 → sfx2 :7069 | DIVERGENCE [LABEL] | label→SNK_ChooseSessionButTxt="CHOOSE SESSION"; orig 0x1f0×0x80 container = ARCH-DIV |
| **Spurious "Create" button** | created :7049 "Create" (slot1) | drawn | btn1 → state4 → CREATE_SESSION :7071,:7083 | **EXTRA + BUG** | **Orig has NO 4th button** (3 total: list/OK/Back). Port's "Create" shifts OK→slot2, Back→slot3, breaking the index contract + ESC. **FIX: delete :7049**; renumber: OK→slot1, Back→slot2 |
| OK / New button (orig slot1) | port "OK" is slot2 :7050 | drawn | :7073 `s_button_index==2` → CREATE_SESSION, **no chosen-index write** | **BUG (join lost)** | Orig OK JOINS the highlighted session: write chosen row index (cursor-1) to session block, return=CREATE_SESSION (lobby entry). Port OK blindly →CREATE_SESSION. After deleting Create, change handler to `s_button_index==1` (OK, now slot1); the chosen-row write is ARCH-DIV (no list) but the **OK→CREATE_SESSION target is the correct lobby-entry path** so OK should keep →CREATE_SESSION. Net effect: with no list, OK==join==create-lobby is acceptable; the FIX is removing Create so OK is the single primary action |
| BACK button (orig slot2) | port "Back" slot3 :7051 | drawn | :7076 `s_button_index==3` → CONNECTION_BROWSER | DIVERGENCE [LABEL]+index | after Create removed → Back=slot2; change :7076 to `s_button_index==2`; SNK_BackButTxt; width 0x70=112. ESC=2 then correct |
| **btn0 arrow-cycle gate** | — | — | :7069 `s_button_index==0 && s_arrow_input!=0` → sfx2 | **BUG (pattern)** | **Confirmed broken gate** (per brief): keyboard ◄►/▲▼ never sets `s_button_index` (stays -1), so list-row nav via keys is dead — only a mouse arrow-zone click on already-selected slot0 fires. Correct form: `int active_button=(s_button_index>=0)?s_button_index:s_selected_button;` then gate on `active_button==0`. **However** the list itself is ARCH-DIV (no rows to scroll), so the row-nav this would enable has no data. Fix the gate for correctness/consistency; full row-nav stays ARCH-DIV |
| State3 OK-join writes chosen index | — | — | port writes nothing | MISSING — ARCH-DIV (excluded) | DXPTYPE session block (+0xc00); net layer |
| **List rows / NEW SESSION row / scroll / selection bar / live refresh** | not created | not rendered (no overlay case) | — | **MISSING — ARCH-DIV** (excluded) | DXPTYPE enumeration |
| State4 list-nav | repurposed as create redirect :7083 | — | — | BUG/MISSING — ARCH-DIV (excluded after Create removal) | after deleting Create, state4 is unused; orig state4 = row nav (ARCH-DIV) |
| Slide-in 0x10 / slide-out 16 | :7061 / :7094 | — | — | DIVERGENCE [ANIM] | orig in 0x20(32), out 0x18(24); cosmetic |

PORT-WIRING CHECK: render-overlay dispatch? N — none (session list = ARCH-DIV). arrow dispatch? N — none (no value selectors). input uses active_button fallback? **N** :7069 (raw `s_button_index==0`) → **the confirmed pattern BUG**.
SCREEN VERDICT: the one screen with genuine in-scope behavioral BUGS. (1) spurious Create button + shifted OK/Back indices+ESC; (2) broken arrow-gate pattern at :7069; net-list itself stays ARCH-DIV.
Ordered fixes: **1.** Delete spurious "Create" button :7049; renumber dispatch OK→`==1` :7073, Back→`==2` :7076; remove dead state4 :7083 / route OK directly to state5 (BUG/EXTRA, L1+L3). **2.** Fix arrow gate :7069 to `active_button=(s_button_index>=0)?s_button_index:s_selected_button` (BUG, L3) — for consistency (row data ARCH-DIV). **3.** Swap labels to SNK_* + Back width 112 (L1). **4.** (opt) anim 32/24.
Fix counts S9 — L1: 2 (remove Create / renumber; label+width swaps). L2: 0. L3: 2 (OK-index after Create removal; arrow-gate pattern). EXTRA: 1 (Create). ARCH-DIV: session rows/NEW-SESSION/scroll/selection-bar/live-refresh/list-nav (6), highlight (1).

---

## Summary
- **S5** [Y]: L1×2, L2×1 (missing "ARE YOU SURE?" prompt), L3×0. ARCH-DIV: controller dialog, highlight. Top fix: add "ARE YOU SURE?" prompt.
- **S6** [Y]: L1×2 (labels; hover-desc content), L3×1 (cup-gating verify). ARCH-DIV: rebake states, highlight. Top fix: hover-desc → SNK_RaceTypeText + state-4 button→SNK remap.
- **S7** [Y]: L1×1 (label batch), L2×0, L3×0 — correctly wired, model exemplar. Top fix: SNK label swaps. ARCH-DIV: highlight.
- **S8** [Y]: L1×1 (label/width batch), L3×0 in-scope (arrow gate inert; no list). Top fix: SNK labels/widths. ARCH-DIV: net rows/scroll/bar/nav (4), highlight.
- **S9** [Y]: L1×2, L3×2 (spurious Create + OK index; broken arrow gate :7069). EXTRA×1. **Highest-priority screen.** Top fix: remove "Create" + renumber OK/Back + fix arrow gate. ARCH-DIV: net rows/NEW-SESSION/scroll/bar/refresh/nav (6), highlight.

**Overall top fix (cross-screen):** S9 remove spurious "Create" button + renumber OK(slot1)/Back(slot2)/ESC + fix the `s_button_index`→`active_button` arrow-gate at :7069 (and inert twin at S8 :7001). This is the only set of true behavioral bugs in scope; everything else is L1 SNK_* label localization (batchable), the S5 missing exit prompt, and the S6 hover-description content/index fix.

**No-guessing notes:** S6 `s_cup_unlock_tier` integer-vs-bitfield semantics = [UNCERTAIN] — verify its writer before trusting the gating equivalence (memory note warns of a |=1/|=2 bitfield where value 2 alone would mis-unlock). All SNK strings cited from `frontend_snk_strings.md` (verified exports). Net-list rendering on S8/S9 explicitly **ARCH-DIV (DXPTYPE)** per brief — excluded from fix counts; only the button-set + input-handler bugs are counted.

File: `re/analysis/frontend_fixlist/fix_05.md`
