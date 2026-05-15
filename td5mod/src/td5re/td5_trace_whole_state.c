/*
 * td5_trace_whole_state.c -- Tick-aligned whole-state snapshot writer.
 *
 * Output: append-only binary file consumed by tools/diff_whole_state.py.
 * See td5_trace_whole_state.h and re/analysis/whole_state_diff_design.md.
 */

#include "td5_trace_whole_state.h"

#include "td5_platform.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "trace_ws"

/* ---- Format constants -- MUST MATCH frida_whole_state_snapshot.js ----- */
#define WS_FILE_MAGIC          0x57354454u   /* 'TD5W' little-endian       */
#define WS_FORMAT_VERSION      ((uint16_t)1)
#define WS_ACTOR_COUNT         ((uint16_t)6)
#define WS_ACTOR_STRIDE        ((uint16_t)0x0388)
#define WS_GLOBALS_BYTES       128
#define WS_FILE_HEADER_BYTES   16
#define WS_RECORD_HEADER_BYTES 8
#define WS_RECORD_BYTES        (WS_RECORD_HEADER_BYTES + \
                                (WS_ACTOR_COUNT * WS_ACTOR_STRIDE) + \
                                WS_GLOBALS_BYTES)
/* = 5560 */

/* ---- Module state ----------------------------------------------------- */
static FILE   *s_fp;
static int     s_max_ticks;
static int     s_ticks_written;
static int     s_closed;

/* ---- Helpers ---------------------------------------------------------- */
static void ws_write_le_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void ws_write_le_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
}
static void ws_write_le_f32(uint8_t *p, float f) {
    union { float f; uint32_t u; } u;
    u.f = f;
    ws_write_le_u32(p, u.u);
}

static void ws_write_file_header(void) {
    uint8_t hdr[WS_FILE_HEADER_BYTES];
    memset(hdr, 0, sizeof(hdr));
    ws_write_le_u32(hdr + 0,  WS_FILE_MAGIC);
    ws_write_le_u16(hdr + 4,  WS_FORMAT_VERSION);
    ws_write_le_u16(hdr + 6,  WS_ACTOR_COUNT);
    ws_write_le_u16(hdr + 8,  WS_ACTOR_STRIDE);
    ws_write_le_u16(hdr + 10, 0);
    ws_write_le_u32(hdr + 12, 0);  /* record_count - left at 0; diff reads to EOF */
    fwrite(hdr, 1, sizeof(hdr), s_fp);
    fflush(s_fp);
}

/* ---- Globals-blob serializer ----------------------------------------- */
/* Offsets here MUST match frida_whole_state_snapshot.js fillGlobalsBlob(). */
static void ws_pack_globals(uint8_t blob[WS_GLOBALS_BYTES],
                            const TD5_WholeStateGlobals *g)
{
    memset(blob, 0, WS_GLOBALS_BYTES);
    if (!g) return;
    ws_write_le_u32(blob + 0x00, (uint32_t)g->game_state);
    ws_write_le_u32(blob + 0x04, (uint32_t)g->game_paused);
    ws_write_le_u32(blob + 0x08, (uint32_t)g->race_end_fade_state);
    ws_write_le_u32(blob + 0x0C, g->sim_time_accumulator);
    ws_write_le_f32(blob + 0x10, g->sim_tick_budget);
    ws_write_le_u32(blob + 0x14, (uint32_t)g->simulation_tick_counter);
    ws_write_le_f32(blob + 0x18, g->normalized_frame_dt);
    ws_write_le_f32(blob + 0x1C, g->instant_fps);
    ws_write_le_u32(blob + 0x20, (uint32_t)g->view_count);
    ws_write_le_u32(blob + 0x24, (uint32_t)g->splitscreen_count);
    ws_write_le_u32(blob + 0x28, (uint32_t)g->random_seed_for_race);
    ws_write_le_u32(blob + 0x2C, (uint32_t)g->race_session_random_seed);
    ws_write_le_u32(blob + 0x30, (uint32_t)g->race_random_seed_table0);
    ws_write_le_u32(blob + 0x34, g->player_control_bits[0]);
    ws_write_le_u32(blob + 0x38, g->player_control_bits[1]);
    /* 0x3C..0x53: race_slot_state_table[6] (24 bytes, 6 x u32) */
    for (int i = 0; i < 6; i++) {
        ws_write_le_u32(blob + 0x3C + (i * 4), g->race_slot_state_table[i]);
    }
    /* 0x54..0x59: race_slot_player_flags[6] */
    for (int i = 0; i < 6; i++) {
        blob[0x54 + i] = g->race_slot_player_flags[i];
    }
    /* 0x5A..0x5F: race_order_array[6] */
    for (int i = 0; i < 6; i++) {
        blob[0x5A + i] = g->race_order_array[i];
    }
    /* 0x60..0x7F: reserved (already zero from memset). */
}

/* ---- Public API ------------------------------------------------------- */

int td5_trace_whole_state_open(const char *out_path, int max_ticks) {
    if (s_fp) {
        TD5_LOG_W(LOG_TAG, "already open; close before reopening");
        return 1;
    }
    s_closed       = 0;
    s_ticks_written = 0;
    s_max_ticks    = (max_ticks > 0) ? max_ticks : 0;

    const char *path = (out_path && out_path[0]) ? out_path
                                                 : "log/whole_state_port.bin";
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        TD5_LOG_E(LOG_TAG, "fopen(%s) failed", path);
        return 0;
    }
    /* 256 KiB block buffer -- absorbs ~46 records of buffered I/O. */
    static char s_io_buf[256 * 1024];
    setvbuf(s_fp, s_io_buf, _IOFBF, sizeof(s_io_buf));

    ws_write_file_header();
    TD5_LOG_I(LOG_TAG, "opened %s (max_ticks=%d)", path, s_max_ticks);
    return 1;
}

int td5_trace_whole_state_is_open(void) {
    return (s_fp != NULL && !s_closed);
}

void td5_trace_whole_state_emit(uint32_t frame, uint32_t sim_tick,
                                const void *actor_base,
                                const TD5_WholeStateGlobals *globals)
{
    if (!s_fp || s_closed) return;
    if (!actor_base)       return;
    if (s_max_ticks > 0 && s_ticks_written >= s_max_ticks) {
        td5_trace_whole_state_close();
        return;
    }

    /* Build the record into a single stack buffer, then write once. */
    uint8_t rec[WS_RECORD_BYTES];

    /* Record header. */
    ws_write_le_u32(rec + 0, frame);
    ws_write_le_u32(rec + 4, sim_tick);

    /* Actors -- verbatim copy of 6 x 0x388 = 5424 bytes. */
    const size_t actors_off  = WS_RECORD_HEADER_BYTES;
    const size_t actors_size = (size_t)WS_ACTOR_COUNT * WS_ACTOR_STRIDE;
    memcpy(rec + actors_off, actor_base, actors_size);

    /* Globals blob. */
    ws_pack_globals(rec + actors_off + actors_size, globals);

    fwrite(rec, 1, sizeof(rec), s_fp);
    s_ticks_written++;
    if ((s_ticks_written & 0x3F) == 0) fflush(s_fp);
}

void td5_trace_whole_state_close(void) {
    if (!s_fp || s_closed) return;
    fflush(s_fp);
    fclose(s_fp);
    s_fp = NULL;
    s_closed = 1;
    TD5_LOG_I(LOG_TAG, "closed; records=%d", s_ticks_written);
}
