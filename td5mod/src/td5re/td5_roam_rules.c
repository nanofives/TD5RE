/**
 * td5_roam_rules.c -- GEO FREE ROAM: posted speed limits + turn restrictions of the real map (PORT-ONLY, roam LG).
 * Contract: td5_roam_rules.h. The reader of <place>/RESTRICTIONS.JSON (re/tools/geo_turn_restrictions.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "td5_platform.h"
#include "td5_geo.h"
#include "td5_geo_frame.h"
#include "td5_roam_rules.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

static const char *const k_kind[TD5_RESTR_KIND_COUNT] = {
    "?", "no_left_turn", "no_right_turn", "no_straight_on", "no_u_turn", "no_entry", "no_exit",
    "only_left_turn", "only_right_turn", "only_straight_on", "only_u_turn"
};

const char *td5_roam_rules_kind_name(int kind)
{
    return (kind > 0 && kind < TD5_RESTR_KIND_COUNT) ? k_kind[kind] : "?";
}

static int rl_kind_of(const char *s)
{
    int k;
    if (!s) return 0;
    for (k = 1; k < TD5_RESTR_KIND_COUNT; k++) if (!strcmp(s, k_kind[k])) return k;
    return 0;
}

static struct {
#ifdef _WIN32
    SRWLOCK          lock;
#endif
    char             place[64];
    int              done;
    TD5_RoamRestr   *r;
    int              n;
    TD5_RoamRestrLoad load;
} L;                                         /* zero-initialised: SRWLOCK_INIT is {0} */

static char *rl_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64L << 20)) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1u);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) b[n] = '\0';
    return b;
}

static int rl_ids(const cJSON *arr, int64_t *out, int cap)
{
    const cJSON *m;
    int n = 0;
    for (m = arr ? arr->child : NULL; m && n < cap; m = m->next) {
        const cJSON *id = cJSON_GetObjectItem(m, "id");
        if (cJSON_IsNumber(id)) out[n++] = (int64_t)id->valuedouble;
    }
    return n;
}

static void rl_parse(const char *place)
{
    char path[512];
    char *json;
    cJSON *root, *arr, *it;
    int cap, n = 0;
    TD5_RoamRestr *r;
    memset(&L.load, 0, sizeof L.load);
    L.r = NULL; L.n = 0;
    td5_geo_source_path(path, sizeof path, place, "RESTRICTIONS.JSON");
    json = rl_slurp(path);
    if (!json) { TD5_LOG_I(LOG_TAG, "ROAM_RULES: no %s (turn restrictions off)", path); return; }
    root = cJSON_Parse(json);
    free(json);
    if (!root) { TD5_LOG_W(LOG_TAG, "ROAM_RULES: %s does not parse", path); return; }
    td5_geo_src_json_to_game(root);          /* source frame -> game frame (every {x, z} object) */
    L.load.file = 1;
    arr = cJSON_GetObjectItem(root, "restrictions");
    cap = cJSON_GetArraySize(arr);
    r = cap > 0 ? (TD5_RoamRestr *)calloc((size_t)cap, sizeof *r) : NULL;
    for (it = arr ? arr->child : NULL; it; it = it->next) {
        TD5_RoamRestr *q;
        const cJSON *vn, *vw, *w;
        L.load.relations++;
        if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "motorcar"))) { L.load.not_car++; continue; }
        if (!rl_kind_of(cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind")))) { L.load.unknown_kind++; continue; }
        if (!r || n >= cap) break;
        q = &r[n];
        memset(q, 0, sizeof *q);
        q->id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "id"));
        q->kind = rl_kind_of(cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind")));
        q->only = q->kind >= TD5_RESTR_ONLY_LEFT;
        q->nfrom = rl_ids(cJSON_GetObjectItem(it, "from"), q->from, TD5_RESTR_WAYS);
        q->nto = rl_ids(cJSON_GetObjectItem(it, "to"), q->to, TD5_RESTR_WAYS);
        vn = cJSON_GetObjectItem(it, "via_nodes");
        vw = cJSON_GetObjectItem(it, "via_ways");
        q->nvia_way = rl_ids(vw, q->via_way, TD5_RESTR_WAYS);
        if (q->nvia_way == 0 && vn && vn->child) {
            q->via_node = 1;
            q->vx = cJSON_GetNumberValue(cJSON_GetObjectItem(vn->child, "x"));
            q->vz = cJSON_GetNumberValue(cJSON_GetObjectItem(vn->child, "z"));
        }
        for (w = vw ? vw->child : NULL; w; w = w->next) {
            const cJSON *pts = cJSON_GetObjectItem(w, "pts");
            const int np = cJSON_GetArraySize(pts);
            int e;
            for (e = 0; e < 2 && np > 0 && q->nvpt < TD5_RESTR_VIAPTS; e++) {
                const cJSON *p = cJSON_GetArrayItem(pts, e == 0 ? 0 : np - 1);
                q->vpx[q->nvpt] = cJSON_GetNumberValue(cJSON_GetObjectItem(p, "x"));
                q->vpz[q->nvpt] = cJSON_GetNumberValue(cJSON_GetObjectItem(p, "z"));
                q->nvpt++;
            }
        }
        if (q->nfrom == 0 || q->nto == 0 || (!q->via_node && q->nvpt == 0)) { L.load.incomplete++; continue; }
        L.load.by_kind[q->kind]++;
        n++;
    }
    cJSON_Delete(root);
    L.r = r; L.n = n;
    L.load.kept = n;
    TD5_LOG_I(LOG_TAG, "ROAM_RULES: %s: %d relations, %d kept for cars (left out: %d not for cars, %d unknown kind, "
              "%d incomplete) | no_left %d no_right %d no_straight %d no_u %d no_entry %d no_exit %d | only_left %d "
              "only_right %d only_straight %d only_u %d", path, L.load.relations, n, L.load.not_car,
              L.load.unknown_kind, L.load.incomplete, L.load.by_kind[1], L.load.by_kind[2], L.load.by_kind[3],
              L.load.by_kind[4], L.load.by_kind[5], L.load.by_kind[6], L.load.by_kind[7], L.load.by_kind[8],
              L.load.by_kind[9], L.load.by_kind[10]);
}

const TD5_RoamRestr *td5_roam_rules_restrictions(const char *place, int *n, TD5_RoamRestrLoad *load)
{
    const TD5_RoamRestr *out;
    if (n) *n = 0;
    if (load) memset(load, 0, sizeof *load);
    if (!place || !place[0]) return NULL;
#ifdef _WIN32
    AcquireSRWLockExclusive(&L.lock);
#endif
    if (!L.done || strcmp(L.place, place) != 0) {
        snprintf(L.place, sizeof L.place, "%s", place);
        rl_parse(place);                     /* an older list is left allocated: a caller may still hold it */
        L.done = 1;
    }
    out = L.r;
    if (n) *n = L.n;
    if (load) *load = L.load;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&L.lock);
#endif
    return out;
}
