# TD5RE Texture Audit & Missing-Feature TODO — 2026-04-13

Cross-reference of every PNG / PRR / DAT texture in `re/assets/` against the
source-port code in `td5mod/src/td5re/*.c`. Auditor: Claude.

Method: grep for every filename stem AND every atlas-entry name
(`td5_asset_find_atlas_entry(NULL, "...")`) AND every PNG/TGA path literal.
Results below name only files that are **unreferenced** and therefore flag a
hole in a feature.

---

## 1. Static sprite atlas (`re/assets/static/sprites/*.png`, 40 files)

Atlas descriptors are in `re/assets/static/static.hed` and 27 of the 40 sprite
PNGs are pulled by name via `td5_asset_find_atlas_entry()`.

**USED (25 + SKIDMARK atlas-only) — all wired:**
BLACKBAR, BLACKBOX, BRAKED, CHASSIS, COLOURS, FADEWHT, FONT, GEARNUMBERS,
INWHEEL, NUMBERS, PAUSETXT, RAINDROP, RAINSPL, REPLAY, SCANBACK, SCANDOTS,
SELBOX, SEMICOL, SHADOW, SLIDER, SMOKE, SNOWDROP, SPEEDO, SPEEDOFONT, UTURN
(plus atlas entry SKIDMARK used by `td5_vfx.c:543`).

**UNUSED (15 PNGs never looked up in atlas nor loaded by path):**

| Sprite | Purpose (inferred) | Missing feature |
|---|---|---|
| `CAR0.png` / `CAR1.png` / `CAR2.png` | Mini-car indicator icons (minimap / HUD grid) | HUD minimap car markers not drawn from atlas — TODO below |
| `DAMAGE.png` / `DAMAGEB1.png` / `DAMAGEB2.png` | Vehicle damage indicator HUD | Damage HUD never rendered (the port tracks `damage_lockout` in physics but does not draw it) |
| `FADEYEL.png` | Yellow fade overlay (caution / warning flash) | No caution flag / yellow-flash path |
| `MPH.png` | "MPH"/"KPH" unit label sprite next to speedo | Speedometer shows digits but no unit label sprite |
| `POLICE_RED.png` / `POLICE_BLUE.png` | Police siren body (on top of cop cars) | Cop cars load (cop.zip / sp5-7.zip) but police-vehicle lightbar overlay not drawn |
| `POLICELT_RED.png` / `POLICELT_BLUE.png` | Flashing police lightbar frames | Police chase/pursuit FX missing |
| `POSITION.png` | Race position label ("1st", "2nd"…) | HUD currently renders position via numeric font path; dedicated ordinal sprite unused |
| `SKY.png` | 2D sky fallback | Port uses 3D `sky.prr`; 2D sky fallback path never wired — OK to leave unused, but loader should gracefully skip |
| `SPARK.png` | Collision spark VFX | Sparks on metal-on-metal impact not spawned in `td5_vfx.c` |
| `WHEELS.png` | Dashboard steering-wheel / wheel indicator | Unknown consumer; possibly FF/steering-wheel status icon |

## 2. Static tpages (`re/assets/static/tpage*.dat + .png`)

FULLY WIRED: `tpage0..5`, `tpage12`, `static.hed`, `sky.prr`, `wheel.prr`.
`environ0.png` is **unused** (old pre-extraction artifact; `environs/` folder
is authoritative).

## 3. Frontend UI (`re/assets/frontend/*.png`, 57 files)

The runtime loads TGA files from `original/Front End/frontend.zip` via
`frontend_load_tga()`. Only **3 PNGs** in this directory are actually
consumed from disk:

- `ButtonBits.png` (`td5_frontend.c:2854`)
- `ArrowButtonz.png` (`td5_frontend.c:2880`)
- `ButtonLights.png` (`td5_frontend.c:2904`)

All others are **extracted reference backups** — not loaded at runtime. The
functional equivalents are pulled as TGA from the original ZIP. That is
working as designed; no action needed on the PNGs themselves.

**However**, some TGA consumers are missing — i.e., the game code references
files but never actually draws them. Features flagged below in §9.

## 4. Frontend buttons (`re/assets/frontend_buttons/`, 4 files)

Duplicates of `#3` above; same three PNGs are loaded from the sibling
`frontend/` folder, not from here. This directory is redundant.

## 5. Loading screens (`re/assets/loading/LOAD00–LOAD19.png`)

FULLY WIRED — random selection `rand() % 20` (`td5_game.c:2961`). `Load04.png`
is a case-variant duplicate (`LOAD04.png` vs `Load04.png`). On case-
insensitive Windows they collide; on case-sensitive mounts only one is
reached.

## 6. Legals (`re/assets/legals/LEGAL{1,2}.png`)

FULLY WIRED (`td5_fmv.c:790`). 5 s splash on boot.

## 7. Extras — background gallery & band photos (`re/assets/extras/`)

| File | Used | Where |
|---|---|---|
| `Pic1.png` – `Pic5.png` | YES | `td5_frontend.c:3021–3025`, cross-faded background slideshow |
| `Fear Factory.png` | **NO** | MusicTest screen has explicit TODO at `td5_frontend.c:6544` ("Update 'Now Playing' with band name + song title") |
| `Gravity Kills.png` | **NO** | same MusicTest screen — band-photo panel never loaded |
| `Junkie XL.png`   | **NO** | same |
| `KMFDM.png`       | **NO** | same |
| `PitchShifter.png`| **NO** | same |

See `Screen_MusicTestExtras()` (state 0 comment: *"Release gallery images,
load band photos"* — never implemented). → TODO below.

## 8. Mugshots (`re/assets/mugshots/`, 27 files)

The ExtrasGallery (`td5_frontend.c:7185`) names all 27 in
`s_gallery_names[]` and loads them as TGA from `Front End/Extras/Mugshots.zip`.
The PNG extractions in `re/assets/mugshots/` are reference backups, not a
runtime consumer. FULLY WIRED in behaviour.

## 9. Environs (`re/assets/environs/`, 27 files — chrome/reflection maps)

`td5_asset_load_environs_pages()` (`td5_asset.c:2185–2227`) hard-codes
exactly four:

```
SUN.png   TREE.png   MSUN.png   BRIDGE.png
```

The other **23 textures are never loaded**:
`ATRE1, CTUN1, HTUN1, HTUN2, ITUN1, KTRE1, KTUN1, MBRD1, MBWL1, MTRE1, MTUN1,
NBRD1, NBRD2, NTUN1, QTUN1, SBRD1, SUNBKP, UTUN1, UTUN2, WBRD1, WTUN1,
YSHP1, ZTUN1`.

These are tunnel/bridge/tree env-map variants. Original selected per-track
from LEVELINF.DAT (see `0x42F990` + `g_track_environment_config`). Port
currently feeds every track the same 4-map rotation → **cars show sun/tree
chrome reflections inside tunnels.** → TODO below.

## 10. Tracks (`re/assets/tracks/trak00–trak19.png`)

FULLY WIRED — track-select thumbnails, `"trak%04d.tga"` lookup.

## 11. Traffic (`re/assets/traffic/model*.prr + skin*.png`, 31 × 2)

FULLY WIRED — `td5_asset_load_traffic_model()` loads by index.

## 12. Cars (`re/assets/cars/<tag>/*`)

FULLY WIRED — per-slot loader reads `himodel.dat`, `carparam.dat`,
`CARSKIN{0-3}`, `CARHUB{0-3}`, `carpic{0-3}`.

## 13. Levels (`re/assets/levels/levelNNN/`)

FULLY WIRED — `strip.dat`, `left.trk`, `right.trk`, `checkpt.num`,
`levelinf.dat`, `textures.dat`, `models.dat`, `FORWSKY.png`, `backsky.png`,
`traffic.bus`.

---

# TODO — missing features implied by unused textures

Priority tiers: **P1** visibly wrong in-game · **P2** missing gameplay system
· **P3** cosmetic polish.

## [P1] Per-track environment-map selection
- File: `td5mod/src/td5re/td5_asset.c:2185` (`td5_asset_load_environs_pages`)
- Current code loads 4 hard-coded maps (SUN/TREE/MSUN/BRIDGE).
- Original behaviour: driven by `LEVELINF.DAT` (via
  `g_track_environment_config`). Each track picks its own env-map set
  (tunnels pick `*TUN*`, bridges pick `*BRD*`, trees pick `*TRE*`).
- Action: read env-map name list from `levelinf.dat`, iterate
  `re/assets/environs/*.png` by that list instead of the hard-coded 4.
- Verify: drive through Tokyo/Italy tunnels and confirm chrome uses tunnel
  env-map, not SUN.

## [P1] Vehicle damage HUD (DAMAGE / DAMAGEB1 / DAMAGEB2)
- File: `td5mod/src/td5re/td5_hud.c` (no damage draw path today)
- Physics already maintains `actor->damage_lockout` (`td5_physics.c:2177+`).
- Action: draw DAMAGE.png as base panel + DAMAGEB1/B2 as flashing / progressive
  damage overlays, anchored top-right of HUD. Frame-drive by damage_lockout.
- Asset: `re/assets/static/sprites/DAMAGE.png`, `DAMAGEB1.png`, `DAMAGEB2.png`
  (all present, all have entries in `static.hed`).

## [P2] Collision-impact spark VFX (SPARK)
- File: `td5mod/src/td5re/td5_vfx.c` (spark entry point absent)
- Atlas entry `SPARK` exists but no call site. Original spawns sparks on
  metal-on-metal / wall impacts (impulse magnitude gate).
- Action: add `td5_vfx_spawn_spark(pos, normal, impulse)`; call from
  `td5_physics.c:2166 "--- 10. Impact magnitude and damage effects ---"`.

## [P2] Police chase / cop-car lightbar (POLICE_*, POLICELT_*)
- Files: `td5_render.c` (no police-light pass) + `td5_ai.c` (pursuit logic
  stub at line 2281).
- Four PNGs present, never drawn. Feature is cop-pursuit mode — a real TD5
  gameplay mode behind CupData unlock path.
- Action:
  1. Draw POLICE_BLUE / POLICE_RED as the siren-bar geometry sprite on the
     cop cars (cop.zip / sp5-7.zip as loaded in `td5_asset.c:2274–2277`).
  2. Drive flash animation with POLICELT_BLUE / POLICELT_RED alternation
     (60 Hz toggle or 2-frame cycle).
  3. Only render when actor is `cop` slot AND `pursuit_mode != 0`.

## [P2] Music-Test Extras band-photo panel
- File: `td5mod/src/td5re/td5_frontend.c:6498` (`Screen_MusicTestExtras`)
- Screen state 0 has a TODO comment: *"Release gallery images, load band
  photos"*. Five band portraits exist:
  `Fear Factory / Gravity Kills / Junkie XL / KMFDM / PitchShifter`.
- Action: load correct band photo when user cycles
  `s_music_test_track_idx 0..11` (map 12 tracks → 5 bands per TD5 tracklist)
  and update the "Now Playing" surface with band + song title text.

## [P2] 2D sky fallback (SKY)
- File: `td5mod/src/td5re/td5_render.c` (sky rendering currently PRR-only)
- Low priority — only needed if `sky.prr` fails to load (robustness path).
- Action: if `sky.prr` load fails, upload `SKY.png` sprite and render as a
  fullscreen quad background. Today a `sky.prr` failure produces a black
  background.

## [P3] Speedo unit label (MPH)
- File: `td5mod/src/td5re/td5_hud.c:904` (speedo draw)
- The port reads `speed_kph` preference (`td5_frontend.c:3359, 3873`) but
  does not paint an "MPH" / "KPH" label sprite next to the digits.
- Action: draw MPH.png (or a KPH variant if present in atlas) to the right
  of SPEEDOFONT digits.

## [P3] Minimap car markers (CAR0 / CAR1 / CAR2)
- File: `td5mod/src/td5re/td5_hud.c` (minimap draws track outline only)
- Three tiny car icons intended to mark player (CAR0), opponents (CAR1),
  and traffic (CAR2) on the minimap.
- Action: after minimap track outline, iterate actors, project world XZ →
  minimap UV, blit CAR0/CAR1/CAR2 by actor type.

## [P3] Yellow fade overlay (FADEYEL)
- File: `td5mod/src/td5re/td5_vfx.c:553` (FADEWHT is used; FADEYEL mirror
  path absent)
- Likely a caution-flag / warning flash (used when player is off-track or
  going wrong way). FADEWHT pathway already exists — clone it.

## [P3] Race-position ordinal sprite (POSITION)
- File: `td5mod/src/td5re/td5_hud.c:1816` (`TD5_HUD_POSITION_LABEL`)
- Today the label is composited from the numeric font (`NUMBERS`). The
  authored `POSITION.png` sprite strip ("1st / 2nd / 3rd …") is unused.
- Action: replace numeric-font composition with single-glyph blit from
  POSITION atlas for positions 1..6.

## [P3] WHEELS sprite
- Purpose uncertain. Likely a steering-wheel / force-feedback status icon on
  controller setup screen.
- Action: identify consumer via Ghidra (search xrefs to `"WHEELS"` string in
  `TD5_d3d.exe`), then wire.

## [P3] Dedup / clean ups
- `re/assets/static/environ0.png` — stale extraction (environs live in
  `re/assets/environs/`). Safe to delete.
- `re/assets/frontend_buttons/` — exact duplicates of 4 files already in
  `re/assets/frontend/`. Safe to delete.
- `re/assets/loading/Load04.png` — case-variant duplicate of `LOAD04.png`.
  Pick one.
- `re/assets/frontend/Copy of Controllers.png`, `Untitled-1.png`,
  `OldBodyText.png` — author leftovers, not referenced anywhere. Safe to
  delete (will never be loaded, current code uses TGA-in-ZIP path).

---

## Summary table

| Category | Files | Used | Unused | Status |
|---|---:|---:|---:|---|
| Static tpages + headers | 13 | 12 | 1 (environ0) | FULLY WIRED |
| Static sprites | 40 | 25 | 15 | **PARTIALLY WIRED** |
| Frontend PNG | 57 | 3 | 54 (backups) | BY DESIGN (uses TGA ZIP) |
| Frontend buttons | 4 | 0 | 4 (dupes) | REDUNDANT |
| Loading | 21 | 21 | 0 | FULLY WIRED |
| Legals | 2 | 2 | 0 | FULLY WIRED |
| Extras (gallery+bands) | 11 | 5 (Pic1-5) | 6 (bands + ?) | **PARTIALLY WIRED** |
| Mugshots | 27 | 27 (via ZIP) | 0 | FULLY WIRED |
| Environs | 27 | 4 | 23 | **PARTIALLY WIRED** (per-track selection missing) |
| Tracks | 20 | 20 | 0 | FULLY WIRED |
| Traffic | 62 | 62 | 0 | FULLY WIRED |
| Cars | per-slot | all | 0 | FULLY WIRED |
| Levels | per-track | all | 0 | FULLY WIRED |

Net: **3 major partially-wired categories**, totalling roughly **44 unused
textures**. Most map to 4 missing / stubbed features:

1. Per-track env-map selection (23 files)
2. Damage / police / spark combat-VFX HUD (10 files)
3. Music-Test band panel (5 files)
4. Minor HUD polish — MPH, POSITION, CAR0-2, FADEYEL, WHEELS, SKY (~9 files)
