# TD5RE texture audit — 2026-04-14

- PNGs present in `re/assets/`: **695**
- Source files scanned: **40**
- Literal PNG paths in code: **15**
- Format-string PNG paths in code: **5**

## Classification summary

- `literal`       — PNG path hardcoded in source: **7**
- `format`        — matched by a runtime `snprintf` pattern: **688**
- `stem-in-code`  — filename stem appears in code (likely atlas-entry-name): **0**
- `orphan`        — no evidence of use anywhere: **0**

## TODO — textures the code expects but are missing on disk

- [ ] `re/assets/benchmark.png`

## Format-string PNG paths in code

- `re/assets/%s/%s.png` → **644** PNGs on disk match
- `re/assets/levels/level%03d/FORWSKY.png` → **0** PNGs on disk match
- `re/assets/levels/level%03d/textures/tex_%03d.png` → **21** PNGs on disk match
- `re/assets/loading/load%02d.png` → **20** PNGs on disk match
- `re/assets/static/tpage%d.png` → **3** PNGs on disk match

## Literal hardcoded paths

- `re/assets/benchmark.png` — MISSING
- `re/assets/environs/BRIDGE.png` — OK
- `re/assets/environs/MSUN.png` — OK
- `re/assets/environs/SUN.png` — OK
- `re/assets/environs/TREE.png` — OK
- `re/assets/extras/pic1.png` — OK
- `re/assets/extras/pic2.png` — OK
- `re/assets/extras/pic3.png` — OK
- `re/assets/extras/pic4.png` — OK
- `re/assets/extras/pic5.png` — OK
- `re/assets/frontend/ArrowButtonz.png` — OK
- `re/assets/frontend/ButtonBits.png` — OK
- `re/assets/frontend/ButtonLights.png` — OK
- `re/assets/legals/legal1.png` — OK
- `re/assets/legals/legal2.png` — OK

## Orphan PNGs — no evidence of use (0)

Candidates for deletion. Each filename stem does not appear anywhere in source, and no runtime format string on file can build this path.

## Stem-in-code PNGs (0)

Filename stem appears in source — typically loaded via atlas-entry-name lookups or bespoke asset paths that aren't pure format strings.

