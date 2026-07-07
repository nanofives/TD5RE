/**
 * td5_pending.c -- Dev/QA "pending to test" checklist + in-game overlay.
 *
 * See td5_pending.h. The list is now backed by a TRACKED, hand-editable CSV,
 *     td5mod/src/td5re/pending_to_test.csv
 * with three columns:  summary , detail , status
 *   - summary : the one-line item (also the right-edge overlay text)
 *   - detail  : the longer test-focus note shown by the ENTER-key modal
 *   - status  : "pending" (or blank) = shown;  anything else ("tested",
 *               "wontfix", ...) = hidden from the list & overlay
 *
 * This CSV is the SINGLE SOURCE OF TRUTH -- it replaces the old compiled-in
 * k_seed[] / k_pending_detail[] arrays and the untracked td5re_pending.txt.
 * Edit it directly (any text editor / spreadsheet) to add, reword, annotate,
 * or retire items; the game reads it on launch. RFC4180 quoting: every field
 * is wrapped in double-quotes and an embedded " is doubled ("").
 *
 * Runtime path resolution (pending_path): first "pending_to_test.csv" next to
 * the exe (release layout), else "td5mod/src/td5re/pending_to_test.csv" under
 * the exe dir (dev layout -- both exes deploy to the project root, so the
 * tracked source-tree copy is found automatically). The file that was loaded
 * is the one written back to.
 *
 * [2026-07-06] Migrated from the compiled arrays + td5re_pending.txt to this
 * CSV. In-game SUPR/DELETE no longer removes a row and tombstones it; it sets
 * that row's status to "tested" and rewrites the CSV (row order preserved, so
 * a status flip is a one-cell diff). The list/overlay only ever show rows whose
 * status is pending, so the visible behaviour is unchanged.
 */
#include "td5_pending.h"
#include "td5_platform.h"
#include "td5re.h"

#include <string.h>
#include <stdio.h>

#define LOG_TAG "pending"

#define PENDING_MAX         512   /* > row count, room to grow */
#define PENDING_TEXT_MAX    160   /* summary column                     */
#define PENDING_DETAIL_MAX  1024  /* detail column (long test note)     */
#define PENDING_STATUS_MAX  24    /* status column                      */

typedef struct {
    char summary[PENDING_TEXT_MAX];
    char detail [PENDING_DETAIL_MAX];
    char status [PENDING_STATUS_MAX];
} PendingItem;

/* ALL rows exactly as loaded, every status. */
static PendingItem s_items[PENDING_MAX];
static int s_count = 0;

/* The visible view: indices into s_items[] whose status is "pending". The
 * public accessors (count/text/detail_text/delete) all operate on this view,
 * so callers (td5_fe_devscreens.c, td5_hud.c) are unchanged. */
static int s_view[PENDING_MAX];
static int s_view_count = 0;

static int  s_overlay_on = 0;
static char s_csv_path[600];      /* resolved CSV we loaded == we write back */

/* ---- small helpers ------------------------------------------------------ */

/* status is "pending" if blank or (case-insensitively) the word "pending". */
static int status_is_pending(const char *s) {
    static const char *kw = "pending";
    int i;
    if (!s) return 1;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 1;
    for (i = 0; kw[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != kw[i]) return 0;
    }
    /* trailing junk after "pending" -> treat as non-pending */
    return (s[i] == '\0' || s[i] == ' ' || s[i] == '\t');
}

static int file_exists(const char *path) {
    TD5_File *f = td5_plat_file_open(path, "rb");
    if (f) { td5_plat_file_close(f); return 1; }
    return 0;
}

static const char *pending_path(void) {
    static int done = 0;
    if (!done) {
        char cand[600];
        td5_plat_ini_resolve_path("pending_to_test.csv", cand, sizeof cand);
        if (file_exists(cand)) {
            snprintf(s_csv_path, sizeof s_csv_path, "%s", cand);
        } else {
            char dev[600];
            td5_plat_ini_resolve_path("td5mod/src/td5re/pending_to_test.csv",
                                      dev, sizeof dev);
            /* dev tree if present, otherwise fall back to the exe-dir name
             * (which is where a first-ever save will create the file). */
            snprintf(s_csv_path, sizeof s_csv_path, "%s",
                     file_exists(dev) ? dev : cand);
        }
        done = 1;
    }
    return s_csv_path;
}

/* ---- CSV parse ---------------------------------------------------------- */

/* Read one RFC4180 field starting at *pp into out[]; advance *pp past the
 * trailing comma (if any). Handles a double-quoted field with embedded ""
 * escapes as well as a bare unquoted field. Records are one-per-line here
 * (the writer never emits embedded newlines), so this is line-local. */
static void csv_field(const char **pp, char *out, size_t out_n) {
    const char *p = *pp;
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '"') {
                if (p[1] == '"') { if (o < out_n - 1) out[o++] = '"'; p += 2; continue; }
                p++;                 /* closing quote */
                break;
            }
            if (o < out_n - 1) out[o++] = *p;
            p++;
        }
        while (*p && *p != ',') p++; /* skip anything up to the separator */
    } else {
        while (*p && *p != ',') { if (o < out_n - 1) out[o++] = *p; p++; }
        while (o > 0 && (out[o-1] == ' ' || out[o-1] == '\t' || out[o-1] == '\r'))
            o--;                     /* trim trailing ws on bare fields */
    }
    out[o] = '\0';
    if (*p == ',') p++;
    *pp = p;
}

/* First non-comment line is the header iff its first field is "summary". */
static int is_header_line(const char *p) {
    static const char *kw = "summary";
    int i;
    if (*p == '"') p++;
    for (i = 0; kw[i]; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != kw[i]) return 0;
    }
    return 1;
}

static void pending_load_file(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "rb");
    static char buf[262144];         /* whole file; must match td5_pending_save */
    size_t      n;
    char       *line;
    int         first = 1;
    if (!f) { TD5_LOG_W(LOG_TAG, "no CSV at %s (empty list)", path); return; }
    n = td5_plat_file_read(f, buf, sizeof buf - 1);
    td5_plat_file_close(f);
    if (n == 0) return;
    buf[n] = '\0';

    line = buf;
    while (line && *line) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        {
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#') { /* comment line -- skip */ }
            else if (first && is_header_line(p)) {
                first = 0;                 /* skip the "summary,detail,status" row once */
            } else if (*p && s_count < PENDING_MAX) {
                PendingItem *it = &s_items[s_count];
                first = 0;
                csv_field(&p, it->summary, sizeof it->summary);
                csv_field(&p, it->detail,  sizeof it->detail);
                csv_field(&p, it->status,  sizeof it->status);
                if (it->summary[0]) {
                    if (!it->status[0])
                        strcpy(it->status, "pending");
                    s_count++;
                }
            }
        }
        if (!nl) break;
        line = nl + 1;
        while (*line == '\r' || *line == '\n') line++;
    }
}

static void rebuild_view(void) {
    int i;
    s_view_count = 0;
    for (i = 0; i < s_count; i++)
        if (status_is_pending(s_items[i].status))
            s_view[s_view_count++] = i;
}

/* ---- CSV write ---------------------------------------------------------- */

/* Append one quoted CSV field (with a trailing sep or newline) to buf. */
static int csv_put(char *buf, int len, int cap, const char *s, char sep) {
    if (len < cap) buf[len++] = '"';
    for (; *s && len < cap - 2; s++) {
        if (*s == '"') buf[len++] = '"';   /* double an embedded quote */
        buf[len++] = *s;
    }
    if (len < cap) buf[len++] = '"';
    if (len < cap) buf[len++] = sep;
    return len;
}

void td5_pending_save(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "wb");
    static char buf[262144];
    int         len  = 0, i;
    if (!f) { TD5_LOG_W(LOG_TAG, "save: cannot open %s", path); return; }
    len += snprintf(buf + len, sizeof buf - (size_t)len, "summary,detail,status\r\n");
    for (i = 0; i < s_count && len < (int)sizeof buf - (PENDING_TEXT_MAX + PENDING_DETAIL_MAX + 32); i++) {
        len = csv_put(buf, len, (int)sizeof buf, s_items[i].summary, ',');
        len = csv_put(buf, len, (int)sizeof buf, s_items[i].detail,  ',');
        len = csv_put(buf, len, (int)sizeof buf,
                      s_items[i].status[0] ? s_items[i].status : "pending", '\r');
        if (len < (int)sizeof buf) buf[len++] = '\n';
    }
    if (len < 0) len = 0;
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    td5_plat_file_write(f, buf, (size_t)len);
    td5_plat_file_close(f);
}

/* ---- lifecycle ---------------------------------------------------------- */

int td5_pending_init(void) {
    s_count = 0;
    pending_load_file();
    rebuild_view();
    TD5_LOG_I(LOG_TAG, "pending-test: %d shown / %d total (%s)",
              s_view_count, s_count, pending_path());
    return 1;
}

void td5_pending_shutdown(void) { /* writes happen on delete; nothing to flush */ }

/* ---- public accessors (operate on the pending-status view) -------------- */

int td5_pending_count(void)          { return s_view_count; }

const char *td5_pending_text(int i) {
    return (i >= 0 && i < s_view_count) ? s_items[s_view[i]].summary : "";
}

/* Longer test-focus note for the ENTER-key modal (the detail column). Falls
 * back to a generic note when the row has no detail recorded yet. */
const char *td5_pending_detail_text(int i) {
    if (i < 0 || i >= s_view_count) return "";
    {
        const char *d = s_items[s_view[i]].detail;
        return (d && d[0]) ? d
             : "No additional test notes recorded for this item yet -- use the summary above as the test focus.";
    }
}

/* SUPR/DELETE on the menu: mark the row tested (status="tested") and persist.
 * It drops out of the pending view (so it "disappears" from the list like a
 * delete did) but the row survives in the CSV as a status change. */
void td5_pending_delete(int i) {
    if (i < 0 || i >= s_view_count) return;
    strcpy(s_items[s_view[i]].status, "tested");
    rebuild_view();
    td5_pending_save();
}

int  td5_pending_overlay_on(void)      { return s_overlay_on; }
void td5_pending_set_overlay(int on)   { s_overlay_on = on ? 1 : 0; }
void td5_pending_toggle_overlay(void)  { s_overlay_on = !s_overlay_on; }

/* The in-race overlay draw lives in td5_hud.c (td5_hud_draw_pending_overlay) so it
 * uses the same HUD font/size as the debug-stats overlay. This module only owns
 * the on/off state above plus the list. */
