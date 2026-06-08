# Frontend Font Atlas Resolution

RESEARCH ONLY. Resolves which TGA is which font, why the prior `smalltext.png`
swap garbled labels, and the correct port loader params per font.

Source archive: `original/Front End/frontend.zip`
All font TGAs are **8-bit colormapped (image-type 1), bottom-origin
(flags=0x08), 256-entry 24-bit BGR palette** (idlen=0). Decode = read palette,
index the pixels, **flip vertically** (bottom-origin), map index→BGR.

The pre-extracted PNGs in `re/assets/frontend/` were produced with exactly that
decode: dimensions and RGB are **byte-identical** to the TGA (verified
`np.array_equal` on smalltext). So the extraction is faithful in arrangement,
size and color — the problem is purely the **background/glyph color convention**
each font uses vs. what the port's colorkey blitter expects.

---

## Per-font findings

### SmallText  (button labels, table/list text)
- TGA: `smalltext.tga` — 252×132, 8bpp colormapped, bottom-origin.
- Cell 12×12, **21 cols × 11 rows**, first char 0x20 (space).
  Blit index: `idx = char-0x20; srcX=(idx%21)*12; srcY=(idx/21)*12`.
- **Background = palette idx 255 = WHITE (255,255,255).**
- **Glyphs = DARK** (grayscale ramp; idx 117 = (12,12,12) near-black up the ramp).
  Verified pure grayscale (every used palette entry has R==G==B, range 0..255):
  this is an **inverted intensity/alpha map** (white bg = empty, dark = glyph).
- Layout spot-check (rendered cells, NEAREST-upscaled): 'S' idx51→(108,24),
  '0' idx16→(192,0), 'Z' idx58→(192,24) all visually land on the right glyph.
  21-col/12px/first-0x20 layout **VERIFIED**.
- Repo `re/assets/frontend/smalltext.png` (252×132, RGBA, alpha all 255):
  arrangement CORRECT, but it is **dark-on-white** (matches TGA). NOT directly
  usable with a black colorkey blit (see Prior-failure diagnosis).
- Colorkey: WHITE is the transparent color in the source. There is no
  `TD5_COLORKEY_WHITE`; the practical fix is to **invert** the atlas to
  white-on-black and key black.

### SmallTextb  (bold / highlighted-row variant)
- TGA: `smalltextb.tga` — 252×132, same geometry/layout as SmallText.
- **Background = WHITE (255,255,255). Glyphs = BLUE (10,12,247).**
- Repo `re/assets/frontend/smalltextb.png` (252×132): arrangement CORRECT,
  blue-on-white. Same colorkey problem as SmallText (white bg stays opaque).

### MenuFont  (big title banners)
- TGA: `mainfont.tga` — 252×1152, 8bpp colormapped, bottom-origin.
- Cell 36×36, **7 cols × 32 rows**, first char 0x20.
  Blit: `idx=char-0x20; srcX=(idx%7)*36; srcY=(idx/7)*36`.
- **Background = palette idx 254 = pure RED (255,0,0). Glyphs = YELLOW (227,215,8)**,
  italic. Verified visually (top 8 rows render the expected ! " # $ % & ... 0-9 A-...).
- NO repo PNG existed (`mainfont.png` absent). Freshly exported (see below).
- Colorkey: RED. The port already has `TD5_COLORKEY_RED` (keys R>=248,G<8,B<8 →
  alpha 0) — it keys 89.9% of mainfont (the background) and leaves the yellow
  glyphs. Works directly. (Note: faithful text color is YELLOW, not white.)

### BodyText  (the font the port currently uses for ALL frontend text, scaled)
- TGA: `BodyText.tga` — 240×552, 8bpp colormapped, bottom-origin.
- Cell 24×24, **10 cols × 23 rows**, first char 0x20.
  Blit: `idx=char-0x20; srcX=(idx%10)*24; srcY=(idx/10)*24`.
- **Background = palette idx 255 = BLACK (0,0,0). Glyphs = WHITE (253,252,253).**
- Repo `re/assets/frontend/BodyText.png` (240×552): CORRECT, white-on-black.
- This is the ONE font whose convention matches the port: white glyphs on a
  black background → `TD5_COLORKEY_BLACK` is correct, which is why BodyText
  renders fine today. (`OldBodyText.tga` is an identical-geometry older copy.)

### Not fonts (for the record)
- `MainMenuText.TGA` 248×20 — a single pre-rendered string strip (yellow-on-black),
  not a glyph atlas.
- `smalfont.TGA` 252×700 — black glyphs on RED bg, 7-col(@36)/9-col(@28); an
  alternate small font, not the one `DrawFrontendFontStringPrimary` uses.
- `Small TD5 Wht.TGA` 128×80, `Mono.tga` 64×32×24bpp (uncompressed RGB) — logos/misc.

---

## Prior-failure diagnosis  (why the naive smalltext.png swap garbled labels)

The port loads via `td5_asset_load_png_to_buffer(path, TD5_COLORKEY_BLACK, ...)`
then blits 12×12 cells. `TD5_COLORKEY_BLACK` (td5_asset.c apply_colorkey) sets
alpha=0 where **R<8 && G<8 && B<8** — i.e. it makes BLACK transparent.

`smalltext.png` is **dark glyphs (≈(12,12,12)) on a WHITE (255,255,255)
background**. Under a black colorkey:
- the WHITE background is NOT keyed → stays fully opaque (white block), and
- the near-black GLYPH pixels ARE keyed → punched to alpha 0 (holes).

Net result: solid white cells with the letters cut out as transparent holes —
i.e. inverted, garbled labels. The layout/cell math was fine; the bug is that
SmallText's background is white, the exact opposite of what `COLORKEY_BLACK`
expects. (BodyText works precisely because its background really is black.)

---

## Recommendation (port loader params per font)

| Font | Atlas asset | Dims | Cell | Cols | First | Glyph layout | Colorkey | Notes |
|------|-------------|------|------|------|-------|--------------|----------|-------|
| SmallText | `smalltext_white_on_black.png` (re-extracted, see below) | 252×132 | 12×12 | 21 | 0x20 | `idx%21,idx/21` | `TD5_COLORKEY_BLACK` | inverted from grayscale ramp → white-on-black |
| SmallTextb | `smalltextb_white_on_black.png` (re-extracted) | 252×132 | 12×12 | 21 | 0x20 | `idx%21,idx/21` | `TD5_COLORKEY_BLACK` | inverted (blue→white) on black |
| MenuFont | `mainfont.png` (re-extracted, fresh) | 252×1152 | 36×36 | 7 | 0x20 | `idx%7,idx/7` | `TD5_COLORKEY_RED` | yellow glyphs; key the red bg directly |
| BodyText | `BodyText.png` (existing repo PNG, OK) | 240×552 | 24×24 | 10 | 0x20 | `idx%10,idx/10` | `TD5_COLORKEY_BLACK` | already correct / in use |

### Re-extracted assets written to `re/assets/frontend/`
- `smalltext_white_on_black.png` — SmallText, luminance inverted (255-gray) so
  white bg→black, dark glyph→white; colorkey-black ready. 252×132. **Verified
  visually: clean white glyphs on black, layout intact.**
- `smalltextb_white_on_black.png` — SmallTextb, alpha mask from
  (255 - min(R,G)) so blue glyph→white, white bg→black. 252×132.
- `mainfont.png` — MenuFont straight TGA→PNG (yellow-on-red), no repo PNG
  previously existed. 252×1152. Use with `TD5_COLORKEY_RED`.

(The existing `smalltext.png` / `smalltextb.png` are kept; they are faithful
dark-on-white copies but should NOT be fed to a black-colorkey blit.)

### Alternative to inversion (closer to original semantics)
SmallText is a pure grayscale **intensity** map (white=empty). The original
treats the value as coverage/alpha rather than RGB. A future loader could add a
`TD5_COLORKEY_WHITE_AS_ALPHA` mode: `alpha = 255 - luminance`, keep glyph color
configurable (white for SmallText, blue/highlight for SmallTextb), avoiding the
need to bake separate inverted PNGs. The inverted PNGs above are the
zero-new-code option that works with the existing `TD5_COLORKEY_BLACK` path.

---

## RE / source corroboration
- `td5_frontend.c:1418` — `[CONFIRMED @ 0x00424110] DrawFrontendFontStringPrimary:
  12×12 glyph atlas, 21 columns. col=(c-0x20)%21*12, row=(c-0x20)/21*12` →
  matches `smalltext.tga` geometry exactly.
- `td5_frontend.c:559-566` — port's current FONT_COLS=10/FONT_CELL=24/240×552
  = BodyText.tga, the scaled-body-text font in use today.
- `td5_asset.c` apply_colorkey: BLACK keys R<8&G<8&B<8; RED keys R>=248&G<8&B<8.
