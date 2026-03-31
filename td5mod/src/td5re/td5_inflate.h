/**
 * td5_inflate.h -- Minimal DEFLATE decompressor for ZIP archive reading
 *
 * This provides a single function: td5_inflate_mem_to_mem()
 * which decompresses raw DEFLATE data (no zlib/gzip headers).
 *
 * Implementation uses miniz's tinfl decompressor. To use:
 *   1. Add miniz.c from https://github.com/richgel999/miniz to the build, OR
 *   2. Define TD5_INFLATE_USE_ZLIB and link against zlib
 *
 * If neither is available, a compile error is produced with instructions.
 */

#ifndef TD5_INFLATE_H
#define TD5_INFLATE_H

#include <stddef.h>

/**
 * Decompress raw DEFLATE data from src_buf to out_buf.
 *
 * @param out_buf      Output buffer (must be at least out_buf_len bytes)
 * @param out_buf_len  Expected uncompressed size
 * @param src_buf      Compressed DEFLATE data (raw, no zlib header)
 * @param src_buf_len  Compressed data length
 * @return             Actual decompressed size, or 0 on failure
 */
size_t td5_inflate_mem_to_mem(void *out_buf, size_t out_buf_len,
                              const void *src_buf, size_t src_buf_len);

#endif /* TD5_INFLATE_H */
