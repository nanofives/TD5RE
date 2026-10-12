# GEO mast-arm traffic lights with street-name plates (round 1021 D)

Mariano, item 16: the signals are fine (`level091 e91 s32 p516:SIGN_POST`) "but we need to
implement traffic lights where the post extends upwards into the road, these traffic lights
contain the names of the streets". His example photo (`_archive/round1021/item16_mastarm_example.png`):
a pole at the corner, a horizontal arm over the carriageway, signal heads on the arm, a blue plate
with white capitals ("AVENIDA 51") hung from it.

TD5RE-only (no original counterpart). Module `td5_tg_mastarm.c`, hooks in `td5_tg_furniture.c`
(`tg_geo_signals_prepare` / `tg_emit_geo_signals`) and `td5_tg_pages.c` (`tg_emit_textures`).

## What it builds

At every real OSM signal the existing geo-signals placement decides (which node, which span, which
side, how far out) the pole now carries:

* **Pole** to 6.3 m over the road surface at its own span (same footprint as the old post).
* **Arm** at 5.8 m, from the pole across to a little past the centreline.
* **Heads** (1 for a half-carriageway under 3.8 m, else 2) over the lane centres of the pole-side
  half, each with a dark backplate and the three lamp billboards. The lamps carry the same
  `TD5_MESH_TAG_SIGNAL_LAMP + phase` tags, so the renderer's red/amber/green cycle is unchanged.
* **Plate**, hung BELOW the arm from two short brackets (as in the photo), two faces: the **cross
  street** faces the driver who approaches, the **street you are driving on** is on the back.
* **Cross-direction head** at the arm tip, turned a quarter. Its lamps use the same phase tags as the
  main heads, so it shows the SAME state, not the opposite (the renderer colours a lamp from its tag
  alone; there is no tag for "the other direction").
* **Countdown display** on the arm next to the facing head: black box, dot-matrix "25". The face is the
  additive lamp billboard tagged GREEN, so it is bright while the signal is green and dim otherwise.
  The digits are STATIC: the renderer can recolour a lamp per frame from the wall clock but cannot swap
  a texture, so a real countdown would need a renderer hook (digit pages + remaining-time lookup).

Everything over the carriageway is above `TD5_TG_GUARD_OVERHEAD` (4.4 m): head bottom 4.7 m, plate
bottom 5.0 m, arm 5.7 m. The pole is the old footprint, so the on-road guard judges it exactly as
before. Measured on Mariano's route: no prop mesh rejected at a signal span.

## Where the names come from

`pl_build` (pure function of ROUTE.JSON + SIGNALS.JSON + ROADS.JSON, no node list):

* route street = ROUTE.JSON's own name at the nearest route node, else the nearest way;
* cross street = nearest way at the signal node with a different interned name id (the same
  "skip the race street's id" rule `td5_geo_attrs_cross_name` and the HUD use), within 26 m;
* if the cross street is the race street, or unknown, the face shows the other name.

Why not the node list: a streamed build writes TEXTURES.DAT **before** the prepass that sees it, a
non-streamed one **after** the models. Both orders must find the same plates, so the table is rebuilt
wherever it is needed and is deterministic.

## Text -> track page

A TEXTURES.DAT page is 64x64, <= 256 colours, and the container has no other size. A plate is a
**cell**: 64x16 (one line) or 64x32 (two lines), 1 texel = 2.2 cm of plate, packed 4 rows to a page.

* `td5_font_plate_raster` (td5_font.c) draws one or two UTF-8 lines with stb_truetype into a coverage
  buffer, self-contained (no GPU atlas, no platform call, safe for the headless geo autobuild). Face:
  Arial Bold if present (closest to the real Helvetica Bold plates), else the HUD / menu face.
  Accents: the name is upper-cased through Latin-1 (`Azcuénaga` -> `AZCUÉNAGA`) and the block gets
  headroom for an accented capital.
* Layout (`pl_plan_text`): one line if the cap height is >= 7.5 texels, else the best two-line split
  at a space (>= 6.8), else abbreviate one word at a time, longest first, **only until it fits**
  (`DIAGONAL 73` stays whole; `PLAZA MIGUEL DE AZCUÉNAGA` becomes `PZA. MIGUEL / DE AZCUÉNAGA`).
  The two faces of a plate must be the same height, so the shorter one is remade two rows tall.
* Palette: entry 0 plate blue, entry 15 white, 14 steps between for antialiasing; a 1-texel white
  border ring inside a 1-texel blue margin (scenery samples LINEAR + WRAP, quad UVs are inset to texel
  centres so a tap never reaches the next cell).
* Pages are **appended after `TD5_TG_PAGE_COUNT`** (720) and only when plates exist: a synthetic
  build and a geo build with the knob off write the page file they always did. Cap
  `TD5_TG_MASTARM_PAGES_MAX` 32 (static-asserted below `TD5_PAGE_LEVEL_CEIL`). La Plata partido route:
  11 extra pages, 23 cells.

## Knobs

* `TD5RE_GEO_MASTARM=0` -> the old post light, byte for byte (default on).
* `TD5RE_GEO_SIGNALS=0` still turns every signal off.
* The old post stays for: no known street name, carriageway under 5.5 m, and within 6 spans of the
  finish gantry (an arm through the banner).

## Orientation was measured, not derived

The first cut assumed a left-handed frame and drew the text mirrored. `far_left` in `ma_emit_plate`
is the measured answer for both a left-side and a right-side pole (frames
`log/tour_r2_*`, `tour_r4_*`). If the winding of the geo frame ever changes, look at a frame first.

## Not done / next

* **Minimap street labels.** NOT a job for this page emitter: the minimap is drawn at race time, where
  `td5_hudfont_get_exact` already draws UTF-8 (that is how "ON Diagonal 73" is drawn). The cheap plan:
  `td5_minimap_streets.c` `mms_build_geo` keeps the way's `name_id` per segment (it already walks the
  ways); `hud_minimap_streets` draws the name once per named road at the segment nearest the minimap
  centre, rotated with the map, skipped when the label would overlap the route or an earlier label.
  ~half a day; text at the minimap's 170 px is 7-8 px, so only the avenues are worth labelling.
* A real counting-down display (see above) and an opposite-phase cross head.
* Heads over the far (oncoming) half for the pole-on-the-oncoming-side case: today both are over the
  pole-side half, which for a left pole is the oncoming half (cosmetic, nothing obeys a light).
* Street-name plate at junctions with no OSM signal node (corner nameplates on a plain post).
