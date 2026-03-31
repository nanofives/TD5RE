The active world transform used by projection routines is pointed to by `004BF6B8`.

Observed live structure:
- row0 at `ptr + 0x00..0x08`
- row1 at `ptr + 0x0C..0x14`
- row2 at `ptr + 0x18..0x20`
- translation at `ptr + 0x24..0x2C`

The matrix rows match:
- `004AB040..004AB048`
- `004AB04C..004AB054`
- `004AB058..004AB060`

That path feeds:
- `0042E2E0`
- `0042E370`
- `0042E4F0`
- and the broader world projection code

Use:
- `python .\tools\patch_td5_active_world_matrix.py <pid> --mode row0 --scale 1.3333333`
