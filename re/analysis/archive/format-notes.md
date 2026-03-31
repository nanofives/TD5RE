# Test Drive 5 Format Notes

## Easy Asset Surface
- Most replaceable art is exposed in ZIP archives as `.tga` files.
- Car archives in `cars/*.zip` contain `carskin*.tga`, `carhub*.tga`, `carpic*.tga`, audio, and a `config.nfo`.
- Frontend and loading art is exposed in `[Front End/frontend.zip](../Front%20End/frontend.zip)` and `[loading.zip](../loading.zip)`.
- Traffic skins in `[traffic.zip](../traffic.zip)` are also standard `.tga` textures paired with proprietary `.prr` models.

## Proprietary / Undecoded Surface
- Car geometry: `himodel.dat`
- World geometry and track textures: `models.dat`, `textures.dat`
- Static object models: `.prr`, `.hed`, `tpage*.dat`
- Track and AI lane data: `.trk`
- Traffic / route tables: `.bus`
- Miscellaneous configuration: `Config.td5`, `CupData.td5`

## Conservative Parser Shapes Implemented
- `config.nfo`: line-oriented Latin-1 text metadata for car archive identity / display fields.
- `levelinf.dat`: fixed 100-byte structure exposed as 25 little-endian `uint32` values.
- `checkpt.num`: fixed 96-byte structure exposed as 24 little-endian `uint32` values in 6 groups of 4.
- `traffic.bus` / `trafficb.bus`: packed 32-bit records, surfaced as `u32`, `u16_pair`, and `u8_quad`.
- `carparam.dat`: fixed 268-byte structure exposed as 67 little-endian `int32` values; the first 64 form 16 quartets and the last 3 are a trailer.
- `.trk`: exposed as raw bytes plus a conservative byte-triplet view because shipped lane files are consistently divisible by 3.

## Working Rule
Do not assign gameplay semantics to a field until it has been correlated either:
- across multiple archives with consistent patterns, or
- in-game via controlled edits / runtime observation.
