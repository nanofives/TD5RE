# I18N handoff — round 1011 C3 (OSM lit / maxspeed / street names)

New user-visible strings added by this round, with the es_AR lines to paste
into `re/assets/frontend/lang/es_AR.txt`.

The catalog key **is** the exact English string (`td5_i18n.c`'s `td5_tr`), the
file is UTF-8, and `#` at column 0 is a comment. A translation whose `%`-specs
do not match the key is DROPPED at load (`i18n_fmt_specs_match`), so the `%s`
must survive.

## Lines to add

```
ON %s=EN %s
```

## Context for the translator

| Key | Where it shows | Example | Budget |
|---|---|---|---|
| `ON %s` | Race HUD, bottom centre of each pane. Names the real street the car is currently driving on, from the OSM route. | `ON Avenida 53` → `EN Avenida 53` | The street name itself can reach 43 bytes on the La Plata cache; the prefix should stay short. |

**The street name is never translated.** It is a proper noun taken verbatim
from the map (`Avenida 53`, `Diagonal 73`, `Plaza Miguel de Azcuénaga`) and is
substituted into `%s` unchanged. Only the preposition is translated.

## Accents: this round changed how they render

Every other text path in the port draws **one byte per glyph**, which is
correct for this catalog because `i18n_decode_field` converts it from UTF-8 to
Latin-1 once at load. An OSM street name is different: it is still **UTF-8 at
runtime**, so `Azcuénaga` holds the two bytes `C3 A9` for its `é`.

Drawing those two bytes as two Latin-1 glyphs produces `AzcuÃ©naga` — which is
exactly the mojibake this round was asked to chase. The street line therefore
decodes real codepoints with `td5_utf8_next()` and draws them with
`td5_hudfont_get_exact()`, which — unlike `td5_hudfont_get()` — does **not**
fold the acute accent away.

That fold is deliberate and stays in place for all menu text (`td5_font.c`,
`fold_accent_cp`, 2026-07-22: the menu face watermarks its accented glyphs, and
dropping the accent reads fine in a stylised menu). It is the wrong behaviour
for a real-world proper noun: a street blade reading `Azcuenaga` where the map
says `Azcuénaga` is misspelt the way a real road sign would be.

So, for a translator or anyone re-testing this: on the HUD street line,
**accented characters are expected to render with their accents**, and seeing
`Ã©` there would be a regression, not a font limitation.

## Not added

No new string was needed for the speed limit or the lamps: the limit is a
number drawn on a sign face, and lamps carry no text.
