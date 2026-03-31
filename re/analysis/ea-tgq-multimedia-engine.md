# EA TGQ/TGV Multimedia Streaming Engine Analysis

> Address range: 0x451560 -- 0x456250 (streaming core) + IDCT_DAT section (IDCT/colorspace)
> ~90 functions total, largest unmapped subsystem in TD5_d3d.exe
> Codec: **EA TGQ** (JPEG-IDCT video + EA ADPCM audio)
> File: `Movie\intro.tgq` (only known caller: `PlayIntroMovie` at 0x43C440)

---

## 1. Architecture Overview

The multimedia engine is a self-contained AVI-like streaming library developed by
Electronic Arts. It plays `.tgq` files (EA's proprietary multimedia container) with:

- **Video**: JPEG-style IDCT-based codec with 8x8 and 16x16 block modes, YCbCr 4:2:0
  color space, quality-adjustable quantization, and inter-frame (motion-compensated)
  support via frame types 5/6/7.
- **Audio**: Three codecs -- raw PCM (type 0), EA ADPCM 4-bit (type 10 at 0x4564F3),
  and EA IMA-like (type 7 at 0x455AD0). Default: 22050 Hz, 16-bit, stereo.
- **Threading**: Two worker threads -- a streaming/decode thread and a DirectSound
  audio feeder thread, synchronized via InterlockedExchange state machine.

### Call Graph (simplified)

```
PlayIntroMovie (0x43C440)
  +-> OpenAndStartMediaPlayback (0x452E20)
  |     +-> OpenMultimediaStream (0x451990)   -- alloc 0x668-byte context
  |     |     +-> OpenWavStream (0x451A10)    -- mmioOpenA, parse RIFF
  |     |     +-> InitStreamDirectSound (0x456110) -- DirectSoundCreate
  |     |     +-> InitializeVideoDecoder (0x4539E0) -- DDraw surfaces, LUTs
  |     +-> LaunchStreamPlaybackThread (0x452910)
  |           +-> StreamThreadEntryPoint (0x452990)
  |                 +-> RunStreamingPlaybackLoop (0x4529D0)
  |                 |     +-> ProcessStreamChunk (0x452CC0)
  |                 |     |     +-> ReadAndDispatchChunk (0x451D70) -- FourCC switch
  |                 |     +-> DecodeVideoFrame (0x4542C0)
  |                 |     +-> SwapDoubleBuffer (0x4534A0)
  |                 |     +-> PresentDecodedFrame (0x452F20) -- Blt to DDraw
  |                 +-> CloseMultimediaStream (0x452830)
  +-> StartDecoderThread (0x456080) -- audio feeder
        +-> AudioDecoderThreadFunc (0x455D60)
              +-> GetAudioPlayPosition (0x455FF0)
              +-> WriteAudioToDirectSound (0x455DE0)
```

---

## 2. Stream Context Struct (0x668 bytes)

Allocated by `OpenMultimediaStream` (0x451990) via `calloc(1, 0x668)`.
The first DWORD (`ctx[0]`) points to an **outer config struct** (the `param_3` argument).

### Outer Config Struct (pointed to by ctx[0])

| Offset | Type   | Description |
|--------|--------|-------------|
| 0x00   | ptr    | Back-pointer to stream context (set during open) |
| 0x04   | uint32 | **Flags bitfield** (see below) |
| 0x08   | uint32 | Surface capability flags |
| 0x0C   | ptr    | IDirectDraw* (for QueryInterface) |
| 0x10   | ptr    | IDirectDrawSurface* (primary) |
| 0x14   | ptr    | IDirectDrawSurface* (back buffer A) |
| 0x18   | ptr    | IDirectDrawSurface* (back buffer B, overlay) |
| 0x1C   | ptr    | IDirectDrawSurface* (video surface A) |
| 0x20   | ptr    | IDirectDrawSurface* (video surface B, double-buffer) |
| 0x24   | ptr    | IDirectDrawPalette* |
| 0x28   | ptr    | Palette data buffer (1024 bytes, for 8-bit mode) |
| 0x2C   | int    | Pixel format: R mask bit count |
| 0x30   | int    | Pixel format: G mask bit count |
| 0x34   | int    | Pixel format: B mask bit count |
| 0x38   | int    | Pixel format: R shift |
| 0x3C   | int    | Pixel format: G shift |
| 0x40   | int    | Pixel format: B shift |
| 0x44   | uint32 | Combined RGB mask |
| 0x48   | int    | Bits per pixel (8, 15, 16, or 32) |
| 0x4C   | ptr    | Off-screen buffer pointer A (for flag 0x800 mode) |
| 0x50   | ptr    | Off-screen buffer pointer B |
| 0x54   | ptr    | IDirectSound* |
| 0x58   | ptr    | IDirectSoundBuffer* |
| 0x5C   | ptr    | IDirectSoundBuffer* (streaming) |
| 0x60   | int    | Video height (from frame header) |
| 0x64   | int    | Video width (from frame header) |
| 0x68   | int    | X offset |
| 0x6C   | int    | Y offset |
| 0x70   | int    | Override display width |
| 0x74   | int    | Override display height |
| 0x78   | int    | Brightness (0-200, default 100) |
| 0x7C   | int    | Audio subchunk repeat count (looping) |
| 0x80   | int    | Audio header count |
| 0x84   | int    | Has audio flag (0/1) |
| 0x88   | int    | Video-only flag |
| 0x8C   | int    | DirectSound volume |
| 0x90   | int    | Total playback time (ms) |
| 0x94   | int    | Frame counter (InterlockedIncrement) |
| 0x98   | int    | A/V sync drift |
| 0x9C   | int    | **Playback state** (InterlockedExchange) |
| 0xA0   | HWND   | Window handle (for DirectSound cooperative level) |
| 0xA4   | HMMIO  | mmio file handle |
| 0xA8   | int    | Thread priority |
| 0xAC   | int    | (reserved) |
| 0xB0   | ptr    | User callback function pointer |
| 0xB4+  |        | Palette RGB entries (256 x 3 bytes, offset 0xB4-0x3B3) |
| 0x3B4+ |        | Converted palette (256 x 4 bytes, for pixel format) |
| 0x3D8  | int    | Auto-close flag (set by OpenAndStartMediaPlayback) |

### Playback State Machine (config+0x9C)

| Value | Meaning |
|-------|---------|
| 0     | Initial / not started |
| 1     | Starting up |
| 2     | Running (normal decode loop) |
| 3     | Paused (Sleep(20) spin) |
| 4     | Stop requested |
| 5     | Stopped (non-threaded) |
| 6     | Released / freed |

### Flags Bitfield (config+0x04)

| Bit(s)     | Meaning |
|------------|---------|
| 0x00000001 | X offset enabled |
| 0x00000002 | Y offset enabled |
| 0x00000004 | Center horizontally |
| 0x00000008 | Center vertically |
| 0x00000010 | Interlaced (top field) |
| 0x00000020 | Interlaced (bottom field) |
| 0x00000040 | 32-bit pixel mode |
| 0x00000080 | Override display width |
| 0x00000100 | Override display height |
| 0x00000200 | Query surface format |
| 0x00000400 | Use back buffer A |
| 0x00000800 | Off-screen buffer mode (no DDraw surface lock) |
| 0x00001000 | Flip after frame |
| 0x00002000 | Set DirectSound volume |
| 0x00004000 | Custom brightness |
| 0x00008000 | Looping playback |
| 0x00010000 | Threaded playback |
| 0x00020000 | (Thread priority flag, paired with 0x10000) |
| 0x00040000 | User callback enabled |
| 0x00080000 | External mmio handle (don't open/close) |
| 0x00100000 | Custom brightness set |
| 0x00200000 | (reserved) |
| 0x00400000 | Custom DirectSound volume |
| 0x00800000 | Audio enabled (DirectSound) |
| 0x01000000 | Use DDraw for video |
| 0x02000000 | Restore surfaces on lost |
| 0x04000000 | External palette |
| 0x08000000 | Use overlay surface |
| 0x10000000 | (additional feature flags...) |

### Stream Context (0x668 bytes, at `ctx` pointer)

All offsets as int32 indices (multiply by 4 for byte offset).

| Index  | Byte Off | Description |
|--------|----------|-------------|
| 0x00   | 0x000    | Pointer to outer config struct |
| 0x01   | 0x004    | Display surface width |
| 0x02   | 0x008    | Display surface height |
| 0x03   | 0x00C    | Blit dest width |
| 0x04   | 0x010    | Blit dest height |
| 0x05   | 0x014    | Blit X origin |
| 0x06   | 0x018    | Blit Y origin |
| 0x07   | 0x01C    | **Video read buffer** (1,200,000 bytes) |
| 0x08   | 0x020    | **Audio read buffer** (300,000 bytes) |
| 0x09   | 0x024    | Video write cursor |
| 0x0A   | 0x028    | Audio write cursor |
| 0x0B   | 0x02C    | Error counter (overlay Blt) |
| 0x0C   | 0x030    | Error counter (overlay surface) |
| 0x0D   | 0x034    | Error counter (back-B Blt) |
| 0x0E   | 0x038    | Error counter (video-A Blt) |
| 0x0F   | 0x03C    | Video decoder initialized flag |
| 0x10   | 0x040    | Motion compensation: horizontal flag |
| 0x11   | 0x044    | Motion compensation: vertical flag |
| 0x12   | 0x048    | Quality parameter (0-100) |
| 0x13   | 0x04C    | **Surface mode** (1=primary, 2=offscreen, 4=overlay) |
| 0x14   | 0x050    | Offscreen surface valid flag |
| 0x15   | 0x054    | Overlay active flag |
| 0x16   | 0x058    | Audio: channel count (1 or 2) |
| 0x17   | 0x05C    | Audio: sample rate (e.g. 22050) |
| 0x18   | 0x060    | Audio: bits per sample (16) |
| 0x19   | 0x064    | Audio: codec type (0=PCM, 7=EA IMA, 10=EA ADPCM) |
| 0x1A   | 0x068    | **Audio ring buffer** [24 slots, 4 ints each] start |
|  ...   | ...      | Slots 0x1A..0x79 (24 slots x 4 dwords = 96 dwords) |
| 0x7A   | 0x1E8    | **Video ring buffer** [24 slots, 4 ints each] start |
|  ...   | ...      | Slots 0x7A..0xD9 (24 slots x 4 dwords = 96 dwords) |

Ring buffer slot layout (4 dwords per slot):
- [0] = data pointer (into video/audio read buffer)
- [1] = data size
- [2] = sequence number
- [3] = chunk type

| Index  | Byte Off | Description |
|--------|----------|-------------|
| 0xDA   | 0x368    | Audio initialized flag |
| 0xDB   | 0x36C    | DirectSound buffer size |
| 0xDC   | 0x370    | DirectSound write cursor |
| 0xDD   | 0x374    | Audio consumer index |
| 0xDE   | 0x378    | Audio producer index |
| 0xDF   | 0x37C    | Video consumer index |
| 0xE0   | 0x380    | Video producer index |
| 0xE1   | 0x384    | Video pending count |
| 0xE2   | 0x388    | Audio pending count |
| 0xE3   | 0x38C    | Video sequence counter |
| 0xE4   | 0x390    | Audio sequence counter |
| 0xE5   | 0x394    | Audio playback frame (InterlockedExchange) |
| 0xE7   | 0x39C    | EOF flag |
| 0xE8   | 0x3A0    | Current frame type |
| 0xE9   | 0x3A4    | Last chunk read result |
| 0xEA   | 0x3A8    | Current chunk dispatch result |
| 0xEB   | 0x3AC    | Audio subchunk counter (for repeat) |
| 0xEC   | 0x3B0    | Loop restart file offset |
| 0xED   | 0x3B4    | Audio decode tick counter |
| 0xEE   | 0x3B8    | Audio thread run flag (InterlockedExchange) |
| 0xEF   | 0x3BC    | Audio thread handle |
| 0xF0   | 0x3C0    | DS buffer play position (emulated or real) |
| 0xF1   | 0x3C4    | Emulated audio flag (no DS buffer) |
| 0xF2   | 0x3C8    | DS speaker config flag |
| 0xF3   | 0x3CC    | DS volume |
| 0xF4   | 0x3D0    | DS buffer ownership flag |
| 0xF5   | 0x3D4    | DS device ownership flag |
| 0xF6   | 0x3D8    | Auto-close on complete |
| 0xF7   | 0x3DC    | Created DDraw surface flag |
| 0xF8-FB| 0x3E0-3EC| ADPCM decoder state (4 x int predictor/step) |
| 0xFC-FE| 0x3F0-3F8| EA ADPCM codec state |
| 0x100  | 0x400    | **Dequantization table** (64 ints = 256 bytes) |
| 0x140  | 0x500    | IDCT scratch: DC predictor |
| 0x141  | 0x504    | IDCT scratch: Cb predictor |
| 0x142  | 0x508    | IDCT scratch: Cr predictor |
| 0x184  | 0x610    | Start tick (GetTickCount) |
| 0x185  | 0x614    | End tick (GetTickCount) |
| 0x188  | 0x620    | Palette frame: width |
| 0x189  | 0x624    | Palette frame: entry count |
| 0x18A  | 0x628    | Inter-frame: delta width |
| 0x18B  | 0x62C    | Inter-frame: delta height |
| 0x18C  | 0x630    | Inter-frame: motion X blocks |
| 0x18D  | 0x634    | Inter-frame: motion Y blocks |
| 0x18E  | 0x638    | Inter-frame: allocated motion size |
| 0x190  | 0x640    | Pixel working buffer pointer |
| 0x191  | 0x644    | Previous frame buffer (for inter) |
| 0x192  | 0x648    | Temp buffer C |
| 0x193  | 0x64C    | Temp buffer D |
| 0x194  | 0x650    | Dequant temp A |
| 0x195  | 0x654    | Motion vector buffer A |
| 0x196  | 0x658    | Motion vector buffer B |
| 0x197  | 0x65C    | Inter-frame Y plane |
| 0x198  | 0x660    | Inter-frame Cb plane |
| 0x199  | 0x664    | Inter-frame Cr plane |

---

## 3. Ring Buffer Architecture

Two parallel ring buffers, each with **24 slots** and a stride of **4 dwords (16 bytes)**:

- **Video ring** at ctx+0x1E8 (indices 0x7A..0xD9)
- **Audio ring** at ctx+0x068 (indices 0x1A..0x79)

### Synchronization

Producer (I/O thread in `ReadAndDispatchChunk`):
1. Reads chunk from mmio into the linear read buffer
2. Writes pointer+size into ring slot at `producer_index % 24`
3. `InterlockedIncrement(&producer_index)`
4. `InterlockedIncrement(&pending_count)`

Consumer (decode thread in `RunStreamingPlaybackLoop`):
1. Reads slot at `consumer_index % 24`
2. Decodes video frame / writes audio to DS buffer
3. `InterlockedIncrement(&consumer_index)`
4. `InterlockedDecrement(&pending_count)`

The ring buffers can hold up to 24 chunks ahead, with a high-water mark of 0x16 (22)
for both audio and video before the producer stops reading.

### Buffer Sizes

- Video read buffer: **1,200,000 bytes** (calloc)
- Audio read buffer: **300,000 bytes** (calloc)
- Both are circular: write cursor wraps to start when reaching end

---

## 4. EA TGQ Container Format

The file is read via Win32 mmio API (`mmioOpenA`, `mmioRead`, `mmioSeek`).
The buffer size is set to 0x2000 (8KB) via `mmioSetBuffer`.

### Chunk Structure

Each chunk is 8 bytes header: 4-byte FourCC (big-endian) + 4-byte size (big-endian).
The chunk reader at 0x451D70 dispatches on FourCC:

| FourCC (hex)   | ASCII    | Type | Handler |
|----------------|----------|------|---------|
| 0x44414553     | `SEAD`   | Audio header (simple) | 0x452280 -- sets 22050Hz/16-bit/stereo |
| 0x6c484353     | `SCHl`   | Audio header (extended) | 0x4526A0 -- parses tagged params |
| 0x684e5331     | `1SNh`   | Audio header (variant) | 0x4526A0 |
| 0x43444e53     | `SNDC`   | Audio data | 0x452300 |
| 0x644e5331     | `1SNd`   | Audio data (variant) | 0x452300 |
| 0x54475666     | `fVGT`   | Video frame (type 2) | FillAudioStreamBuffer(ctx, size, 2) |
| 0x66564754     | `TVGf`   | Video frame (type 2) | same |
| 0x5447566b     | `kVGT`   | Video key frame (type 1) | FillAudioStreamBuffer(ctx, size, 1) |
| 0x6b564754     | `TVGk`   | Video key frame (type 1) | same |
| 0x54475170     | `pQGT`   | Video (type 3) | FillAudioStreamBuffer(ctx, size, 3) |
| 0x6656554d     | `MUVf`   | Video (type 3) | same |
| 0x54514970     | `pIQT`   | Video (type 4) | FillAudioStreamBuffer(ctx, size, 4) |
| 0x66325655     | `UV2f`   | Video (type 4) | same |
| 0x4d414465     | `eDAM`   | Video+audio (type 7) | FillAudioStreamBuffer(ctx, size, 7) |
| 0x6544414d     | `MADe`   | Video+audio (type 7) | same |
| 0x4d41446b     | `kDAM`   | Video key+audio (type 5) | FillAudioStreamBuffer(ctx, size, 5) |
| 0x6b44414d     | `MADk`   | Video key+audio (type 5) | same |
| 0x4d41446d     | `mDAM`   | Video+audio (type 6) | FillAudioStreamBuffer(ctx, size, 6) |
| 0x6d44414d     | `MADm`   | Video+audio (type 6) | same |
| 0x5343436c     | `lCCS`   | Subtitle/caption (skip) | mmioSeek past, return 4 |
| 0x6c434353     | `SCCl`   | Subtitle/caption (skip) | same |
| 0x6c4c4353     | `SCLl`   | Subtitle (skip) | same |
| 0x5343446c     | `lDCS`   | Subtitle data | to audio dispatch |
| 0x5343486c     | `lHCS`   | Subtitle header | 0x4526A0 (audio header parser) |
| 0x6c484353     | `SCHl`   | see above | |
| 0x444e4553     | `SEND`   | End of stream | return -1 (EOF) |
| 0x6c454353     | `SCEl`   | End of stream | return -1 (EOF) |
| 0x654e5331     | `1SNe`   | End of stream | return -1 (EOF) |
| 0x44414554     | `TEAD`   | (legacy header) | |

### Audio Codec Tags (ctx+0x64, i.e. ctx[0x19])

| Value | Codec |
|-------|-------|
| 0     | Raw PCM (mono -> duplicate to stereo, or direct copy) |
| 7     | EA IMA ADPCM (30-byte frames -> 112 samples, at 0x455AD0) |
| 10    | EA ADPCM (nibble-per-sample, at 0x4564F3) |

---

## 5. Video Codec: EA TGQ (JPEG-IDCT)

### Frame Types (stored in video ring slot[3])

| Type | Name | Description |
|------|------|-------------|
| 1    | Palette / key | 8-bit palettized intra-frame with embedded RGB palette |
| 2    | Delta palette | 8-bit inter-frame (swaps previous/current frame buffer) |
| 3    | TGQ intra (quality) | JPEG-style IDCT, 8x8 blocks, quality-scaled quant table |
| 4    | TGQ intra (full) | JPEG-style IDCT, 8x8 or 16x16 blocks, full quant table |
| 5    | TGV key (motion) | Inter-frame with motion vectors (key) |
| 6    | TGV motion | Inter-frame with motion vectors |
| 7    | TGV motion (alt) | Inter-frame with motion vectors (alternate ref) |

### Frame Header Parsing (`ParseVideoFrameHeader` at 0x455070)

- **Type 1 (palette):** Header contains width (16-bit), height (16-bit), palette width,
  palette height, and RGB entries (3 bytes each), brightness-scaled to 0-255.
- **Type 3 (TGQ):** Width (16-bit LE at offset 0), height (16-bit LE at offset 2),
  quality byte at offset 1. Sets config+0x64 (width) and config+0x60 (height).
- **Type 4 (TGQ full):** Same as type 3, plus flags byte at offset 7 (bit 1 = horiz MC,
  bit 0 = vert MC).
- **Types 5-7 (TGV):** Width at word offset 4, height at word offset 10.

### Decode Pipeline (`DecodeVideoFrame` at 0x4542C0)

```
ParseVideoFrameHeader (0x455070)   -- extract width/height/quality
ComputeBlitGeometry (0x454830)     -- center/offset/clip
+-- If flag 0x800: use off-screen buffers directly
+-- Else: Lock DDraw surface (vtable+0x64 = Lock, flags 0x801)
    +-- QuerySurfacePixelFormat if needed
Switch on frame type:
  case 1: DecodeType1PaletteFrame (0x45B8D0 + 0x45BA30 + 0x45BAE0)
  case 2: DecodeType2DeltaPalette (0x45B420 + 0x45BAE0)
  case 3: BuildDequantizationTable (0x454690)
           +-- 16-bit: DecodeCompressedPixelBlock16 (0x4548C0)
           +-- 32-bit: DecodeCompressedPixelBlock32 (0x454BD0)
  case 4: BuildFullQuantTable (0x454750)
           +-- 8x8 blocks: DecodeVideoFrameBlocks8x8 (0x454E90)
           +-- 16x16 blocks: DecodeVideoFrameBlocks16x16 (0x454F30)
           +-- overlay mode: DecodeBlocksOverlay (0x454FD0)
  case 5/6/7: DecodeMotionCompensatedFrame (0x45B2B0)
Unlock DDraw surface (vtable+0x80 = Unlock)
SwapDoubleBuffer (0x4534A0)
```

### JPEG IDCT Path

The IDCT is a standard 8-point AAN (Arai-Agui-Nakajima) algorithm:

1. **Entropy decode** (`FUN_00456F70`): Reads variable-length coded DCT coefficients
   from a bitstream. Uses a 64-entry zigzag reorder table at 0x4D75AC. Dequantizes
   using the scaled quantization table at ctx+0x400.

2. **Column IDCT** (`FUN_0045A84C`): 8-point butterfly with floating-point twiddle
   factors stored at 0x4D7598-0x4D75A8. Operates on the 8x8 coefficient block in-place.

3. **Row IDCT** (`FUN_0045A98B`): Same butterfly transform applied to rows. Outputs to
   separate buffer addresses (staggered by 0x20 or 0x40 bytes for 8x8/16x16 modes).

4. **YCbCr-to-RGB conversion** (`FUN_004573F9`): Uses three 128-entry lookup tables:
   - Y LUT at 0x4D3930 (luma -> pixel contribution)
   - Cb LUT at 0x4D3D30 (blue-difference chroma)
   - Cr LUT at 0x4D3B30 (red-difference chroma)
   Formula: `pixel = Y_LUT[Y] + Cb_LUT[Cb] + Cr_LUT[Cr]`

   The LUT builder at 0x453520 uses ITU-601 coefficients:
   - Cr->R: 0x166E9 (91881 = 1.402 * 65536)
   - Cb->G: -0x5816 (-22550 = -0.344 * 65536)
   - Cr->G: -0xB6CF (-46799 = -0.714 * 65536)
   - Cb->B: 0x1C5A1 (116129 = 1.772 * 65536)

5. **Clipping table** at 0x4D3F30: 1024-entry table mapping values to [0,255] range
   (initialized in `InitializeVideoDecoder`).

### Block Decode Modes

For frame types 3 and 4, blocks can be one of three sizes:
- **3 bytes**: Solid color fill (single Y, Cb, Cr)
- **6 bytes**: 2x2 gradient (4 corner colors, 2 chroma values per corner pair)
- **12 bytes**: 4x4 gradient (full Y/Cb/Cr per 2x2 sub-block)
- **>12 bytes**: Full IDCT-decoded block (variable-length bitstream)

The IDCT decode functions `FUN_0045791F` (for 16x16) and `FUN_004580FE` (for 8x8 in
16-bit) handle 6 planes: 4 Y blocks (8x8 each forming a 16x16 luma area) + 1 Cb + 1 Cr,
which is standard **YCbCr 4:2:0** subsampling.

### Quantization

`BuildDequantizationTable` (0x454690) computes:
```c
for (row = 0; row < 8; row++)
    for (col = 0; col < 8; col++)
        dequant[row*8+col] = base_table[row*8+col] * scale(quality, row, col);
```
where scale is linearly interpolated from quality (0-100), similar to JPEG quality factor.

The base quantization table at 0x47E2D8 is the standard JPEG luminance table (scaled).

---

## 6. Thread Model

### Thread 1: Streaming Playback (main decode)

Created by `LaunchStreamPlaybackThread` (0x452910) via `_beginthread` (0x45BFC8).
Entry point: `StreamThreadEntryPoint` (0x452990) -> `RunStreamingPlaybackLoop` (0x4529D0).

State transitions via `InterlockedExchange` on config+0x9C:
```
0 (init) -> 1 (starting) -> 2 (running) -> 4 (stop requested) -> 5/6 (stopped/freed)
                                ^              |
                                |  3 (paused)  |
                                +------<-------+
```

The main loop:
1. Set state = 2 (running)
2. Check for pause (state==3) or stop (state==4)
3. `ProcessStreamChunk` -- read next chunk from file into ring buffer
4. If audio not initialized yet, call `InitAudioPlayback` (0x4562C0)
5. If video slot has data and A/V sync < 3 frames behind, call `DecodeVideoFrame`
6. Call `PresentDecodedFrame` (0x452F20) to Blt to screen
7. `SwapDoubleBuffer` (0x4534A0) -- swap surface pointers
8. A/V sync: wait loop (up to 151 iterations) if video is ahead of audio
9. Goto 1

### Thread 2: Audio Feeder

Created by `StartDecoderThread` (0x456080) via `_beginthread`.
Entry point: `AudioDecoderThreadFunc` (0x455D60).
Thread priority: THREAD_PRIORITY_TIME_CRITICAL (0x0F).

The audio thread:
1. Polls `InterlockedExchange(ctx+0x3B8, 1)` -- run flag
2. Calls `GetAudioPlayPosition` (0x455FF0) to get current DS buffer play cursor
3. If enough audio data available, calls `WriteAudioToDirectSound` (0x455DE0)
4. Increments tick counter, sleeps 10ms
5. Exits when run flag becomes 0

Audio shutdown (`StopDecoderThread` 0x4560C0):
- Sets run flag to 0 via InterlockedExchange
- Spins with Sleep(10) until thread signals completion (flag becomes non-zero)

### DirectSound Initialization (`InitAudioPlayback` 0x4562C0)

Creates a secondary DirectSound buffer with:
- Format: as parsed from audio header (default 22050 Hz, 16-bit, stereo)
- Buffer size: `sample_rate * 4` bytes (i.e., 1 second of audio)
- Flags: DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS
- Starts playback immediately with `Play(0, 0, DSBPLAY_LOOPING)`

If DirectSound creation fails, falls back to emulated audio timing
(ctx[0xF1] = 1, uses tick-based position with 0x372 increment per poll).

---

## 7. PlayIntroMovie Integration (0x43C440)

The game's intro movie player at 0x43C440:

1. Sets up a 0x7B4-byte config block at DAT_004BCB90
2. Captures DXSound/DXDraw objects from the engine DLLs
3. Creates two DDraw surfaces (front + back, 640x480, 16-bit)
4. Opens `"Movie\intro.tgq"` via `OpenAndStartMediaPlayback` (0x452E20)
5. Enters a message pump loop:
   - PeekMessage/GetMessage for user input
   - ESC/Enter/Space -> stop playback
   - Numpad +/- -> adjust volume
   - Sleep(30ms) per iteration
6. Waits for `IsStreamPlaying` (0x452E80) to return false
7. Cleans up DDraw surfaces

The shutdown callback `RequestIntroMovieShutdown` (0x43C3C0) is installed at
DX::app + 0x138 offset and can be triggered externally.

Config flags set by PlayIntroMovie: `0x11140C | 0x3000000` =
- 0x04 (center H) | 0x08 (center V) | 0x400 (back buffer) | 0x1000 (flip)
- 0x10000 (threaded) | 0x100000 (brightness) | 0x800000 (audio)
- 0x1000000 (DDraw video) | 0x2000000 (restore surfaces)

---

## 8. Function Summary with Confirmed Names

### Streaming Core

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x451990 | `OpenMultimediaStream` | ~0x80 | Allocs 0x668 ctx, calls OpenWav + InitDS + InitVideo |
| 0x451A10 | `OpenWavStream` | ~0x1E0 | mmioOpen, sets 8KB buffer, pre-fills ring buffers |
| 0x451BB0 | `SkipRIFFHeaders` | ~0x1C0 | Skips SEAD/SCHl/1SNh headers, sets audio flag |
| 0x451D70 | `ReadAndDispatchChunk` | ~0x510 | FourCC switch, dispatches to FillBuffer/ParseAudio |
| 0x452280 | `ParseSimpleAudioHeader` | ~0x80 | SEAD chunk: hardcodes 22050/16-bit/stereo/codec10 |
| 0x452300 | `DecodeAudioChunk` | ~0x3A0 | PCM copy/mono-expand, EA IMA decode, EA ADPCM decode |
| 0x4521A0 | `FillVideoRingBuffer` | ~0x90 | Writes chunk ptr/size into video ring slot |
| 0x4526A0 | `ParseExtendedAudioHeader` | ~0x120 | SCHl tagged parameter parser |
| 0x4527C0 | `ReadTaggedParam` | (helper) | Reads variable-length tagged value |
| 0x452830 | `CloseMultimediaStream` | ~0x60 | Records duration, frees all resources |
| 0x452890 | `ReleaseStreamResources` | ~0x80 | Closes mmio, frees buffers, zeros ring slots |
| 0x452910 | `LaunchStreamPlaybackThread` | ~0x80 | _beginthread + spin-wait for startup |
| 0x452990 | `StreamThreadEntryPoint` | ~0x40 | Wrapper: run loop + auto-close + thread exit |
| 0x4529D0 | `RunStreamingPlaybackLoop` | ~0x2F0 | Main decode loop with A/V sync |
| 0x452CC0 | `ProcessStreamChunk` | ~0x100 | Reads next chunk, handles looping/audio-follows |
| 0x452DC0 | `SeekToLoopPoint` | ~0x60 | mmioSeek to saved offset for looping |
| 0x452E10 | `NullStreamCallback` | ~0x10 | Returns 0 (stub) |
| 0x452E20 | `OpenAndStartMediaPlayback` | ~0x60 | Open + set auto-close + launch thread |
| 0x452E60 | `SetStreamVolume` | ~0x20 | Sets volume, rebuilds color LUTs |
| 0x452E80 | `IsStreamPlaying` | ~0x40 | Returns true if state is 2 or 3 |
| 0x452EC0 | `StopStreamPlayback` | ~0x60 | Sets state=4, waits for 5 or 6 |
| 0x452F20 | `PresentDecodedFrame` | ~0x290 | Blt/BltFast decoded frame to DDraw surface |

### Video Init/Decode

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x453380 | `CreateVideoOutputSurface` | ~0xF0 | CreateSurface for video frame |
| 0x453470 | `ResetIDCTState` | ~0x30 | Zeros DC/Cb/Cr predictors |
| 0x4534A0 | `SwapDoubleBuffer` | ~0x40 | Swaps surface A<->B and offscreen A<->B |
| 0x4534E0 | `BuildColorConversionLUTs` | ~0x40 | Dispatches to 16-bit or 8-bit LUT builder |
| 0x453520 | `BuildYCbCrToRGB16LUT` | ~0x160 | ITU-601 YCbCr->RGB with pixel format packing |
| 0x453680 | `PackPixelFromRGB` | (helper) | Packs R/G/B into pixel format word |
| 0x4538B0 | `BuildYCbCrToRGB32LUT` | (helper) | 32-bit variant |
| 0x453B20 | `ExtractPixelFormatInfo` | ~0xC0 | Reads DDraw pixel format, computes shift/mask |
| 0x453BE0 | `CountTrailingZeroBits` | (helper) | Returns bit position of lowest set bit |
| 0x453C00 | `CountSetBits` | (helper) | Returns popcount of mask |
| 0x453C10 | `QuerySurfaceDimensions` | ~0x50 | GetSurfaceDesc -> extract width/height |
| 0x453C60 | `BuildYCbCrToPackedPixelLUT` | ~0x300 | Main LUT builder for all 3 channels |
| 0x453F70 | `QuerySurfacePixelFormat` | ~0x170 | Probes DDraw for overlay/offscreen caps |
| 0x4539E0 | `InitializeVideoDecoder` | ~0x580 | Surfaces, LUTs, clipping table at 0x4D3F30 |
| 0x454020 | `ReleaseVideoResources` | ~0x150 | Frees all decode buffers and surfaces |
| 0x454170 | (related helper) | | |
| 0x4542C0 | `DecodeVideoFrame` | ~0x3D0 | Master dispatcher by frame type |
| 0x454690 | `BuildDequantizationTable` | ~0xC0 | Quality-scaled JPEG quant table |
| 0x454750 | `BuildFullQuantTable` | ~0xE0 | Full (quality=100) quant table |
| 0x454830 | `ComputeBlitGeometry` | ~0x90 | Center/offset/clip for blit rectangles |
| 0x4548C0 | `DecodeCompressedPixelBlock16` | ~0xB0 | Type 3 blocks, 16-bit output |
| 0x4549D0 | `WriteBlock16_3to6` | (helper) | Writes 2x2 color block to 16-bit surface |
| 0x454BD0 | `DecodeCompressedPixelBlock32` | ~0xB0 | Type 3 blocks, 32-bit output |
| 0x454CE0 | `WriteBlock32_3to6` | (helper) | Writes 2x2 color block to 32-bit surface |
| 0x454E90 | `DecodeVideoFrameBlocks8x8` | ~0xA0 | Type 4, 8x8 IDCT blocks, 16-bit |
| 0x454F30 | `DecodeVideoFrameBlocks16x16` | ~0xA0 | Type 4, 16x16 IDCT blocks, 32-bit |
| 0x454FD0 | `DecodeVideoFrameBlocksOverlay` | ~0xA0 | Type 4, overlay surface mode |
| 0x455070 | `ParseVideoFrameHeader` | ~0x210 | Extracts width/height/quality/palette |
| 0x4552B0 | `CreateVideoSurfaces` | ~0x2D0 | Creates offscreen/overlay DDraw surfaces |
| 0x455870 | `RestoreLostSurfaces` | ~0xE0 | Calls Restore on all lost DDraw surfaces |

### Inter-frame / Motion Compensation

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x45B2B0 | `DecodeMotionCompensatedFrame` | ~0x170 | Types 5/6/7, motion vectors + residual |
| 0x45B420 | `DecodeType2DeltaPalette` | ~0x330 | Delta frame with palette, swaps buffers |
| 0x45ACD0 | `InitMotionVectorDecoder` | ~0xC0 | Sets up MV prediction/quantization tables |
| 0x45AD90 | `BuildMotionVectorLUT` | (helper) | One-time init for MV decode tables |
| 0x45AF50 | `ApplyMotionBlock` | (helper) | Applies motion vector to 16x16 block |

### Palette / Color

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x45B8D0 | `DecodeType1PaletteFrame` | ~0x160 | Allocs working buffers, decodes palette image |
| 0x45BA30 | `ConvertPaletteToPixelFormat` | ~0xB0 | Builds 256-entry pixel lookup from RGB palette |
| 0x45BAE0 | `BlitDecodedToSurface` | ~0x220 | Copies decoded pixels to DDraw surface pitch |
| 0x45A588 | `CopyPixelBuffer32` | (helper) | 32-bit memcpy-like pixel buffer copy |

### IDCT (in IDCT_DAT section)

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x456F70 | `IDCTDecodeDCAndAC` | ~0x490 | Huffman-decode + dequantize 64 coefficients |
| 0x45791F | `IDCTDecode16x16Block` | ~0x800 | Full 16x16 MCU: 6 planes x (8 row + 8 col IDCT + YCbCr) |
| 0x4580FE | `IDCTDecode8x8Block16` | (similar) | 8x8 variant for 16-bit surfaces |
| 0x4590BF | `IDCTDecode8x8BlockOverlay` | (similar) | 8x8 variant for overlay mode |
| 0x4592AD | `IDCTDecode16x16MCBlock` | (similar) | 16x16 with MC horizontal flag |
| 0x45949B | `IDCTDecode16x16MCBlockV` | (similar) | 16x16 with MC vertical flag |
| 0x459689 | `IDCTDecode8x8Block16NoMC` | (similar) | 8x8, 16-bit, no motion comp |
| 0x459877 | `IDCTDecode8x8Block32NoMC` | (similar) | 8x8, 32-bit, no motion comp |
| 0x45A84C | `IDCT8ColumnButterfly` | ~0x13F | AAN column butterfly |
| 0x45A98B | `IDCT8RowButterfly` | ~0xCB | AAN row butterfly |
| 0x4573F9 | `YCbCrToPackedPixel16` | ~0x200 | LUT-based Y+Cb+Cr -> packed 16-bit pixel |

### DirectSound

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x456080 | `StartAudioDecoderThread` | ~0x40 | _beginthread + TIME_CRITICAL priority |
| 0x4560C0 | `StopAudioDecoderThread` | ~0x50 | Signal thread + spin-wait |
| 0x456110 | `InitStreamDirectSound` | ~0xC0 | DirectSoundCreate + GetCaps |
| 0x4561D0 | `CreateDirectSoundDevice` | ~0x80 | DirectSoundCreate + SetCooperativeLevel |
| 0x456210 | `GetDirectSoundVolume` | (helper) | Reads IDirectSoundBuffer volume |
| 0x456250 | `ReleaseDirectSoundDevice` | ~0x50 | Releases DSBuffer + DSDevice |
| 0x4562A0 | `ReleaseDirectSoundObject` | (helper) | Release + null |
| 0x4562C0 | `InitAudioPlayback` | ~0x170 | Creates DS buffer, starts looping playback |
| 0x456430 | `ReleaseDSStreamBuffer` | ~0x20 | Release streaming DS buffer |
| 0x455D60 | `AudioDecoderThreadFunc` | ~0x80 | Main audio feeder loop |
| 0x455DE0 | `WriteAudioToDirectSound` | ~0x150 | Lock/Write/Unlock DS buffer |
| 0x455FF0 | `GetAudioPlayPosition` | ~0x60 | GetCurrentPosition or emulated |

### Audio Codecs

| Address  | Confirmed Name | Size | Notes |
|----------|---------------|------|-------|
| 0x4564F3 | `DecodeEAADPCM` | ~0x1A0 | Nibble-based ADPCM, step table at 0x47FB40 |
| 0x455AD0 | `DecodeEAIMA_Stereo` | ~0x190 | EA IMA, 30-byte frames -> 112 samples (stereo) |
| 0x455950 | `DecodeEAIMA_Mono` | ~0x180 | EA IMA, 15-byte frames -> 112 samples (mono) |

---

## 9. Key Global Data

| Address    | Size   | Description |
|------------|--------|-------------|
| 0x47E2D8   | 256B   | Base JPEG quantization matrix (64 ints) |
| 0x4D75AC   | 256B   | Reverse zigzag scan order table (64 ints) |
| 0x4D3820   | 256B   | Scaled dequantization table (runtime, 64 ints) |
| 0x4D3930   | 512B   | Y (luma) -> pixel LUT (128 entries) |
| 0x4D3B30   | 512B   | Cr -> pixel LUT (128 entries) |
| 0x4D3D30   | 512B   | Cb -> pixel LUT (128 entries) |
| 0x4D3F30   | 4096B  | Clipping table (1024 entries, maps to 0-255) |
| 0x4D3600   | varies | IDCT coefficient scratch area |
| 0x4D3000   | 512B   | IDCT output buffer (8x8 x 2 planes) |
| 0x47FB40   | 5632B  | ADPCM decode delta table (88 step levels x 16 nibbles x int32) |
| 0x481180   | 32B    | ADPCM step adaptation table (8 entries x int32) |
| 0x47FB20   | 16B    | EA IMA filter coefficient table A (4 entries x int32) |
| 0x47FB30   | 16B    | EA IMA filter coefficient table B (4 entries x int32) |
| 0x482CC0   | 256B   | MV quantization scale table (8x8 matrix, int32) |
| 0x4D1250   | 256B   | MV base quantization table (64 entries, int32) |
| 0x4D4F30   | varies | YCbCr-to-YUV packed pixel LUT (runtime) |
| 0x4CF96C   | 4B     | Motion compensation init-once flag |
| 0x4CF970   | 4B     | Motion vector LUT init-once flag |
| 0x4D7598   | 20B    | IDCT twiddle factor constants (5 floats) |

---

## 10. Key Observations

1. **Codec Identity**: This is EA's proprietary TGQ format, also known as "EA TGQ" or
   "Electronic Arts TGQ Video". It is documented in FFmpeg as `ea_tgq` and in
   multimedia.cx wiki. The container uses EA SCxl chunk headers with FourCC tags.

2. **Single Caller**: Only `PlayIntroMovie` (0x43C440) uses this library. The movie file
   is `Movie\intro.tgq`. The engine is general-purpose but TD5 uses it for the one intro.

3. **Audio Fallback**: If DirectSound creation fails, the engine falls back to emulated
   audio timing (ctx[0xF1]=1) using tick-based position estimation with a fixed
   increment of 0x372 per poll (~882 bytes/10ms at 22050 Hz stereo 16-bit).

4. **A/V Sync**: The playback loop waits up to 151 iterations (each ~2ms) for audio to
   catch up to video. The drift value at config+0x98 tracks frames of audio behind.

5. **Double Buffering**: Two DDraw surfaces at config+0x1C and config+0x20 are swapped
   after each frame via `SwapDoubleBuffer`. Similarly, off-screen buffers at
   config+0x4C and config+0x50 are swapped.

6. **Widescreen Impact**: The movie player creates its own DDraw surfaces at the
   hardcoded 640x480 resolution. For widescreen patches, the intro movie would need
   either letterboxing or surface scaling. The `_DAT_00474834 = 0x280` (640) and
   `_DAT_00474838 = 0x1E0` (480) in PlayIntroMovie are the dimension constants.

---

## 11. Motion-Compensated Inter Frames (Types 5/6/7) -- COMPLETE

### Overview

Frame types 5, 6, and 7 implement P-frame motion compensation with DCT residual coding.
There are NO B-frames -- only forward-predicted P-frames referencing a single previous frame.
The codec is EA's TGV inter-frame format, using Huffman-coded motion vectors and JPEG-style
IDCT residuals in YCbCr 4:2:2 color space with 16x16 macroblock granularity.

### Frame Type Semantics

| Type | FourCC        | Meaning |
|------|---------------|---------|
| 5    | `kDAM`/`MADk` | **Key inter-frame** -- no motion vectors (MV disabled), pure IDCT intra |
| 6    | `mDAM`/`MADm` | **P-frame** -- motion vectors enabled, references previous reconstructed frame |
| 7    | `eDAM`/`MADe` | **P-frame (alt ref)** -- same as type 6 but uses alternate reference buffer (ctx[0x199]) |

### Dispatcher: `DecodeMotionCompensatedFrame` (0x45B2B0)

```c
DecodeMotionCompensatedFrame(ctx, frame_data, surface_ptr, pitch, frame_type)
```

**Reference Frame Buffers** (lazy-allocated on first call):
- `ctx[0x197]` -- YCbCr plane A (width * height * 4 bytes)
- `ctx[0x198]` -- YCbCr plane B (width * height * 4 bytes)
- `ctx[0x199]` -- YCbCr plane C (width * height * 4 bytes)

These form a **double-buffer swap** system (NOT a ring buffer):
- Types 5 and 6: decode into `ctx[0x198]`, then swap A<->B at the end
- Type 7: decode into `ctx[0x199]` (no swap) -- allows referencing a held keyframe

After decoding, `ctx[0x197]` always holds the most recent reconstructed frame
(for type 5/6) or `ctx[0x199]` holds the alt-ref (for type 7).

**Buffer swap logic** (at function tail):
```c
if (frame_type != 7) {
    temp = ctx[0x197];
    ctx[0x197] = ctx[0x198];  // newly decoded becomes "previous"
    ctx[0x198] = temp;         // old previous becomes next decode target
}
```

### Frame Header (Types 5/6/7)

Parsed by `ParseVideoFrameHeader` (0x455070), case 5/6/7:
```
Offset 0x00: (8 bytes) chunk header (FourCC + size)
Offset 0x08: uint16 -- video width  (stored at config+0x64)
Offset 0x0A: uint16 -- video height (stored at config+0x60)
Offset 0x0D: int8   -- quality parameter (signed, passed to InitMotionVectorDecoder)
Offset 0x10: uint16[2] -- bitstream start (big-endian, Huffman-coded MV + residual data)
```

### Motion Vector Initialization: `InitMotionVectorDecoder` (0x45ACD0)

Called per-frame with:
```c
InitMotionVectorDecoder(
    bitstream_ptr,        // frame_data + 0x10
    mv_enabled,           // (frame_type != 5) ? 1 : 0
    quality               // signed byte from frame_data[0x0D]
);
```

**Algorithm:**
1. If first call, run `BuildHuffmanDecodeTables()` to initialize Huffman VLC tables
2. Set up bitstream reader: `DAT_004d264c` = data ptr, `DAT_004d2650` = bit buffer,
   `DAT_004d2654` = 32 bits available
3. Compute DC quantization scale: `DAT_004d254c = DAT_004d124c * (DAT_00482cc0 << 15)`
   (fixed-point 16.16 multiply)
4. Compute per-coefficient quantization: loop over 63 AC coefficients:
   ```c
   for (i = 0; i < 63; i++) {
       quant_table[i] = fixed_mul(base_quant[i], coeff_scale[i] * quality * 0x1000);
   }
   ```
   where `base_quant` at `0x4D1250` (64 entries) and `coeff_scale` at `0x482CC4` (8x8 table)
5. Store `DAT_004cf968 = mv_enabled` (0 for type 5, 1 for type 6/7)

### Macroblock Processing: `ParseJPEGHeaders` (0x45AF50)

This is the per-macroblock decoder, called in a 16x16 grid over the frame:

```c
// In DecodeMotionCompensatedFrame:
for (y = 0; y < height; y += 16) {
    for (x = 0; x < width; x += 16) {
        block_idx = ((y * width + x) >> 1);
        ParseJPEGHeaders(decode_buf + block_idx*4, ref_buf + block_idx, width);
    }
}
```

**Per-macroblock algorithm:**

1. **Read block control flags** (if MV enabled):
   - If top 2 bits of bitstream are zero: read 2 bits -> `flags = 0` (all-intra macroblock)
   - If bit 31 set: `flags = 0x3FF` (all inter/skip), consume 1 bit
   - Otherwise: read 8 bits as `flags`, advance by 8 bits

   The 6 low bits of `flags` control each of the 6 blocks:
   - Bit 0: Y block 0 (top-left 8x8)
   - Bit 1: Y block 1 (top-right 8x8)
   - Bit 2: Y block 2 (bottom-left 8x8)
   - Bit 3: Y block 3 (bottom-right 8x8)
   - Bit 4: Cb block (half-res chroma)
   - Bit 5: Cr block (half-res chroma)

   **Bit = 0**: INTRA -- full IDCT decode from bitstream (no reference needed)
   **Bit = 1**: INTER -- copy from reference frame + add delta offset

2. **Read motion vector** (if any `flags` bit is set):
   ```c
   mv_x = BitstreamReadHuffmanCode();
   mv_y = BitstreamReadHuffmanCode();
   // Adjust block pointers by motion vector:
   decode_ptr += (mv_y * half_width + (mv_x >> 1)) * 4;
   parity = mv_x & 1;  // sub-pixel horizontal flag
   ```
   Motion vectors are **integer-pel** in Y direction, with a **half-pel flag** in X
   (lowest bit of mv_x). The Huffman code uses a 6-bit VLC table at `DAT_004d244c`
   (64 entries), producing signed values centered at 0.

   The MV range is determined by the `quality` parameter and the quantization scale
   tables at 0x482CC0 (8x8 matrix of scale factors). Larger quality values = larger
   search range.

3. **Decode each block** (6 total per macroblock):

   For **INTRA blocks** (flag bit = 0):
   ```c
   ncoeffs = IDCT_DecodeCoefficients();  // VLC Huffman + dequant
   if (ncoeffs == 1) {
       FillBlock_DC(output_block, stride);  // DC-only, fill 8x8 with constant
   } else {
       IDCT_FullBlock8x8(output_block, stride);  // Full 8-point AAN IDCT
   }
   ```

   For **INTER blocks** (flag bit = 1):
   ```c
   delta = BitstreamReadHuffmanCode() - 0x40;  // signed delta, range ~[-64, +63]
   DequantizeIDCT_Block(ref_ptr, parity, half_width, output_block, delta);
   ```
   This copies the 8x8 block from the reference frame at the motion-compensated
   position, adding the scalar `delta` to every pixel. The `parity` flag selects
   between two column-interleave patterns for half-pel horizontal interpolation.

4. **Apply IDCT transform** to all 6 blocks:
   ```c
   IDCT_Transform8x8_Caller();  // 16 calls to IDCT_Transform8x8_Block
   ```
   This applies the full 2D IDCT to all decoded blocks in scratch memory.

### DequantizeIDCT_Block (0x45AB80) -- Inter Block Copy

```c
DequantizeIDCT_Block(ref_ptr, parity, half_width, output_block, delta)
```

Copies an 8x8 block from the reference frame, adding a constant `delta` offset:
- `parity = 0`: reads bytes at even column offsets (standard alignment)
- `parity = 1`: reads bytes at odd column offsets (half-pel shifted right)
- Row stride = `half_width * 4` bytes

This implements **half-pel horizontal motion compensation** via column selection.
There is no vertical half-pel interpolation (integer-pel only in Y direction).

### DequantizeIDCT_Block_Half (0x45AC60) -- Chroma Inter Block

Same as above but for half-resolution chroma blocks:
- Reads with stride `half_width * 8` (2x vertical subsampling)
- Output stride is 0x20 (32 bytes per row = 8 pixels x 4 bytes)

### Color Space: YCbCr 4:2:2

The motion compensation path uses **4:2:2 chroma subsampling**:
- 4 luma (Y) blocks: 8x8 each, forming a 16x16 luma area
- 1 Cb block: 8x8, covers the full 16x16 area at half horizontal resolution
- 1 Cr block: 8x8, covers the full 16x16 area at half horizontal resolution

After all macroblocks are decoded, `YCbCrToRGB_Row_16bit_422` (0x45A5CC) converts
each row from packed YCbCr to RGB565/555 pixel format using lookup tables:
```c
for (row = 0; row < height - 1; row++) {
    YCbCrToRGB_Row_16bit_422(ycbcr_plane, rgb_surface, width/2);
    rgb_surface += pitch;
}
```

### Huffman VLC Tables

`BuildHuffmanDecodeTables` (0x45AD90) is a one-time initialization that builds three
decode acceleration tables from static code tables:

- **DC table** (source at 0x481EE4): variable-length codes for DC coefficient deltas
- **AC table** (source at 0x4824C4): run-length/value codes for AC coefficients
- **MV table** (built in the function tail at 0x4D244C): 6-bit Huffman codes for
  motion vector components, range [-16, +16] in 64 entries

The VLC decoding uses multi-level lookup:
1. Fast path: top 9 bits -> direct decode from `DAT_004d144c` (512 entries)
2. Medium: top 6+8 bits -> `DAT_004d1c4c` (256 entries)
3. Slow: extended lookup from `DAT_004d204c` (256 entries)
4. EOB marker: code 0x2F = end of block, code 0x3F = end of macroblock

### Summary of Motion Compensation Properties

| Property | Value |
|----------|-------|
| Frame types | P-frames only (no B-frames) |
| Macroblock size | 16x16 pixels (4 Y + 1 Cb + 1 Cr blocks of 8x8) |
| Chroma subsampling | 4:2:2 |
| Motion vector resolution | Integer-pel vertical, half-pel horizontal |
| MV coding | Huffman VLC, signed, per-macroblock |
| MV range | Quality-dependent, typically +/- 16 pixels |
| Reference frames | Single reference (double-buffer swap), plus alt-ref for type 7 |
| Per-block decision | 6-bit flag per macroblock: intra or inter per block |
| Inter mode | Copy from reference + scalar delta (no full residual DCT) |
| Intra mode | Full IDCT decode (same as type 3/4 frames) |
| Transform | 8-point AAN (Arai-Agui-Nakajima) IDCT |

---

## 12. EA ADPCM Audio Decoder -- COMPLETE

### Overview

The EA ADPCM decoder at `DecodeEAADPCM` (0x4564F3) is a **lookup-table-based ADPCM**
variant specific to Electronic Arts. It is NOT standard IMA ADPCM or MS ADPCM -- it uses
EA's own precomputed delta tables indexed by both nibble value and step index.

Audio codec type 10 in the stream context (`ctx[0x19] == 10`).

### Decoder State

Per-channel state is packed into two 32-bit words:
- `*param_6` = CONCAT16(predictor_R, predictor_L) -- two 16-bit signed predictors
- `*param_7` = CONCAT16(step_index_R, step_index_L) -- two 16-bit step indices

Stored at `ctx[0xFC]` and `ctx[0xFE]` in the stream context.

### Per-Sample Algorithm

```c
// For each input byte (one byte = two samples, one per channel):
byte = *input++;

// LEFT CHANNEL (high nibble):
nibble_L = byte >> 4;                              // 0..15
predictor_L += delta_table[nibble_L][step_index_L]; // add signed delta
predictor_L = clamp(predictor_L, -32768, 32767);   // 16-bit signed clamp
output[i*2+0] = (int16_t)predictor_L;              // write left sample

step_index_L += adapt_table[nibble_L & 7];         // update step (use low 3 bits)
step_index_L = clamp(step_index_L, 0, 0x1600);     // clamp step index

// RIGHT CHANNEL (low nibble):
nibble_R = byte & 0x0F;
predictor_R += delta_table[nibble_R][step_index_R];
predictor_R = clamp(predictor_R, -32768, 32767);
output[i*2+1] = (int16_t)predictor_R;

step_index_R += adapt_table[nibble_R & 7];
step_index_R = clamp(step_index_R, 0, 0x1600);
```

### Stereo Interleaving

The codec is inherently **stereo** with **byte-interleaved nibbles**:
- Each input byte produces exactly 2 output samples (one left, one right)
- High nibble (bits 7-4) = left channel
- Low nibble (bits 3-0) = right channel
- Output is interleaved 16-bit PCM: L, R, L, R, ...
- Output stride is 4 bytes per input byte (2 samples x 2 bytes each)

### Delta Table: `DAT_0047FB40` (5632 bytes)

A 2D lookup table of **signed 32-bit deltas** indexed as:
```c
delta = delta_table_base[nibble * 4 + step_index_byte_offset]
// where step_index_byte_offset = step_index (0..0x1600, already in byte units)
// nibble = 0..15
```

**Table dimensions:** 88 step levels x 16 nibble values = 1408 entries of int32.

**Layout:** The table is organized with nibble as the fast axis (stride 4 bytes)
and step level as the slow axis (stride 64 bytes = 16 entries x 4 bytes).

The step index is a byte offset into this table (multiples of 64 bytes),
clamped to [0, 0x1600]. Since 0x1600 / 64 = 88, there are **88 step levels**
(indices 0 through 87).

**Sample values at step level 0 (nibbles 0-15):**
```
[0, 1, 3, 4, 7, 8, 10, 11, 0, -1, -3, -4, -7, -8, -10, -11]
```

**Sample values at step level 87 (nibbles 0-15):**
```
[1415, 4520, 7625, 10730, 13835, 16940, 20045, 23150,
 -1415, -4520, -7625, -10730, -13835, -16940, -20045, -23150]
```

The table is symmetric: `delta[n+8] = -delta[n]` for each step level.
Values grow exponentially with step level, from +/- 11 at level 0
to +/- 23150 at level 87.

### Step Adaptation Table: `DAT_00481180` (32 bytes)

8 entries of signed int32, indexed by `nibble & 7`:
```
adapt_table[8] = { -64, -64, -64, -64, 128, 256, 384, 512 }
```

- Nibbles 0-3 (and 8-11): step index decreases by 64 (one level down)
- Nibble 4 (and 12): step index increases by 128 (two levels up)
- Nibble 5 (and 13): step index increases by 256 (four levels up)
- Nibble 6 (and 14): step index increases by 384 (six levels up)
- Nibble 7 (and 15): step index increases by 512 (eight levels up)

Since each step level is 64 bytes apart, the adaptation is:
- Small deltas (nibbles 0-3): move down 1 step level
- Larger deltas (nibbles 4-7): move up 2/4/6/8 step levels

This is the hallmark of **EA ADPCM** -- IMA ADPCM uses a different adaptation
table with values {-1, -1, -1, -1, 2, 4, 6, 8} and a separate step size table.
EA ADPCM pre-bakes the step size into the delta table itself.

### Mono Variant: `DecodeADPCMBlock` (0x456450)

A general-purpose mono ADPCM decoder with identical algorithm but single-channel:
- Reads nibbles alternating high/low from consecutive bytes
- Single predictor and step index
- State stored in `*param_7` (predictor, int16) and `*param_8` (step index, int32)
- Same delta and adaptation tables

### Comparison: EA ADPCM vs EA IMA vs Standard IMA

| Feature | EA ADPCM (type 10) | EA IMA (type 7) | IMA ADPCM |
|---------|-------------------|-----------------|-----------|
| Step table | Pre-computed delta LUT (88 levels x 16 nibbles) | Filter coefficients (2-tap predictor) | 89-entry step size table |
| Adaptation | {-64,-64,-64,-64,128,256,384,512} byte offsets | Implicit in filter coefficients | {-1,-1,-1,-1,2,4,6,8} index offsets |
| Prediction | `pred += delta_table[nibble][step]` | `pred = c0*s0 + c1*s1 + shift(nibble)` | `pred += step * nibble_expansion` |
| Frame size | 1 byte per stereo sample pair | 15B mono / 30B stereo per 28 samples | 4-bit per sample |
| Tables at | 0x47FB40 (delta), 0x481180 (adapt) | 0x47FB20 (coeff A), 0x47FB30 (coeff B) | N/A |

### EA IMA Codec (Type 7) -- Supplementary

The EA IMA decoder at 0x455AD0 (stereo) / 0x455950 (mono) uses a **2-tap IIR filter**:
```c
// Per 28-sample block:
coeff_A = coeff_table_A[header >> 4];    // at 0x47FB20
coeff_B = coeff_table_B[header >> 4];    // at 0x47FB30
shift = (header & 0x0F) + 8;

// Per sample:
output = (coeff_A * prev_1 + coeff_B * prev_0 + 0x80 + (signed_nibble >> shift)) >> 8;
output = clamp(output, -32768, 32767);
```

Filter coefficient table (0x47FB20, 4 entries each):
```
coeff_A = {0, 240, 460, 392}
coeff_B = {0, 0, -208, -220}
```

Frame structure: 15 bytes mono (1 header + 14 nibble-pairs = 28 samples),
30 bytes stereo (2 headers + 28 nibble-pairs interleaved = 28 sample pairs).
Output is upsampled 2x (each sample duplicated) for mono, giving 112 output samples
per block (56 stereo pairs).

### Audio Chunk Dispatch Summary (`DecodeAudioChunk` at 0x452300)

| Codec (ctx[0x19]) | Channels | Handler | Output format |
|--------------------|----------|---------|---------------|
| 0 (PCM) | 1 | Mono->stereo duplicate | 16-bit interleaved |
| 0 (PCM) | 2 | Direct copy | 16-bit interleaved |
| 7 (EA IMA) | 1 | `DecodeEAIMA_Mono` (0x455950) | 16-bit, 2x upsampled |
| 7 (EA IMA) | 2 | `DecodeEAIMA_Stereo` (0x455AD0) | 16-bit interleaved |
| 10 (EA ADPCM) | 2 | `DecodeEAADPCM` (0x4564F3) | 16-bit interleaved |

Default audio format (from `ParseSimpleAudioHeader`): 22050 Hz, 16-bit, stereo, codec 10.
