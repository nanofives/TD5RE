# Viewport And Projection Override Test

Enable:
- `tools\enable-viewport-override-test.bat`

Disable:
- `tools\disable-d3d-trace.bat`

Behavior:
- passive logging remains enabled
- `SetViewport2(640x480)` is rewritten to the live TD5 internal render dimensions
- identity `SetTransform(PROJECTION)` is rewritten for a `16:9` target aspect by reducing the X scale

Goal:
- determine whether TD5's race camera begins to show wider horizontal FOV without breaking geometry or HUD
