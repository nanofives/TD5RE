## TD5 Horizontal Projection Test

This test does not patch any file on disk.

It attaches to the running `TD5_d3d.exe` process and repeatedly overwrites the EXE's live horizontal projection terms:

- `004AB0B0`
- `004AB0B8`

It leaves the vertical terms unchanged:

- `004AB0A4`
- `004AB0A8`

Current test target:

- real render size: `1920x1440`
- virtual width used for horizontal projection: `2560`

Command:

- `tools\\run-td5-horizontal-projection-test.bat`

Expected effect:

- if these terms control horizontal framing, the race view should become wider without changing the renderer size
- if there is no visible change, the remaining widescreen control sits further downstream than these scale globals
