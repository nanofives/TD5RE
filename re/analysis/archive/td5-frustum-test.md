`0042D5B0` writes the frustum/camera basis into:

- `004AAFA0..004AAFC0`
- secondary matrix-like terms at `004AB070..004AB090`

Later consumers:
- `0042DCA0`
- `0042DE10`

This is upstream of the screen-space rectangles and projection scalars.

Tools:
- monitor:
  - `python .\tools\monitor_td5_frustum.py <pid>`
- patch:
  - `python .\tools\patch_td5_frustum.py <pid> --mode both --scale 1.3333333`

Current experiment:
- scale the first and third basis rows live during a race
- if the visible framing changes, this confirms the real widescreen lock is in the frustum basis
