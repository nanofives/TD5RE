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
/* [ROUND 1012 D2] how many hash-chosen sites were refused for standing in a
 * real square. Reported, because "the landmark moved" and "the landmark never
 * had anywhere to go" are different tracks. */
static int s_pf_plaza_refused;

/* [J7 item 1] "todavia hay edificios que no tienen lados" (Mariano,
 * 2026-10-03). Some of the shipped set pieces in td5_tg_prefab_data.h are OPEN
 * SHELLS: lifted verbatim out of L23, where a piece that only ever faced the
 * road needs no back and no roof. In the original that is invisible. Standing
 * one alone beside a generated road, it is a building you can see straight
 * through, and the MODELS.DAT census found exactly that -- 2 instances of the
 * same prefab open on seed 4172065417, with closed_same_pages=0, i.e. open on
 * EVERY instance rather than in some branch.
 *
 * re/tools/tg_prefab_audit.py measures the table offline and generates
 * td5_tg_prefab_close_data.h: for each open component, the quads that complete
 * its own BOUNDING BOX on the planes it does not already wall, carrying that
 * component's dominant texture page. 7 open components across 5 of the 24
 * prefabs, 25 quads in total. Doing it offline rather than at runtime keeps the
 * analysis (vertex welding, wall clustering) out of the generator entirely and
 * makes the added geometry reviewable as data.
 *
 * The closure is the component's bbox, so a piece whose open part is L-shaped
 * comes out as the enclosing block rather than the L. That is a deliberate
 * trade: it is a slight simplification of the silhouette, and it is the only
 * shape derivable from the piece itself without inventing architecture.
 *
 * TD5RE_TG_PREFAB_CLOSE=0 takes the plain path below and is byte-identical to
 * the build before this change. */
static int tg_prefab_write(TG_Buf *blk, const TG_PrefabDef *d, int pf,
                           double ox, double oy, double oz,
                           double ca, double sa)
{
    /* Big enough for the largest prefab (lm00, 975 verts) plus the most
     * closing quads any one piece takes (lm15, 8 -> 32 verts). */
    static float        v[(1024 + 64) * 5];
    static unsigned int lgt[1024 + 64];
    static unsigned short cmd[(64 + 32) * 3];
    int nq, first, i, k, nv, ncmd;

    if (pf < 0 || pf >= TD5_TG_PREFAB_N
        || !td5_env_flag_on("TD5RE_TG_PREFAB_CLOSE"))
        return tg_write_prefab_mesh(blk, d->v, d->l, d->nv, d->c, d->ncmd,
                                    TD5_TG_PAGE_LM_BASE, ox, oy, oz, ca, sa);

    first = (int)k_pfclose[pf][0];
    nq    = (int)k_pfclose[pf][1];
    if (nq <= 0
        || d->nv + nq * 4 > (int)(sizeof(lgt) / sizeof(lgt[0]))
        || d->ncmd + nq > (int)(sizeof(cmd) / sizeof(cmd[0]) / 3))
        return tg_write_prefab_mesh(blk, d->v, d->l, d->nv, d->c, d->ncmd,
                                    TD5_TG_PAGE_LM_BASE, ox, oy, oz, ca, sa);

    /* The prefab first, UNCHANGED, so the closure can only ever add faces. */
    memcpy(v, d->v, (size_t)d->nv * 5 * sizeof(float));
    memcpy(lgt, d->l, (size_t)d->nv * sizeof(unsigned int));
    memcpy(cmd, d->c, (size_t)d->ncmd * 3 * sizeof(unsigned short));
    nv = d->nv;
    ncmd = d->ncmd;

    for (i = 0; i < nq; i++) {
        const TG_PrefabCloseQuad *q = &k_pfclose_quads[first + i];
        for (k = 0; k < 20; k++) v[nv * 5 + k] = q->v[k];
        /* 0xFFA0A0A0 is the dominant baked value across this corpus, so a
         * closing wall sits at the same brightness as the piece it closes
         * instead of flaring white next to it. */
        for (k = 0; k < 4; k++) lgt[nv + k] = 0xFFA0A0A0u;
        cmd[ncmd * 3 + 0] = q->page_local;
        cmd[ncmd * 3 + 1] = 0;            /* triangles */
        cmd[ncmd * 3 + 2] = 1;            /* quads     */
        nv += 4;
        ncmd++;
    }
    return tg_write_prefab_mesh(blk, v, lgt, nv, cmd, ncmd,
                                TD5_TG_PAGE_LM_BASE, ox, oy, oz, ca, sa);
}

/* Per-BUILD, exactly like tg_acct_reset and the R12 flora ledgers: a second
 * generation in the same process must not inherit the first one's placements.
 * That is the R9 water-table crash class (stale gen-1 span indices read against
 * a shorter gen-2 track), so this is not a theoretical tidy-up. */
void tg_prefab_reset(void)
{
    s_pf_n = 0;
    s_pf_emitted = 0;
    s_pf_plaza_refused = 0;
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

/* [GEO PHASE 5] The BIGGEST set piece that fits inside a given footprint,
 * with `salt` deciding between equally good candidates.
 *
 * WHY "FITS INSIDE" IS THE RULE. The geo landmark fallback stamps one of
 * these on a real OSM footprint that has already been nudged clear of the
 * carriageway (td5_tg_city.c tg_geo_emit_one). If the piece stays inside that
 * footprint it inherits the clearance for free, and the question "can the
 * fallback land on the road" is answered by construction rather than by a
 * second standoff calculation that could disagree with the first.
 *
 * Biggest-that-fits, because a 9 m set piece rattling around inside a 40 m
 * civic block reads as a model on a car park. `salt` is a hash of the OSM way
 * id at the call site, so two similar footprints do not both get lm00 -- and
 * it is a hash, not a draw, so the standing no-RNG rule holds.
 *
 * Returns -1 when nothing fits, which is an ordinary answer: the caller then
 * extrudes the real footprint as it always did. */
/* [J7] Is this set piece selectable at all?
 *
 * Four of the five open prefabs are closed by td5_tg_prefab_close_data.h. The
 * fifth, lm12, cannot be: its open island spans 21.7 x 17.8 m while the walls
 * inside it span only 5.5 x 14.2 m, so a box drawn from either extent lands in
 * open air rather than against the architecture. Rendering the emitted mesh
 * confirmed it -- the closure buried the piece at one yaw and stood in front of
 * its detailed face at another, which is worse than the hole it was closing.
 *
 * So lm12 is not closed, it is withdrawn, and tg_prefab_fit's documented
 * "nothing fits" answer takes over: the caller extrudes the real footprint, as
 * it did before any prefab existed. One piece of 24. */
int tg_prefab_usable(int i)
{
    if (i < 0 || i >= TD5_TG_PREFAB_N) return 0;
    if (!td5_env_flag_on("TD5RE_TG_PREFAB_CLOSE")) return 1;
    return !k_pfclose_exclude[i];
}

int tg_prefab_fit(double fx_max, double fz_max, unsigned int salt)
{
    int i, best = -1, nfit = 0, pick;
    double best_area = 0.0;

    if (!(fx_max > 0.0) || !(fz_max > 0.0)) return -1;
    for (i = 0; i < TD5_TG_PREFAB_N; i++) {
        const double fx = k_tg_prefabs[i].fx, fz = k_tg_prefabs[i].fz;
        if (!tg_prefab_usable(i)) continue;
        if (!(fx > 0.0) || !(fz > 0.0)) continue;
        if (fx > fx_max || fz > fz_max) continue;
        if (fx * fz > best_area) best_area = fx * fz;
        nfit++;
    }
    if (nfit == 0 || !(best_area > 0.0)) return -1;
    /* Everything within 25% of the best area is "as good"; the salt chooses
     * among them so the same civic block does not always draw the same piece. */
    nfit = 0;
    for (i = 0; i < TD5_TG_PREFAB_N; i++) {
        const double a = (double)k_tg_prefabs[i].fx * k_tg_prefabs[i].fz;
        if (!tg_prefab_usable(i)) continue;
        if (k_tg_prefabs[i].fx > fx_max || k_tg_prefabs[i].fz > fz_max) continue;
        if (a >= best_area * 0.75) nfit++;
    }
    pick = (int)(salt % (unsigned)(nfit > 0 ? nfit : 1));
    for (i = 0; i < TD5_TG_PREFAB_N; i++) {
        const double a = (double)k_tg_prefabs[i].fx * k_tg_prefabs[i].fz;
        if (!tg_prefab_usable(i)) continue;
        if (k_tg_prefabs[i].fx > fx_max || k_tg_prefabs[i].fz > fz_max) continue;
        if (a < best_area * 0.75) continue;
        best = i;
        if (pick-- <= 0) break;
    }
    return best;
}

/* Write ONE set piece straight into a live scenery buffer, no DECIDE phase.
 *
 * The two-phase table above exists because tg_landmarks_place runs before any
 * mesh buffer does; the geo fallback runs INSIDE the scenery loop with a buffer
 * already in hand, and its site comes from a footprint rather than from a
 * standoff, so it has nothing to defer. The caller owns moff/nmesh/guard
 * bookkeeping exactly as it does for a footprint it extrudes itself. */
int tg_prefab_write_at(TG_Buf *blk, int pf, double ox, double oy, double oz,
                       double ca, double sa)
{
    const TG_PrefabDef *d;
    if (!blk || pf < 0 || pf >= TD5_TG_PREFAB_N) return 1;
    d = &k_tg_prefabs[pf];
    if (!tg_prefab_write(blk, d, pf, ox, oy, oz, ca, sa))
        return 0;
    s_pf_emitted++;
    return 1;
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
        ok = tg_prefab_write(meshes, d, s_pf[i].pf,
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
              "available set pieces), %d site(s) refused for standing in a "
              "real square", s_pf_n, s_pf_emitted, TD5_TG_PREFAB_N,
              s_pf_plaza_refused);
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

    /* [ROUND 1012 D2] NOT IN A REAL SQUARE. "there were landmarks on the plaza
     * at the beginning of the race." These are the SHIPPED TD5 set pieces, laid
     * by the synthetic landmark walk, which knows nothing about the real world
     * -- so on a geo track a 34 x 27 m building lands in the middle of Plaza
     * Miguel de Azcuenaga because the hash said span 20. A square is where a
     * city does NOT build; the real geometry that belongs there (a mapped
     * monument, a lawn) comes from the geo emitters. Refusing here is enough on
     * its own: the caller already tries several hash-chosen spans and gives up
     * quietly, so the landmark simply moves down the street or does not appear.
     *
     * tg_geo_open_space_at returns 0 before touching anything on a synthetic
     * build, so the synthetic byte-identity contract is unaffected. */
    if (td5_env_flag_on("TD5RE_GEO_PREFAB_PLAZA")
        && tg_geo_open_space_at(si, x, z, tg_prefab_half_depth(pf))) {
        s_pf_plaza_refused++;
        return 0;
    }

    /* Local +X runs along the road and local +Z along the normal, so a building
     * exported facing its original street still faces this one. */
    return tg_prefab_add(si, pf, x, y, z, tx * (double)side, tz * (double)side);
}
