# Inflate/ZIP Decompression Library Analysis

**Date:** 2026-03-19
**Address range:** 0x447490 - 0x4480FA (code) + static tables in .rdata/.data

## Overview

TD5 contains a custom DEFLATE (RFC 1951) inflate implementation embedded directly in the executable.
It is **not** standard zlib -- there is no zlib header/trailer parsing, no adler32, and no `z_stream`
struct. Instead it uses a flat set of global variables as its decompression state. The implementation
supports all three DEFLATE block types (stored, fixed Huffman, dynamic Huffman) and includes CRC-32
verification against the ZIP local file header's expected CRC.

The inflate library is invoked exclusively through the ZIP archive layer (`DecompressZipEntry` at
0x4405B0), which reads ZIP local file headers and dispatches to either raw copy (method 0) or
inflate (method 8).

## Identification: Custom Inflate (not zlib/puff)

Evidence this is a bespoke implementation rather than a standard library:

1. **No z_stream struct** -- all state lives in flat globals at 0x47b1c4-0x47b1f4
2. **No zlib/gzip header parsing** -- `InflateDecompress` immediately reads DEFLATE block headers
3. **No adler32** -- only CRC-32 for ZIP entry verification
4. **Custom Huffman table format** -- 8-byte entries (1B bits, 1B pad, 2B symbol, 4B child_ptr),
   with a binary tree overflow for codes longer than the primary table bits
5. **32KB sliding window** -- output buffer is 0x8000 bytes, flushed via callback
6. **64KB input buffer** -- refilled from file via `ReadTrackStaticDataChunk` / `fread_game`
7. **Integrated CRC-32** -- computed incrementally during output flush

## Call Chain

```
ReadArchiveEntry (0x440790)
  |-- fopen_game(filename) -> if file exists on disk, read directly
  |-- else: ParseZipCentralDirectory (0x43FC80) -> find entry in ZIP
  |       |-- DecompressTrackDataStream (0x43FBC0) -> skip bytes in central dir
  |       '-- fseek to local header offset (DAT_004cf988)
  '-- DecompressZipEntry (0x4405B0)
        |-- malloc 64KB input buffer (DAT_004c3760)
        |-- fread ZIP local file header (30 bytes)
        |-- Check signature 0x04034B50
        |-- method == 0: raw copy + CRC-32 verify
        '-- method == 8: InflateDecompress (0x447FE2)
              |-- Initialize state globals
              |-- Loop: read BFINAL + BTYPE bits
              |   |-- BTYPE=0: InflateProcessStoredBlock (0x447AA6)
              |   |-- BTYPE=1: InflateProcessFixedHuffmanBlock (0x447BBB)
              |   '-- BTYPE=2: InflateProcessDynamicHuffmanBlock (0x447C42)
              |         |-- Read HLIT, HDIST, HCLEN
              |         |-- InflateBuildDecodeTable for code-length alphabet
              |         |-- Decode literal/length + distance code lengths
              |         |-- InflateBuildDecodeTable for literal/length codes (9-bit primary)
              |         |-- InflateBuildDecodeTable for distance codes (6-bit primary)
              |         '-- InflateDecodeHuffmanCodes (0x447715)
              |               |-- Lookup literal/length from 9-bit primary table
              |               |-- value < 256: emit literal byte
              |               |-- value == 256: end-of-block
              |               |-- value 257-285: length code
              |               |     |-- Read extra length bits
              |               |     |-- Lookup distance from 6-bit primary table
              |               |     |-- Read extra distance bits
              |               |     '-- Copy from sliding window
              |               '-- Flush when output position >= 0x8000
              '-- InflateFlushOutputAndUpdateCrc32 (0x447490) on final block
```

## Functions (9 total)

| Address    | Name                              | Size   | Purpose |
|------------|-----------------------------------|--------|---------|
| 0x447490   | InflateFlushOutputAndUpdateCrc32  | ~102B  | Flush output window: advance output ptr OR call write callback, then compute CRC-32 over flushed bytes using table at 0x475160 |
| 0x4474F6   | InflateRefillInputBuffer          | ~12B   | Trampoline: calls ReadTrackStaticDataChunk to fread 64KB into DAT_004c3760 |
| 0x447502   | InflateWriteOutputChunk           | ~25B   | Trampoline: sets DAT_0047b1f4 = byte count, calls ReadCompressedTrackStreamChunk (fwrite to output stream) |
| 0x44751B   | InflateBuildDecodeTable           | ~510B  | Build canonical Huffman decode table from code lengths. Primary table = flat lookup (2^N entries, 8B each). Overflow = binary tree nodes (pair of uint32 child pointers). |
| 0x447715   | InflateDecodeHuffmanCodes         | ~913B  | Core Huffman decode loop: literal/length + distance decode, LZ77 copy, sliding window management. Largest function in the library. |
| 0x447AA6   | InflateProcessStoredBlock         | ~273B  | DEFLATE block type 0: align to byte boundary, read 16-bit length + ~length check, copy raw bytes |
| 0x447BBB   | InflateProcessFixedHuffmanBlock   | ~135B  | DEFLATE block type 1: populate static Huffman tables (lit 0-143=8bit, 144-255=9bit, 256-279=7bit, 280-287=8bit; dist all 5bit), build decode tables, call InflateDecodeHuffmanCodes |
| 0x447C42   | InflateProcessDynamicHuffmanBlock | ~416B  | DEFLATE block type 2: read dynamic Huffman header (HLIT/HDIST/HCLEN), decode code-length alphabet, decode lit/len + dist code lengths with RLE (codes 16/17/18), build both decode tables, call InflateDecodeHuffmanCodes |
| 0x447FE2   | InflateDecompress                 | ~280B  | Top-level inflate entry point: zero state, loop over DEFLATE blocks (BFINAL/BTYPE dispatch), final CRC flush, return decompressed size or 0 on error |

## Inflate State Globals (0x47b1c4 - 0x47b1f4)

| Address      | Type     | Name (proposed)       | Description |
|--------------|----------|-----------------------|-------------|
| 0x47b1c4     | uint32*  | pHuffTreeNodePool     | Current allocation pointer into Huffman tree node pool (0x477184) |
| 0x47b1c8     | uint32   | bitAccumulator        | Bit buffer (LSB-first, holds partially consumed input bits) |
| 0x47b1cc     | uint32   | bitsAvailable         | Number of valid bits in bitAccumulator |
| 0x47b1d0     | uint32   | outputPosition        | Current write position in output window (0 - 0x7FFF) |
| 0x47b1d4     | uint32   | inputPosition         | Current read position in input buffer (0 - 0xFFFF; 0x10000 = needs refill) |
| 0x47b1d8     | uint32   | runningCrc32          | Running CRC-32 value |
| 0x47b1dc     | uint32   | expectedCrc32         | Expected CRC-32 from ZIP local header field at offset 0x0E |
| 0x47b1e0     | char*    | pOutputBuffer         | Pointer to output window base (= DAT_004c3764 initially) |
| 0x47b1e4     | char*    | pInputBuffer          | Pointer to input buffer base (= DAT_004c3760) |
| 0x47b1e8     | uint32   | totalBytesWritten     | Total decompressed byte count (returned as result) |
| 0x47b1ec     | uint32   | streamingModeFlag     | 0 = direct output (write to pOutputBuffer), nonzero = streaming (callback via InflateWriteOutputChunk) |
| 0x47b1f0     | uint32   | lastWriteResult       | Return value from last streaming write callback |
| 0x47b1f4     | uint32   | pendingWriteSize      | Byte count to pass to streaming write callback |

## Static Data Tables (.rdata)

| Address      | Size     | Name                      | Description |
|--------------|----------|---------------------------|-------------|
| 0x474EA0     | 68B      | kBitMaskTable             | uint32[17]: bitmasks 0x0, 0x1, 0x3, 0x7, ..., 0xFFFF (mask for N bits) |
| 0x474F00     | 76B      | kCodeLengthOrder          | uint32[19]: DEFLATE code-length alphabet permutation {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15} |
| 0x474F60     | 120B     | kLengthBaseTable          | uint32[30]: base lengths for length codes 257-285 (3,4,5,...,258) |
| 0x474FE0     | 120B     | kLengthExtraBitsTable     | uint32[30]: extra bits for length codes 257-285 (0,0,...,1,1,...,5,5,0) |
| 0x475060     | 120B     | kDistanceBaseTable        | uint32[30]: base distances for distance codes 0-29 (1,2,3,4,5,7,9,...,24577) |
| 0x4750E0     | 120B     | kDistanceExtraBitsTable   | uint32[30]: extra bits for distance codes 0-29 (0,0,0,0,1,1,...,13,13) |
| 0x475160     | 1024B    | kCrc32LookupTable         | uint32[256]: CRC-32 lookup table (polynomial 0xEDB88320, same as zlib/PKZIP) |

## Working Buffers (.data / .bss)

| Address      | Size      | Name                      | Description |
|--------------|-----------|---------------------------|-------------|
| 0x47557C     | ~0x484B   | codeLengthScratch         | uint32[289+]: scratch array for code lengths (used by all 3 block types) |
| 0x475560-78  | 24B       | buildTableState           | Temporaries for InflateBuildDecodeTable (count, bits, base_ptr, table_ptr, symbol) |
| 0x475A7C     | 68B       | bitLengthCounts           | uint32[17]: count of codes at each bit length |
| 0x475AC0     | 68B       | nextCodeTable             | uint32[17]: next code value at each bit length |
| 0x475B04     | ~0x484B   | reversedCodes             | uint32[289+]: bit-reversed canonical codes |
| 0x475F84     | ~0x1000B  | litLenDecodeTable         | Literal/length Huffman decode table (512 entries x 8B = 4096B primary) |
| 0x476F84     | ~0x200B   | distDecodeTable           | Distance Huffman decode table (64 entries x 8B = 512B primary) |
| 0x477184     | ~0x3A00B  | huffTreeNodePool          | Overflow binary tree nodes for codes exceeding primary table bits |
| 0x004c3760   | ptr       | g_pZipInputBuffer         | malloc'd 64KB input buffer for reading compressed data from ZIP |
| 0x004c3764   | ptr       | g_pDecompressOutputBuffer | Output destination pointer (set by ReadArchiveEntry caller) |

## Huffman Decode Table Format

Each entry in the primary decode table is 8 bytes:
```
struct HuffmanEntry {
    uint8_t  bits;      // +0: number of bits consumed (or 0x10 = tree node marker)
    uint8_t  pad;       // +1: unused
    uint16_t symbol;    // +2: decoded symbol value
    uint32_t childPtr;  // +4: pointer to tree node pair (only if bits == 0x10)
};
```

For codes that fit within the primary table width (9 bits for lit/len, 6 bits for distance):
- `bits` = actual code length, `symbol` = decoded value
- Duplicated across all bit-patterns that share the same prefix

For codes longer than the primary table width:
- `bits` = 0x10 (marker), `childPtr` = pointer to a pair of uint32 tree nodes
- Tree nodes: `node[0]` = left child (bit=0), `node[1]` = right child (bit=1)
- Leaf nodes store the symbol value directly (< 0x121)
- Internal nodes store pointer to next node pair (>= 0x121)
- Sentinel value 0x121 = uninitialized/error

## ZIP Layer (above inflate)

| Address    | Name                         | Purpose |
|------------|------------------------------|---------|
| 0x43FC80   | ParseZipCentralDirectory     | Scan ZIP end-of-central-dir (0x06054B50) + central dir entries (0x02014B50), find named entry, set DAT_004cf988 = local header file offset |
| 0x4405B0   | DecompressZipEntry           | Read ZIP local header (0x04034B50), dispatch method 0 or 8, verify CRC-32 |
| 0x440790   | ReadArchiveEntry             | Top-level: try direct file read first, else find in ZIP + decompress |
| 0x4409B0   | GetArchiveEntrySize          | Top-level: try direct fseek/ftell, else parse ZIP central dir for uncompressed size |
| 0x43FB70   | ReadTrackStaticDataChunk     | fread 64KB into input buffer (called by inflate input refill) |
| 0x43FB90   | ReadCompressedTrackStreamChunk | fwrite output chunk to streaming file handle |
| 0x43FBC0   | DecompressTrackDataStream    | Read N bytes from central directory stream (byte-at-a-time from input buffer) |

## ZIP State Globals

| Address      | Type     | Description |
|--------------|----------|-------------|
| 0x4cf97c     | FILE*    | Current ZIP file handle |
| 0x4cf978     | uint32   | Remaining bytes in central directory stream |
| 0x4cf980     | uint32   | Last central directory chunk read size |
| 0x4cf984     | uint32   | Current file seek position for central directory reads |
| 0x4cf988     | uint32   | Local file header offset (from central directory) |
| 0x4cf974     | FILE*    | Streaming output file handle (for streaming mode decompression) |

## Dual-Mode Operation

The inflate library supports two output modes controlled by `DAT_0047b1ec` (streamingModeFlag):

1. **Direct mode** (flag == 0): Output bytes written directly to `g_pDecompressOutputBuffer`.
   Used by `ReadArchiveEntry` for in-memory decompression of ZIP entries.

2. **Streaming mode** (flag != 0): Output flushed via `InflateWriteOutputChunk` -> `ReadCompressedTrackStreamChunk`
   -> `FUN_00448512` (fwrite wrapper). Used by `ParseZipCentralDirectory` for scanning
   encrypted/compressed central directory entries without full decompression to memory.

## CRC-32 Verification

- CRC-32 polynomial: 0xEDB88320 (reflected, same as PKZIP/zlib)
- Expected CRC read from ZIP local header offset +0x0E (stored in DAT_0047b1dc)
- Running CRC computed in `InflateFlushOutputAndUpdateCrc32` every 32KB window flush
- Final check: `DAT_0047b1d8 == DAT_0047b1dc` in `InflateDecompress`; mismatch returns 0 (failure)
