# Custom PE Sections: IDCT_DAT and UVA_DATA

## Section Overview

| Property     | IDCT_DAT               | UVA_DATA               |
|-------------|------------------------|------------------------|
| Start       | `0x004d1000`           | `0x004d3000`           |
| End         | `0x004d2fff`           | `0x004d7fff`           |
| Size        | 8,192 bytes (0x2000)   | 20,480 bytes (0x5000)  |
| Permissions | Read / Write           | Read / Write           |
| Execute     | No                     | No                     |

Both sections are **zero-initialized on disk** and populated at runtime. They serve as scratch buffers and precomputed lookup tables for the game's proprietary video/image codec (the EA TGQ multimedia engine).

---

## IDCT_DAT (0x004d1000 -- 0x004d2fff)

### Purpose: IDCT Workspace and Constants

This section is the **working memory for the Inverse Discrete Cosine Transform** used during video frame decoding. It holds both fixed-point IDCT constants and intermediate computation results.

### Data Layout

| Offset   | Address      | Size    | Description |
|----------|-------------|---------|-------------|
| 0x000    | `004d1000`  | 4 bytes | `DAT_004d1000` -- Row stride (param_2 << 2), set per IDCT call |
| 0x004    | `004d1004`  | 4 bytes | `DAT_004d1004` -- Output pointer (current row/column target) |
| 0x008    | `004d1008`  | 8 bytes | `_DAT_004d1008` -- Intermediate accumulator (double-width) |
| 0x010    | `004d1010`  | 4 bytes | `DAT_004d1010` -- Intermediate accumulator |
| 0x014    | `004d1014`  | 4 bytes | `DAT_004d1014` -- Intermediate accumulator |
| 0x018    | `004d1018`  | 4 bytes | IDCT constant: `0x5a82799a` (fixed-point cos(pi/4) * sqrt(2)) |
| 0x01C    | `004d101c`  | 4 bytes | `_DAT_004d101c` -- IDCT twiddle factor (float: ~0.5411961) |
| 0x020    | `004d1020`  | 4 bytes | `_DAT_004d1020` -- IDCT twiddle factor (float: ~1.3065630) |
| 0x024    | `004d1024`  | 4 bytes | `_DAT_004d1024` -- IDCT twiddle factor (float: ~0.3826834) |
| 0x028    | `004d1028`  | 4 bytes | `_DAT_004d1028` -- Rounding bias (float, large value ~2^33) |
| 0x02C    | `004d102c`  | 0x20    | 8x int32 output buffer (IDCT column 0) |
| 0x04C    | ...         | ...     | Additional column output buffers |

The constants at `0x004d1018`--`0x004d1028` hold the fixed-point IDCT twiddle factors. The hex values `0x0a3f8bd4` (at +0x1c), `0x3fa73d75` (at +0x20), and `0x3ec3ef15` (at +0x24) decode to the standard IDCT cosine coefficients when interpreted as IEEE 754 floats.

### Accessing Functions

| Function | Address | Role |
|----------|---------|------|
| `FUN_00456d9e` | `0x00456d9e` | **IDCT driver** -- sets up stride, runs 8 column passes (`FUN_00456b31`) then 8 row passes (`FUN_00456c94`) |
| `FUN_00456b31` | `0x00456b31` | **IDCT column pass** -- 1D 8-point IDCT on a column, writes 8 outputs at stride-9 offsets |
| `FUN_00456c94` | `0x00456c94` | **IDCT row pass** -- 1D 8-point IDCT on a row, writes 8 contiguous outputs |
| `FUN_0045af50` | `0x0045af50` | **Macroblock decoder** -- calls IDCT for each of 6 blocks (4 luma + 2 chroma) per macroblock |

The IDCT is a standard 8x8 integer IDCT using the AAN (Arai-Agui-Nakajima) algorithm with fixed-point arithmetic. `FUN_00456d9e` performs column-first then row processing, which is the classic two-pass separable IDCT approach.

### Classification

**Runtime workspace** -- not a precomputed table. The constants region (0x18--0x28) is initialized once when the codec starts; the rest is scratch space reused per 8x8 block decode. The section is writable because every IDCT invocation overwrites it.

---

## UVA_DATA (0x004d3000 -- 0x004d7fff)

### Purpose: YUV-to-RGB Conversion LUTs and Video Decode Buffers

This section contains **three large color-space conversion lookup tables**, a dequantized coefficient buffer, bitstream state, and Huffman/VLC decode tables.

### Data Layout

| Offset    | Address        | Size        | Description |
|-----------|---------------|-------------|-------------|
| 0x0000    | `004d3000`    | 0x600 (1536)| **IDCT coefficient block buffer** -- 6 blocks x 64 int32 values (dequantized DCT coefficients for one macroblock; written by `FUN_00456f70` and `FUN_0045a84c`/`FUN_0045a98b`) |
| 0x0600    | `004d3600`    | 0x100 (256) | **Dequantized output buffer** -- 64 x int32, output of entropy decode + dequant (`FUN_00456f70`) |
| 0x0700    | --            | 0x220       | Miscellaneous codec state / padding |
| 0x0820    | `004d3820`    | 0x110       | **Quantization table** (64 int32 entries + state) |
| 0x0920    | `004d3920`    | 4 bytes     | Bitstream read pointer |
| 0x0924    | `004d3924`    | 4 bytes     | Bits remaining in current word |
| 0x0928    | `004d3928`    | 4 bytes     | Current bitstream shift register |
| 0x0930    | `004d3930`    | 0x200 (512) | **Y LUT** -- 128-entry int32 table: Y component to packed RGB |
| 0x0B30    | `004d3b30`    | 0x200 (512) | **Cr LUT** -- 128-entry int32 table: Cr (V) to packed R+G partial |
| 0x0D30    | `004d3d30`    | 0x200 (512) | **Cb LUT** -- 128-entry int32 table: Cb (U) to packed G+B partial |
| 0x0F30    | `004d3f30`    | varies       | Potential overflow / padding for Cb table |
| (0x2598+) | `004d75ac`    | 0x100 (256) | **Zigzag scan order table** -- 64 x int32, maps zigzag index to raster index (in .data, not UVA_DATA -- but referenced by UVA_DATA code) |
| (0x269C+) | `004d769c`    | ~0x300      | **VLC/Huffman decode tables** -- multiple sub-tables at `004d769c`, `004d76cc`, `004d77ac`, `004d77ec`, `004d782c`, `004d786c`, `004d78ac`, `004d792c`, `004d79ac` used by `FUN_004588e0` for variable-length code decoding |

### YUV-to-RGB LUT Generation

The three color LUTs are populated at runtime by two initialization functions:

- **`FUN_004538b0`** (`0x004538b0`) -- standard pixel format init
- **`FUN_00453520`** (`0x00453520`) -- parameterized pixel format init

Both iterate 128 entries (covering signed 7-bit range, representing quantized YUV values), computing packed RGB contributions using the standard YUV-to-RGB fixed-point coefficients:

| Constant (hex) | Decimal | Meaning |
|----------------|---------|---------|
| `0x166e9`      | +91,881 | 1.402 * 65536 (Cr to R) |
| `-0xb6cf`      | -46,799 | -0.714 * 65536 (Cr to G) |
| `-0x5816`      | -22,550 | -0.344 * 65536 (Cb to G) |
| `0x1c5a1`      | +116,129| 1.772 * 65536 (Cb to B) |

These are the ITU-R BT.601 YCbCr-to-RGB conversion factors in Q16.16 fixed-point.

### Color Reconstruction (FUN_004573f9 / FUN_00457684)

The final pixel assembly reads the three LUTs to combine Y, Cb, and Cr contributions:

```
pixel[i] = Y_LUT[Y_val >> 17 & 0x7F]
         + Cb_LUT[Cb_val >> 17 & 0x7F]
         + Cr_LUT[Cr_val >> 17 & 0x7F]
```

- **`FUN_004573f9`** (`0x004573f9`) -- writes 16 pixels as int32 (32bpp ARGB output)
- **`FUN_00457684`** (`0x00457684`) -- writes 16 pixels as int16 (16bpp RGB565 output)

Each function processes 8 pixel pairs per call and is invoked 16 times per macroblock (covering the 16x16 pixel macroblock).

### Accessing Functions Summary

| Function | Address | Role |
|----------|---------|------|
| `FUN_00453520` | `0x00453520` | YUV LUT init (parameterized pixel format) |
| `FUN_004538b0` | `0x004538b0` | YUV LUT init (standard pixel format) |
| `FUN_00456f70` | `0x00456f70` | Entropy decode + dequantize (VLC/RLE codec) |
| `FUN_0045a84c` | `0x0045a84c` | IDCT column pass (video codec variant) |
| `FUN_0045a98b` | `0x0045a98b` | IDCT row pass (video codec variant) |
| `FUN_004573f9` | `0x004573f9` | YUV-to-RGB reconstruct (32bpp) |
| `FUN_00457684` | `0x00457684` | YUV-to-RGB reconstruct (16bpp) |
| `FUN_004588e0` | `0x004588e0` | VLC Huffman decode (MPEG-style) |
| `FUN_0045791f` | `0x0045791f` | Full-quality macroblock decode + 32bpp output |
| `FUN_004580fe` | `0x004580fe` | Full-quality macroblock decode + 16bpp output |
| `FUN_00459877` | `0x00459877` | Reduced-quality macroblock decode (motion-compensated) |

### Classification

**Runtime-populated LUTs + codec workspace**. The three color conversion tables (Y, Cr, Cb) are computed once per video stream initialization based on the target pixel format. The coefficient buffers and bitstream state are scratch space rewritten for every macroblock.

---

## Architecture Summary

Both custom PE sections exist to give the EA TGQ video codec its own dedicated, page-aligned memory regions separate from the general `.data` section. This was likely done for:

1. **Cache locality** -- grouping all codec hot data together improves L1/L2 cache utilization during video playback, which was critical on late-1990s CPUs.
2. **Separation of concerns** -- keeping codec state out of `.data` prevents accidental corruption from other game systems and makes the memory layout deterministic.
3. **Page alignment** -- PE sections are page-aligned (4KB), ensuring the codec buffers start on known boundaries which may help with prefetch and SIMD alignment.

The naming convention is descriptive:
- **IDCT_DAT** = Inverse Discrete Cosine Transform Data
- **UVA_DATA** = UV (chrominance) and A (possibly "all" or "assembly") Data, covering the full YUV-to-RGB pipeline

### Total Custom Section Memory

| Section   | Size (bytes) | Size (KB) |
|-----------|-------------|-----------|
| IDCT_DAT  | 8,192       | 8 KB      |
| UVA_DATA  | 20,480      | 20 KB     |
| **Total** | **28,672**  | **28 KB** |
