"""
patch_sprite_loop_flag.py

Fixes menu sprite centering at 2560x1440.

Problem:
  FUN_00425540's first sprite loop (VA 0x425582) calls FUN_004251a0 with
  flag=0x10 (DDBLTFAST_WAIT). dgVoodoo2 ignores destX/destY for 0x10 calls,
  so those sprites land at (0,0) regardless of the +960/+480 offset the
  BltFast trampoline (0x45C229) applies.

  The second sprite loop (VA 0x425621) already uses flag=0x11
  (DDBLTFAST_WAIT|DDBLTFAST_SRCCOLORKEY) and dgVoodoo2 respects
  destX/destY for that flag — those sprites are correctly centered.

Fix:
  Change PUSH 0x10 (6A 10) at VA 0x42557c to PUSH 0x11 (6A 11).
  File offset: 0x2557c; operand byte: 0x2557d.
  One byte change: 0x10 -> 0x11.

  The composite surface (DAT_00496260) has no color key set, so the
  SRCCOLORKEY flag is benign — dgVoodoo2 will draw all pixels unchanged
  while now correctly honouring the offset coordinates.
"""

import struct

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

PUSH_VA    = 0x0042557c   # PUSH 0x10  (2 bytes: 6A 10)
FO         = lambda va: va - IMAGE_BASE


def apply():
    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    off = FO(PUSH_VA)
    current = exe[off:off+2]
    print(f"Bytes at VA 0x{PUSH_VA:08X} (file 0x{off:06X}): {current.hex(' ')}")

    if current == bytes([0x6A, 0x11]):
        print("Already patched (0x11) — nothing to do.")
        return
    if current != bytes([0x6A, 0x10]):
        print(f"WARNING: expected 6A 10, got {current.hex(' ')} — aborting.")
        return

    exe[off + 1] = 0x11
    with open(EXE, 'wb') as f:
        f.write(exe)
    print("Patched: PUSH 0x10 -> PUSH 0x11 at VA 0x{:08X} (file 0x{:06X})".format(PUSH_VA, off))
    print("Done.")


def verify():
    with open(EXE, 'rb') as f:
        exe = f.read()
    off = FO(PUSH_VA)
    current = exe[off:off+2]
    status = "OK (patched)" if current == bytes([0x6A, 0x11]) else \
             "ORIGINAL (0x10)" if current == bytes([0x6A, 0x10]) else "UNEXPECTED"
    print(f"VA 0x{PUSH_VA:08X} (file 0x{off:06X}): {current.hex(' ')}  [{status}]")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
