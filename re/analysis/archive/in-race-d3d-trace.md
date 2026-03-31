# In-Race D3D Trace

Enable trace:
- `tools\enable-d3d-trace.bat`

Disable trace:
- `tools\disable-d3d-trace.bat`

Run goal:
- reach a race
- drive for 10 to 20 seconds
- exit the game normally if possible

Capture focus in `TD5_d3d.exe.ddraw-proxy.log`:
- `IDirect3DViewport3::SetViewport2`
- `IDirect3DDevice3::SetTransform`
- `IDirect3DDevice3::MultiplyTransform`
- `IDirect3DDevice3::SetCurrentViewport`

Current wrapper mode:
- passive logging only
- no fake resolution injection
- no widescreen coercion
