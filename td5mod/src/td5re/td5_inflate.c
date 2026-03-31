/**
 * td5_inflate.c -- DEFLATE decompressor bridge
 *
 * Provides td5_inflate_mem_to_mem() which the asset module uses for ZIP
 * decompression. This file bridges to whichever inflate implementation
 * is available in the build.
 *
 * Priority:
 *   1. If MINIZ_HEADER_FILE_ONLY is not defined and miniz.h is found,
 *      use tinfl_decompress_mem_to_mem directly.
 *   2. If TD5_INFLATE_USE_ZLIB is defined, use zlib's inflate.
 *   3. Otherwise, provide a self-contained implementation based on
 *      the public-domain tinfl decompressor from miniz.
 *
 * For the TD5RE source port, option 3 is the default -- we embed a
 * minimal standalone inflate that handles all DEFLATE streams used by
 * TD5's ZIP archives (stored, fixed Huffman, dynamic Huffman).
 */

#include "td5_inflate.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ========================================================================
 * Option 1: Use miniz if available (add miniz.c to the build)
 * ======================================================================== */

#if defined(TD5_INFLATE_USE_MINIZ)

#include "miniz.h"

size_t td5_inflate_mem_to_mem(void *out_buf, size_t out_buf_len,
                              const void *src_buf, size_t src_buf_len)
{
    return tinfl_decompress_mem_to_mem(out_buf, out_buf_len,
                                       src_buf, src_buf_len, 0);
}

/* ========================================================================
 * Option 2: Use system zlib
 * ======================================================================== */

#elif defined(TD5_INFLATE_USE_ZLIB)

#include <zlib.h>

size_t td5_inflate_mem_to_mem(void *out_buf, size_t out_buf_len,
                              const void *src_buf, size_t src_buf_len)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    strm.next_in  = (Bytef *)src_buf;
    strm.avail_in = (uInt)src_buf_len;
    strm.next_out = (Bytef *)out_buf;
    strm.avail_out = (uInt)out_buf_len;

    /* -MAX_WBITS = raw deflate (no zlib header) */
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
        return 0;

    int ret = inflate(&strm, Z_FINISH);
    size_t result = strm.total_out;
    inflateEnd(&strm);

    if (ret != Z_STREAM_END)
        return 0;

    return result;
}

/* ========================================================================
 * Option 3: Self-contained DEFLATE decompressor
 *
 * Based on the public-domain tinfl decompressor by Rich Geldreich
 * (extracted from miniz, simplified for our single use case).
 *
 * This handles all three DEFLATE block types:
 *   - Type 0: Stored (uncompressed)
 *   - Type 1: Fixed Huffman codes
 *   - Type 2: Dynamic Huffman codes
 *
 * Limitations vs full miniz:
 *   - Memory-to-memory only (no streaming)
 *   - Output buffer must be large enough for the entire result
 *   - No zlib/gzip header parsing (raw DEFLATE only, which is what ZIP uses)
 * ======================================================================== */

#else

/* Huffman decoding tables */
#define TINFL_MAX_HUFF_TABLES    3
#define TINFL_MAX_HUFF_SYMBOLS_0 288
#define TINFL_MAX_HUFF_SYMBOLS_1 32
#define TINFL_MAX_HUFF_SYMBOLS_2 19
#define TINFL_FAST_LOOKUP_BITS   10
#define TINFL_FAST_LOOKUP_SIZE   (1 << TINFL_FAST_LOOKUP_BITS)
#define TINFL_LZ_DICT_SIZE       32768

typedef struct {
    uint8_t  code_size[TINFL_MAX_HUFF_SYMBOLS_0];
    int16_t  look_up[TINFL_FAST_LOOKUP_SIZE];
    int16_t  tree[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
} tinfl_huff_table;

typedef struct {
    uint32_t       num_bits, bit_buf;
    const uint8_t *in_ptr, *in_end;
    uint8_t       *out_ptr, *out_start, *out_end;
    tinfl_huff_table tables[TINFL_MAX_HUFF_TABLES];
} tinfl_state;

static const uint16_t s_length_base[31] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258,0,0
};
static const uint8_t s_length_extra[31] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0
};
static const uint16_t s_dist_base[32] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,0,0
};
static const uint8_t s_dist_extra[32] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,0,0
};
static const uint8_t s_length_dezigzag[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int tinfl_get_bits(tinfl_state *s, int n)
{
    while (s->num_bits < (uint32_t)n) {
        if (s->in_ptr >= s->in_end) return -1;
        s->bit_buf |= (uint32_t)(*s->in_ptr++) << s->num_bits;
        s->num_bits += 8;
    }
    int val = (int)(s->bit_buf & ((1u << n) - 1));
    s->bit_buf >>= n;
    s->num_bits -= (uint32_t)n;
    return val;
}

static int tinfl_build_table(tinfl_huff_table *t,
                             const uint8_t *code_sizes, int num_syms)
{
    int i, j, k;
    uint16_t next_code[17];
    uint16_t total_syms[17];

    memset(t->look_up, 0, sizeof(t->look_up));
    memset(t->tree, 0, sizeof(t->tree));
    memset(total_syms, 0, sizeof(total_syms));
    memset(t->code_size, 0, sizeof(t->code_size));

    for (i = 0; i < num_syms; i++) {
        t->code_size[i] = code_sizes[i];
        if (code_sizes[i])
            total_syms[code_sizes[i]]++;
    }

    uint32_t code = 0;
    next_code[0] = next_code[1] = 0;
    for (i = 1; i <= 15; i++) {
        code = (code + total_syms[i]) << 1;
        next_code[i + 1] = (uint16_t)code;
    }

    /* Check for valid code */
    if (code != (uint32_t)(1 << 16) && (code - total_syms[15]) != 0) {
        /* Might be an incomplete tree -- allow it for single-code tables */
    }

    for (i = 0; i < num_syms; i++) {
        int sz = code_sizes[i];
        if (!sz) continue;
        uint32_t c = next_code[sz]++;
        uint32_t rev = 0;
        for (j = 0; j < sz; j++)
            rev = (rev << 1) | ((c >> j) & 1);

        if (sz <= TINFL_FAST_LOOKUP_BITS) {
            int16_t entry = (int16_t)((sz << 9) | i);
            for (k = rev; k < TINFL_FAST_LOOKUP_SIZE; k += (1 << sz))
                t->look_up[k] = entry;
        } else {
            int16_t *slot = &t->look_up[rev & (TINFL_FAST_LOOKUP_SIZE - 1)];
            if (*slot == 0) {
                /* Find next free tree node pair */
                int tree_next = -1;
                for (k = 0; k < TINFL_MAX_HUFF_SYMBOLS_0 * 2 - 2; k += 2) {
                    if (t->tree[k] == 0 && t->tree[k + 1] == 0) {
                        tree_next = k;
                        break;
                    }
                }
                if (tree_next < 0) return -1;
                *slot = (int16_t)(-(tree_next + 1));
            }

            int node = -(*slot) - 1;
            for (j = TINFL_FAST_LOOKUP_BITS; j < sz - 1; j++) {
                int bit = (int)(rev >> j) & 1;
                if (t->tree[node + bit] == 0) {
                    int next_free = -1;
                    for (k = node + 2; k < TINFL_MAX_HUFF_SYMBOLS_0 * 2 - 2; k += 2) {
                        if (t->tree[k] == 0 && t->tree[k + 1] == 0) {
                            next_free = k;
                            break;
                        }
                    }
                    if (next_free < 0) return -1;
                    t->tree[node + bit] = (int16_t)(-(next_free + 1));
                    node = next_free;
                } else {
                    node = -t->tree[node + bit] - 1;
                }
            }
            int bit = (int)(rev >> (sz - 1)) & 1;
            t->tree[node + bit] = (int16_t)i;
        }
    }
    return 0;
}

static int tinfl_decode_sym(tinfl_state *s, tinfl_huff_table *t)
{
    while (s->num_bits < 15) {
        if (s->in_ptr >= s->in_end) break;
        s->bit_buf |= (uint32_t)(*s->in_ptr++) << s->num_bits;
        s->num_bits += 8;
    }

    int16_t entry = t->look_up[s->bit_buf & (TINFL_FAST_LOOKUP_SIZE - 1)];
    if (entry >= 0) {
        int sz = entry >> 9;
        s->bit_buf >>= sz;
        s->num_bits -= (uint32_t)sz;
        return entry & 0x1FF;
    }

    /* Walk the tree for longer codes */
    int node = -entry - 1;
    int bits_used = TINFL_FAST_LOOKUP_BITS;
    do {
        int bit = (int)(s->bit_buf >> bits_used) & 1;
        bits_used++;
        int16_t next = t->tree[node + bit];
        if (next >= 0) {
            s->bit_buf >>= bits_used;
            s->num_bits -= (uint32_t)bits_used;
            return next;
        }
        node = -next - 1;
    } while (bits_used < 16);

    return -1; /* decode error */
}

static void tinfl_build_fixed_tables(tinfl_state *s)
{
    uint8_t lit_sizes[288];
    uint8_t dist_sizes[32];
    int i;

    for (i = 0;   i <= 143; i++) lit_sizes[i] = 8;
    for (i = 144; i <= 255; i++) lit_sizes[i] = 9;
    for (i = 256; i <= 279; i++) lit_sizes[i] = 7;
    for (i = 280; i <= 287; i++) lit_sizes[i] = 8;
    tinfl_build_table(&s->tables[0], lit_sizes, 288);

    for (i = 0; i < 32; i++) dist_sizes[i] = 5;
    tinfl_build_table(&s->tables[1], dist_sizes, 32);
}

static int tinfl_inflate_block(tinfl_state *s, int is_dynamic)
{
    if (is_dynamic) {
        int num_lit = tinfl_get_bits(s, 5) + 257;
        int num_dist = tinfl_get_bits(s, 5) + 1;
        int num_code_len = tinfl_get_bits(s, 4) + 4;
        if (num_lit < 0 || num_dist < 0 || num_code_len < 0) return -1;

        uint8_t code_len_sizes[19];
        memset(code_len_sizes, 0, sizeof(code_len_sizes));
        for (int i = 0; i < num_code_len; i++) {
            int v = tinfl_get_bits(s, 3);
            if (v < 0) return -1;
            code_len_sizes[s_length_dezigzag[i]] = (uint8_t)v;
        }
        tinfl_build_table(&s->tables[2], code_len_sizes, 19);

        uint8_t all_sizes[288 + 32];
        memset(all_sizes, 0, sizeof(all_sizes));
        int total = num_lit + num_dist;
        int idx = 0;

        while (idx < total) {
            int sym = tinfl_decode_sym(s, &s->tables[2]);
            if (sym < 0) return -1;

            if (sym < 16) {
                all_sizes[idx++] = (uint8_t)sym;
            } else if (sym == 16) {
                int rep = tinfl_get_bits(s, 2) + 3;
                if (rep < 0 || idx == 0) return -1;
                uint8_t prev = all_sizes[idx - 1];
                for (int i = 0; i < rep && idx < total; i++)
                    all_sizes[idx++] = prev;
            } else if (sym == 17) {
                int rep = tinfl_get_bits(s, 3) + 3;
                if (rep < 0) return -1;
                for (int i = 0; i < rep && idx < total; i++)
                    all_sizes[idx++] = 0;
            } else { /* sym == 18 */
                int rep = tinfl_get_bits(s, 7) + 11;
                if (rep < 0) return -1;
                for (int i = 0; i < rep && idx < total; i++)
                    all_sizes[idx++] = 0;
            }
        }

        tinfl_build_table(&s->tables[0], all_sizes, num_lit);
        tinfl_build_table(&s->tables[1], all_sizes + num_lit, num_dist);
    } else {
        tinfl_build_fixed_tables(s);
    }

    /* Decode lit/len + distance symbols */
    for (;;) {
        int sym = tinfl_decode_sym(s, &s->tables[0]);
        if (sym < 0) return -1;

        if (sym < 256) {
            if (s->out_ptr >= s->out_end) return -1;
            *s->out_ptr++ = (uint8_t)sym;
        } else if (sym == 256) {
            return 0; /* end of block */
        } else {
            sym -= 257;
            if (sym >= 29) return -1;

            int length = (int)s_length_base[sym];
            int extra = s_length_extra[sym];
            if (extra) {
                int eb = tinfl_get_bits(s, extra);
                if (eb < 0) return -1;
                length += eb;
            }

            int dist_sym = tinfl_decode_sym(s, &s->tables[1]);
            if (dist_sym < 0 || dist_sym >= 30) return -1;

            int distance = (int)s_dist_base[dist_sym];
            extra = s_dist_extra[dist_sym];
            if (extra) {
                int eb = tinfl_get_bits(s, extra);
                if (eb < 0) return -1;
                distance += eb;
            }

            /* Copy from back-reference */
            uint8_t *src = s->out_ptr - distance;
            if (src < s->out_start) return -1;
            if (s->out_ptr + length > s->out_end) return -1;

            for (int i = 0; i < length; i++)
                s->out_ptr[i] = src[i]; /* byte-by-byte for overlapping refs */
            s->out_ptr += length;
        }
    }
}

size_t td5_inflate_mem_to_mem(void *out_buf, size_t out_buf_len,
                              const void *src_buf, size_t src_buf_len)
{
    if (!out_buf || !src_buf || out_buf_len == 0 || src_buf_len == 0)
        return 0;

    tinfl_state state;
    memset(&state, 0, sizeof(state));

    state.in_ptr   = (const uint8_t *)src_buf;
    state.in_end   = state.in_ptr + src_buf_len;
    state.out_start = (uint8_t *)out_buf;
    state.out_ptr  = state.out_start;
    state.out_end  = state.out_start + out_buf_len;
    state.bit_buf  = 0;
    state.num_bits = 0;

    int bfinal;
    do {
        bfinal = tinfl_get_bits(&state, 1);
        int btype = tinfl_get_bits(&state, 2);
        if (bfinal < 0 || btype < 0) return 0;

        if (btype == 0) {
            /* Stored block: discard remaining bits in current byte */
            state.bit_buf = 0;
            state.num_bits = 0;

            if (state.in_ptr + 4 > state.in_end) return 0;
            uint16_t len  = (uint16_t)(state.in_ptr[0] | (state.in_ptr[1] << 8));
            /* uint16_t nlen -- complement, skip validation */
            state.in_ptr += 4;

            if (state.in_ptr + len > state.in_end) return 0;
            if (state.out_ptr + len > state.out_end) return 0;
            memcpy(state.out_ptr, state.in_ptr, len);
            state.in_ptr += len;
            state.out_ptr += len;
        }
        else if (btype == 1 || btype == 2) {
            if (tinfl_inflate_block(&state, btype == 2) != 0)
                return 0;
        }
        else {
            return 0; /* invalid block type */
        }
    } while (!bfinal);

    return (size_t)(state.out_ptr - state.out_start);
}

#endif /* inflate backend selection */
