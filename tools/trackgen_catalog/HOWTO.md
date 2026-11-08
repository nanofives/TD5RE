# How to add a piece or a texture to the auto track

Written from the code as it is on master (round U). Every name below is a real
function or macro; the [Code map](code.html) page has live `file:line` links for
all of them, regenerated with this site. The generator is `td5_trackgen.c` plus
the `td5_tg_*.c` modules; shared declarations live in `td5_trackgen_internal.h`.

## The four things a piece is made of

| Part | Where it lives | What it decides |
|---|---|---|
| Texture page | a slot in TEXTURES.DAT, named by a `TD5_TG_PAGE_*` macro | what it looks like |
| Mesh | an emitter function that appends PRR meshes to the entry's `TG_Buf` | its shape |
| Selection rule | the emitter's early returns: biome row, knob, span tests | where it appears |
| Kind tag | `tg_guard_mark(..., TG_GK_<KIND>, si)` right after the emitter call | how the guard judges it, and its MESHTAG name in this catalogue and in the free-cam picker |

A level is built as ENTRIES of `TD5_TG_SPANS_PER_ENTRY` (4) spans. For each span
the scenery loop in `td5_trackgen.c` (function `tg_scenery_entry`, the block that
starts at the road mesh and ends with the geo signals) calls the emitters in a
fixed order, each wrapped as `TG_SUB(<zone>, emitter(...))` for the timing
counters, and stamps a kind on whatever the emitter appended. The budget is
`TG_MAX_MESHES_PER_ENTRY` (4 x 96 = 384 meshes per entry).

## Add a texture page

### 1. Reserve a slot

Pages are numbered by a chain of `#define`s in `td5_trackgen_internal.h`. Do
**not** append to someone else's block and do **not** renumber: read the
"[R3] RESERVED PAGE SLOTS" comment there. Each work area owns a fixed-size block
measured from its own `*_BASE`. Two options:

- **A free slot in an existing block.** Blocks are reserved bigger than used
  (for example `TD5_TG_PAGE_R9_INFRA` has `TD5_TG_R9_INFRA_N` 14 slots). The
  [Textures](textures.html) page marks pages that no face uses; a slot whose
  page is a flat colour or a duplicate is usually spare. Add a named macro
  `#define TD5_TG_PAGE_<NAME> (TD5_TG_PAGE_<BLOCK> + k)`.
- **A new block.** Add `TD5_TG_PAGE_<AREA>_BASE` plus a `_N` count AFTER the
  last block and move `TD5_TG_PAGE_COUNT`. Put it after the landmark block
  (`TD5_TG_PAGE_LM_BASE` + `TD5_TG_LM_PAGES`), not before it: the prefab
  meshes address their pages as `TD5_TG_PAGE_LM_BASE + local index`, so a block
  inserted in front of it shifts every landmark page and changes MODELS.DAT.
  Appending at the end only grows TEXTURES.DAT. The loader cap is 1024 pages
  and the street-name plate pages (`td5_tg_mastarm.c`) and the roam furniture
  pages (`FU_PAGE_BASE` in `td5_roam_furniture.c`) ride after
  `TD5_TG_PAGE_COUNT`, so they follow automatically.

### 2. Write the page

All pages are written by `tg_emit_textures_n` in `td5_tg_pages.c`, into
`TG_Buf pages[...]` indexed by slot. A page is: 3 pad bytes, 1 type byte
(0 opaque, 1 colour-keyed with index 0 transparent), a u32 palette size, the
palette as **B, G, R** triples, then 64 x 64 index bytes.

- **Procedural page.** Copy the shape of a small existing writer, for example
  `tg_emit_texture_page_r11_sign_post` (16-colour palette built in a loop, then
  one index per texel from a formula). Call it from `tg_emit_textures_n`:
  `tg_emit_texture_page_<name>(&pages[TD5_TG_PAGE_<NAME>]);`
- **Real page from a shipped track.** Do not hand-copy bytes. Use the manifest
  importer: `python re/tools/gen_tg_pages.py MANIFEST.json` reads
  `re/assets/levels/<level>/textures.src/` and writes a header
  `td5_tg_real_tex_<name>.h` with `k_real_<name>_pal / _paln / _idx` arrays
  (see `re/tools/manifests/*.json` for real examples). The fastest way to fill a
  manifest is the in-game free-cam picker on a shipped track (dev build): click
  the wall you like, then `python re/tools/gen_tg_pages.py --from-pick
  MANIFEST.json SET` pastes the pick in. Then in `tg_emit_textures_n`:
  `tg_emit_real_page(&pages[TD5_TG_PAGE_<NAME> + i], k_real_<name>_pal[i],
  k_real_<name>_paln[i], k_real_<name>_idx[i], keyed);`
  `tg_emit_real_page_dim` gives a night (dimmed palette) copy of the same art.
- Real pages are only used when `tg_real_textures_enabled()`; keep a procedural
  fallback in the same slot for the knob-off path, as the existing sets do.

### 3. Check it

Rebuild (a header edit needs `build_standalone.bat`, not `fastbuild.bat`), run
the game once on the auto track, rebuild this site
(`python tools/trackgen_catalog/build.py`) and find the slot on the
[Textures](textures.html) page. It is badged *real* if it matched a shipped page
byte for byte, *procedural* otherwise.

## Add a piece (a new mesh)

### 1. Write the emitter

Put it in the module that owns the area (routing table in the repo
`CLAUDE.md`: city facades and furniture in `td5_tg_city.c` /
`td5_tg_furniture.c`, terrain and biomes in `td5_tg_terrain.c`, bridges and
tunnels in `td5_tg_bridge.c`, streets in `td5_tg_streets.c`). Declare it in
`td5_trackgen_internal.h`. The signature the scenery loop expects is:

```c
int tg_emit_<name>(const TG_NodeList *nl, int si,
                   TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh);
```

Return 0 only on out-of-memory; "nothing to place here" returns 1. Before each
mesh you append, record its offset: `moff[(*nmesh)++] = blk->len;`. The
offsets must stay ascending (the guard compacts the buffer with one forward
cursor).

Mesh helpers you can call instead of writing PRR bytes by hand:

- `tg_emit_box_mesh(blk, cx, cy, cz, hx, hy, hz, fx, fz, page, tile, color)`
  in `td5_tg_city.c`: a 6-face box, oriented along the road direction `(fx, fz)`,
  UVs tiling every `tile` world units. Buildings, piers and kiosks start here.
- `tg_emit_billboard_mesh(blk, wx, wy, wz, half_w, height, page, tag)`: a
  camera-facing sprite (trees, people, lamps), drawn from a colour-keyed page.
- For anything else, copy the header-writing part of `tg_emit_box_mesh`: render
  type 259, one command per page, quads de-indexed (4 vertices each).

A good model to copy whole is `tg_emit_r11_sign` in `td5_tg_furniture.c`: it
shows the knob gate, the start-grid / bridge / tunnel / overpass exclusions, the
mesh budget check, the side-street occupancy test (`tg_xstreet_occupies`) and
the element inventory (`TG_ACCT_MASK_TEST`) that keeps two pieces of furniture
off the same spot.

### 2. Decide where it appears

The selection rule is the emitter's own early returns. The usual inputs:

- **Biome.** `const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];` The table
  `k_biomes[]` in `td5_tg_terrain.c` (one row per biome: CITY, FIELDS, FOREST,
  INDUSTRIAL, ALPINE, COAST, ORIENTAL, ALPTOWN) already carries the wall page,
  ground page, flora set, prop and road surface per biome. A per-biome choice
  that is a property of the biome belongs in that row, not in an `if` chain.
- **Span state.** `tg_span_in_tunnel(si)`, `tg_span_in_bridge_run(si)`,
  `tg_up_clear_span(si)` (overpass), `tg_side_blocked(si, side)`,
  `tg_road_wet_any(si)`, and `si <= TD5_TG_GRID_SPAN` for the start grid.
- **Randomness.** Do not draw from the shared RNG in an emitter: the layout
  would move for every later piece. Hash the seed and the span, as the prefab
  placement and the biome layout do (see the header comment of
  `td5_tg_prefab.c`).
- **Knob.** Gate a new piece with an environment knob while it is being tuned:
  `td5_env_flag_on("TD5RE_<NAME>")` is ON unless the variable is `0`;
  `td5_env_flag_off(...)` is OFF unless it is `1`. A default-off knob keeps the
  synthetic identity gate byte-identical until you decide to ship it.

### 3. Wire it into the scenery loop and give it a kind

In `tg_scenery_entry` (`td5_trackgen.c`), add the call where its order makes
sense (furniture that must see what is already on the span goes last, like the
direction sign), and stamp the kind on what it appended:

```c
{
    size_t x0 = meshes.len;
    if (!TG_SUB(TG_SUB_PROPS, tg_emit_<name>(nl, si, &meshes, moff, &nmesh,
                                              TG_MAX_MESHES_PER_ENTRY))) {
        ok = 0; break;
    }
    tg_guard_mark(x0, meshes.len, TG_GK_PROP, si);
}
```

Pick the kind for how the guard must treat it (`k_guard_kind_class` in
`td5_tg_guard.c`): SCENERY kinds (`BUILDING`, `CITY`, `BLOCK`, `CROSS`,
`FLORA`, `TERRAIN`, ...) are rejected if they stand in the carriageway; `PROP`
is the street-furniture class; EXEMPT kinds (`ROAD`, `DECK`, `TUNNEL`,
`GANTRY`, `ENDWALL`) are allowed over or around the road and must only be used
for geometry that is authored to be there. Do not reach for an exempt kind to
make a rejection go away: the rejection is the bug report. The kind is what this
catalogue and the free-cam picker show, through MESHTAG.BIN.

A brand-new kind means a new `TG_GK_*` enum value (before `TG_GK_COUNT`), a
name in `k_guard_kind_name`, a class in `k_guard_kind_class` and a row in the
other per-kind tables in `td5_tg_guard.c`, plus a line in `KIND_INFO` in
`tools/trackgen_catalog/build.py`.

### 4. A set piece lifted from a shipped track (prefab)

Whole buildings from a real track do not need an emitter. `re/tools/td5_geomlib.py
prefabs` exports them into `td5_tg_prefab_data.h` (geometry) and
`td5_tg_real_tex_landmarks.h` (their pages, in the `TD5_TG_PAGE_LM_BASE`
block). The two files are positional and must be regenerated together. Placement
is decided by `tg_landmarks_place` and emitted by `tg_prefab_emit_span`
(`td5_tg_prefab.c`), tagged `BUILDING`. Raising the page count means updating
`TD5_TG_LM_PAGES` (there is a compile-time check against the header).

## Verify

1. **Build.** `fastbuild.bat td5_tg_<module>.c` for a .c-only change,
   `build_standalone.bat` after any header edit.
2. **Identity, knob off.** With the new knob unset (or off), the synthetic
   gate must stay byte-identical: move `re/assets/geo/SELECTED.TXT` aside, run
   `pwsh verify/geo_tags_identity.ps1 -Arm synthetic -Tag mine -NoControl` and
   compare MODELS.DAT with the reference (sha256 `E2F1F33C...`). A new page
   appended at the end changes TEXTURES.DAT only, which is expected.
3. **Look at it.** `pwsh verify/geo_tour.ps1 -Tag mine -Track 60 -Spans
   "a:@600,0,600,0,-10" -Extra @{TD5RE_<NAME>=1}` captures free-cam frames on the
   auto track in one launch (knobs go only through `-Extra`). Read the PNGs.
4. **See it in the catalogue.** `python tools/trackgen_catalog/gen_levels.py`
   (optional, more seeds), then `python tools/trackgen_catalog/serve.py --build`.
   The new piece shows up as a card under its kind with its page family, and its
   emitter appears in the [Code map](code.html).

## Regenerating this site

```
python tools/trackgen_catalog/gen_levels.py      # optional: extra seeds, ~1-3 min each
python tools/trackgen_catalog/serve.py --build   # build + serve on http://127.0.0.1:6700/
```

The build reads `re/assets/levels/level090` (the last auto track you ran, which
should be the synthetic reference seed 99991) and every `levels/seed<N>/`
directory that `gen_levels.py` stored. MESHTAG.BIN only exists from a DEV build;
from a RELEASE build every piece shows as kind OTHER.
