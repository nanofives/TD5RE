# TD5 Car Studio — custom-car authoring

Self-contained toolset for building **drop-in custom cars** for the TD5RE source
port. Cars produced here land in `re/assets/cars/custom_<name>/` and the engine
auto-enumerates them in the SELECT CAR screen (slots 76+) with no rebuild — see
`td5_customcar.c` and `re/TD5_MODDING_GUIDE.md` §2.6.

## Contents (everything in this folder)

| File | What it is |
|------|------------|
| `td5_car_studio.py` | Web GUI: local server + 3D viewport, the easy way to author a car |
| `td5_car_import.py` | The build engine + a standalone CLI (`import`/`texture`/`verify`/`new`/`doctor`/`stats`) |
| `td5_car_physics_ref.py` | Fleet physics reference: effect hints, per-field min/median/max + exemplar cars, archetype presets (drives the Studio Physics helpers + `stats`) |
| `index.html`, `car_studio.js` | The browser UI served by the studio |
| `vendor/` | three.js, downloaded here on first run (git-ignored, regenerates) |

The only external dependency is the shared PRR mesh codec `re/tools/mesh_tool.py`
(reused, not duplicated, so the format stays single-sourced). Python needs
`numpy` + `pillow` (`pip install numpy pillow`).

## Run

```bash
# from the project root:
python re/tools/car_studio/td5_car_studio.py          # opens the browser GUI
```

Load a `.glb`/`.gltf`/`.obj`, pick a donor for physics, edit the live carparam
fields (wheel gizmos update live), then **Build car**.

CLI equivalent (no browser):

```bash
python re/tools/car_studio/td5_car_import.py import \
    --model mycar.glb --name "My Cool Car" --skin body.png
python re/tools/car_studio/td5_car_import.py doctor re/assets/cars/custom_mycar
```

three.js (vendored to `vendor/`) needs internet on the first run only; offline
after. Refresh with `--revendor`.
