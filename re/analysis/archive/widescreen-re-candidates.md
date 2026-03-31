# Widescreen RE Candidates

Confirmed dead ends:
- wrapper-level display-mode coercion
- viewport override at `IDirect3DViewport3::SetViewport2`
- identity projection override at `IDirect3DDevice3::SetTransform(PROJECTION)`
- isolated patch of `0x10009525` (`0.75f -> 0.5625f`)

Named M2DX paths now identified:
- `DXD3D::Create` at `0x100010B0`
  - calls internal mode/setup dispatcher `0x100034D0`
- `DXD3D::FullScreen` at `0x10002170`
- `DXD3D::SetRenderState` at `0x10001770`
- `DXD3D::BeginScene` at `0x100015A0`
- `DXDraw::Create` at `0x100062A0`
- display mode change helper at `0x10008ED0`
- surface allocation path at `0x10004F30`
- internal viewport init callback at `0x100033F0`
- internal viewport cleanup callback at `0x10003490`

Renderer facts proven:
- higher render dimensions are tracked in globals like `10061B8C/10061B90/10061B94`
- the Direct3D3 path still issues a legacy `640x480` viewport setup
- widening the viewport and the identity D3D projection does not change the visible camera
- therefore the visible 4:3 frustum is being baked deeper in engine-side math before or outside the D3D viewport/projection calls we intercepted

Most likely next patch targets:
- code using `10061B8C/10061B90/10061B94` for camera/frustum math
- internal routines after mode change, especially around:
  - `0x10003AB7` -> `0x10004F30`
  - `0x10004E1A` -> `0x10001770`
  - unresolved math-heavy regions around `0x1000E5FE` / `0x1000E75B`

Working conclusion:
- true widescreen remains feasible in principle, but it is now clearly a deeper `M2DX.dll` camera/frustum reverse-engineering task, not a wrapper configuration task.
