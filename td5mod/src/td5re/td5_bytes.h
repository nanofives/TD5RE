/*
 * td5_bytes.h -- shared little-endian byte-buffer read/write helpers.
 *
 * [2026-07-09, A10 refactor] Consolidates the several independent
 * byte[0] | byte[1]<<8 | ... implementations that had accreted across
 * td5_asset.c (ZIP central-directory parsing), td5_save.c (config
 * serialization), td5_fe_carstats.c (config.nfo field decode),
 * td5_ai.c (route-state queue span reads), and td5_inflate.c (DEFLATE
 * block-length reads) into one place.
 */

#ifndef TD5_BYTES_H
#define TD5_BYTES_H

#include <stdint.h>

static inline uint16_t td5_read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t td5_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Signed convenience wrappers -- several call sites read a little-endian
 * int16 out of a route-state/config scratch buffer. */
static inline int16_t td5_read_le16s(const uint8_t *p)
{
    return (int16_t)td5_read_le16(p);
}

static inline int32_t td5_read_le32s(const uint8_t *p)
{
    return (int32_t)td5_read_le32(p);
}

static inline void td5_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void td5_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif /* TD5_BYTES_H */
