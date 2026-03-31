# Test Drive 5 Modding Workflow

## Tooling
Use `tools/td5_tool.py` as the entry point for non-destructive inspection and archive work.

## Common Commands
```powershell
python .\tools\td5_tool.py inventory --out .\reports\inventory.json
python .\tools\td5_tool.py inspect-pe .\M2DX.dll --out .\reports\m2dx-imports.json
python .\tools\td5_tool.py inspect-archive .\cars\cop.zip --out .\reports\cop-car.json
python .\tools\td5_tool.py inspect-archive .\level001.zip --out .\reports\level001.json
python .\tools\td5_tool.py export-editable .\level001.zip .\workspace\editable\level001
python .\tools\td5_tool.py import-editable .\level001.zip .\workspace\editable\level001 .\workspace\build\level001-from-json.zip
python .\tools\td5_tool.py compare-archives .\level001.zip .\workspace\build\level001-from-json.zip --out .\reports\level001-compare.json
python .\tools\td5_tool.py export-editable .\cars\cop.zip .\workspace\editable\cop-meta
python .\tools\td5_tool.py import-editable .\cars\cop.zip .\workspace\editable\cop-meta .\workspace\build\cop-win11-meta.zip
python .\tools\td5_tool.py compare-archives .\cars\cop.zip .\workspace\build\cop-win11-meta.zip --out .\reports\cop-win11-meta-compare.json
python .\tools\td5_tool.py unpack .\cars\cop.zip .\workspace\cars\cop
python .\tools\td5_tool.py repack .\workspace\cars\cop .\workspace\build\cop.zip
python .\tools\td5_tool.py report --out .\reports\td5-report.json
```

## Safe Workflow
1. Unpack the archive you want to study into `workspace/`.
2. Replace only exposed media files first: `.tga` and `.wav`.
3. For structured files, export editable JSON and rebuild to a parallel archive.
4. Use `compare-archives` to verify that only intended members changed.
5. Repack to a parallel output archive instead of overwriting the original.
6. Use the JSON reports to track which binary structures are still opaque.
7. Only start binary patching after the runtime hook points in `M2DX.dll` and `TD5_d3d.exe` are documented.

## JSON Editing Rule
- For traffic files, `raw_u32` is the authoritative editable field.
- `records_preview` is diagnostic output only and may not reflect manual JSON edits until the file is re-exported.

## Current Feasibility
- Texture/UI/audio upgrades: ready now.
- Data-driven race or traffic experiments: plausible once structured files are correlated.
- New engine behavior and gameplay systems: requires hook work and binary analysis.
- New car or track imports: blocked on geometry and track format decoding.

## Current Validators
- `config.nfo` can now be exported/imported as editable JSON text/lines for safe metadata changes.
- `carparam.dat` is validated as `268` bytes / `67` signed `int32`.
- `levelinf.dat` is validated as `100` bytes / `25` unsigned `int32`.
- `checkpt.num` is validated as `96` bytes / `24` unsigned `int32`.
- `.trk` reports now include a triplet view to support lane/path reverse engineering.

## Sample Mod Artifact
- `[cop-win11-meta.zip](../workspace/build/cop-win11-meta.zip)` is a demonstrator archive rebuilt from editable JSON.
- `[cop-win11-meta-compare.json](../reports/cop-win11-meta-compare.json)` confirms that only `config.nfo` changed relative to the original `cars/cop.zip`.
- `[level001-traffic-exp.zip](../workspace/build/level001-traffic-exp.zip)` is an experimental level variant with a one-record `traffic.bus` change.
- `[level001-traffic-exp-compare.json](../reports/level001-traffic-exp-compare.json)` confirms that only `traffic.bus` changed relative to the original `level001.zip`.
