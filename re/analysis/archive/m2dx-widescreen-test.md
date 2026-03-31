# M2DX Widescreen Test

Apply:
- `tools\apply-m2dx-widescreen-test.bat`

Restore:
- `tools\restore-m2dx-original.bat`

Test focus:
- menu aspect ratio
- whether race draws correctly
- whether cars/body geometry render
- HUD placement
- camera/FOV feel
- whether the game freezes while audio continues

Current patch:
- file: `workspace\experiments\m2dx-widescreen\M2DX.aspect-16x9.dll`
- offset: `0x9525`
- float: `0.75 -> 0.5625`
- VA: `0x10009525`
