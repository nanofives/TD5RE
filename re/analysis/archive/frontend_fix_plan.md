# TD5 Frontend Faithfulness — Master Fix Plan (2026-05-31)

Goal: make all 30 frontend screens faithful to the original. Built from the complete
port-vs-original diff (`re/analysis/frontend_diff/diff_*.md`) over the exhaustive spec.
Strategy: **fix cross-cutting FOUNDATIONS first** (each closes gaps on many screens), then
the residual per-screen items. Verify at each phase.

## Cross-cutting findings (appear on nearly every screen)

The port's frontend diverges mostly through 4 shared root causes, not 30 independent bugs:

### F1 — FONT SYSTEM  (highest impact; touches ~all screens)
- Port uses ONLY the 24px **BodyText** atlas (which is actually the original's *localized/CJK* font, 24×24/10-col), **scaled** down for small text. The original's default-English path uses:
  - **SmallText** (12×12, 21-col) — button labels + table/panel/list text.
  - **SmallTextb** (bold 12×12) — the highlighted/inserted row (root of the S23/S25 highlight bug — there's no bold variant in the port).
  - **MenuFont** (36×36, 7-col) — the big title-strip banners ("OPTIONS", etc.).
- RISK: a naive SmallText swap already "ruined labels" once — the repo `smalltext.png` likely isn't the right atlas/layout. **Prerequisite: extract & verify the real `SmallText2.tga`/`SmallTextb`/`MenuFont` from `original/Front End/frontend.zip` before any code change.**
- Closes: button-label size on ALL menu screens; table text on 23/24/25; title banners on 12/13/14/20/21/etc.; the highlight-row bug on 23/25.

### F2 — ANIMATION MODEL  (touches all animated screens)
- Port slides are **wall-clock/ms-timed**; original is **`g_frontendAnimFrameCounter` frame-count, transitions on exact equality** at per-screen thresholds (0x14/0x18/0x20/0x23/0x27…). Port uses a uniform 16 everywhere → wrong speeds + "abrupt" transitions; dialogs 26/27/29 have no horizontal slide; caret blink (25) is wall-clock not `anim&0x20`.
- Closes: transition feel on all screens; the "abrupt animation" the user reported; caret blink (25).

### F3 — DECOUPLED-DRAW LAYER  (the flush)
- Port has no flush-equivalent for: **selection-highlight edge bars** (0xc000, `RenderFrontendDisplayModeHighlight`) — missing on every menu; **album/band cover art** (Music Test 19, `UpdateExtrasGalleryDisplay`); list rows/highlight (8/9), lobby panels (11).
- Closes: selection highlight everywhere; Music Test album art (the headline omission).

### F4 — SNK_ LABEL STRINGS  (touches all text)
- All labels are hardcoded English guesses, several **wrong** (S6 hover text wrong namespace; S20 "Stats"/"Automatic"/"Backwards"; S25 default "PLAYER"; missing headers). Original reads `LANGUAGE.DLL` `SNK_*` tables.
- PREREQUISITE: extract the `SNK_*` string tables from `LANGUAGE.DLL` (not in the EXE).
- Closes: correct label TEXT (and lengths → correct widths) on all screens.

## Real BUGs (port regressions — fix regardless of foundations)
| Screen | Bug | Fix |
|---|---|---|
| 23 & 25 | High-score highlight golds **row 0** (`:4743`); should bold the **inserted rank** (`s_score_insert_pos`, computed at `:10208` but never read) | gate highlight on `i==s_score_insert_pos` + bold font (needs F1 SmallTextb) |
| 4 | Fade states 1/3 are **no-ops** (never call the fade renderer) → legal splash pops | wire `frontend_render_fade()` (`:6259/:6275`) |
| 9 | Spurious "Create" button + OK doesn't join selected session | remove extra button; OK joins highlighted row |
| 16 | Fog row gating (2 bugs — verify) | per diff_15 |
| 17 | Split-mode/catchup shown as ON/OFF instead of mode-name/numeric 0..9 | render real value text |
| 14 | Device-source ◄► cycling MISSING + device-name panel MISSING (P0) | wire `td5_input_set_input_source` on rows 0/2 + draw name panel |
| 3 | flag-IMAGE buttons replaced by text buttons; header missing | use Language.tga flags + add header |

## MISSING whole elements
- **19 album cover art** (F3). **11 lobby panels**, **8/9 list rows** (netplay — partly ARCH-DIV). **22 credits scroll-reel text** (port shows image pages only). **14 device-name panel**. **15 SFX-mode name box**. **3 flag images**. **28 empty centered box**.

## ARCH-DIV (intentional — out of faithfulness scope unless you say otherwise)
- Netplay rendering (8/9/10/11/29) — no DXPTYPE peer layer.
- Name entry input (25) via Win32 `GetAsyncKeyState` vs DXInput edit-box (behavior details still fixable: caret, default-name, box).
- D3D11 vs DDraw surface model (translucent vs opaque panels — can be matched).

## Phasing (verify each before next)
1. **F1 prerequisite** — extract/verify the real SmallText/SmallTextb/MenuFont atlases. (investigation; in progress)
2. **F1 font system** — implement the 3 fonts in the bake + fe_draw_text; rebuild; visual check. ⚠ check with user before the swap (prior attempt regressed).
3. **F4 prerequisite** — extract LANGUAGE.DLL SNK_ tables; then route labels.
4. **F2 animation** + **F3 decoupled draws** (selection highlight, then album art).
5. **Per-screen residuals** (bugs table + missing elements), screen by screen, against diff_*.md.
6. ARCH-DIV items only if requested.

Self-contained BUGs (23/25 highlight, 4 fades, 17 values) can be done alongside F1 since they're small.
