# td5_trackgen — custom track builder

Build a drivable Test Drive 5 track from a **centerline path** (the simplest,
game-agnostic shape any other game's track reduces to). The tool emits the full
`re/assets/levels/levelNNN/` editable-source set and registers the track in
`re/assets/levels/custom_tracks.json`, so it becomes selectable in the frontend
and AutoRace-able **with no recompile**.

## Quick start

```bash
# A built-in sample (oval / figure8 / straight):
python re/tools/td5_trackgen.py sample oval

# From your own centerline (CSV or JSON):
python re/tools/td5_trackgen.py build re/tools/td5_trackgen_examples/sprint.csv --p2p --name "MY SPRINT"

# List what's registered:
python re/tools/td5_trackgen.py list
```

Each build prints the assigned **slot** and **level**. Drive it:

```bat
td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=<slot>
```

or pick it in the Quick Race / Track Selection menu (custom tracks appear after
the built-in tracks).

## Input: the neutral centerline spec

**CSV** — one centerline node per row, header `x,z[,y,width,lanes,surface]`:

```
x,z,width,lanes
0,0,9000,6
0,8000,9000,6
1500,16000,6000,4
...
```

**JSON** — adds global params, per-node overrides, checkpoints, traffic:

```json
{
  "name": "TEST OVAL",
  "circuit": true,
  "lane_width": 1500,
  "default_lanes": 4,
  "nodes": [ {"x":0,"z":0}, {"x":0,"z":6000,"lanes":6,"width":9000}, ... ],
  "checkpoints": "auto:4",
  "weather": 2,
  "fog": {"enabled":0,"r":0,"g":0,"b":0},
  "traffic_enable": 0
}
```

Units are TD5 world units (a lane is ~1500 wide, like Moscow). `x,z` are the
road centerline; `y` is optional elevation (default flat). `width`/`lanes`/
`surface` may be set per node or default globally.

## What it generates

| File | Contents |
|------|----------|
| `strip.json` | collision/logic ribbon — spans + vertices (packed to STRIP.DAT on load) |
| `left/right.trk.csv` | AI route tables (via the shared `build_routes` corridor synth) |
| `levelinf.json` | circuit flag, checkpoint spans, weather, fog, traffic-enable, span count |
| `checkpt.json` | 96-byte reverse page-swap record (minimal) |
| `traffic.json` | ambient-traffic spawn queue |
| `custom_tracks.json` | the runtime registry manifest (slot → level + params) |

A track with no `models.bin` mesh is drawn by the engine's **strip-ribbon render
fallback** as a solid grey road, so it is visible and drivable immediately.

## Hybrid detection (per the design)

- **Lanes / sublanes** — taken from each node's `lanes` (auto-default from
  `default_lanes`); the road width subdivides into `lanes+1` rail vertices.
- **Checkpoints** — `"auto:N"` evenly spaces N checkpoints, or pass an explicit
  `[span, ...]` list. Stored in `levelinf.checkpoint_spans`.
- **Branches** — the spec reserves a `branches` field; junction/fork emission is
  a follow-on (the tool warns if branches are present so nothing is silently
  dropped).

## Follow-ons (not in v1)

- Branch / junction emission (fork roads, the `[lo,hi,base]` remap table).
- Per-span lane-transition span types (3/4) for smoother lane-count changes.
- Textured `models.bin` mesh generation (v1 uses the flat render fallback).
- Reverse-direction (`stripb.json`) generation.
- 3D-mesh and native-game importers feeding the same neutral spec.
