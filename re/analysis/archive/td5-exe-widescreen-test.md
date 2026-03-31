## TD5 EXE Widescreen Test

This test patches `TD5_d3d.exe` instead of `M2DX.dll`.

- Target VA: `0x45D78C`
- File offset: `0x5D78C`
- Original float: `0.5625`
- Test float: `0.75`

Reason:

- Live capture showed the EXE projection code already uses the real renderer size:
  - width `1920`
  - height `1440`
  - center `960,720`
- The remaining 4:3 behavior comes from projection depth being derived as:
  - `width * 0.5625`
- This test changes that to:
  - `width * 0.75`

Files:

- Patched copy: `workspace/experiments/td5-widescreen/TD5_d3d.aspect-test.exe`
- Apply helper: `tools/apply-td5-exe-widescreen-test.bat`
- Restore helper: `tools/restore-td5-exe-original.bat`

Expected outcomes to check:

- menu still works
- race still renders
- horizontal view looks wider
- HUD remains aligned
- no frozen rendering
