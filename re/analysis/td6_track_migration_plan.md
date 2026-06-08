# TD6 → TD5 Track Migration — Infrastructure Plan

> ## ✅ PHASE 1 MILESTONE A — DONE (2026-06-02, branch fix-1780453436-624376-11913, commit 37c24ec, UNMERGED)
> **A TD6 track drives on the faithful TD5 engine.** Proof chain:
> - `re/tools/convert_td6_tracks.py convert 10 7` → `re/assets/levels/level007/` (TD6 level010:
>   STRIP.DAT + STRIPB.DAT passthrough + synthesized LEVELINF.DAT). Converter also has a `report`
>   mode (format diff + mesh render_type histogram — confirms TD6 meshes are 0x04).
> - C change (worktree, 3 files): `--OverrideTrackZip` / `[Game] OverrideTrackZip` dev knob →
>   `td5_asset_level_number()` returns it directly → loose-file loader resolves `level007/`.
>   **Inert when 0 → faithful byte-identical** (no regression sweep needed).
> - Runtime (level007, OverrideTrackZip=7): `Strip loaded: spans=517 vertices=4631 jumps=1`;
>   junction sentinels `span[0]=type9 span[449]=type10`; `LEVELINF.DAT loaded: CIRCUIT`;
>   6 racers spawn; **no crash, 175 FPS**; car traverses the TD6 spline (span 1→121+);
>   collision wireframe traces the correct TD6 rail/span geometry (shot_td6_wireframe.png).
> - **Confirms the central thesis: TD6 strip.dat is engine-compatible** (the C parser
>   `td5_track_load_strip` uses absolute span_offset/vertex_offset @td5_track.c:2109, so TD6's
>   inserted metadata block is skipped cleanly; 24B spans + 6B verts parse 1:1).
>
> **Known Milestone-A gaps (expected, → Milestone B / Phase 2):** no visible road mesh (models.dat
> omitted → placeholder; world renders sky-blue under the wireframe); player spawn is rough
> (`start_span=556 > span_count=517`, transient garbage pos_y) because per-track metadata tables
> (grid-spawn span, checkpoints @ 629/1182/… , boundary sentinels, camera profiles) are still keyed
> to the *selected* TD5 track, not the TD6 one — that is exactly the **track registry (Phase 2)**.
> Next: **Milestone B** = de-index models.dat (0x04→0x03) + repack textures from texture*.zip →
> visible TD6 world; then Phase 2 registry to fix per-track metadata + selectability.

Status: **Phase 1 Milestone A complete; planning for B + registry**, 2026-06-02. Evidence-grounded from direct inspection of the
TD6 asset files (`Test Drive 6/`) and the TD5RE source-port track tables. No code written yet.
Companion to [`modding_strategy.md`](modding_strategy.md) (Phase 1 "data-ize the tables") and the
already-shipped **TD6 car port** (memory `project_td6_car_port_2026-06-02`), whose offline-convert +
runtime-transcode pattern this plan extends from cars to tracks.

---

## 0. TL;DR & recommendation

TD6 is the **same Pitbull engine one revision up** — and the track data proves it. TD6's `strip.dat`,
`stripb.dat` and `models.dat` use the **same container layouts** as TD5 (5×u32 strip header + 24-byte
spans + 6-byte verts; `(size, cumulative-offset)` mesh directory). The deltas are: (a) meshes are
`0x104` **indexed** instead of TD5 `0x103` **expanded** (the exact car-port delta — transcoder reusable);
(b) textures are **externalized** into separate `texture*.zip` archives + a `textures.dir` index instead
of a bundled `textures.dat`; (c) per-track metadata that TD5 keeps in separate files
(`levelinf.dat`, `checkpt.num`) is **folded into a new block inside `strip.dat`**; (d) boundary rails
`left/right.trk` are replaced by `spline1-4.td6`; (e) traffic `.bus` → `.trf`; (f) explicit collision
proxies `COL_*.prr` (also `0x104`).

**Recommended strategy: A — offline transcode (mirror the car port), not a runtime native-TD6 loader.**
A Python tool `convert_td6_tracks.py` reads a TD6 level + its texture archive + objects and emits a
**TD5-shaped `levelNNN/` asset directory** that the existing faithful engine loads with near-zero engine
changes. TD6 quirks stay quarantined in the converter; the faithful baseline + trace oracle stay intact
(per `modding_strategy.md` "fork only behind a flag, last-responsible-moment"). The only runtime change is
a small **track-registry override** that lets the hardcoded 19/20 tables be extended/replaced from a data
file — which is also `modding_strategy.md` Phase 1 done for real.

**TD6 roster: 17 drivable tracks** (each with forward + reverse + models): `level000-004`, `level010-019`,
`level030`, `level032`. (`level090-095` are single-file showroom/garage scenes, not race tracks.)

---

## 1. The roster (what we'd be adding)

| TD6 zip | reverse? | models | notes |
|---|---|---|---|
| level000–004 | ✓ stripb | ✓ | the 5 "big" tracks (~1.8–2 MB geometry) — likely the marquee city circuits |
| level010–019 | ✓ stripb | ✓ | 10 "small" tracks (~0.4–0.5 MB) |
| level030, level032 | ✓ stripb | ✓ | 2 extra (drag/bonus?) |
| level090–095 | ✗ | ✗ | showroom/garage (`garage.dat` only) — **not** race tracks, skip |

Exact city/name mapping → resolve from TD6 `frontend.zip` preview TGAs (London, NewYork, Rome, Maui,
Tahoe, CapeH, England, Ireland, Italysm, F1Track…) + TD6.exe schedule tables (RE task, §9).

---

## 2. Format gap analysis (TD5 vs TD6, per file) — **"where are the gaps"**

Evidence: hexdumps of `original/level001.zip` (Moscow) vs `Test Drive 6/LEVELS/level000.zip`.

| Concern | TD5 (faithful) | TD6 | Gap severity | Disposition |
|---|---|---|---|---|
| **Track spline network** | `strip.dat` / `stripb.dat`: hdr `{span_off, span_cnt, vtx_off, vtx_cnt, total}`, **24B spans, 6B verts** | **Same layout** + an **extra metadata block** inserted between the 5×u32 header (ends `0x14`) and the span table (`span_off=0x188`). Header continues `[0x14]=21, [0x18]=224, [0x1c]=392`, then int16 triples. Per-span field semantics in the latter 14 bytes differ slightly (origin/link encoding). | **LOW–MED** | Parse passes already (visualizer `parse_strip` ingests TD6 unchanged: level000=3340 spans). Need to RE the extra block + reconcile per-span field deltas. |
| **Track geometry** | `models.dat`: `(size, cumulative-offset)` mesh directory; meshes `0x103` **expanded** (44B per-corner verts, real per-prim normals) | `models.dat`: **same directory layout**; meshes `0x104` **indexed** (32B verts pos+normal+uv, u16 tri-list) | **LOW** | **Existing car transcoder `td5_asset_transcode_td6_mesh` (0x104→0x103) generalizes** — apply per-mesh while walking the models.dat directory. |
| **Textures** | `textures.dat` (atlas) **bundled in the level zip** | Externalized to `LEVELS/texture*.zip` keyed by a 64-byte-record `textures.dir` (name + WxH; 228 entries in texture0) | **HIGH** | Biggest pipeline gap. Converter resolves model texture refs → `textures.dir` → pull TGA from the right `texture*.zip` → repack into a TD5-style `textures.dat` **or** loose `tex_NNN.png` (the port already supports loose `re/assets/levels/levelNNN/textures/tex_NNN.png`, `td5_asset.c:1462`). |
| **Circuit/P2P + checkpoints** | `levelinf.dat` (DWORD[0]=circuit flag) + `checkpt.num` | **Both absent** — folded into the `strip.dat` extra header block | **MED** | Converter synthesizes a TD5 `levelinf.dat` + `checkpt.num` from the decoded block; or teach loader to read inline (more invasive). |
| **Boundary / route** | `left.trk`/`right.trk` (+b) — boundary rails (int16) | `spline1-4.td6` (+b) — 4 splines, int16 x,z pairs, one entry per span | **MED** | RE which spline = drivable bounds vs AI lanes; convert 2 of them → `left/right.trk`. (Used by lateral wall containment 0x406CC0 + AI routing.) |
| **Traffic** | `traffic.bus`/`trafficb.bus` | `level*.trf`/`level*b.trf` | **LOW** | Structured records; convert or stub (traffic optional). |
| **Collision** | implicit (strip spans + left/right rails) | explicit `COL_*.prr` (0x104 meshes) + `objects/levelN/CM*.env` | **LOW** | TD5 collision is strip-driven; ignore TD6 explicit collision for v1 (containment already works off spans/rails). |
| **Sky** | `backsky.tga`/`forwsky.tga` **in level zip** | global `sky.zip` | **LOW** | Convert/pick a sky TGA per track or reuse a TD5 sky. |
| **Misc** | — | `level.mov` (camera-path/trackside?), `level*.tcl` (often all-0xFF placeholder) | **LOW/UNKNOWN** | `level.mov` may carry trackside-camera profiles (TD5 keeps those in-binary, `td5_camera_profiles.h`) — RE opportunity, optional. |

**Net:** the two hardest things in any track port — the spline/collision network and the mesh format —
are **already ~compatible** (strip) or **already solved** (mesh transcode). The real work is the
**texture-archive remap** and **synthesizing the metadata files** TD6 deleted.

---

## 3. What we can do with them — **"what can we do"**

1. **Drive them** — the primary goal. With strip + transcoded models + remapped textures + synthesized
   levelinf/checkpt, a TD6 track loads through the faithful TD5 pipeline (physics, AI containment, camera,
   HUD) unchanged.
2. **Forward + reverse** — every TD6 track ships `stripb.dat`, so reverse works like TD5 (and the recently
   fixed reverse-checkpoint-record path applies).
3. **Visualize** — top-down maps for free (the strip format is compatible; §7).
4. **Mix & match** — because the override is a data registry, you can *add* TD6 tracks alongside TD5,
   *replace* a TD5 slot with a TD6 one, or *remove* tracks — exactly the "additions and removals" ask.
5. **Future** — TD6 cars (already ported) on TD6 tracks; TD6 trackside cameras (`level.mov`) if RE'd.

---

## 4. Migration strategy — A (recommended) vs B

### Strategy A — offline transcode + thin runtime registry (RECOMMENDED)
- New tool `re/tools/convert_td6_tracks.py` (sibling of `convert_td6_cars.py`). Per TD6 track it emits a
  TD5-shaped `re/assets/levels/level<NNN>/` (or a repacked `level<NNN>.zip`) containing:
  `strip.dat`/`stripb.dat` (passthrough or re-fielded), `models.dat` (passthrough; meshes transcoded at
  runtime by the existing 0x104 path), `textures.dat` **or** loose `tex_NNN.png`, synthesized
  `levelinf.dat` + `checkpt.num`, `left/right.trk` (+b) from splines, sky, traffic.
- Runtime change is minimal: a **track registry** that extends the hardcoded tables (§5) so the new level
  numbers/pools/names/previews are selectable.
- **Pros:** faithful engine untouched → trace oracle stays valid; TD6 quirks isolated; mirrors the proven
  car-port workflow; assets are inspectable/diffable; aligns with `modding_strategy.md` "data over code."
- **Cons:** converter must understand both formats; a re-pack step per track.

### Strategy B — runtime native TD6 loader
- Teach `td5_asset.c`/`td5_track.c` to detect a TD6 level layout and load `texture*.zip`+`textures.dir`,
  inline strip metadata, `spline*.td6`, `.trf`, `COL_*.prr` natively.
- **Pros:** no asset duplication; "engine speaks TD6."
- **Cons:** couples TD6 format knowledge into the faithful core; bigger diff against the trace oracle;
  higher regression risk on the TD5 path. Defer unless end-user "drop a TD6 install and play" is required.

**Recommendation: A**, with the per-mesh 0x104 transcode kept at runtime (already exists) so `models.dat`
is essentially passthrough. Revisit B only if/when end-users should load an unmodified TD6 install.

---

## 5. Override the track table — **"how to override to allow additions and removals"**

### Current hardcoded surface (exact, verified)
| Table | File:line | Count | Indexed by | Must change to add? |
|---|---|---|---|---|
| `k_schedule_to_pool[19]` | `td5_asset.c:1437` | 19 | schedule slot | ✓ |
| `k_pool_to_zip[19]` | `td5_asset.c:1441` | 19 | pool | ✓ (maps pool→level zip number) |
| guard `track_index >= 19` | `td5_asset.c:1445` | — | — | ✓ (the count gate) |
| drag special-case `==30` | `td5_asset.c:1431` | — | game_type | keep |
| `s_schedule_to_pool_index[20]` | `td5_asset.c:2896` | 20 | schedule slot | ✓ |
| `s_track_pool_span_count_table[18]` | `td5_asset.c:2907` | 18 | pool | ✓ (checkpoint record selector, fwd) |
| `s_track_pool_reverse_span_count_table[15]` | `td5_asset.c:2914` | 15 | pool | ✓ (reverse record, `-1`=none) |
| `s_traffic_pool_to_row[25]` | `td5_asset.c:2923` | 25 | pool | optional (traffic) |
| `s_track_display_names[26]` | `td5_frontend.c:716` | **26 (6 spare: "TRACK 21".."TRACK 26")** | name index | ✓ (fill spares) |
| `s_track_schedule_to_name_index[20]` | `td5_frontend.c:752` | 20 | schedule slot | ✓ |
| `s_track_schedule_to_tga_index[20]` | `td5_frontend.c:759` | 20 | schedule slot → pool/TGA | ✓ |
| `s_track_lock_table[26]` | `td5_frontend.c:281` | 26 | track | ✓ |
| `s_track_markers[20]` | `td5_frontend.c:559` | 20 | pool | preview start/finish dots |
| `s_per_track_camera_profiles[~40]` | `td5_camera_profiles.h` | ~40 | pool (world_x−1) | optional (fallback=chase) |
| network cap `track <= 18` | `td5_frontend.c:~10011` | — | — | optional (MP) |

### Design: a single source-of-truth **track registry**
Replace the scattered `static const` arrays with one **append-after-builtins** registry, loaded from a
human-editable data file (matches the INI/JSON style already in the project):

```ini
; re/assets/tracks/track_registry.ini  (faithful defaults = byte-identical to the 19 builtins)
[track.20]              ; new schedule slot 20 (first TD6 add)
name      = LONDON, ENGLAND
level_zip = 100          ; -> level100.zip (converted TD6 level000); keep TD6 out of the 1..39 range
pool      = 20
tga       = 20           ; preview index -> re/assets/tracks/trak0020.png
circuit   = 1            ; from synthesized levelinf
checkpts  = 100          ; checkpoint-record id (fwd)
checkpts_rev = 101       ; or -1
camera_profile = -1      ; -1 => chase fallback
locked    = 0
```

- A loader `td5_track_registry_load()` parses this once at init into a dynamic
  `struct TrackEntry registry[]`, **seeded with the 19 faithful builtins** so an absent/empty file =
  byte-identical behavior (the trace oracle stays green).
- Every hardcoded lookup (`td5_asset_level_number`, frontend name/tga/lock, span-count records) becomes a
  registry read keyed by schedule/pool. The `>= 19` guards become `>= registry_count`.
- **Additions** = new `[track.N]` rows. **Removals** = a `disabled = 1` row or simply omitting it from the
  schedule order. **Reorder** = a `[schedule]` order list. This is literally "override the table."
- Use level zip numbers **≥ 100** for converted TD6 tracks so they never collide with TD5's `1..39`.

### Stopgap before the full registry (fast single-track proof)
Mirror the car port's `--PlayerCarArchive`: add `[Game] OverrideTrackZip=<NNN>` / `--OverrideTrackZip=NNN`
so `td5_asset_level_number()` returns that zip for the selected slot. Lets us drive **one** converted TD6
track end-to-end before building the registry (de-risks the converter first, the table plumbing second).

---

## 6. Frontend integration — **"integrate them in the frontend"**

The 30-screen frontend is fully ported; the track selector (screen 21) is data-driven off the §5 tables.

1. **Names** — fill `s_track_display_names` spares (slots 20-25 already exist) or grow + drive from the
   registry. Source names from TD6 `frontend.zip` preview TGAs / TD6.exe.
2. **Previews** — two options, both cheap:
   - **(a) Render from strip** (recommended, consistent look): `track_preview_render.py` already parses
     TD6 strip → emit `re/assets/tracks/trak00<NN>.png` (RGBA-transparent, per the start/finish-dot +
     RGBA gotchas already solved). Also bakes `trak_markers.dat` start/finish dots.
   - **(b) Reuse TD6 art** — extract the real TD6 preview TGAs from its `frontend.zip` (London/NewYork/
     Rome/Maui/Tahoe/F1Track…) → convert to the 152×224 RGBA preview format.
3. **Selector counts** — the selector's max index, the ◄►/page logic, and lock table all key off the
   registry count; growing it surfaces the new tracks automatically.
4. **Direction toggle** — Forwards/Backwards already works off `s_track_direction`; TD6 ships `stripb`, so
   reverse + the start/finish-dot swap come along.
5. **Circuit vs P2P** — drives the laps UI; comes from the synthesized `levelinf` circuit flag.
6. **Schedule/championship** — v1 targets **Quick Race** (single-track selectable). Championship schedule
   integration is a later phase (it threads through save/unlock structures — same deferral as the car
   port's menu integration).

---

## 7. Visualize all changes — **"visualize all changes between TD5 and TD6"**

Three concrete deliverables:

1. **Format diff doc** — §2 of this file (machine-checkable: a `tools` script can hexdump-diff any
   TD5/TD6 file pair and regenerate the table).
2. **Track maps** — `track_preview_render.py` already renders top-down lane-centerline maps and **already
   parses TD6 strip.dat** (verified: level000 = 3340 spans). Run `render`/`debug` over all 17 TD6 levels →
   a contact sheet of TD6 track shapes (and side-by-side vs the 19 TD5 tracks). The `debug` mode shows
   branches/junctions/start — ideal for spotting strip-field divergences visually.
3. **Mesh/texture inventory** — a small script to dump, per TD6 level: mesh count + render_type histogram
   (prove all `0x104`), texture refs resolved through `textures.dir`, and any unmapped textures. This is
   the "what's different" audit that gates the converter.

A `convert_td6_tracks.py --report <level>` dry-run mode produces 1+2+3 per track without writing assets.

---

## 8. Phased roadmap

**Phase 0 — RE the TD6 deltas (Ghidra on TD6.exe; see §9).** Decode the `strip.dat` extra block
(checkpoint/circuit), the per-span field deltas vs TD5, the 4 `spline*.td6` roles, `.trf`, and the
`textures.dir` record layout. *Deliverable: a field-accurate format spec.* Gates everything else.

**Phase 1 — Converter + single-track proof (Strategy A core).** `convert_td6_tracks.py` for ONE track
(suggest a small one, e.g. level010). Emit TD5-shaped assets (transcoded models, repacked textures,
synthesized levelinf/checkpt, splines→trk). Add the `OverrideTrackZip` stopgap (§5). Drive it on the
faithful engine. *Deliverable: one TD6 track drivable.*

**Phase 2 — Track registry (data-over-code override).** Build `track_registry.ini` + loader; route the
15 hardcoded tables through it; faithful defaults = trace-green. *Deliverable: add/remove/reorder tracks
from a data file.*

**Phase 3 — Frontend.** Names, previews (render all 17), lock table, selector counts, circuit flag, reverse.
*Deliverable: all 17 TD6 tracks selectable in Quick Race with previews.*

**Phase 4 — Batch convert all 17 + visualization deliverable.** Run the converter over the full roster;
produce the side-by-side TD5/TD6 map contact sheet + format-diff report.

**Phase 5 (optional) — Polish/faithful extras.** TD6 trackside cameras from `level.mov`; TD6 traffic;
explicit `COL_*.prr` collision; championship-schedule integration; TD6 cars on TD6 tracks.

---

## 9. Open RE questions (need Ghidra on **TD6.exe**, not the TD5 project)

The Ghidra pool holds `TD5_d3d.exe`. These need `Test Drive 6/TD6.exe` loaded (new program):

1. **`strip.dat` extra block** (`0x14`→`span_off`): is `{21, 224, 392}` a `{checkpoint_count, ?, span_offset}`
   header? Are the int16 triples checkpoint span indices? Where's the circuit flag?
2. **Per-span 24B field semantics** — TD5 span0 `01 11 09 84 …` vs TD6 `01 00 00 84 …` diverge after the
   `left/right vtx` + first link; confirm origin/link encoding so the converter can re-field if needed.
3. **`spline1-4.td6`** — which is drivable-left, drivable-right, AI-fast-line, AI-slow-line? (4 splines ×
   int16 x,z × span_count.) Maps to TD5 `left/right.trk` + AI routing.
4. **`textures.dir`** — exact 64-byte record (name field width, the `0x80/0x80` = WxH, any page/format id).
5. **`.trf`** traffic record layout; **`level.mov`** purpose (trackside camera?).
6. **TD6 schedule/name tables** in TD6.exe → authoritative track names + circuit flags + roster order.

Until Phase 0 lands these, the converter can still do a **passthrough-first** pass (copy strip/models,
remap textures, default circuit=guess-from-geometry) to get a track *driving*, then tighten fidelity as the
fields are decoded.

---

## 10. Risks & mitigations

- **Texture remap wrong** → wrong/missing track textures. Mitigate: loose `tex_NNN.png` override path
  already exists; dry-run report flags unmapped refs before driving.
- **Synthesized checkpoints wrong** → unreachable stages / mis-fired banners (cf. the reverse-checkpoint
  saga). Mitigate: Phase 0 decodes the real block; until then, geometry-derived even spacing + manual drive.
- **Per-span field drift** → bad containment/AI line. Mitigate: visualize (`debug` render) first; the strip
  parser already round-trips geometry, so divergence shows up as a visibly wrong map.
- **Trace-oracle regression** → mitigated by the registry's faithful-default seeding (empty file = identical
  behavior) and keeping all TD6 work behind the converter + registry, never in the faithful core.
- **Frontend table-count desync** (15 arrays must agree) → the registry collapses them to one source of truth.
</content>
</invoke>
