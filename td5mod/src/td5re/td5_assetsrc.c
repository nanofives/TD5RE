/**
 * td5_assetsrc.c -- Editable-source "pack-on-load" asset layer.
 *
 * See td5_assetsrc.h for the design. This module turns an editable source file
 * into the binary .DAT image the existing parsers expect, entirely in memory,
 * so the loose .DAT files can be retired from re/assets/.
 *
 * Each format registers a descriptor mapping the bare .DAT name (case-
 * insensitive) to its editable source filename and an encoder. The source
 * directory is resolved with the SAME logic the loose/extracted loader steps
 * use (td5_asset_resolve_source_path in td5_asset.c), so forward/reverse level
 * selection and the car/static folder mapping all keep working.
 *
 * cJSON (vendored, deps/cjson/) parses the table/geometry JSON. Integer
 * fidelity: values are read from the parsed double (cJSON_GetNumberValue) and
 * rounded -- never from valueint, which cJSON clamps to INT_MAX (would corrupt
 * u32 fields such as sky_animation_index = 0xFFFFFFFF).
 */
#include "td5_assetsrc.h"
#include "td5_asset.h"            /* td5_asset_resolve_source_path */
#include "deps/cjson/cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>      /* car-dir enumeration for the self-test */

#ifdef _WIN32
#  define td5_src_stricmp _stricmp
#else
#  include <strings.h>
#  define td5_src_stricmp strcasecmp
#endif

/* ========================================================================
 * Small file / JSON helpers
 * ======================================================================== */

/* Read a whole file into a malloc'd, NUL-terminated buffer (for cJSON_Parse).
 * Uses stdio directly so the --selftest path works before any engine init. */
static unsigned char *td5_src_read_file(const char *path, int *out_size)
{
    if (out_size) *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_size) *out_size = (int)nread;
    return buf;
}

static int td5_src_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* A field's value node: the Python editors wrap each field as {"value": X, ...};
 * also accept a bare value so hand-written compact JSON works. */
static const cJSON *td5_json_value_node(const cJSON *field)
{
    if (!field) return NULL;
    if (cJSON_IsObject(field))
        return cJSON_GetObjectItemCaseSensitive(field, "value");
    return field;
}

/* Round a JSON number to a 64-bit integer (no math.h dependency). All values
 * here are integers stored exactly as doubles (< 2^53), so this is exact. */
static long long td5_json_round(const cJSON *num)
{
    double d = cJSON_GetNumberValue(num);
    return (long long)(d >= 0.0 ? d + 0.5 : d - 0.5);
}

/* Write `size` little-endian bytes of v (two's complement covers signed). */
static void td5_put_le(unsigned char *out, int offset, int size, long long v)
{
    unsigned long long uv = (unsigned long long)v;
    for (int k = 0; k < size; k++)
        out[offset + k] = (unsigned char)((uv >> (8 * k)) & 0xFFu);
}

static int td5_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Byte length a hex string decodes to, or -1 if its length is odd. */
static int td5_hex_len(const char *s)
{
    size_t n = strlen(s);
    return (n % 2u) ? -1 : (int)(n / 2u);
}

/* Decode hex string into dst; returns bytes written, or -1 on bad input. */
static int td5_hex_decode(const char *s, unsigned char *dst)
{
    size_t n = strlen(s);
    if (n % 2u) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = td5_hexval((unsigned char)s[i]);
        int lo = td5_hexval((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        dst[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return (int)(n / 2u);
}

/* ========================================================================
 * Tier 1: LEVELINF.DAT (100 bytes) -- mirrors re/tools/levelinf_editor.py
 *
 * Every byte 0x00..0x63 is covered by a field (incl. padding), so a fully
 * populated levelinf.json deterministically reproduces the original 100 bytes.
 * ======================================================================== */

#define TD5_LEVELINF_SIZE 0x64

typedef struct {
    const char *name;
    int         offset;
    int         size;   /* 1, 2 or 4 */
    int         count;  /* 1 = scalar, >1 = array */
} td5_field_def;

static const td5_field_def k_levelinf_fields[] = {
    { "track_type",          0x00, 4, 1  },
    { "smoke_enable",        0x04, 4, 1  },
    { "checkpoint_count",    0x08, 4, 1  },
    { "checkpoint_spans",    0x0C, 4, 7  },
    { "weather_type",        0x28, 4, 1  },
    { "density_pair_count",  0x2C, 4, 1  },
    { "traffic_enable",      0x30, 4, 1  },
    { "density_pairs",       0x34, 2, 12 },
    { "_pad_4C",             0x4C, 1, 8  },
    { "sky_animation_index", 0x54, 4, 1  },
    { "total_span_count",    0x58, 4, 1  },
    { "fog_enabled",         0x5C, 4, 1  },
    { "fog_color_r",         0x60, 1, 1  },
    { "fog_color_g",         0x61, 1, 1  },
    { "fog_color_b",         0x62, 1, 1  },
    { "_pad_63",             0x63, 1, 1  },
};

/* Encode a fixed-layout flat-struct format from a {field:{value}} JSON object.
 * Zero-inits `total` bytes then writes each field at its offset, matching the
 * Python write_all_fields discipline. Returns 0 on success. */
static int td5_encode_flat_struct(const char *src_path,
                                  const td5_field_def *fields, size_t nfields,
                                  int total, void **out_buf, int *out_size)
{
    unsigned char *text = td5_src_read_file(src_path, NULL);
    if (!text) return 1;
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    if (!root) return 1;

    unsigned char *out = (unsigned char *)calloc(1, (size_t)total);
    if (!out) { cJSON_Delete(root); return 1; }

    int ok = 1;
    for (size_t i = 0; i < nfields && ok; i++) {
        const td5_field_def *f   = &fields[i];
        const cJSON         *val = td5_json_value_node(
                                       cJSON_GetObjectItemCaseSensitive(root, f->name));
        if (!val) { ok = 0; break; }

        if (f->count == 1) {
            if (!cJSON_IsNumber(val)) { ok = 0; break; }
            td5_put_le(out, f->offset, f->size, td5_json_round(val));
        } else {
            if (!cJSON_IsArray(val) || cJSON_GetArraySize(val) != f->count) {
                ok = 0; break;
            }
            int idx = 0;
            const cJSON *el = NULL;
            cJSON_ArrayForEach(el, val) {
                if (!cJSON_IsNumber(el)) { ok = 0; break; }
                td5_put_le(out, f->offset + idx * f->size, f->size,
                           td5_json_round(el));
                idx++;
            }
        }
    }

    cJSON_Delete(root);
    if (!ok) { free(out); return 1; }
    *out_buf  = out;
    *out_size = total;
    return 0;
}

static int td5_src_encode_levelinf(const char *src_path, const char *zip_path,
                                   void **out_buf, int *out_size)
{
    (void)zip_path;
    return td5_encode_flat_struct(src_path, k_levelinf_fields,
                                  sizeof(k_levelinf_fields) /
                                      sizeof(k_levelinf_fields[0]),
                                  TD5_LEVELINF_SIZE, out_buf, out_size);
}

/* ========================================================================
 * Tier 1: CARPARAM.DAT (268 bytes) -- mirrors re/tools/carparam_editor.py
 * (tuning 0x00..0x8B + physics 0x8C..0x10B; every byte covered).
 * ======================================================================== */

#define TD5_CARPARAM_SIZE 0x10C

static const td5_field_def k_carparam_fields[] = {
    /* ---- tuning table (0x00..0x8B) ---- */
    { "chassis_corners_front",     0x00, 2, 16 },
    { "chassis_corners_rear",      0x20, 2, 16 },
    { "wheel_pos_FL",              0x40, 2, 4  },
    { "wheel_pos_FR",              0x48, 2, 4  },
    { "wheel_pos_RL",              0x50, 2, 4  },
    { "wheel_pos_RR",              0x58, 2, 4  },
    { "traffic_alt_wheel_A",       0x60, 2, 8  },
    { "traffic_alt_wheel_B",       0x70, 2, 8  },
    { "chassis_top_y",             0x80, 2, 1  },
    { "suspension_height_ref",     0x82, 2, 1  },
    { "envelope_reference_y",      0x84, 2, 1  },
    { "traffic_y_offset",          0x86, 2, 1  },
    { "collision_mass",            0x88, 2, 1  },
    { "_pad_8A",                   0x8A, 2, 1  },
    /* ---- physics table (0x8C..0x10B) ---- */
    { "torque_curve",              0x8C, 2, 16 },
    { "vehicle_inertia",           0xAC, 4, 1  },
    { "half_wheelbase",            0xB0, 4, 1  },
    { "front_weight_dist",         0xB4, 2, 1  },
    { "rear_weight_dist",          0xB6, 2, 1  },
    { "drag_coefficient",          0xB8, 2, 1  },
    { "gear_ratio_table",          0xBA, 2, 8  },
    { "upshift_rpm_table",         0xCA, 2, 8  },
    { "downshift_rpm_table",       0xDA, 2, 8  },
    { "suspension_damping",        0xEA, 2, 1  },
    { "suspension_spring_rate",    0xEC, 2, 1  },
    { "suspension_feedback",       0xEE, 2, 1  },
    { "suspension_travel_limit",   0xF0, 2, 1  },
    { "suspension_response_factor",0xF2, 2, 1  },
    { "drive_torque_multiplier",   0xF4, 2, 1  },
    { "damping_low_speed",         0xF6, 2, 1  },
    { "damping_high_speed",        0xF8, 2, 1  },
    { "brake_force",               0xFA, 2, 1  },
    { "engine_brake_force",        0xFC, 2, 1  },
    { "max_rpm",                   0xFE, 2, 1  },
    { "top_speed_limit",           0x100,2, 1  },
    { "drivetrain_type",           0x102,2, 1  },
    { "speed_scale_factor",        0x104,2, 1  },
    { "handbrake_grip_modifier",   0x106,2, 1  },
    { "lateral_slip_stiffness",    0x108,2, 1  },
    { "_pad_7E",                   0x10A,2, 1  },
};

static int td5_src_encode_carparam(const char *src_path, const char *zip_path,
                                   void **out_buf, int *out_size)
{
    (void)zip_path;
    return td5_encode_flat_struct(src_path, k_carparam_fields,
                                  sizeof(k_carparam_fields) /
                                      sizeof(k_carparam_fields[0]),
                                  TD5_CARPARAM_SIZE, out_buf, out_size);
}

/* ========================================================================
 * Tier 1: CHECKPT.NUM (96 bytes) -- 4 rows x 6 columns of int32 LE.
 * Source JSON: { "_format":"td5_checkpt", "rows":[[6 ints] x 4] }.
 * ======================================================================== */

#define TD5_CHECKPT_SIZE 96

static int td5_src_encode_checkpt(const char *src_path, const char *zip_path,
                                  void **out_buf, int *out_size)
{
    (void)zip_path;
    unsigned char *text = td5_src_read_file(src_path, NULL);
    if (!text) return 1;
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    if (!root) return 1;

    const cJSON *rows = td5_json_value_node(
                            cJSON_GetObjectItemCaseSensitive(root, "rows"));
    int ok = (rows && cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 4);
    unsigned char *out = ok ? (unsigned char *)calloc(1, TD5_CHECKPT_SIZE) : NULL;
    if (out) {
        int r = 0;
        const cJSON *row = NULL;
        cJSON_ArrayForEach(row, rows) {
            if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 6) { ok = 0; break; }
            int c = 0;
            const cJSON *el = NULL;
            cJSON_ArrayForEach(el, row) {
                if (!cJSON_IsNumber(el)) { ok = 0; break; }
                td5_put_le(out, (r * 6 + c) * 4, 4, td5_json_round(el));
                c++;
            }
            if (!ok) break;
            r++;
        }
    }

    cJSON_Delete(root);
    if (!ok || !out) { free(out); return 1; }
    *out_buf  = out;
    *out_size = TD5_CHECKPT_SIZE;
    return 0;
}

/* ========================================================================
 * Tier 1: TRAFFIC.BUS / TRAFFICB.BUS -- N x 4-byte records:
 *   int16 span | uint8 flags | uint8 lane   (incl. the 0xFFFF sentinel record).
 * Source JSON: { "_format":"td5_traffic", "records":[[span,flags,lane], ...] }.
 * ======================================================================== */

static int td5_src_encode_traffic(const char *src_path, const char *zip_path,
                                  void **out_buf, int *out_size)
{
    (void)zip_path;
    unsigned char *text = td5_src_read_file(src_path, NULL);
    if (!text) return 1;
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    if (!root) return 1;

    const cJSON *recs = td5_json_value_node(
                            cJSON_GetObjectItemCaseSensitive(root, "records"));
    if (!recs || !cJSON_IsArray(recs)) { cJSON_Delete(root); return 1; }

    int n = cJSON_GetArraySize(recs);
    unsigned char *out = (unsigned char *)malloc((size_t)(n > 0 ? n * 4 : 1));
    int ok = (out != NULL);
    int i = 0;
    const cJSON *rec = NULL;
    if (ok) {
        cJSON_ArrayForEach(rec, recs) {
            const cJSON *a, *b, *c;
            if (!cJSON_IsArray(rec) || cJSON_GetArraySize(rec) != 3) { ok = 0; break; }
            a = cJSON_GetArrayItem(rec, 0);
            b = cJSON_GetArrayItem(rec, 1);
            c = cJSON_GetArrayItem(rec, 2);
            if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b) || !cJSON_IsNumber(c)) {
                ok = 0; break;
            }
            td5_put_le(out, i * 4 + 0, 2, td5_json_round(a));  /* span  */
            td5_put_le(out, i * 4 + 2, 1, td5_json_round(b));  /* flags */
            td5_put_le(out, i * 4 + 3, 1, td5_json_round(c));  /* lane  */
            i++;
        }
    }

    cJSON_Delete(root);
    if (!ok) { free(out); return 1; }
    *out_buf  = out;
    *out_size = n * 4;
    return 0;
}

/* ========================================================================
 * Tier 1: LEFT/RIGHT.TRK (+ B reverse variants) -- N x 3 raw bytes/span.
 * Source CSV: one "b0,b1,b2" row per span; '#'-prefixed and blank lines
 * are comments. Parsed without cJSON to keep routes trivially hand-editable.
 * ======================================================================== */

static int td5_src_encode_routes(const char *src_path, const char *zip_path,
                                 void **out_buf, int *out_size)
{
    (void)zip_path;
    int            fsz  = 0;
    unsigned char *text = td5_src_read_file(src_path, &fsz);
    if (!text) return 1;

    size_t         cap = 256, len = 0;
    unsigned char *out = (unsigned char *)malloc(cap);
    int            ok  = (out != NULL);
    char          *p   = (char *)text;

    while (ok && *p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        char *eol = p;
        if (*p == '\n') p++;

        while (line < eol && (*line == ' ' || *line == '\t' || *line == '\r'))
            line++;
        if (line >= eol || *line == '#')
            continue;   /* blank or comment */

        long v[3];
        int  got = 0;
        char *q  = line;
        while (got < 3 && q < eol) {
            char *end = NULL;
            long  val = strtol(q, &end, 10);
            if (end == q) break;
            v[got++] = val;
            q = end;
            while (q < eol && (*q == ',' || *q == ' ' || *q == '\t' || *q == '\r'))
                q++;
        }
        if (got != 3) { ok = 0; break; }

        if (len + 3 > cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(out, cap);
            if (!nb) { ok = 0; break; }
            out = nb;
        }
        out[len++] = (unsigned char)(v[0] & 0xFF);
        out[len++] = (unsigned char)(v[1] & 0xFF);
        out[len++] = (unsigned char)(v[2] & 0xFF);
    }

    free(text);
    if (!ok || len == 0) { free(out); return 1; }
    *out_buf  = out;
    *out_size = (int)len;
    return 0;
}

/* ========================================================================
 * Tier 2: STRIP.DAT / STRIPB.DAT -- track geometry, layout-faithful.
 * Source JSON (strip_tool.py): header[5] | pre_span_hex | spans[24B] |
 * pre_vertex_hex | vertices[6B] | tail_hex. Sections tile the file exactly, so
 * re-concatenation is byte-exact. See re/include/td5_level_formats.h.
 * ======================================================================== */

static int td5_src_encode_strip(const char *src_path, const char *zip_path,
                                void **out_buf, int *out_size)
{
    (void)zip_path;
    unsigned char *text = td5_src_read_file(src_path, NULL);
    if (!text) return 1;
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    if (!root) return 1;

    const cJSON *hdr      = td5_json_value_node(
                                cJSON_GetObjectItemCaseSensitive(root, "header"));
    const cJSON *spans    = cJSON_GetObjectItemCaseSensitive(root, "spans");
    const cJSON *verts    = cJSON_GetObjectItemCaseSensitive(root, "vertices");
    const cJSON *pre_span = cJSON_GetObjectItemCaseSensitive(root, "pre_span_hex");
    const cJSON *pre_vtx  = cJSON_GetObjectItemCaseSensitive(root, "pre_vertex_hex");
    const cJSON *tail     = cJSON_GetObjectItemCaseSensitive(root, "tail_hex");

    const char *pre_span_s = (pre_span && cJSON_IsString(pre_span)) ? pre_span->valuestring : "";
    const char *pre_vtx_s  = (pre_vtx  && cJSON_IsString(pre_vtx))  ? pre_vtx->valuestring  : "";
    const char *tail_s     = (tail     && cJSON_IsString(tail))     ? tail->valuestring     : "";

    int ok = (hdr && cJSON_IsArray(hdr) && cJSON_GetArraySize(hdr) == 5 &&
              spans && cJSON_IsArray(spans) && verts && cJSON_IsArray(verts));

    int pre_span_n = ok ? td5_hex_len(pre_span_s) : -1;
    int pre_vtx_n  = ok ? td5_hex_len(pre_vtx_s)  : -1;
    int tail_n     = ok ? td5_hex_len(tail_s)     : -1;
    if (pre_span_n < 0 || pre_vtx_n < 0 || tail_n < 0) ok = 0;

    int nspans = ok ? cJSON_GetArraySize(spans) : 0;
    int nverts = ok ? cJSON_GetArraySize(verts) : 0;
    int total  = ok ? (20 + pre_span_n + 24 * nspans + pre_vtx_n + 6 * nverts + tail_n) : 0;

    unsigned char *out = (ok && total > 0) ? (unsigned char *)malloc((size_t)total) : NULL;
    if (!out) ok = 0;

    int pos = 0;
    if (ok) {                                   /* header: 5 x u32 LE */
        int idx = 0; const cJSON *el = NULL;
        cJSON_ArrayForEach(el, hdr) {
            if (!cJSON_IsNumber(el)) { ok = 0; break; }
            td5_put_le(out, pos + idx * 4, 4, td5_json_round(el));
            idx++;
        }
        pos += 20;
    }
    if (ok && td5_hex_decode(pre_span_s, out + pos) == pre_span_n) pos += pre_span_n;
    else if (ok) ok = 0;

    /* total_spans (header[4]) — used to validate junction link targets below. */
    long long span_total = 0;
    {
        const cJSON *h4 = cJSON_GetArrayItem(hdr, 4);
        if (h4 && cJSON_IsNumber(h4)) span_total = td5_json_round(h4);
    }

    if (ok) {                                   /* spans: 24 bytes each */
        const cJSON *s = NULL;
        cJSON_ArrayForEach(s, spans) {
            long long f[11]; int j = 0; const cJSON *e = NULL;
            if (!cJSON_IsArray(s) || cJSON_GetArraySize(s) != 11) { ok = 0; break; }
            cJSON_ArrayForEach(e, s) {
                if (!cJSON_IsNumber(e)) { ok = 0; break; }
                f[j++] = td5_json_round(e);
            }
            if (!ok) break;

            /* --- TD6 backward-junction (span type 11) link fix ---------------
             * Field layout: f[6]=link_next(+0x08), f[7]=link_prev(+0x0A),
             * f[0]=span_type. Native TD5 stores a type-11 (main-road backward-
             * return) junction's BRANCH target in link_prev, with link_next = -1.
             * Migrated TD6 strips instead leave the target in link_next (pointing
             * at a real appended branch span >= ring_length) and carry garbage in
             * link_prev, so the TD5 engine + AI read a junk backward branch ->
             * invisible geometry / teleports / AI confusion near junctions on
             * every TD6 track (worst on London: 28 type-11 junctions). Mirror the
             * real target into the field TD5 reads.
             * Guard `link_next in [0,total)`: native type-11 spans have
             * link_next = -1 (out of range) so they are NOT touched -> the strip
             * round-trip stays byte-exact for faithful TD5 tracks. Types 8/9/10
             * already place their target in the field TD5 reads, so only 11 here.
             * See reference_td6_strip_backward_junction_linkfield_2026-06-08. */
            if (f[0] == 11 && span_total > 0 && f[6] >= 0 && f[6] < span_total) {
                f[7] = f[6];     /* link_prev = real branch target */
                f[6] = -1;       /* link_next = -1 sentinel (TD5 convention) */
            }

            td5_put_le(out, pos + 0,  1, f[0]);  td5_put_le(out, pos + 1,  1, f[1]);
            td5_put_le(out, pos + 2,  1, f[2]);  td5_put_le(out, pos + 3,  1, f[3]);
            td5_put_le(out, pos + 4,  2, f[4]);  td5_put_le(out, pos + 6,  2, f[5]);
            td5_put_le(out, pos + 8,  2, f[6]);  td5_put_le(out, pos + 10, 2, f[7]);
            td5_put_le(out, pos + 12, 4, f[8]);  td5_put_le(out, pos + 16, 4, f[9]);
            td5_put_le(out, pos + 20, 4, f[10]);
            pos += 24;
        }
    }
    if (ok && td5_hex_decode(pre_vtx_s, out + pos) == pre_vtx_n) pos += pre_vtx_n;
    else if (ok) ok = 0;

    if (ok) {                                   /* vertices: 6 bytes each */
        const cJSON *v = NULL;
        cJSON_ArrayForEach(v, verts) {
            long long f[3]; int j = 0; const cJSON *e = NULL;
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 3) { ok = 0; break; }
            cJSON_ArrayForEach(e, v) {
                if (!cJSON_IsNumber(e)) { ok = 0; break; }
                f[j++] = td5_json_round(e);
            }
            if (!ok) break;
            td5_put_le(out, pos + 0, 2, f[0]);
            td5_put_le(out, pos + 2, 2, f[1]);
            td5_put_le(out, pos + 4, 2, f[2]);
            pos += 6;
        }
    }
    if (ok && td5_hex_decode(tail_s, out + pos) == tail_n) pos += tail_n;
    else if (ok) ok = 0;

    if (ok && pos != total) ok = 0;

    cJSON_Delete(root);
    if (!ok) { free(out); return 1; }
    *out_buf  = out;
    *out_size = total;
    return 0;
}

/* ========================================================================
 * Tier 3a: TEXTURES.DAT -- per-level palettized track texture set.
 * Source dir textures.src/: textures.json (manifest: page_count, pages[] with
 * offset/pad_hex/type/palette_hex) + indices.bin (page_count*4096 raw indices).
 * Rebuilds [u32 count][u32 offsets[]][per page pad3|type|i32 pal|palette|4096 idx]
 * byte-exact. tpages are folded into TEXTURES.DAT so this covers them too.
 * ======================================================================== */

static int td5_src_encode_textures(const char *src_path, const char *zip_path,
                                   void **out_buf, int *out_size)
{
    (void)zip_path;

    /* Derive the source directory (strip the textures.json filename). */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", src_path);
    char *slash = NULL;
    for (char *p = dir; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (slash) *slash = '\0'; else dir[0] = '\0';

    unsigned char *text = td5_src_read_file(src_path, NULL);
    if (!text) return 1;
    cJSON *root = cJSON_Parse((const char *)text);
    free(text);
    if (!root) return 1;

    char idxpath[600];
    snprintf(idxpath, sizeof(idxpath), "%s/indices.bin", dir);
    int            idx_sz = 0;
    unsigned char *idxbuf = td5_src_read_file(idxpath, &idx_sz);

    const cJSON *pc_j  = cJSON_GetObjectItemCaseSensitive(root, "page_count");
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    int ok = (pc_j && cJSON_IsNumber(pc_j) && pages && cJSON_IsArray(pages));
    int pc = ok ? (int)td5_json_round(pc_j) : 0;
    if (ok && (cJSON_GetArraySize(pages) != pc || pc < 0)) ok = 0;
    if (ok && idx_sz < pc * 4096) ok = 0;       /* one 4096-idx block per page */

    /* Pass 1: compute total size. */
    int total = ok ? (4 + 4 * pc) : 0;
    if (ok) {
        const cJSON *p = NULL;
        cJSON_ArrayForEach(p, pages) {
            const cJSON *off_j = cJSON_GetObjectItemCaseSensitive(p, "offset");
            const cJSON *pal_j = cJSON_GetObjectItemCaseSensitive(p, "palette_hex");
            if (!off_j || !cJSON_IsNumber(off_j) ||
                !pal_j || !cJSON_IsString(pal_j)) { ok = 0; break; }
            int off   = (int)td5_json_round(off_j);
            int pal_n = td5_hex_len(pal_j->valuestring);
            if (off < 0 || pal_n < 0) { ok = 0; break; }
            int end = off + 8 + pal_n + 4096;
            if (end > total) total = end;
        }
    }

    unsigned char *out = (ok && total > 0) ? (unsigned char *)calloc(1, (size_t)total) : NULL;
    if (!out) ok = 0;

    /* Pass 2: emit. */
    if (ok) {
        td5_put_le(out, 0, 4, pc);
        int i = 0;
        const cJSON *p = NULL;
        cJSON_ArrayForEach(p, pages) {
            const cJSON *off_j  = cJSON_GetObjectItemCaseSensitive(p, "offset");
            const cJSON *pad_j  = cJSON_GetObjectItemCaseSensitive(p, "pad_hex");
            const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(p, "type");
            const cJSON *pal_j  = cJSON_GetObjectItemCaseSensitive(p, "palette_hex");
            const char *pad_s = (pad_j && cJSON_IsString(pad_j)) ? pad_j->valuestring : "";
            const char *pal_s = (pal_j && cJSON_IsString(pal_j)) ? pal_j->valuestring : "";
            int off   = (int)td5_json_round(off_j);
            int pad_n = td5_hex_len(pad_s);
            int pal_n = td5_hex_len(pal_s);
            int type  = (type_j && cJSON_IsNumber(type_j)) ? (int)td5_json_round(type_j) : 0;
            if (pad_n != 3 || pal_n < 0 || (pal_n % 3) != 0) { ok = 0; break; }

            td5_put_le(out, 4 + 4 * i, 4, off);
            if (td5_hex_decode(pad_s, out + off) != 3) { ok = 0; break; }
            out[off + 3] = (unsigned char)(type & 0xFF);
            td5_put_le(out, off + 4, 4, pal_n / 3);
            if (td5_hex_decode(pal_s, out + off + 8) != pal_n) { ok = 0; break; }
            memcpy(out + off + 8 + pal_n, idxbuf + (size_t)i * 4096, 4096);
            i++;
        }
    }

    cJSON_Delete(root);
    free(idxbuf);
    if (!ok) { free(out); return 1; }
    *out_buf  = out;
    *out_size = total;
    return 0;
}

/* ========================================================================
 * Tier 3b: MODELS.DAT / HIMODEL.DAT -- meshes (PRR). HYBRID retirement:
 * the runtime-canonical source is a byte-exact copy (models.bin / himodel.bin),
 * so the .DAT is retired with the same byte-exact gate as every other format
 * and zero render risk. Blender editing is provided by the OFFLINE glTF tool
 * (re/tools/prr_to_gltf.py), which regenerates the .bin from an edited .glb.
 * The encoder is a verbatim passthrough of the byte-exact source.
 * ======================================================================== */

static int td5_src_encode_passthrough(const char *src_path, const char *zip_path,
                                      void **out_buf, int *out_size)
{
    (void)zip_path;
    int            sz  = 0;
    unsigned char *buf = td5_src_read_file(src_path, &sz);
    if (!buf || sz <= 0) { free(buf); return 1; }
    *out_buf  = buf;     /* read_file over-allocates by 1 (NUL); harmless slack */
    *out_size = sz;
    return 0;
}

/* ========================================================================
 * Registry + dispatch
 * ======================================================================== */

typedef int (*td5_src_encode_fn)(const char *src_path, const char *zip_path,
                                 void **out_buf, int *out_size);

typedef struct {
    const char       *dat_name;   /* bare .DAT name, matched case-insensitively */
    const char       *src_name;   /* editable source filename (or dir) on disk  */
    td5_src_encode_fn encode;     /* source -> binary .DAT image                */
    int               is_dir;     /* 1 = src_name names a directory (textures)  */
} td5_src_desc;

static const td5_src_desc s_registry[] = {
    { "LEVELINF.DAT",  "levelinf.json",  td5_src_encode_levelinf, 0 },
    { "CARPARAM.DAT",  "carparam.json",  td5_src_encode_carparam, 0 },
    { "CHECKPT.NUM",   "checkpt.json",   td5_src_encode_checkpt,  0 },
    { "TRAFFIC.BUS",   "traffic.json",   td5_src_encode_traffic,  0 },
    { "TRAFFICB.BUS",  "trafficb.json",  td5_src_encode_traffic,  0 },
    { "LEFT.TRK",      "left.trk.csv",   td5_src_encode_routes,   0 },
    { "LEFTB.TRK",     "leftb.trk.csv",  td5_src_encode_routes,   0 },
    { "RIGHT.TRK",     "right.trk.csv",  td5_src_encode_routes,   0 },
    { "RIGHTB.TRK",    "rightb.trk.csv", td5_src_encode_routes,   0 },
    { "STRIP.DAT",     "strip.json",     td5_src_encode_strip,    0 },
    { "STRIPB.DAT",    "stripb.json",    td5_src_encode_strip,    0 },
    { "TEXTURES.DAT",  "textures.src/textures.json", td5_src_encode_textures, 0 },
    { "MODELS.DAT",    "models.bin",     td5_src_encode_passthrough, 0 },
    { "HIMODEL.DAT",   "himodel.bin",    td5_src_encode_passthrough, 0 },
    { NULL, NULL, NULL, 0 }   /* sentinel */
};

/** Pointer to the basename within a path (after the last '/' or '\\'). */
static const char *td5_src_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    return base;
}

void *td5_assetsrc_pack(const char *entry_name, const char *zip_path,
                        int *out_size)
{
    if (out_size) *out_size = 0;
    if (!entry_name || !zip_path)
        return NULL;

    const char *base = td5_src_basename(entry_name);

    for (const td5_src_desc *d = s_registry; d->dat_name; d++) {
        char  src_path[512];
        void *buf = NULL;
        int   sz  = 0;

        if (td5_src_stricmp(base, d->dat_name) != 0)
            continue;

        /* Registered format -- locate its editable source where the .DAT would
         * live. Absent source => fall back to the legacy .DAT loading paths. */
        if (!td5_asset_resolve_source_path(d->src_name, zip_path,
                                           src_path, sizeof(src_path)))
            return NULL;

        if (d->encode(src_path, zip_path, &buf, &sz) != 0 || !buf || sz <= 0) {
            free(buf);
            return NULL;
        }

        if (out_size) *out_size = sz;
        return buf;
    }

    return NULL;
}

int td5_assetsrc_packed_size(const char *entry_name, const char *zip_path)
{
    int   sz  = 0;
    void *buf = td5_assetsrc_pack(entry_name, zip_path, &sz);
    if (!buf)
        return -1;
    free(buf);
    return sz;
}

/* ========================================================================
 * Byte-exact self-test (run via td5re.exe --selftest-assetsrc)
 *
 * For every registered file-based format with both an editable source and the
 * original .DAT present under re/assets/levels/levelNNN/, pack the source and
 * byte-compare against the .DAT. Results are written to `out_log` (default
 * "assetsrc_selftest.log" in the cwd) because the GUI build has no console.
 * Returns 0 if all pass (or none found), nonzero on any mismatch.
 * ======================================================================== */

/* Verify every file-based registered format found in directory `dir`:
 * pack <dir>/<src_name> and byte-compare against <dir>/<dat_name>. */
static void td5_selftest_dir(FILE *lf, const char *dir,
                             int *checked, int *fails)
{
    for (const td5_src_desc *d = s_registry; d->dat_name; d++) {
        char src_path[600], dat_path[600];

        if (d->is_dir)
            continue;   /* directory-based formats (textures) verified separately */

        snprintf(src_path, sizeof(src_path), "%s/%s", dir, d->src_name);
        if (!td5_src_file_exists(src_path))
            continue;

        /* dat_name is upper-case; the on-disk file is lower-case but the
         * Windows filesystem is case-insensitive. */
        snprintf(dat_path, sizeof(dat_path), "%s/%s", dir, d->dat_name);

        int            dat_sz = 0;
        unsigned char *dat    = td5_src_read_file(dat_path, &dat_sz);
        void          *packed = NULL;
        int            psz    = 0;
        int            rc     = d->encode(src_path, NULL, &packed, &psz);

        (*checked)++;
        if (rc != 0 || !packed) {
            if (lf) fprintf(lf, "FAIL  %s/%s encode rc=%d\n",
                            dir, d->src_name, rc);
            (*fails)++;
        } else if (!dat) {
            if (lf) fprintf(lf, "WARN  %s/%s packed=%d (no .DAT to compare)\n",
                            dir, d->src_name, psz);
        } else if (psz != dat_sz || memcmp(packed, dat, (size_t)dat_sz) != 0) {
            int diff = -1;
            int lim  = (psz < dat_sz) ? psz : dat_sz;
            for (int k = 0; k < lim; k++)
                if (((unsigned char *)packed)[k] != dat[k]) { diff = k; break; }
            if (lf) fprintf(lf, "FAIL  %s/%s orig=%d packed=%d firstdiff=%d\n",
                            dir, d->dat_name, dat_sz, psz, diff);
            (*fails)++;
        } else {
            if (lf) fprintf(lf, "PASS  %s/%s %d bytes\n", dir, d->dat_name, psz);
        }

        free(packed);
        free(dat);
    }
}

int td5_assetsrc_selftest(const char *out_log)
{
    FILE *lf = fopen(out_log ? out_log : "assetsrc_selftest.log", "w");
    int   fails = 0, checked = 0;

    if (lf) {
        fprintf(lf, "td5_assetsrc selftest (byte-exact: source -> pack -> compare .DAT)\n");
        fprintf(lf, "================================================================\n");
    }

    /* Level-resident formats: re/assets/levels/levelNNN/ */
    for (int lvl = 0; lvl <= 63; lvl++) {
        char dir[256];
        snprintf(dir, sizeof(dir), "re/assets/levels/level%03d", lvl);
        td5_selftest_dir(lf, dir, &checked, &fails);
    }

    /* Car-resident formats: enumerate re/assets/cars/<code>/ */
    {
        DIR *cars = opendir("re/assets/cars");
        if (cars) {
            struct dirent *e;
            while ((e = readdir(cars)) != NULL) {
                char dir[300];
                if (e->d_name[0] == '.')   /* skip ".", "..", hidden */
                    continue;
                snprintf(dir, sizeof(dir), "re/assets/cars/%s", e->d_name);
                td5_selftest_dir(lf, dir, &checked, &fails);
            }
            closedir(cars);
        }
    }

    if (lf) {
        fprintf(lf, "----------------------------------------------------------------\n");
        fprintf(lf, "assetsrc selftest: %d checked, %d failed\n", checked, fails);
        fclose(lf);
    }
    return fails ? 1 : 0;
}
