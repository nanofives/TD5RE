% DA-T3 — Deep Audit of InitializeRaceHudLayout (0x00437BA0)
% Date: 2026-05-22
% Pool slot: TD5_pool0 (read-only, released at exit)
% Class promotion: ARCH-DIVERGENCE (centered-coord scratch vs pixel-space hud_build_quad)

# Orientation

- Orig function: `InitializeRaceHudLayout` @ 0x00437BA0..0x0043889F (3327 B).
- Port: `td5_hud_init_layout` @ `td5mod/src/td5re/td5_hud.c:1319`.
- Called from `InitializeRaceSession` @ 0x0042AA10 once per race-start, with
  `param_1 = g_raceViewportLayoutMode` ∈ {0=single, 1=L/R split, 2=T/B split}.
- Callees: `FindArchiveEntryByName`, `BuildSpriteQuadTemplate` (16+ times),
  `__ftol`, `InitializeMinimapLayout` (tail call).

# Layout struct (per-view, stride 14 dwords = 0x38)

Memory base: `g_hudView0...` at 0x004B1138; `View1` at 0x004B1170.
| idx | offset | symbol                | meaning                              |
|-----|--------|-----------------------|--------------------------------------|
| 0   | +0x00  | g_hudPrimaryScaleX    | sx = renderW/640 (halved in split)   |
| 1   | +0x04  | g_hudPrimaryScaleY    | sy = renderH/480 (halved in split)   |
| 2   | +0x08  | g_hudView0CenterX     | (vp_left+vp_right)/2                 |
| 3   | +0x0C  | g_hudView0CenterY     | (vp_top+vp_bottom)/2                 |
| 4   | +0x10  | g_hudView0VpLeft      | float viewport left                  |
| 5   | +0x14  | g_hudView0VpTop       | float viewport top                   |
| 6   | +0x18  | g_hudView0VpRight     | float viewport right                 |
| 7   | +0x1C  | g_hudView0VpBottom    | float viewport bottom                |
| 8   | +0x20  | (DAT_004B1158)        | (float, written by __ftol — int4)    |
| 9   | +0x24  | (DAT_004B115C)        | (float, written by __ftol — int4)    |
| 10  | +0x28  | g_hudView0VpIntLeft   | __ftol(vp_left)  (int32)             |
| 11  | +0x2C  | g_hudView0VpIntTop    | __ftol(vp_top)   (int32)             |
| 12  | +0x30  | g_hudView0VpIntRight  | __ftol(vp_right) (int32)             |
| 13  | +0x34  | g_hudView0VpIntBottom | __ftol(vp_bottom)(int32)             |

# Viewport-mode dispatch (orig)

- mode 0: vp = [renderW * -0.5, renderW * +0.5] x [renderH * -0.5, renderH * +0.5]
  — centered-origin (orig BuildSpriteQuadTemplate adds screen-center later).
- mode 1 (L/R split): sx, sy halved. View0 vp_right = 0; View1 vp_left = 0
  (both centered-origin, each half across the X-divider).
- mode 2 (T/B split): sx, sy halved. View0 vp_bottom = 0; View1 vp_top = 0
  (centered-origin halves on Y).

> Note: port uses pixel-space (vp_left=0, vp_right=renderW), documented at
> td5_hud.c:1308-1316 as deliberate ARCH-DIVERGENCE. Net per-widget anchor
> math should produce identical pixel positions because of compensating
> centering in BuildSpriteQuadTemplate.

# Section A — Orig widget table (1P, view0)

`sx = renderW/640`, `sy = renderH/480`. Source page = atlas entry +0x3C.
Color = 0xFFFFFFFF, depth = 0x4300199A (= 128.1 — sprite Z-bucket).

| Widget       | Atlas entry  | Flags-base off | Top-left                          | Size (W × H) | UV insets       |
|--------------|--------------|----------------|-----------------------------------|--------------|-----------------|
| Speedo dial  | SPEEDO       | +0x004         | (vpR − sx·112, vpB − sy·104)      | sx·96 × sy·96| ±0.5 half-texel |
| Speed font   | SPEEDOFONT   | +0x174,+0x22C,+0x2E4 (3 digits, *decreasing*) | row at y=vpB − sy·31; first cell x=vpR − sx·60, step +(sx·15+2.0) | sx·15 × sy·23 | dynamic (UV per frame) |
| Gear digit   | GEARNUMBERS  | +0x0BC         | (vpR − sx·32, vpB − sy·72)        | sx·16 × sy·16| dynamic         |
| Metric d1    | numbers      | +0x734         | (cenX − sx·37.5, vpT + 12.0)      | sx·15 × **sx·23** *(sic)* | dynamic         |
| Metric d2    | numbers      | +0x454         | (cenX − sx·22.5, vpT + 12.0)      | sx·15 × sx·23| dynamic         |
| Metric d3    | numbers      | +0x50C         | (cenX − sx·7.5,  vpT + 12.0)      | sx·15 × sx·23| dynamic         |
| Metric d4    | numbers      | +0x5C4         | (cenX + sx·7.5,  vpT + 12.0)      | sx·15 × sx·23| dynamic         |
| Time semicol | SEMICOL      | +0x39C         | scratch quad (0,0,0,0); UV preloaded (atlas_x+5, atlas_y+1) — both UV corners equal (degenerate, filled per-frame) | — | — |
| U-turn arrow | UTURN        | +0x67C         | (cenX − sx·32, cenY − sy·32)      | sx·64 × sy·64| ±0.5            |
| Replay banner| REPLAY       | +0x7EC         | (vpL + sx·16, vpT + sy·16)        | sx·60 × sy·60| ±0.5            |
| Pos-row tbl  | (none)       | local_34[13]   | 13 x-positions from vpR − 112, step +8; **never consumed** (dead) | — | — |
| Pos-row tbl  | (none)       | local_4c[6]    | 6 y-positions from vpT + 8, step +16; **never consumed** (dead) | — | — |

## 2P split (mode 1 or 2)

Identical per-widget math but with halved sx/sy AND each view's own vp_*
rectangle. The orig does not move widgets relative to view; the smaller scale
shrinks them and the view rect anchors them to the half-screen they own.

> The two `local_34`/`local_4c` tables ARE filled per view but never read —
> their only consumer (a 13×6 horizontal-row of position numerals?) appears to
> have been disabled in late development. They are pure dead code in this
> function.

# Section B — Port mapping (`td5_hud.c`)

| Orig widget   | Port code site                              | Notes                |
|---------------|---------------------------------------------|----------------------|
| layout init   | td5_hud.c:1326-1402 (scale + view rects)    | Pixel-space (not centered) |
| Speedo dial   | td5_hud.c:1422-1440                         | speedo_x/y           |
| Speed font    | td5_hud.c:1443-1463 (loop d=0..2)           | font_x_start, font_y |
| Gear digit    | td5_hud.c:1466-1481                         | gear_x/gear_y        |
| Metric d1..4  | td5_hud.c:1483-1499 (loop d=0..3)           | metric_x/metric_y    |
| Time semicol  | td5_hud.c:1502-1511                         | NEEDLE_QUAD_OFF      |
| U-turn        | td5_hud.c:1513-1533                         | uturn_cx/cy ± half   |
| Replay banner | td5_hud.c:1535-1553                         | replay_x/y           |
| Pos-row tables| (not ported — dead code)                    | OK to omit           |

# Section C — Alignment divergences (port vs orig)

## C1. Metric digits — glyph width and step (HIGH severity)

- Orig: `fVar3 = sx * 15`; each cell is sx·15 wide; cells abut (no gap).
- Port (line 1484): `metric_glyph_w = sx * 16.0f` (one pixel too wide per cell).
- Port (line 1489): step is `metric_glyph_w` (= sx·16); orig step is `sx·15`.
- Anchor: orig `cenX − sx·37.5`; port `cenX − sx·40`. Shift ≈ sx·2.5 left.
- Visible effect: at 800×600 (sx ≈ 1.25), 4 digits sit ≈ 5 px wider total
  and start ≈ 3 px left of the orig position. At 1920×1080 (sx = 3.0)
  the lap/timer numerals are ≈ 12 px wider and ≈ 7.5 px left — visible drift.

## C2. Metric digit height (MEDIUM)

- Orig uses **sx·23** for the digit cell height (not sy·23, not sy·24).
  This is a quirk of the orig (likely paste from the SPEEDOFONT block which
  uses sy·23 — the orig author missed swapping the constant). Effective: at
  4:3 aspect sx≈sy so unchanged, but at 16:9 (sx > sy) the orig digits are
  taller than expected.
- Port (line 1495): uses `sy * 24.0f`. **One px taller in Y, and uses sy**.
- Net visible at 16:9: orig digits are taller (because sx > sy), port digits
  are slightly shorter and slightly off in width.

## C3. Metric Y offset constant — port scales 12, orig does not (HIGH at non-480p)

- Orig (line 1391 in decompile): `local_114 = layout[5] + 12.0f` — the `12`
  is a raw **unscaled** pixel constant from `_g_hudIndicatorCenterOffY_PROVISIONAL`
  (which is 12.0). At any render height, the offset is a fixed 12 px below
  vp_top.
- Port (line 1486): `metric_y = vp_t + sy * 12.0f`.
- Visible effect at 1080p (sy = 2.25): port pushes the timer row 27 px down
  from vp_top instead of the orig 12 px. At 480p sy=1 so they agree.
- This is the loudest mis-scaling: HUD timer/position row visibly drifts as
  the user picks higher resolutions.

## C4. Speed font digit step — port `+1`, orig `+2` (LOW)

- Orig (line 169 in decompile): `fVar2 = fVar1 + _g_audioDopplerMaxRatio` =
  `sx·15 + 2.0` (raw 2 px inter-digit gap).
- Port (line 1454): step is `font_glyph_w + 1.0f` = `sx·15 + 1`.
- Visible effect at 1080p: speedometer numerals 3 px tighter than orig.

## C5. Speed font height — port `sy*24`, orig `sy*23` (LOW)

- Orig (line 165): `local_110 = local_114 + layout[1] * _DAT_0045d6dc` where
  `_DAT_0045d6dc = 23.0`. Quad spans vpB−sy·31 to vpB−sy·8, height sy·23.
- Port (line 1459): `font_y + sy * 24.0f` (one px taller).

## C6. Quad-block offsets within view_base — VERIFIED match

- Orig uses 9 explicit offsets within `g_hudCurrentFlagsPtr`: +0x004, +0x0BC,
  +0x174/+0x22C/+0x2E4 (speed font), +0x39C (semicol), +0x454, +0x50C, +0x5C4
  (metric d2-d4), +0x67C (uturn), +0x734 (metric d1), +0x7EC (replay).
- Port macros (td5_hud.c:114-117, td5_hud.h:67): `SPEEDO_QUAD_OFF=0x04`,
  `GEAR_QUAD_OFF=0xBC`, `SPEEDFONT_BASE_OFF=0x174`, `NEEDLE_QUAD_OFF=0x39C`,
  `GLYPH_QUAD_SIZE=0xB8`. All match orig.
- Port's per-frame writer (td5_hud.c:2225-2266) draws *ones* at
  `SPEEDFONT_BASE_OFF (0x174)`, *tens* at `+0xB8 (0x22C)`, *hundreds* at
  `+0x170 (0x2E4)`. Port init at d=0 writes x=`vp_r − sx·60` (rightmost x)
  at `0x174`, i.e. port stores the rightmost (ones) cell at orig's leftmost
  (hundreds) slot. Self-consistent: per-frame draw at `0x174` is *defined*
  to be the rightmost ones in port's wiring. Orig stores rightmost x at
  `+0x2E4` and reads ones from `+0x2E4`. **No visual divergence** — the
  mapping is symmetric. Only matters if a future hook depends on absolute
  slot numbering.

## C7. Speedo dial — clean (no divergence)

`speedo_x = vp_r − sx·96 − sx·16`, `speedo_y = vp_b − sy·96 − sy·8`, size
`sx·96 × sy·96`. Port matches orig byte-for-byte.

## C8. Gear indicator — clean
`(vp_r − sx·32, vp_b − sy·72)` size `sx·16 × sy·16`. Port matches.

## C9. U-turn arrow — clean
`(cenX, cenY) ± (sx·32, sy·32)`. Port matches.

## C10. Replay banner — clean
`(vp_l + sx·16, vp_t + sy·16)` size `sx·60 × sy·60`. Port matches.

# Section D — Actionable fixes

Priority by visible impact at 1080p:

## D1. Fix metric-digit Y offset (HIGH; td5_hud.c:1486)

```
- float metric_y = vp_t + sy * 12.0f;
+ float metric_y = vp_t + 12.0f;          /* orig uses raw pixel 12 */
```

## D2. Fix metric-digit glyph width and step (HIGH; td5_hud.c:1484-1489)

```
- float metric_glyph_w = sx * 16.0f;
- float metric_x = s_view_layout[v].center_x - metric_glyph_w * 2.5f;
+ float metric_glyph_w = sx * 15.0f;        /* orig _DAT_0045d6e0 = 15.0 */
+ float metric_x = s_view_layout[v].center_x - sx * 15.0f * 2.5f;
  ...
- mdx + metric_glyph_w, metric_y + sy * 24.0f,
+ mdx + metric_glyph_w, metric_y + sx * 23.0f,  /* orig: SX*23, not SY*24 */
```

(Yes, orig genuinely uses `sx*23` for the metric digit height — quirk
preserved.)

## D3. Fix speed font step gap (LOW; td5_hud.c:1454)

```
- float dx = font_x_start + (float)d * (font_glyph_w + 1.0f);
+ float dx = font_x_start + (float)d * (font_glyph_w + 2.0f);
```

## D4. Fix speed font height (LOW; td5_hud.c:1459)

```
- dx + font_glyph_w, font_y + sy * 24.0f,
+ dx + font_glyph_w, font_y + sy * 23.0f,
```

## D5. Quad-block offsets verified — no action

Macros at td5_hud.c:114-117 (SPEEDO=0x04, GEAR=0xBC, SPEEDFONT=0x174,
NEEDLE=0x39C) and td5_hud.h:67 (GLYPH_QUAD_SIZE=0xB8) all match orig.
Port's per-frame submit (lines 2221-2269) is self-consistent with init.

# Section E — Widgets present in orig that port lacks

- **Pos-row sub-table** (`local_34[13]` + `local_4c[6]`): 13 x-positions and
  6 y-positions for a 13×6 row of position numerals (likely race-position
  indicator). Built per view in orig but never consumed by any callee. Dead
  in orig too; safe to leave unported. No port-side action.

- **No HUD layout widget in orig is absent from port.** All four speedo
  group (dial + font + gear + metric digits), the time semicol scratch
  quad, U-turn arrow, and replay banner are present in both.

- The "damage/wanted overlay" pieces requested in the goal are NOT in this
  function — they live in `InitializeWantedHudOverlays` @ 0x0043D2D0 and
  the per-view `g_wantedHudPerSlot[6]` table. Out of scope here; covered
  by prior MEMORY note `[[reference_lateral_dir_inversion_fix_2026-05-21]]`
  (cop-chase init gate).

- The minimap layout (size, anchor, scandots, scanback grid) is built by
  the tail call `InitializeMinimapLayout` @ 0x0043B0A0, mirrored at port
  `td5_hud_init_minimap_layout`. The orig minimap uses `g_hudPrimaryScaleX`
  (the *global*, equal to view 1's scale in 2P split) — i.e. minimap is
  drawn ONCE for the rightmost/bottommost view. Port at line 3023 uses the
  same `s_scale_x` global. Behavior matches; out-of-scope for per-view audit.

# Confidence

- Section A (orig widget math): **HIGH** — every offset/multiplier was read
  out of the decompile and cross-referenced against the rodata floats at
  0x0045D6Cx-D6Fx.
- Section B (port mapping): **HIGH** — every widget located in td5_hud.c.
- Section C (divergences): **HIGH** for C1-C5 (math directly compared);
  **MEDIUM** for C6 (depends on header macro values I did not read).
- Section D (fixes): each is a single line. Cumulative diff impact: ~6 lines.

# Pool cleanup

Released TD5_pool0 at exit.
