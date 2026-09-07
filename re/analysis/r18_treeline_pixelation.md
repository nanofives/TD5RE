# R18 TREELINE — "tree background texture looks very pixelated and weird colors"

**Verdict: UNKNOWN / deferred.** Neither of the two proposed options reduces
world-units-per-texel at a proportionate, verifiable cost. The one approach that
*would* reduce it is identified below with a concrete plan, but it is a
cross-subsystem feature whose appearance cannot be verified here (no game assets)
and whose default-ON form is exactly the kind of unverifiable cosmetic swap the
round was told not to ship. No behavioral change was made. This note + a tight
`R18 TREELINE` marker in `td5_tg_terrain.c` record the measurements so the next
session does not re-derive them.

("Weird colors" was already ruled out upstream: the page is written flag-1 and
decoded through the type-1 path, which runs `alpha_bleed_rgb`. That mitigation
covers the black-fringe theory. This note is about the pixelation only.)

## What draws the tree line (measured, not inferred)

- `td5_tg_terrain.c:tg_emit_far_band` emits, per far-group per side, a 16-vert /
  2-command mesh: 3 apron ring quads on `seg_page[0]` + **one** vertical ridge
  wall quad on `seg_page[1]` (`td5_tg_terrain.c:3684-3687`, `seg_nq[1]=1`).
- The ridge page is the 367..370 treeline family, chosen by `tg_r8_treeline_page`
  (`TD5_TG_PAGE_R8_TREELINE + 0..3`).
- The page is **64x64** (`TD5_TG_TEX_DIM = 64`). The treeline emitter
  (`td5_tg_pages.c:tg_emit_texture_page_fb_treeline_real`) crops the densest
  **20-row** canopy window (`TG_TREELINE_WIN = 20`) out of a shipped foliage
  source page and upsamples it ~2.3-2.6x to fill the ~39-53 foliage rows below
  the procedural crown line. The source foliage pages (`k_real_tree_idx`) are
  themselves 64x64.

## World-units-per-texel (the pixelation number)

`tg_r8_tl_note(tile_w = w/tiles, tile_h = 0.5*(t0+t1), page)` already records
"the WORLD size one 64x64 page tile is drawn at" (its own comment,
`td5_tg_terrain.c:3093`). world-units-per-texel = tile / 64.

- Ridge crest height ≈ `TD5_TG_RIDGE_BASE` = **4500** (floored to `RIDGE_MIN_UP`
  = 1200 above the road it is seen from).
- With `TD5RE_R8_TERRAIN_TREELINE_ASPECT` ON, tile_w = crest height, so the tile
  is square: **~4500 / 64 ≈ 70 wu/texel** in BOTH axes.
- The pixelation is worst on **near** far-bands, where `band_reach` was capped
  small (road cap / min-width floor, `so + FAR_TUCK + 500`): a close, large-on-
  screen wall drawn from a 64x64 page → visibly magnified texels + a hard-stepped
  binary alpha-key crown edge.
- Vertically the foliage carries only **20 distinct source rows** across the
  ~3500-unit foliage band → ~175 wu per *distinct* source row, coarser still.

## Option B — stacked quads: REJECTED with numbers

Replace the single ridge quad with N quads up the wall.

- **If the N quads merely subdivide** v=0..1 across the same page (same seg,
  `seg_nq[1]=N`): identical texels at identical world positions — subdividing a
  quad adds **zero** texels. wu/texel unchanged at 70. Pure cost, no benefit.
- **If the N quads tile the page vertically** (v = 0..N) to get a "finer world
  pitch": the treeline page is a one-shot alpha-keyed silhouette (keyed crown at
  the top, foliage below). Tiling it vertically stacks N crown lines up the wall,
  and the keyed sky-gaps of the middle/lower crowns show **sky through the middle
  of the tree wall** — broken. To avoid that you would redesign the page into a
  separate crown page + a seamless tileable foliage page (new page slots, shared
  state this item does not own), and the foliage still has only 20 source rows to
  tile, so vertical tiling produces visible horizontal banding/moiré — the same
  repetition artifact R8-vary fought horizontally.
- **Geometry cost** (`tg_write_quad_mesh`, `td5_tg_city.c:471-480`): per vertex =
  12 f32 + 1 u32 = **52 B**; per quad (4 verts) = **208 B**. N stacked quads =
  +4(N-1) verts = **+208(N-1) B per treeline band**. Far groups =
  `TD5_TG_FAR_GROUP` (4 spans) → spans/4 groups × 2 sides; a track emits hundreds
  of treeline ridges, so N=3 is on the order of +0.1 MB of MODELS.DAT for **zero
  texel gain** (subdivision) or a **broken/banded** result (vertical tile).

## Option A — larger source page

- **Single larger DAT page: INFEASIBLE (format-locked).** TEXTURES.DAT is the
  original game format (Ghidra `FUN_0040b1d0`): per page `u8 pad[3]; u8 type;
  i32 pal_count; u8 pal[count*3]; u8 indices[4096]` — a fixed 64x64 index block,
  no per-page dimension field. The loader hardcodes 4096 / 64x64
  (`td5_asset.c:2939-3009`) and reads original TD5 tracks with the same loader,
  so `TD5_TG_TEX_DIM` is not a free parameter: raising it changes every page
  emitter (all write `TD5_TG_TEX_TEXELS = 4096`) AND breaks the shipped-track
  format. Out of scope.

- **Native-res loose-PNG override: the ONLY real reducer, but deferred.** A path
  already exists: `tpage_decode_one` (`td5_asset.c:2906-2924`) uploads
  `re/assets/levels/level%03d/textures/tex_%03d.png` at native resolution,
  bypassing the 64x64 page — **but it is gated `c->td6 > 0`**, so it never fires
  for the runtime auto-track (td6 = 0). The auto-track generator writes its
  TEXTURES.DAT into `re/assets/levels/level%03d` (`td5_tg_pages.c:3549`) — the
  same base dir the PNG override reads from. So wiring it is ~2 edits:
  1. Generator renders the treeline variant pages at, e.g., 256x256 and PNG-
     encodes them to `level090/textures/tex_{367..370}.png` (the zlib-free PNG
     encoder already exists).
  2. Un-gate the loader's PNG override for the auto-track treeline pages
     (knob-gated), so td6=0 also checks for the loose PNG.
  world-units-per-texel would drop **70 → ~17.5** at 256x256.

  **Why it is deferred, not shipped here:**
  - **Foliage detail is source-capped at 64x64.** `k_real_tree_idx` sources are
    64x64, so a 256x256 treeline PNG rendered from them is bilinear *blur*, not
    new information. Only the **procedural** crown silhouette (drawn from math,
    `crown = 12 + ...`) gains genuine resolution; the foliage body just stops
    looking blocky by becoming smooth.
  - **Unverifiable here.** No game assets in this environment, so a new higher-
    res render (finer crown, key handling, upscale seams) cannot be inspected at
    the pixel level. A **default-ON** knob swapping the treeline to an
    unverifiable render is precisely the risk the round was warned against.
  - **Blast radius.** Writing generated PNGs under `re/assets/` brushes the hard
    "never touch re/assets" rule, and un-gating the TD6-only PNG path risks any
    track picking up stray `tex_NNN.png` files — a change beyond the tree line.

## Two side-questions the task asked

- **Widening the 20-row crop** (`TG_TREELINE_WIN`): the window is COMPUTED as the
  densest 20-row window; rows above it are sky (keyed) and rows below are trunk
  (bark) — confirmed by `tg_treeline_src_window` and the header comment
  (`td5_trackgen_internal.h:5594-5600`). Widening it necessarily pulls in
  bark/sky and lowers mean canopy density, and it does **not** change
  wu/texel (still a 64x64 page). It affects blur/repetition, not pixelation.
- **Aspect-correction swap** (`TD5RE_R8_TERRAIN_TREELINE_ASPECT`): ON → tile_w =
  crest height → square tiles at ~70 wu/texel and correct aspect. OFF → tile_w =
  `TD5_TG_SPAN_LENGTH` (1500) → ~23 wu/texel horizontally but each tree squashed
  ~3:1 (the "stretched" complaint R8 fixed). So it is exactly a pixelation-for-
  stretch trade; neither setting is a fix.

## Bottom line

- world-units-per-texel today: **~70** (both axes, aspect ON), coarser vertically
  in *source* detail (20 rows).
- Option B: no reduction (subdivide) or broken/banded (vertical tile); +~0.1 MB.
- Option A single page: format-locked, infeasible.
- Option A native-res PNG: reduces to ~17.5, mechanism 90% exists, but is a
  cross-subsystem feature, foliage-detail-capped, and unverifiable without assets
  — deferred with the plan above rather than shipped default-ON.
