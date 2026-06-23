# TD5 Track Studio

Web GUI to **import and edit custom tracks** — the visual companion to
`re/tools/td5_trackgen.py`, and a sibling of the Car Studio
(`re/tools/car_studio/`).

```bash
python re/tools/track_studio/td5_track_studio.py
```

Opens `http://localhost:8766/` in your browser (first run downloads three.js into
`vendor/`; `--no-browser` and `--revendor` flags as per the Car Studio). It edits
the same neutral centerline spec the converter consumes and, on **Build**, writes
`re/assets/levels/levelNNN/` and registers the track in `custom_tracks.json` (via
`td5_trackgen.build_track`) — so it's drivable with no recompile.

## Workflow

1. **Track panel** — *Import* any existing `levelNNN` (it's reversed back into an
   editable centerline: per-span rail-midpoint nodes, lane counts, params), or
   start from a **sample** (oval / figure-8 / straight) or **blank**.
2. **Viewport** — top-down/orbit view of the road ribbon (lanes coloured by
   surface), node handles, branches, checkpoints (red), start (green) / finish
   (blue) lines.
   - **Drag a node** to move it · **click** to select · **shift-click** empty
     ground to add a node · **Delete** removes the selected node.
3. **Selected node** — set lanes / width / surface; *apply to all*.
4. **Branches** — *draw branch*, click points across the track, *Finish*.
   Branches peel off the right and rejoin where they end (see converter notes).
5. **Checkpoints** — auto (evenly spaced) or manual (**Shift+C** on a node).
6. **Environment** — circuit/point-to-point, weather, fog, traffic.
7. **Build & register** — writes the level + registry entry; the status line
   shows the slot and the `--DefaultTrack=N` command to drive it.

## Notes

- Imported tracks are decimated to ~72 editable nodes (the build resamples to
  ~1500-unit spans on the way out). Importing a **branched** track currently
  keeps only the main ring (branches are re-authored in the editor).
- The viewport ribbon is a preview; the exact in-game geometry (fork widening,
  shared-vertex splits) is produced by the converter at Build time.
- Self-contained: stdlib HTTP server + vendored three.js, no extra deps. Imports
  `re/tools/td5_trackgen.py` directly for the build/import engine.
