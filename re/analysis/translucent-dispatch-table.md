# Translucent Dispatch Table (PTR_LAB_00473b9c)

> Address: `0x473b9c` | Size: 28 bytes (7 DWORD function pointers) | .rdata section

## Overview

This is a **function pointer dispatch table** used by the race 3D rendering pipeline to process
different primitive types. It is indexed by a 16-bit opcode stored in the first word of each
primitive command record.

## Table Contents

```
Offset  Address     Function                              Opcode
------  ----------  ------------------------------------  ------
+0x00   0x00431750  EmitTranslucentTriangleStrip           0
+0x04   0x00431750  EmitTranslucentTriangleStrip           1  (duplicate)
+0x08   0x004316F0  SubmitProjectedTrianglePrimitive       2
+0x0C   0x00431690  SubmitProjectedQuadPrimitive           3
+0x10   0x0043E3B0  InsertBillboardIntoDepthSortBuckets    4
+0x14   0x00431730  EmitTranslucentTriangleStripDirect     5
+0x18   0x004316D0  EmitTranslucentQuadDirect              6
```

## Hex Dump

```
00473b9c: 50 17 43 00  50 17 43 00  F0 16 43 00  90 16 43 00
00473bac: B0 E3 43 00  30 17 43 00  D0 16 43 00
```

Followed by unrelated data: float constants (`3F800000` = 1.0f, `42000000` = 32.0f, `3D000000`),
vertex buffer pointer, and the error string `"Failed DrawPrim\n%s"`.

## Dispatch Mechanism

All three callers use the same pattern:

```c
// param points to a primitive command record
// word[0] = opcode (dispatch index)
// word[1] = texture page ID
// word[2..] = primitive-specific data
(*(code *)(&PTR_LAB_00473b9c)[*param])(param);
```

The opcode at `param[0]` (ushort) indexes into the table. Each handler receives the full
command record pointer as its argument.

### Callers

| Address | Function | Context |
|---------|----------|---------|
| 0x431380 | `FlushQueuedTranslucentPrimitives` | Walks linked-list of queued translucent batches |
| 0x4314D8 | `RenderPreparedMeshResource` | Processes mesh command list sequentially |
| 0x4315BA | `SubmitImmediateTranslucentPrimitive` | Single-primitive immediate submission |

## Function Descriptions

### Opcode 0/1: EmitTranslucentTriangleStrip (0x431750)
**Not recognized as a function by Ghidra** (needs CreateFunction).

Disassembly at 0x431750:
```asm
PUSH ESI
XOR  EAX, EAX
PUSH EDI
MOV  EDI, [ESP+0xC]     ; param
MOV  AX, [EDI+2]        ; texture page
XOR  ESI, ESI
MOV  [DAT_0048DA04], EAX
MOV  ECX, [EDI+0xC]     ; vertex data pointer
MOV  [DAT_004AF268], ECX
MOV  SI, [EDI+8]         ; triangle count
...
```
Processes variable-count triangle strips. Sets vertex count = 3 per triangle, iterates
through `[EDI+8]` triangles, calling `ClipAndSubmitProjectedPolygon` for each.

### Opcode 2: SubmitProjectedTrianglePrimitive (0x4316F0)
Sets vertex count to 3, stores texture page from `[param+2]`, stores vertex pointer
from `[param+8]`, then jumps to `ClipAndSubmitProjectedPolygon` (0x4317F0).

### Opcode 3: SubmitProjectedQuadPrimitive (0x431690)
Same as opcode 2 but sets vertex count to 4.

### Opcode 4: InsertBillboardIntoDepthSortBuckets (0x43E3B0)
Complex handler for billboards. Reads triangle count from `[param+8]`, quad count from
`[param+0xA]`, vertex data from `[param+0xC]`. Inserts each primitive into the 4096-bucket
depth sort array at `DAT_004BF6C8` using inverse Z as key. Triangles use stride 0x84,
quads use stride 0xB0.

### Opcode 5: EmitTranslucentTriangleStripDirect (0x431730)
**Not recognized as a function by Ghidra** (needs CreateFunction).

Small wrapper: computes `param+8` as vertex pointer, then calls
`FlushProjectedPrimitiveBuckets` (0x43E2F0 + offset).

```asm
MOV  EAX, [ESP+4]
LEA  ECX, [EAX+8]
PUSH EAX
MOV  [DAT_004AF268], ECX
CALL 0x0043E4B0          ; (relative call target)
POP  ECX
RET
```

### Opcode 6: EmitTranslucentQuadDirect (0x4316D0)
**Not recognized as a function by Ghidra** (needs CreateFunction).

Same pattern as opcode 5 but calls a different target (quad variant):

```asm
MOV  EAX, [ESP+4]
LEA  ECX, [EAX+8]
PUSH EAX
MOV  [DAT_004AF268], ECX
CALL 0x0043E540          ; (relative call target)
POP  ECX
RET
```

## Primitive Command Record Format

```
+0x00: uint16  opcode          ; dispatch table index (0-6)
+0x02: uint16  texture_page    ; texture page ID for BindRaceTexturePage
+0x04: uint16  sort_key        ; depth sort value (used by QueueTranslucentPrimitiveBatch)
+0x06: (varies by opcode)
+0x08: uint16  tri_count       ; number of triangles (opcodes 0/1/4)
+0x0A: uint16  quad_count      ; number of quads (opcode 4)
+0x0C: void*   vertex_data     ; pointer to vertex array
```

## Pipeline Flow

1. During race rendering, primitive commands are built with opcodes 0-6
2. `QueueTranslucentPrimitiveBatch` (0x431460) inserts them into a linked-list sorted by
   `sort_key` (word at +0x02), max 510 batches
3. `FlushQueuedTranslucentPrimitives` (0x431340) walks the linked list, dispatching each
   through this table
4. Each handler populates shared vertex/index buffers (`DAT_004AFB14`, `DAT_004AF314`)
5. After dispatch, the accumulated D3D DrawPrimitive call is issued with:
   - Primitive type 4 (D3DPT_TRIANGLELIST)
   - FVF 0x1C4 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
   - DrawPrimitive via COM vtable offset 0x74

## Ghidra Cleanup Needed

Three code addresses are not recognized as functions:
- `0x431750` -- CreateFunction needed (EmitTranslucentTriangleStrip)
- `0x431730` -- CreateFunction needed (EmitTranslucentTriangleStripDirect)
- `0x4316D0` -- CreateFunction needed (EmitTranslucentQuadDirect)
