# TD5RE — Garage / Custom Car-Body Decal Editor (Design Plan)

**Status:** Plan only — no implementation yet. Authored 2026-06-30 alongside the
profile auto-save + profile-colour-defaults-car-paint fixes.

**Goal (user request):** A "garage" personalization feature that lets players
apply photos / textures / decals to their car body — the *full decal editor*
tier: place, scale, rotate, and layer images on the car, with a live preview.

This is a **port-only** feature (no original-binary basis). It builds on the
existing texture + car-skin pipeline; nothing here requires RE.

---

## 1. Feasibility recap (verified against the current pipeline)

| Capability | State | Where |
|---|---|---|
| Runtime PNG/JPEG decode | ✅ `stb_image` linked | `td5_asset.c` `td5_asset_decode_png_rgba32` / `stbi_load_from_memory` |
| Arbitrary BGRA → GPU texture page | ✅ accepts any W×H | `td5_platform_win32.c` `td5_plat_render_upload_texture(page,pixels,w,h,2)` |
| Per-car dedicated skin pages | ✅ `800 + slot*2` (skin), `+1` (hub) | `td5_asset.c` vehicle load |
| Free GPU pages | ✅ ~220 spare of 1024 | `MAX_TEXTURE_PAGES=1024` |
| Masked colour tint (TD6) | ✅ body-only multiply via `carmask.png` | `td5_asset_load_vehicle_skin_painted` |
| Decode PNG → CPU buffer (for compositing) | ✅ | `td5_asset_load_png_to_buffer(path, key, &px,&w,&h)` (BGRA32) |

**Bottom line:** loading a player image and getting it onto the car at runtime is
already fully supported. The hard part is *not* plumbing — it is **where on the
body the image lands**, which is a UV-layout problem (section 2).

---

## 2. The core problem: car UV layout is a full-body wrap, not a decal canvas

Car body meshes carry per-vertex UVs (`TD5_MeshVertex.tex_u/tex_v`,
`td5_types.h`). For a paintable car the body command's UVs span the **whole
[0,1] page** — the single `carskin*.png` is wrapped over the entire body. Key
consequences:

- The two sides of the car typically **share the same UV region** → a decal
  painted into the skin appears **mirrored on both sides** (and often on the
  hood/roof depending on the car's unwrap). This is fine for a *livery/wrap* but
  wrong for "one logo on the driver door."
- There is **no authored, human-readable UV unwrap atlas** ("door here, hood
  there"). The skin is an artist-painted texture; UV islands are per-car and not
  labelled.

So "put a photo on the body" has two honest interpretations, and the editor must
make the distinction explicit to the player:

1. **UV-space decal (Phase 2):** the player positions the image *on the skin
   texture* (a 2D canvas = the base `carskin`). Cheap, deterministic, but the
   player is painting in UV space — placement near a seam/mirror line behaves
   unintuitively. Good for full wraps, racing stripes, big logos.
2. **Surface-projected decal (Phase 3):** the player positions the image *in the
   3D view*, and we raycast its footprint onto the mesh triangles to find the UV
   texels it covers, then composite there. This is "click the door, logo lands on
   the door" — the intuitive behaviour — but needs mesh raycasting + per-triangle
   UV rasterization. Larger effort.

A full decal editor ultimately wants (2) for placement and (1)'s compositor as
the bake target. The plan ships them in phases so value lands early.

---

## 3. Data model

```c
/* One decal layer (garage personalization). Persisted as JSON; baked to the
 * car skin page at load. */
typedef struct {
    char     image[64];     /* PNG/JPG filename under garage/images/ */
    float    u, v;          /* placement centre (UV space 0..1, Phase 2) OR */
                            /* surface anchor (mesh hit, Phase 3)            */
    float    scale;         /* fraction of skin width                        */
    float    rot_deg;       /* rotation about the decal centre               */
    float    opacity;       /* 0..1 alpha multiplier                         */
    int      blend;         /* 0=alpha-over, 1=multiply, 2=additive          */
    int      mirror;        /* 1 = also stamp the mirrored UV island         */
    int      surface_mode;  /* 0=UV-space (P2), 1=surface-projected (P3)     */
} TD5_Decal;

typedef struct {
    int       base_paint;   /* which carskinN is the canvas (TD5) / -1 grey  */
    uint32_t  base_tint;    /* TD6 body tint (0xRRGGBB) under the decals      */
    int       n_decals;
    TD5_Decal decals[TD5_GARAGE_MAX_DECALS];   /* suggest 8 */
} TD5_CarLivery;
```

Stored **per car code** (e.g. `bmw`, `cp1`) so a livery follows the car, not the
profile — keyed `garage/<carcode>.json`. (Optionally also per-profile: a profile
references a livery name. Decide during Phase 4.)

---

## 4. Render integration (how the livery reaches the car)

Reuse the existing painted-skin bake path. At vehicle asset load
(`td5_asset.c`, where `carskin*.png` is decoded and uploaded to `800+slot*2`):

1. Decode the base canvas:
   - TD5 → `carskin<base_paint>.png`.
   - TD6 → `carskin0.png` then masked tint by `base_tint` (existing
     `td5_asset_load_vehicle_skin_painted`).
2. For each decal layer, decode its image, transform (scale/rotate), and
   **composite CPU-side** into the base BGRA buffer:
   - Phase 2: blit at `(u,v)` directly in texture space (+ mirrored island if
     `mirror`).
   - Phase 3: rasterize the decal quad's footprint over the mesh triangles whose
     projected screen area it covers, writing the covered UV texels.
3. Upload the composited buffer to the car's skin page (one `upload_texture`
   call — no per-frame cost; identical to today).

Net runtime cost: a handful of PNG decodes + one composite + one upload **at race
load**, per car that has a livery. In-race rendering is unchanged (same page bind
per actor).

---

## 5. Live preview

Car-select already renders a 3D car preview (the rotating model). The garage
screen reuses that view and re-bakes the skin page on every edit (debounced), so
the player sees decals on the actual model immediately. Phase 3 also overlays a
2D gizmo (move/scale/rotate handles) projected from the 3D hit point.

For Phase 2 the editor additionally shows the **flat skin canvas** (the base
`carskin` blitted to a panel) with the decal rectangles drawn over it, so the
player understands they are editing UV space.

---

## 6. UI / UX

New frontend screen **GARAGE** (reachable from the main menu and/or a button on
car-select). Screen table entry in `td5_frontend.c` (currently 30 screens).

Layout:
- Left: 3D car preview (live) + flat skin canvas toggle.
- Right: layer list (add / delete / reorder) and the selected layer's
  properties (image picker, scale, rotation, opacity, blend, mirror).
- Image picker: lists PNG/JPG in `garage/images/` (drop-in folder) + a few
  bundled starter decals.

Controls (must work on gamepad — the project is pad-first):
- D-pad / stick: move selected decal (or navigate the layer list).
- Triggers: scale. Bumpers: rotate. Face buttons: add layer / delete / confirm.
- Mouse supported on the canvas (car-select already enables the cursor).

Follows the established frontend idioms: `fe_draw_*` primitives, selector arrows
routine (`fe_draw_option_arrows` — see the recurrent selector-arrows feedback),
`FE_MENU_BTN_X=120` alignment, profile-colour accents.

---

## 7. Persistence & sharing

- `garage/images/` — drop-in user PNGs/JPGs (decoded via `stb_image`).
- `garage/<carcode>.json` — the `TD5_CarLivery` for that car (hand-editable;
  same editable-source spirit as `td5_assetsrc.c`).
- Sharing = zip the JSON + referenced images. (No networking required.)
- Multiplayer/netplay: a remote player's livery would need the image bytes
  transmitted or pre-shared; **out of scope for the first cut** — local liveries
  render only on the local machine's view of that car. Flag clearly.

---

## 8. Phased delivery (each phase shippable on its own)

| Phase | Scope | Effort | Risk |
|---|---|---|---|
| **P0** Drop-in skin replace | `garage/<carcode>/skin.png` overrides `carskin0` at load. No UI. | XS | Low |
| **P1** Livery picker | Garage screen: pick base paint/tint + ONE full-body wrap image from a list. Bake at load. | S | Low |
| **P2** UV-space decal compositor | Multi-layer decals placed on the flat skin canvas, live 3D preview, scale/rotate/opacity/blend/mirror. JSON persistence. | M | Med (compositor correctness, gamepad UX) |
| **P3** Surface-projected decals | Place decals in the 3D view; raycast onto mesh, rasterize covered UV texels. | L | High (mesh raycast + UV raster, seam handling) |
| **P4** Per-profile liveries + sharing | Profiles reference a saved livery; export/import bundle. | S–M | Med |

Recommendation: ship **P0+P1** first (immediately useful, reuses the painted-skin
path with almost no new math), then evaluate appetite for P2/P3. P3 is where the
real engineering lives (the UV/raycast work); it should be its own project.

---

## 9. Key risks / open questions

1. **Mirror/seam behaviour (P2):** painting UV space mirrors across the car.
   Mitigation: show the flat canvas + a `mirror` toggle; document it. True
   per-side control needs P3.
2. **Per-car UV variance:** every car's unwrap differs, so there is no universal
   "door rectangle." P3's raycast approach sidesteps this; P2 cannot.
3. **Image aspect ratio:** a portrait photo on a ~2:1 body wrap stretches. Editor
   should preserve aspect by default and letterbox.
4. **Decode/compositing cost at load:** bounded (a few small images per car).
   Cache the composited skin keyed by livery hash to avoid re-baking unchanged
   liveries.
5. **MP visibility:** local-only for the first cut (no image transport).
6. **Memory:** each custom skin is one extra page (256²–512² BGRA). With 6 racer
   slots that's well within the ~220 spare pages.

---

## 10. Suggested module split

- `td5_garage.c` / `.h` — new module: livery data model, JSON load/save, CPU
  compositor (`td5_garage_bake_skin(carcode, slot)`), decal raster helpers.
- `td5_frontend.c` — the GARAGE screen (screen-table entry + render + input).
- `td5_asset.c` — hook the bake into vehicle skin load (call
  `td5_garage_bake_skin` instead of / after the plain `carskin` upload when a
  livery exists for that car).
- `td5re.ini [Garage]` — enable flag, max decals, image folder path.

This keeps the heavy compositor logic isolated in one module and the render/asset
touch-points minimal (one hook in `td5_asset.c`, one screen in the frontend).
