`0042D5B0` also writes a secondary matrix-like block at:

- `004AB070..004AB090`

This block is produced before:
- `0042DA10`
- `0042D410`
- later clip/projection consumers

Observed example values:
- `ab070 ~= 0.3333333`
- `ab080 ~= -0.3330229`
- `ab090 ~= -0.3330229`

Those look more like scale terms than camera position terms.

Use:
- `python .\tools\patch_td5_frustum_matrix.py <pid> --mode both --scale 1.3333333`
