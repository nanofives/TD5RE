The active view mode entry lives at `0x004AAE10 + mode*0x40`.

Observed layout:
- float-backed primary path:
  - `+0x00` rect0 x min
  - `+0x08` center0 x
  - `+0x10` rect0 x max
  - `+0x18` rect0 y min
  - `+0x20` center0 y
  - `+0x28` rect0 y max
- int-backed secondary path:
  - `+0x04` rect1 x min
  - `+0x0C` center1 x
  - `+0x14` rect1 x max
  - `+0x1C` rect1 y min
  - `+0x24` center1 y
  - `+0x2C` rect1 y max

Use cases:
- Inspect active entry:
  - `python .\tools\monitor_td5_view_table.py <pid>`
- Patch active entry live:
  - `python .\tools\patch_td5_view_table.py <pid> --mode both --virtual-width 2560`

Rationale:
- `0042BB15` calls `0043E640` with rect0 from the active entry.
- `0042BB22` calls `0043E8E0` with center0 from the active entry.
- `0042C027` calls `0043E640` with rect1 from the active entry.
- `0042C034` calls `0043E8E0` with center1 from the active entry.

This is upstream of the derived globals that were previously patched directly.

Important:
- `primary` is the safer next test because it is float-backed and aligns with the visible center values.
- `secondary` can collapse the image if patched alone.
