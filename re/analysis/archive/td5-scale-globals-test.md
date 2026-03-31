`0042D410` scales the copied camera/projection state using:

- `00467364` as a float divider
- `00467368` as an integer scale source

Observed live values:
- `00467364 = 3.0`
- `00467368 = 4096`

These feed the repeated `0.3333333` row values seen in active matrices.

Use:
- `python .\tools\patch_td5_scale_globals.py <pid> --mode aspect --aspect-div 2.25`
