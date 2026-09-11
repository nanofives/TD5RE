/* td5_tg_prefab.c -- auto-track PREFABS: shipped set-piece geometry lifted out
 * of a real TD5 track and stamped beside the generated road (PORT-ONLY).
 *
 * The generator authors everything it places. This module is the exception: the
 * geometry in td5_tg_prefab_data.h is real Moscow architecture, exported by
 * re/tools/td5_geomlib.py prefabs, with its own textures in the
 * TD5_TG_PAGE_LM_BASE page block.
 *
 * TWO PHASES, because placement and emission happen at different times.
 *
 *   DECIDE  tg_landmarks_place (td5_trackgen.c) runs after elevation and before
 *           scenery, walking MERGED biome runs. It calls tg_prefab_add to
 *           record what goes where. It cannot emit: there is no mesh buffer at
 *           that point, and needs_flat can only be judged once elevation is in.
 *   EMIT    tg_scenery_entry walks spans with a live TG_Buf and calls
 *           tg_prefab_emit_span, which drains whatever was recorded for that
 *           span.
 *
 * Placement consumes NO RNG -- every choice is a hash of the build seed and the
 * row's own salt, the same discipline the biome layout and the R21 rolls use.
 * Adding or removing a landmark therefore cannot move the road, which is the
 * property that makes this safe to iterate on.
 *
 * The guard is deliberately left ARMED: prefabs are marked TG_GK_BUILDING, an
 * ordinary scenery kind, so tg_guard_validate_entry will reject one that
 * overlaps the carriageway rather than let it swallow the road. A rejected
 * landmark is a placement bug worth seeing in race.log, not something to
 * silence with an exemption.
 */
#include "td5_trackgen_internal.h"   /* also supplies LOG_TAG */
#include "td5_tg_world.h"

/* One recorded placement. Small and flat: the table is walked once per span
 * during emit, so a linear scan over a handful of entries costs nothing. */
typedef struct {
    int    si;                  /* span that owns the emit */
    int    pf;                  /* index into k_tg_prefabs */
    double ox, oy, oz;          /* world placement */
    double ca, sa;              /* yaw, pre-resolved to cos/sin */
} TG_PrefabPlace;

enum { TG_PREFAB_MAX = 64 };

static TG_PrefabPlace s_pf[TG_PREFAB_MAX];
static int s_pf_n;
static long s_pf_emitted;

/* Per-BUILD, exactly like tg_acct_reset and the R12 flora ledgers: a second
 * generation in the same process must not inherit the first one's placements.
 * That is the R9 water-table crash class (stale gen-1 span indices read against
 * a shorter gen-2 track), so this is not a theoretical tidy-up. */
void tg_prefab_reset(void)
{
    s_pf_n = 0;
    s_pf_emitted = 0;
}

int tg_prefab_count(void) { return s_pf_n; }

int tg_prefab_add(int si, int pf, double ox, double oy, double oz, double yaw_c,
                  double yaw_s)
{
    if (s_pf_n >= TG_PREFAB_MAX) return 0;
    if (pf < 0 || pf >= TD5_TG_PREFAB_N) return 0;
    s_pf[s_pf_n].si = si;
    s_pf[s_pf_n].pf = pf;
    s_pf[s_pf_n].ox = ox;
    s_pf[s_pf_n].oy = oy;
    s_pf[s_pf_n].oz = oz;
    s_pf[s_pf_n].ca = yaw_c;
    s_pf[s_pf_n].sa = yaw_s;
    s_pf_n++;
    return 1;
}

/* Footprint half-extent ACROSS the road, i.e. along the prefab's local Z, which
 * tg_prefab_place aligns with the road normal. Used to set the standoff so the
 * near face lands on the clearance line rather than the centre doing. */
double tg_prefab_half_depth(int pf)
{
    if (pf < 0 || pf >= TD5_TG_PREFAB_N) return 0.0;
    return k_tg_prefabs[pf].fz * 0.5;
}

const char *tg_prefab_name(int pf)
{
    if (pf < 0 || pf >= TD5_TG_PREFAB_N) return "?";
    return k_tg_prefabs[pf].name;
}

/* Drain every placement recorded for span si into this entry's mesh buffer.
 * Mirrors the surrounding loop's contract: record the offset in moff BEFORE
 * appending, keep moff ascending, and guard-mark the byte range just written. */
int tg_prefab_emit_span(int si, TG_Buf *meshes, size_t *moff, int *nmesh,
                        int entry)
{
    int i, ok = 1;

    if (!meshes || !moff || !nmesh) return 1;
    for (i = 0; i < s_pf_n && ok; i++) {
        const TG_PrefabDef *d;
        size_t m0;
        if (s_pf[i].si != si) continue;
        if (*nmesh >= TG_MAX_MESHES_PER_ENTRY) break;
        d = &k_tg_prefabs[s_pf[i].pf];
        m0 = meshes->len;
        moff[*nmesh] = m0;
        ok = tg_write_prefab_mesh(meshes, d->v, d->l, d->nv, d->c, d->ncmd,
                                  TD5_TG_PAGE_LM_BASE,
                                  s_pf[i].ox, s_pf[i].oy, s_pf[i].oz,
                                  s_pf[i].ca, s_pf[i].sa);
        if (!ok) {
            /* tg_write_prefab_mesh only fails its own cursor check, which means
             * the two generated headers disagree. Say so loudly: silently
             * skipping would leave a landmark missing with no explanation. */
            TD5_LOG_E(LOG_TAG, "trackgen: [PREFAB] %s REFUSED at span %d -- its "
                      "commands do not account for %d vertices; regenerate "
                      "td5_tg_prefab_data.h", d->name, si, d->nv);
            break;
        }
        tg_guard_mark(m0, meshes->len, TG_GK_BUILDING, si);
        tg_meshtag_set(entry, *nmesh, TG_GK_BUILDING);
        (*nmesh)++;
        s_pf_emitted++;
    }
    return ok;
}

void tg_prefab_report(void)
{
    int i;
    TD5_LOG_I(LOG_TAG, "trackgen: [PREFAB] %d placed, %ld emitted (of %d "
              "available set pieces)", s_pf_n, s_pf_emitted, TD5_TG_PREFAB_N);
    /* Name them. A bare count cannot distinguish "six plazas" from "four
     * landmarks and two plazas", and those are very different tracks. */
    for (i = 0; i < s_pf_n; i++) {
        const TG_PrefabDef *d = &k_tg_prefabs[s_pf[i].pf];
        TD5_LOG_I(LOG_TAG, "trackgen:   [PREFAB] span %4d  %-16s %d verts, "
                  "%d cmds, %.0fx%.0f h=%.0f at (%.0f,%.0f,%.0f)",
                  s_pf[i].si, d->name, d->nv, d->ncmd, d->fx, d->fz, d->height,
                  s_pf[i].ox, s_pf[i].oy, s_pf[i].oz);
    }
}

/* Resolve one landmark row to a world placement beside the road.
 *
 * Returns 0 when the span is unusable, which is a normal outcome rather than an
 * error: the caller tries a bounded number of hash-chosen spans in the run and
 * gives up quietly. Reasons a span is refused are all "the piece would not sit
 * right there" -- off the end of the walk, or on a bridge or in a tunnel, where
 * there is no ground to stand a building on.
 */
int tg_prefab_place(const TG_NodeList *nl, int nspans, int si, int pf,
                    int side, double clearance)
{
    const TG_Node *n;
    double tx, tz, nx, nz, off, x, z, y;

    if (!nl || si < 1 || si >= nspans - 1) return 0;
    if (tg_span_in_bridge_run(si) || tg_span_in_tunnel(si)) return 0;

    n = &nl->v[si];
    tx = n->tx; tz = n->tz;
    if (!(tx * tx + tz * tz > 0.0)) return 0;

    /* Left normal. side +1 puts the piece left of the direction of travel. */
    nx = -tz * (double)side;
    nz =  tx * (double)side;

    off = n->width * 0.5 + clearance + tg_prefab_half_depth(pf);
    x = n->x + nx * off;
    z = n->z + nz * off;
    /* Stand it on the WORLD, not on the road: beside a graded road the terrain
     * has already been conformed near the verge and falls away past it, so
     * using the road's y would float or bury a piece set back this far. */
    y = tg_world_h(x, z);

    /* Local +X runs along the road and local +Z along the normal, so a building
     * exported facing its original street still faces this one. */
    return tg_prefab_add(si, pf, x, y, z, tx * (double)side, tz * (double)side);
}
