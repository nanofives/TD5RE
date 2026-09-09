/**
 * td5_tg_guard.c -- auto-track GUARD: on-road geometry backstop, MESHTAG sidecar, pavement provenance marks, over-water / coast audits
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

const char *const k_guard_kind_name[TG_GK_COUNT] = {
    "other", "skirt", "road", "branch-road", "tunnel", "gantry", "end-wall",
    "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
    "park-tree", "terrain", "building", "prop", "rail", "branch-side"
};

/* [R8] Is this exempt kind's licence SPAN-LOCAL? A bridge deck, a bore, a
 * gantry, a road quad and an end wall are all authored for ONE span and reach
 * barely past it, so their licence is scoped to a span run and cannot cover a
 * mesh that lands somewhere else.
 *
 * MEASURED EXCEPTION -- the ground skirt, the sea plane and the coastline are
 * legitimately TENS OF THOUSANDS of units wide. On seed 99991 the skirt emitted
 * for span 627 reaches the carriageway at span 647 where the road doubles back
 * on itself; that is the ground doing its job, not a stray. Scoping those would
 * have made the guard reject the ground under the road -- caught by the DIAG
 * dump before it shipped, and the reason this is a per-kind property rather
 * than one global rule. */
static const unsigned char k_guard_kind_local[TG_GK_COUNT] = {
    0,  /* other       */
    0,  /* skirt       -- wide by design, see above */
    1,  /* road        */
    1,  /* branch-road */
    1,  /* tunnel      */
    1,  /* gantry      */
    1,  /* end-wall    */
    1,  /* deck        */
    0,  /* water       -- a sea plane spans the whole seaward side */
    0,  /* coast       */
    1,  /* decal       */
    0, 0, 0, 0, 0,                 /* city block cross flora park-tree */
    0,                             /* terrain -- wide by design, see above */
    0, 0, 0, 0                     /* building prop rail branch-side */
};

static const unsigned char k_guard_kind_class[TG_GK_COUNT] = {
    TG_GKC_SCENERY,  /* other       */
    TG_GKC_UNDER,    /* skirt       */
    TG_GKC_EXEMPT,   /* road        */
    TG_GKC_EXEMPT,   /* branch-road */
    TG_GKC_EXEMPT,   /* tunnel      */
    TG_GKC_EXEMPT,   /* gantry      */
    TG_GKC_EXEMPT,   /* end-wall    */
    TG_GKC_EXEMPT,   /* deck        */
    TG_GKC_UNDER,    /* water       */
    TG_GKC_UNDER,    /* coast       */
    TG_GKC_DECAL,    /* decal       */
    TG_GKC_SCENERY,  /* city        */
    TG_GKC_SCENERY,  /* block       */
    TG_GKC_SCENERY,  /* cross       */
    TG_GKC_SCENERY,  /* flora       */
    TG_GKC_SCENERY,  /* park-tree   */
    /* [R8] Ground, like the skirt: legal under the road, never on top of it.
     * Rejecting a terrain apron that sits BELOW an elevated road (seed 777
     * spans 860..904, dy -9900..-500) would punch a hole under the track; the
     * same emitter putting grass ON the road is R7 item 19 and must still go. */
    TG_GKC_UNDER,    /* terrain     */
    TG_GKC_SCENERY,  /* building    */
    /* [R10 SPAN66] Furniture and people. Both PROP-marked emitters (the R9 INFRA
     * street furniture and the pre-existing spectator/statue/animal layer) set
     * their piece back from the MAIN road edge, which at a frontage gap is
     * side-street tarmac. This class is what makes the guard measure that. */
    TG_GKC_FURNITURE, /* prop       */
    TG_GKC_SCENERY,  /* rail        */
    TG_GKC_SCENERY   /* branch-side */
};

/* [S2h] One row per thread slot (was __thread -- see tg_tslot). */
size_t s_guard_ex_lo[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];

size_t s_guard_ex_hi[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];

unsigned char s_guard_ex_kind[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];

int    s_guard_ex_si[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];

TG_SlotInt s_guard_ex_nx[TG_ACCT_SLOTS];

long   s_guard_rejects;          /* running total across the whole build */

long   s_guard_residual;         /* on-road meshes STILL present post-drop */

long   s_guard_rej_kind[TG_GK_COUNT];   /* [R8] breakdown by kind */

long   s_guard_ex_scope_hits;    /* [R8] exempt marks that fell out of scope */

/* [R10] Two health counters for the compaction pass itself, reported per build.
 * `unsorted` = mesh offsets an emitter recorded out of ascending order (the R9
 * underpass/building bug: harmless-looking, and the reason a write cursor that
 * assumed ascending order ran off the end of the heap block). `unsafe` = meshes
 * the cursor bound refused to move. Both MUST be 0; they are logged so that a
 * future emitter which reintroduces either is loud instead of silent. */
long   s_guard_unsorted;

long   s_guard_unsafe;

/* Default ON -- this is the fix, not an opt-in. TD5RE_R7_GUARD=0 pins the old
 * unguarded output for an A/B, and TD5RE_R7_GUARD_REPORT=1 makes it report-only
 * (log but keep the bytes) for diagnosing what it would drop. */
int tg_guard_enabled(void) { return td5_env_flag_on("TD5RE_R7_GUARD"); }

/* Report-only is an opt-in (default OFF): log what WOULD be dropped but keep the
 * bytes, for diagnosing the guard without changing the geometry. */
int tg_guard_report_only(void)
{
    return td5_env_flag_off("TD5RE_R7_GUARD_REPORT");
}

/* [R8] The precision pass (area coverage + kind classes + span-scoped
 * exemptions). Default ON; TD5RE_R8_GUARD=0 pins the R7 vertex-only test for a
 * single-variable A/B. */
static int tg_guard_r8(void) { return td5_env_flag_on("TD5RE_R8_GUARD"); }

/* [R8] TD5RE_R8_GUARD_DIAG=<span>: dump every mesh of the entry containing that
 * span with its kind, coverage, height band and verdict. 0 = off. */
static int tg_guard_diag_span(void)
{
    return td5_env_int("TD5RE_R8_GUARD_DIAG", 0, 0, 100000);
}

void tg_guard_ex_reset(void) { s_guard_ex_n(tg_tslot()) = 0; }

/* Record [lo, hi) of the current entry's mesh buffer as geometry of `kind`
 * emitted for span `si`. Called at the emit sites in tg_emit_models; ranges the
 * emitters never mark fall through as TG_GK_OTHER, which is SCENERY -- the
 * fail-safe direction (validated, not licensed). */
void tg_guard_mark(size_t lo, size_t hi, int kind, int si)
{
    int t, e;
    if (hi <= lo) return;
    t = tg_tslot();                       /* [S2h] this thread's row */
    if (s_guard_ex_n(t) < TD5_TG_GUARD_EX_MAX) {
        e = s_guard_ex_n(t);
        s_guard_ex_lo[t][e] = lo;
        s_guard_ex_hi[t][e] = hi;
        s_guard_ex_kind[t][e] = (unsigned char)kind;
        s_guard_ex_si[t][e] = si;
        s_guard_ex_n(t)++;
    }
}

/* Kind of the mesh at byte offset `off`, plus the span it was marked for.
 * The NARROWEST matching mark wins. Marks NEST -- an emitter group's range in
 * tg_emit_models encloses the specific ranges its own emitters marked inside it
 * (the decal, for one) -- and the specific one is the answer, whichever order
 * the two were recorded in. */
static int tg_guard_kind_of(size_t off, int *pmark_si)
{
    int i, kind = TG_GK_OTHER;
    size_t best = (size_t)-1;
    if (pmark_si) *pmark_si = -1;
    const int t = tg_tslot();
    for (i = 0; i < s_guard_ex_n(t); i++)
        if (off >= s_guard_ex_lo[t][i] && off < s_guard_ex_hi[t][i]) {
            const size_t w = s_guard_ex_hi[t][i] - s_guard_ex_lo[t][i];
            if (w <= best) {
                best = w;
                kind = (int)s_guard_ex_kind[t][i];
                if (pmark_si) *pmark_si = s_guard_ex_si[t][i];
            }
        }
    return kind;
}

/* ===================== [PICK MESHTAG] emitter-kind provenance ==============
 * Per-(entry, slot) emitter kind (TG_GK_*), captured during scenery assembly
 * and written to level<NN>/MESHTAG.BIN NEXT TO MODELS.DAT -- a SEPARATE file,
 * so MODELS.DAT stays byte-identical (the trackgen A/B invariant). It exists
 * only to let the dev free-cam geometry picker name a hovered mesh
 * ("flora"/"guardrail"/"building"). Dev-only; a no-op in RELEASE.
 *
 * The kind is the same value tg_guard_validate_entry already computes per kept
 * mesh (tg_guard_kind_of); we capture it there, in FINAL slot order. Entries
 * the guard never validates (guard off, or branch-corridor entries s0>=ring)
 * are filled by a fallback at block assembly, where moff[] is still the
 * uncompacted offset the guard marks are keyed by. Runtime index == generator
 * slot == on-disk slot, so (entry, slot) round-trips to the render walk. */
#ifndef TD5RE_RELEASE
#define TD5_TG_MESHTAG_STRIDE 256          /* slots/entry (renderer's block cap) */
#define TD5_TG_MESHTAG_MAGIC  0x4741544Du  /* 'MTAG' */
#define TD5_TG_MESHTAG_NONE   0xFFu
static unsigned char *s_meshtag = NULL;    /* [entry*STRIDE + slot] = TG_GK_* */
static int            s_meshtag_nentries = 0;
void tg_meshtag_reset(int nentries)
{
    free(s_meshtag);
    s_meshtag = NULL;
    s_meshtag_nentries = 0;
    if (nentries <= 0) return;
    s_meshtag = (unsigned char *)malloc((size_t)nentries * TD5_TG_MESHTAG_STRIDE);
    if (s_meshtag) {
        memset(s_meshtag, TD5_TG_MESHTAG_NONE,
               (size_t)nentries * TD5_TG_MESHTAG_STRIDE);
        s_meshtag_nentries = nentries;
    }
}
void tg_meshtag_set(int entry, int slot, int kind)
{
    if (!s_meshtag || entry < 0 || entry >= s_meshtag_nentries) return;
    if (slot < 0 || slot >= TD5_TG_MESHTAG_STRIDE) return;
    if (kind < 0 || kind > 0xFE) return;
    s_meshtag[(size_t)entry * TD5_TG_MESHTAG_STRIDE + slot] = (unsigned char)kind;
}
/* Fill a slot the guard did not (still NONE) from the live guard marks, using
 * the UNCOMPACTED offset. Called at block assembly; no-op if already set. */
void tg_meshtag_fallback(int entry, int slot, size_t off)
{
    int msi;
    if (!s_meshtag || entry < 0 || entry >= s_meshtag_nentries) return;
    if (slot < 0 || slot >= TD5_TG_MESHTAG_STRIDE) return;
    if (s_meshtag[(size_t)entry * TD5_TG_MESHTAG_STRIDE + slot] != TD5_TG_MESHTAG_NONE)
        return;
    s_meshtag[(size_t)entry * TD5_TG_MESHTAG_STRIDE + slot] =
        (unsigned char)tg_guard_kind_of(off, &msi);
}
void tg_meshtag_write(const char *dir)
{
    char path[512];
    FILE *f;
    unsigned int hdr[5];
    if (!s_meshtag || s_meshtag_nentries <= 0 || !dir) return;
    snprintf(path, sizeof path, "%s/MESHTAG.BIN", dir);
    f = fopen(path, "wb");
    if (!f) return;
    hdr[0] = TD5_TG_MESHTAG_MAGIC;
    hdr[1] = 1;                              /* version */
    hdr[2] = td5_trackgen_last_seed();
    hdr[3] = (unsigned)s_meshtag_nentries;
    hdr[4] = TD5_TG_MESHTAG_STRIDE;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(s_meshtag, (size_t)s_meshtag_nentries * TD5_TG_MESHTAG_STRIDE, 1, f);
    fclose(f);
}
/* --- runtime side: lazily load the sidecar and name a (entry, slot) --------- */
static unsigned char *s_meshtag_rt = NULL;
static int            s_meshtag_rt_nentries = 0;
static int            s_meshtag_rt_stride = 0;
static unsigned int   s_meshtag_rt_seed = 0xFFFFFFFFu;
static int            s_meshtag_rt_loaded = 0;
static void tg_meshtag_rt_load(void)
{
    char path[320];
    FILE *f;
    unsigned int hdr[5];
    unsigned int seed = td5_trackgen_last_seed();
    long need;

    if (s_meshtag_rt_loaded && s_meshtag_rt_seed == seed) return;  /* current */

    free(s_meshtag_rt);
    s_meshtag_rt = NULL;
    s_meshtag_rt_nentries = 0;
    s_meshtag_rt_stride = 0;
    s_meshtag_rt_seed = seed;
    s_meshtag_rt_loaded = 1;

    snprintf(path, sizeof path, "re/assets/levels/level%03d/MESHTAG.BIN",
             td5_trackgen_level_number());
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != TD5_TG_MESHTAG_MAGIC) {
        fclose(f);
        return;
    }
    s_meshtag_rt_nentries = (int)hdr[3];
    s_meshtag_rt_stride   = (int)hdr[4];
    need = (long)s_meshtag_rt_nentries * s_meshtag_rt_stride;
    if (s_meshtag_rt_nentries <= 0 || s_meshtag_rt_stride <= 0 ||
        need <= 0 || need > (64L << 20)) {
        s_meshtag_rt_nentries = 0;
        fclose(f);
        return;
    }
    s_meshtag_rt = (unsigned char *)malloc((size_t)need);
    if (!s_meshtag_rt) {
        s_meshtag_rt_nentries = 0;
        fclose(f);
        return;
    }
    if (fread(s_meshtag_rt, (size_t)need, 1, f) != 1) {
        free(s_meshtag_rt);
        s_meshtag_rt = NULL;
        s_meshtag_rt_nentries = 0;
    }
    fclose(f);
}
const char *td5_trackgen_mesh_kind_name(int entry, int slot)
{
    unsigned char k;
    tg_meshtag_rt_load();
    if (!s_meshtag_rt) return NULL;
    if (entry < 0 || entry >= s_meshtag_rt_nentries) return NULL;
    if (slot < 0 || slot >= s_meshtag_rt_stride) return NULL;
    k = s_meshtag_rt[(size_t)entry * s_meshtag_rt_stride + slot];
    if (k == TD5_TG_MESHTAG_NONE || k >= TG_GK_COUNT) return NULL;
    return k_guard_kind_name[k];
}
#else  /* TD5RE_RELEASE -- feature compiled out */
#define tg_meshtag_reset(n)          ((void)0)
#define tg_meshtag_set(e, s, k)      ((void)0)
#define tg_meshtag_fallback(e, s, o) ((void)0)
#define tg_meshtag_write(dir)        ((void)0)
const char *td5_trackgen_mesh_kind_name(int entry, int slot)
{ (void)entry; (void)slot; return NULL; }
#endif /* TD5RE_RELEASE */

const char *const k_pave_src_name[TG_PVS_COUNT] = {
    "city-slab", "verge-band", "branch-slab", "branch-verge", "arm"
};

/* [S2h] One row per thread slot (was __thread -- see tg_tslot). */
static size_t        s_pave_mark_lo[TG_ACCT_SLOTS][TD5_TG_PAVE_MARK_MAX];

static size_t        s_pave_mark_hi[TG_ACCT_SLOTS][TD5_TG_PAVE_MARK_MAX];

static unsigned char s_pave_mark_src[TG_ACCT_SLOTS][TD5_TG_PAVE_MARK_MAX];

static int           s_pave_mark_si[TG_ACCT_SLOTS][TD5_TG_PAVE_MARK_MAX];

TG_SlotInt    s_pave_mark_nx[TG_ACCT_SLOTS];

void tg_pave_mark_reset(void) { s_pave_mark_n(tg_tslot()) = 0; }

void tg_pave_mark(size_t lo, size_t hi, int src, int si)
{
    const int t = tg_tslot();
    int e;
    if (hi <= lo || s_pave_mark_n(t) >= TD5_TG_PAVE_MARK_MAX) return;
    e = s_pave_mark_n(t);
    s_pave_mark_lo[t][e]  = lo;
    s_pave_mark_hi[t][e]  = hi;
    s_pave_mark_src[t][e] = (unsigned char)src;
    s_pave_mark_si[t][e]  = si;
    s_pave_mark_n(t)++;
}

/* Which pavement emitter produced the mesh at byte offset `off`, or -1. */
int tg_pave_src_of(size_t off, int *pmark_si)
{
    int i, src = -1;
    size_t best = (size_t)-1;
    if (pmark_si) *pmark_si = -1;
    const int t = tg_tslot();
    for (i = 0; i < s_pave_mark_n(t); i++)
        if (off >= s_pave_mark_lo[t][i] && off < s_pave_mark_hi[t][i]) {
            const size_t w = s_pave_mark_hi[t][i] - s_pave_mark_lo[t][i];
            if (w <= best) {
                best = w;
                src = (int)s_pave_mark_src[t][i];
                if (pmark_si) *pmark_si = s_pave_mark_si[t][i];
            }
        }
    return src;
}

/* Little-endian scalar reads over the assembled mesh bytes. */
float tg_rd_f32(const unsigned char *p)
{
    unsigned int u = (unsigned int)p[0] | ((unsigned int)p[1] << 8)
                   | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

unsigned int tg_rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

unsigned int tg_rd_u16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Byte length of the mesh whose 0x38 header starts at b[off]. The header stores
 * the vertex-block offset (0x30) and the de-indexed vertex count (0x08), and the
 * vertices are the last thing in the record, so end = vtx_off + count*44. Returns
 * 0 if the header does not look like one of ours (fail safe: caller keeps it). */
size_t tg_guard_mesh_len(const unsigned char *b, size_t off, size_t cap)
{
    unsigned int vtxoff, vtxcnt;
    if (off + TD5_TG_MESH_DISK_SIZE > cap) return 0;
    if (tg_rd_u16(b + off) != 259) return 0;
    vtxcnt = tg_rd_u32(b + off + 0x08);
    vtxoff = tg_rd_u32(b + off + 0x30);
    if (vtxoff < TD5_TG_MESH_DISK_SIZE) return 0;
    return (size_t)vtxoff + (size_t)vtxcnt * TD5_TG_VTX_SIZE;
}

int tg_guard_nearest_node(const TG_NodeList *nl, int lo, int hi,
                                 double wx, double wz)
{
    int i, best = lo;
    double bd = 1e300;
    for (i = lo; i <= hi; i++) {
        double dx = wx - nl->v[i].x, dz = wz - nl->v[i].z;
        double d2 = dx * dx + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    if (bd > TD5_TG_GUARD_NEAR_MAX * TD5_TG_GUARD_NEAR_MAX &&
        td5_env_flag_on("TD5RE_R11_GUARD_NEAR"))
        return -1;
    return best;
}

/* [R10 SPAN66] The tarmac policy class `cls` may not stand on. Everything except
 * FURNITURE gets the carriageway authority verbatim, so this is bit-identical to
 * the pre-R10 call for every other kind. */
static double tg_guard_reach(const TG_NodeList *nl, int si, double side, int cls)
{
    if (cls == TG_GKC_FURNITURE) return tg_footway_reach(nl, si, side);
    return tg_carriageway_reach(nl, si, side);
}

/* Is a quad with this intrusion, height band and policy class illegal? */
static int tg_guard_quad_bad(int cls, double intr, double dy_lo, double dy_hi)
{
    if (intr <= TD5_TG_GUARD_PEN)        return 0;   /* edge-hugging: legal */
    if (dy_hi < -TD5_TG_GUARD_UNDER)     return 0;   /* buried under the road */
    if (cls == TG_GKC_EXEMPT)            return 0;   /* authored across it */
    if (cls == TG_GKC_DECAL)                         /* legal only flush */
        return !(dy_lo >= -TD5_TG_GUARD_FLUSH && dy_hi <= TD5_TG_GUARD_FLUSH);
    /* [R8 item 7] UNDER: the ground skirt, the sea plane and the coastline are
     * ALLOWED to reach across the carriageway -- they are the surface it is laid
     * on -- but only from BELOW. This is the discrimination R8 item 7 turned out
     * to need. MEASURED: seed 99991's skirt for spans 643..647 lies over the
     * carriageway at spans 626..629 at dy +250..+450 (the road doubles back on
     * itself at a different height), which is the user's "tiles spilling over
     * the road around 627" exactly. A blanket skirt exemption licenses that; a
     * span-scoped one would instead have rejected the perfectly good ground the
     * same emitter lays UNDER the road. Height is the axis that separates them. */
    if (cls == TG_GKC_UNDER)
        return dy_hi > TD5_TG_GUARD_FLUSH;
    if (dy_lo > TD5_TG_GUARD_OVERHEAD)   return 0;   /* clears it overhead */
    return 1;
}

/* Accumulate one candidate group's verdict into the running worst. `bad` quads
 * outrank clean ones outright, so the reported hit always explains the verdict. */
static void tg_guard_hit_keep(TG_GuardHit *h, int *have_bad, int bad,
                              double intr, double dy_lo, double dy_hi, int si)
{
    if (*have_bad && !bad) return;
    if (bad && !*have_bad) { *have_bad = 1; h->intr = -1e300; }
    if (intr > h->intr) {
        h->intr = intr; h->dy_lo = dy_lo; h->dy_hi = dy_hi; h->si = si;
    }
}

/* Scan the mesh at `off` against the carriageway under policy class `cls`.
 * Returns 1 when some part of it is illegal, filling *hit with the worst
 * offending group. When nothing is illegal *hit still carries the worst
 * intrusion seen, which is what the DIAG dump prints. */
static int tg_guard_mesh_scan(const TG_NodeList *nl, int ring,
                              const unsigned char *b, size_t off, size_t mlen,
                              int win_lo, int win_hi, int cls, TG_GuardHit *hit)
{
    const int    tag    = (int)tg_rd_u16(b + off + 0x02);
    const unsigned int vtxcnt = tg_rd_u32(b + off + 0x08);
    const unsigned int vtxoff = tg_rd_u32(b + off + 0x30);
    const int r8 = tg_guard_r8();
    TG_GuardHit worst;
    unsigned int vi;
    int bad = 0, have_bad = 0;

    worst.intr = -1e300; worst.dy_lo = 0.0; worst.dy_hi = 0.0; worst.si = -1;
    /* Two running maxima, because the widest-covering quad of a mesh is often
     * NOT the illegal one (a terrain apron's deepest quad can be buried under
     * the road while a different quad of the same mesh stands on it). Reporting
     * the first would have made the DIAG dump lie about why a mesh was dropped,
     * which is the kind of half-truth that produced the diagnosis this round had
     * to re-measure. Once anything is illegal, only illegal quads compete. */

    if (tag) {
        /* Billboard. The vertices are LOCAL offsets about a 24.8 world origin
         * and the quad is turned at the camera every frame, so its footprint is
         * not a quad at all -- it is a DISC of radius max|local x| about the
         * origin. Testing it as a disc is both cheaper and orientation-correct;
         * the R7 code added local x to world X, which only held where the road
         * happened to run along Z. */
        const double ox = (double)tg_rd_f32(b + off + 0x1C) / 256.0;
        const double oy = (double)tg_rd_f32(b + off + 0x20) / 256.0;
        const double oz = (double)tg_rd_f32(b + off + 0x24) / 256.0;
        double half = 0.0, top = 0.0, bot = 0.0;
        double lateral, rp, rn, cover, pen, roady;
        int si;
        for (vi = 0; vi < vtxcnt; vi++) {
            const unsigned char *vp = b + off + vtxoff + (size_t)vi * TD5_TG_VTX_SIZE;
            double lx, ly;
            if ((size_t)(vp + 12 - b) > off + mlen) break;
            lx = (double)tg_rd_f32(vp);
            ly = (double)tg_rd_f32(vp + 4);
            if (lx < 0.0) lx = -lx;
            if (lx > half) half = lx;
            if (ly > top)  top = ly;
            if (ly < bot)  bot = ly;
        }
        if (!r8) {
            /* R7 fallback path, kept BYTE-FAITHFUL: R7 added the billboard's
             * local x straight onto world X and tested each resulting point.
             * That is only correct where the road runs along Z, but the A/B
             * knob is worthless unless it reproduces what R7 actually did. */
            for (vi = 0; vi < vtxcnt; vi++) {
                const unsigned char *vp = b + off + vtxoff
                                        + (size_t)vi * TD5_TG_VTX_SIZE;
                double vx, vy, vz, lat, r, d;
                int s2;
                if ((size_t)(vp + 12 - b) > off + mlen) break;
                vx = ox + (double)tg_rd_f32(vp);
                vy = oy + (double)tg_rd_f32(vp + 4);
                vz = oz + (double)tg_rd_f32(vp + 8);
                s2 = tg_guard_nearest_node(nl, win_lo, win_hi, vx, vz);
                if (s2 < 0 || s2 >= ring) continue;
                if (vy > nl->v[s2].y + TD5_TG_GUARD_OVERHEAD) continue;
                if (vy < nl->v[s2].y - TD5_TG_GUARD_UNDER)    continue;
                lat = (vx - nl->v[s2].x) * nl->v[s2].tz
                    - (vz - nl->v[s2].z) * nl->v[s2].tx;
                r = tg_guard_reach(nl, s2, (lat >= 0.0) ? 1.0 : -1.0, cls);
                d = r - (lat >= 0.0 ? lat : -lat);
                if (d > worst.intr) {
                    worst.intr = d; worst.si = s2;
                    worst.dy_lo = worst.dy_hi = vy - nl->v[s2].y;
                }
            }
            bad = (worst.intr > TD5_TG_GUARD_PEN && cls != TG_GKC_EXEMPT);
            if (hit) *hit = worst;
            return bad;
        }
        si = tg_guard_nearest_node(nl, win_lo, win_hi, ox, oz);
        if (si < 0 || si >= ring) { if (hit) *hit = worst; return 0; }
        roady = nl->v[si].y;
        lateral = (ox - nl->v[si].x) * nl->v[si].tz
                - (oz - nl->v[si].z) * nl->v[si].tx;
        rp = tg_guard_reach(nl, si, 1.0, cls);
        rn = tg_guard_reach(nl, si, -1.0, cls);
        cover = (lateral + half < rp ? lateral + half : rp)
              - (lateral - half > -rn ? lateral - half : -rn);
        pen = (lateral >= 0.0 ? rp : rn) - (lateral >= 0.0 ? lateral : -lateral);
        if (!r8 || cover < pen) cover = pen;
        bad = tg_guard_quad_bad(cls, cover, oy + bot - roady, oy + top - roady);
        tg_guard_hit_keep(&worst, &have_bad, bad, cover,
                          oy + bot - roady, oy + top - roady, si);
        if (hit) *hit = worst;
        return bad;
    }

    /* Opaque geometry: de-indexed QUADS, which is what every writer in this file
     * emits (road strip, box faces, facade cells, decals). Walking them four at
     * a time is what makes the lateral INTERVAL meaningful -- a per-mesh bounding
     * box would fuse a building's near and far walls into one slab and a whole
     * tunnel entry would read as covering the road. */
    for (vi = 0; vi < vtxcnt; vi += 4) {
        /* EACH VERTEX IS MEASURED IN ITS OWN SPAN'S FRAME, and its lateral is
         * NORMALISED by that span's own reach (t = +-1 is the road edge). A
         * quad spans a whole road segment, so its far corner belongs to the
         * next node; projecting all four into one frame put a lateral error of
         * (span length x curvature) on the near corner, and on a bend that read
         * as ~750 units of false coverage. It rejected 34 perfectly good verge
         * bands on seed 777 -- every one of them at si = mark_si+1, which is
         * what gave the artefact away. Normalising per vertex removes the frame
         * error AND handles a mesh whose corners resolve to different spans,
         * which is exactly the doubling-back case this guard has to judge. */
        double t[4], dy[4];
        double t_lo, t_hi, dy_lo, dy_hi, r_rep = 0.0, t_best = 1e300;
        double cover, pen;
        int k, n = 0, si0 = -1, qs_lo = -1, qs_hi = -1;

        for (k = 0; k < 4 && vi + (unsigned)k < vtxcnt; k++) {
            const unsigned char *vp = b + off + vtxoff
                                    + (size_t)(vi + (unsigned)k) * TD5_TG_VTX_SIZE;
            double vx, vy, vz, lateral, r;
            int si;
            if ((size_t)(vp + 12 - b) > off + mlen) break;
            vx = (double)tg_rd_f32(vp);
            vy = (double)tg_rd_f32(vp + 4);
            vz = (double)tg_rd_f32(vp + 8);
            si = tg_guard_nearest_node(nl, win_lo, win_hi, vx, vz);
            if (si < 0 || si >= ring) continue;
            lateral = (vx - nl->v[si].x) * nl->v[si].tz
                    - (vz - nl->v[si].z) * nl->v[si].tx;
            r = tg_guard_reach(nl, si, (lateral >= 0.0) ? 1.0 : -1.0, cls);
            if (!(r > 0.0)) continue;
            t[n] = lateral / r;
            dy[n] = vy - nl->v[si].y;
            r_rep += r;
            /* Report the span the mesh reaches DEEPEST into, not an arbitrary
             * corner's: that is the span the user is standing on. */
            if (si0 < 0 || (lateral >= 0.0 ? t[n] : -t[n]) < t_best) {
                t_best = (lateral >= 0.0 ? t[n] : -t[n]);
                si0 = si;
            }
            if (qs_lo < 0 || si < qs_lo) qs_lo = si;
            if (qs_hi < 0 || si > qs_hi) qs_hi = si;
            n++;
        }
        if (n <= 0) continue;
        /* [R11 BRIDGE item 7a] A QUAD MUST BE SMALL ENOUGH TO FRAME.
         *
         * The per-vertex normalisation above is what makes `cover` meaningful,
         * and it rests on an assumption nobody had had to state: that a quad's
         * four corners resolve to spans NEAR EACH OTHER, so mixing their t
         * values describes one stretch of road. Extending the overpass arms to
         * the drawn edge broke that assumption for the first time. The deck is
         * one quad 66000 units wide; on seed 20260901 its left corner resolved
         * into span 1089's frame and its right corner into span 1167's, forty
         * spans apart. t_lo came from one road and t_hi from the other, cover
         * came out at a full 6000 of carriageway, and the guard REJECTED the
         * deck at a span its own emitter never touched -- three "the guard is
         * eating the track" warnings, all marked @1130, with no actual overlap
         * anywhere (two separate geometric caps were built to find one and
         * found nothing).
         *
         * So: if a quad's corners straddle more spans than the exemption scope
         * itself covers, there is no single frame in which to judge it, and a
         * number computed across two frames is not evidence. Skip it. This is
         * strictly a refusal to make a claim, not a new licence -- the guard
         * already skips any vertex it cannot place (si < 0 above).
         *
         * TD5RE_R11_GUARD_NEAR=0 restores the unframed judgement for an A/B. */
        if (qs_hi - qs_lo > TD5_TG_GUARD_EX_SPANS &&
            td5_env_flag_on("TD5RE_R11_GUARD_NEAR"))
            continue;
        r_rep /= (double)n;
        t_lo = t_hi = t[0];
        dy_lo = dy_hi = dy[0];
        pen = 0.0;
        for (k = 0; k < n; k++) {
            double d;
            if (t[k] < t_lo) t_lo = t[k];
            if (t[k] > t_hi) t_hi = t[k];
            if (dy[k] < dy_lo) dy_lo = dy[k];
            if (dy[k] > dy_hi) dy_hi = dy[k];
            /* R7's per-vertex penetration, height-gated per vertex as it was. */
            if (dy[k] > TD5_TG_GUARD_OVERHEAD || dy[k] < -TD5_TG_GUARD_UNDER)
                continue;
            d = (1.0 - (t[k] >= 0.0 ? t[k] : -t[k])) * r_rep;
            if (d > pen) pen = d;
        }
        /* Road half-widths cancel: the overlap is computed in t and scaled back
         * by the representative reach, so it reads in world units as before. */
        cover = ((t_hi < 1.0 ? t_hi : 1.0) - (t_lo > -1.0 ? t_lo : -1.0)) * r_rep;
        if (!r8 || cover < pen) cover = pen;
        {   /* keep scanning: the DIAG dump wants the WORST offending quad,
             * not the first one found. */
            const int qbad = tg_guard_quad_bad(cls, cover, dy_lo, dy_hi);
            tg_guard_hit_keep(&worst, &have_bad, qbad, cover, dy_lo, dy_hi, si0);
            if (qbad) bad = 1;
        }
    }
    if (hit) *hit = worst;
    return bad;
}

/* R7-shaped wrapper kept for the residual self-check and the exempt-scope test:
 * "is this mesh illegal under class `cls`", reporting depth and span. */
static int tg_guard_mesh_on_road_cls(const TG_NodeList *nl, int ring,
                                     const unsigned char *b, size_t off,
                                     size_t mlen, int win_lo, int win_hi,
                                     int cls, double *pdepth, int *psi)
{
    TG_GuardHit hit;
    int bad = tg_guard_mesh_scan(nl, ring, b, off, mlen, win_lo, win_hi,
                                 cls, &hit);
    if (bad) {
        if (pdepth) *pdepth = hit.intr;
        if (psi)    *psi = hit.si;
    }
    return bad;
}

int    s_r9_wet_total;

int    s_r9_wet_kind[TG_GK_COUNT];

int    s_r9_wet_first_span = -1;

double s_r9_wet_worst;

/* Which kinds are ALLOWED over water. The deck, its structure and rails, the
 * water itself, the shore band and the ground SKIRT all legitimately meet the
 * water -- a crossing flies over it and a bank descends through it (R8 made the
 * gorge skirt a shore that crosses the surface on purpose). Everything else
 * standing above the surface is the defect item 10 reports. */
static int tg_r9_wet_kind_ok(int kind)
{
    return kind == TG_GK_DECK || kind == TG_GK_WATER || kind == TG_GK_COAST ||
           kind == TG_GK_ROAD || kind == TG_GK_BRANCHROAD ||
           kind == TG_GK_GANTRY || kind == TG_GK_RAIL || kind == TG_GK_TUNNEL ||
           kind == TG_GK_SKIRT;
}

/* REJECT, not just report. Default ON. Same argument as the R7 on-road guard:
 * four rounds of per-emitter placement patches failed because the safety
 * depended on every emitter remembering to ask, and the cure was one post-emit
 * pass over the assembled bytes. "Nothing but the crossing stands on water" is
 * the same kind of rule, so it gets the same kind of enforcement -- and a future
 * emitter that forgets is caught for free. TD5RE_R9_BRIDGE_WATERGUARD=0 leaves
 * it report-only.
 *
 * [R10] This knob was parked OFF at the R9 merge because turning it on killed
 * seed 777 during generation -- silently, with race.log frozen mid-build and no
 * [ERR] line. IT WAS NEVER THIS PASS'S BUG. Measured, not reasoned:
 *
 *   - the process died with exit code 0xC0000374 (STATUS_HEAP_CORRUPTION),
 *     inside free() called from tg_emit_models;
 *   - a red-zoned TG_Buf allocator caught the write as an UNDERFLOW of the next
 *     heap block, raised during tg_guard_validate_entry;
 *   - the compaction cursor there ran to w=34616 in a buffer whose content was
 *     30264 bytes and whose allocation was 32768 -- 1848 bytes past the end.
 *
 * The cursor overran because moff[] was NOT in ascending byte order. R9's city
 * underpass (item 13) appends its meshes and records their offsets, but the
 * building offset `b0` next to it had been captured BEFORE that block and was
 * pushed AFTER it, so on every underpass span moff[] stepped backwards. The
 * compaction walked moff[] in recorded order with one forward write cursor, so
 * a backwards offset let the cursor overtake the read position and keep going.
 *
 * That made it look like this knob's fault: the drift only starts once some
 * mesh is dropped, and how far it runs scales with how many are dropped. Seed
 * 777 drops 207 and crossed the end of the allocation; seed 99991 drops 127 and
 * the same latent overrun stayed inside it (harmless, invisible). Guard off,
 * the overrun was still there on 777 at three other entries -- just short of
 * the allocation edge. Both the stale `b0` and the compaction's assumption are
 * fixed; see tg_emit_models (b0) and tg_guard_validate_entry (`ord`). */
static int tg_r9_waterguard_enabled(void)
{
    return td5_env_flag_on("TD5RE_R9_BRIDGE_WATERGUARD");
}

int s_r9_wet_rejected;

int    s_r9_wet_ready;

static int    s_r9_wet_span[TD5_TG_R9_WET_MAX];

static double s_r9_wet_surf[TD5_TG_R9_WET_MAX];

static double s_r9_wet_out[TD5_TG_R9_WET_MAX];

static double s_r9_wet_in[TD5_TG_R9_WET_MAX];

static double s_r9_wet_side[TD5_TG_R9_WET_MAX];  /* 0 = river (both sides) */

static int    s_r9_wet_count;

/* [CRASH] Which node list the table above was built FROM. The span indices it
 * stores are only meaningful for that list: every reader dereferences
 * nl->v[s + 1], and the build loop is the only thing that guarantees
 * s + 1 < nl->count. s_r9_wet_ready alone could not carry that guarantee
 * across a SECOND generation in the same process, because the single place
 * that cleared it (tg_r9_bridge_report) returns early when its report knob is
 * off, while the warm site in td5_trackgen.c builds the table on every
 * generation. With the flag stuck at 1, generation 2 kept generation 1's spans
 * and read one node past the end of a shorter list -- an access violation
 * inside the over-water audit (crash.1.log, 2026-09-09). Keying the freshness
 * on the list itself makes the invariant hold no matter who forgets to reset. */
static const TG_NodeList *s_r9_wet_nl;

static int    s_r9_wet_nl_count;

int tg_r9_water_table_stale(const TG_NodeList *nl)
{
    return !s_r9_wet_ready || s_r9_wet_nl != nl || s_r9_wet_nl_count != nl->count;
}

void tg_r9_water_table_build(const TG_NodeList *nl)
{
    int s;
    s_r9_wet_ready = 0;
    s_r9_wet_count = 0;
    s_r9_wet_nl = nl;
    s_r9_wet_nl_count = nl->count;
    for (s = 0; s + 1 < nl->count && s_r9_wet_count < TD5_TG_R9_WET_MAX; s++) {
        double side;
        /* [TOPOLOGY-FIRST] from the road module's shore table. */
        if (tg_span_in_bridge_run(s) && tg_bridge_run_is_water(nl, s) &&
            tg_water_span_clear(s)) {
            s_r9_wet_span[s_r9_wet_count] = s;
            s_r9_wet_surf[s_r9_wet_count] = tg_bridge_water_surf_y(nl, s);
            s_r9_wet_out [s_r9_wet_count] = tg_r11_wet_reach(nl, s);
            s_r9_wet_in  [s_r9_wet_count] = 0.0;
            s_r9_wet_side[s_r9_wet_count] = 0.0;
            s_r9_wet_count++;
        } else if ((side = tg_water_side(s)) != 0.0) {
            s_r9_wet_span[s_r9_wet_count] = s;
            s_r9_wet_surf[s_r9_wet_count] = tg_road_shore_y(s, side > 0.0);
            s_r9_wet_out [s_r9_wet_count] = (double)TD5_TG_WATER_EXTENT
                                          + tg_road_shore_d(s, side > 0.0);
            s_r9_wet_in  [s_r9_wet_count] = nl->v[s].width * 0.5
                                          + tg_road_shore_d(s, side > 0.0);
            s_r9_wet_side[s_r9_wet_count] = side;
            s_r9_wet_count++;
        }
    }
    /* [S0] READY LAST. This used to be set at the TOP of the function, before
     * a single element existed, which is the one publish order that cannot
     * work: a parallel entry seeing ready==1 would read a table that is still
     * being filled, and this table decides whether a mesh is DROPPED
     * (tg_r9_water_audit_mesh), so a torn read silently deletes or keeps
     * buildings. Single-threaded the old order was harmless because the
     * function ran to completion before anything read it. */
    TG_COMPILER_BARRIER();
    s_r9_wet_ready = 1;
}

/* Is the world point (cx,cz) inside the water rectangle of the span whose near
 * node is n0? Exactly the predicate the corner loop below used to inline, hoisted
 * so the AABB sampling and the geometry sampling cannot drift apart. */
static int tg_r13_wet_point(const TG_Node *n0, double len, double inner,
                            double outer, double side, double cx, double cz)
{
    const double dx = cx - n0->x, dz = cz - n0->z;
    const double along = dx * n0->tx + dz * n0->tz;
    double lat = dx * n0->tz - dz * n0->tx;
    if (along < 0.0 || along > len) return 0;
    if (side == 0.0) return (lat > -outer && lat < outer);
    lat *= side;
    return (lat > inner && lat < outer);
}

/* [R13 FACES item 6] Does this mesh actually PUT GEOMETRY over the water?
 *
 * The audit used to answer that with the four corners of the mesh's axis-aligned
 * BOUNDING BOX. A bounding-box corner is not a point of the mesh: it is the
 * furthest-out place the mesh could possibly reach, on both axes at once. So a
 * building standing squarely on dry land beside a river was condemned whenever
 * the diagonal of its box happened to poke into the river rectangle, and the
 * BIGGER the mesh, the more of its box is empty air and the likelier that is.
 * That bias is the whole defect: a run-END frontage carries its corner returns,
 * so it is the largest mesh on the street (440 verts against 112 on seed
 * 20260901 span 1200), and it was the one the audit ate. What survived at a
 * bridge exit was the first run-INTERIOR span -- a front plane, a roof and no
 * lateral face anywhere: the reported "flat card".
 *
 * The replacement samples the geometry itself: every vertex, plus one centroid
 * per quad so a face that straddles the rectangle without landing a vertex in it
 * is still caught. Same rectangle, same height gate, same accounting -- only the
 * sample points change. TD5RE_R13_FACES_WATERGEOM=0 restores the box corners. */
static int tg_r13_wet_geom_hit(const unsigned char *b, size_t off, size_t mlen,
                               size_t vtxoff, unsigned vtxcnt,
                               double ox, double oz, const TG_Node *n0,
                               double len, double inner, double outer,
                               double side)
{
    double qx = 0.0, qz = 0.0;
    unsigned vi, inq = 0;

    for (vi = 0; vi < vtxcnt; vi++) {
        const unsigned char *vp = b + off + vtxoff + (size_t)vi * TD5_TG_VTX_SIZE;
        double vx, vz;
        if ((size_t)(vp + 12 - b) > off + mlen) break;
        vx = ox + (double)tg_rd_f32(vp);
        vz = oz + (double)tg_rd_f32(vp + 8);
        if (tg_r13_wet_point(n0, len, inner, outer, side, vx, vz)) return 1;
        qx += vx; qz += vz;
        if (++inq == 4) {
            if (tg_r13_wet_point(n0, len, inner, outer, side,
                                 qx * 0.25, qz * 0.25)) return 1;
            qx = 0.0; qz = 0.0; inq = 0;
        }
    }
    return 0;
}

static int tg_r9_water_audit_mesh(const TG_NodeList *nl, const unsigned char *b,
                                  size_t off, size_t mlen, int kind)
{
    /* Same 0x38 header fields tg_guard_mesh_scan parses: count at 0x08, vertex
     * block offset at 0x30, billboard tag at 0x02 (its vertices are LOCAL about
     * a 24.8 origin). One XZ bounding box per mesh, tested against the water
     * table above. */
    const size_t vtxoff = (size_t)tg_rd_u32(b + off + 0x30);
    const unsigned vtxcnt = tg_rd_u32(b + off + 0x08);
    const int tag = (int)tg_rd_u16(b + off + 0x02);
    double ox = 0.0, oy = 0.0, oz = 0.0;
    double x0 = 0.0, x1 = 0.0, z0 = 0.0, z1 = 0.0, ytop = -1e30;
    unsigned vi;
    int w, have = 0;

    if (!td5_env_flag_on("TD5RE_R9_BRIDGE_REPORT")) return 0;
    if (tg_r9_wet_kind_ok(kind)) return 0;
    if (vtxcnt == 0 || vtxcnt > 65536) return 0;
    if (tg_r9_water_table_stale(nl)) tg_r9_water_table_build(nl);
    if (tag) {
        ox = (double)tg_rd_f32(b + off + 0x1C) / 256.0;
        oy = (double)tg_rd_f32(b + off + 0x20) / 256.0;
        oz = (double)tg_rd_f32(b + off + 0x24) / 256.0;
    }
    for (vi = 0; vi < vtxcnt; vi++) {
        const unsigned char *vp = b + off + vtxoff + (size_t)vi * TD5_TG_VTX_SIZE;
        double vx, vy, vz;
        if ((size_t)(vp + 12 - b) > off + mlen) break;
        vx = ox + (double)tg_rd_f32(vp);
        vy = oy + (double)tg_rd_f32(vp + 4);
        vz = oz + (double)tg_rd_f32(vp + 8);
        if (!have) { x0 = x1 = vx; z0 = z1 = vz; ytop = vy; have = 1; }
        if (vx < x0) x0 = vx;
        if (vx > x1) x1 = vx;
        if (vz < z0) z0 = vz;
        if (vz > z1) z1 = vz;
        if (vy > ytop) ytop = vy;
    }
    if (!have) return 0;

    for (w = 0; w < s_r9_wet_count; w++) {
        const int s = s_r9_wet_span[w];
        const TG_Node *n0, *n1;
        /* [CRASH] Second line of defence for the same defect the freshness key
         * above fixes: a span index is only ever read together with its far
         * node, so a table entry that no longer has one is skipped, not
         * dereferenced. Cheap, and it cannot mask a real hit -- inside a valid
         * table every entry passes. */
        if (s < 0 || s + 1 >= nl->count) continue;
        n0 = &nl->v[s]; n1 = &nl->v[s + 1];
        const double surf = s_r9_wet_surf[w];
        const double outer = s_r9_wet_out[w], inner = s_r9_wet_in[w];
        const double side = s_r9_wet_side[w];
        double ax = n1->x - n0->x, az = n1->z - n0->z;
        double len = sqrt(ax * ax + az * az);
        int c;
        if (ytop <= surf + TD5_TG_R9_WET_LIFT) continue;   /* at/under the surface */
        {   /* cheap reject: the node cannot reach this box at all */
            const double nx = n0->x < x0 ? x0 : (n0->x > x1 ? x1 : n0->x);
            const double nz = n0->z < z0 ? z0 : (n0->z > z1 ? z1 : n0->z);
            const double dd = (nx - n0->x) * (nx - n0->x)
                            + (nz - n0->z) * (nz - n0->z);
            if (dd > (outer + len) * (outer + len)) continue;
        }
        {
            int hit = 0;
            if (td5_env_flag_on("TD5RE_R13_FACES_WATERGEOM")) {
                hit = tg_r13_wet_geom_hit(b, off, mlen, vtxoff, vtxcnt, ox, oz,
                                          n0, len, inner, outer, side);
            } else {
                for (c = 0; c < 4 && !hit; c++)
                    hit = tg_r13_wet_point(n0, len, inner, outer, side,
                                           (c & 1) ? x1 : x0, (c & 2) ? z1 : z0);
            }
            if (!hit) continue;
            s_r9_wet_total++;
            s_r9_wet_kind[kind]++;
            if (s_r9_wet_first_span < 0) s_r9_wet_first_span = s;
            if (ytop - surf > s_r9_wet_worst) s_r9_wet_worst = ytop - surf;
            if (!tg_r9_waterguard_enabled()) return 0;
            s_r9_wet_rejected++;
            tg_acct(TG_ACCT_R9_BRIDGE, s);
            /* [R13 FACES item 6] SAY SO. This is the SECOND path in the
             * generator that removes an assembled mesh, and it was the silent
             * one: the on-road guard logs its first eight rejects by kind and
             * span, this logged nothing at all, so three buildings vanishing at
             * a bridge exit left a race.log reading "on-road guard: clean". A
             * removal path that cannot be seen in the log costs a whole round
             * to rediscover, so it now reports in the same shape as its
             * sibling. Capped the same way, since the totals and the per-kind
             * breakdown are already in the R9BRIDGE summary line. */
            if (s_r9_wet_rejected <= 8)
                TD5_LOG_W(LOG_TAG, "over-water audit: rejected %s mesh at span "
                          "%d (%.0f above the surface, %u verts)",
                          k_guard_kind_name[kind], s, ytop - surf, vtxcnt);
            return 1;                          /* one strike per mesh: DROP it */
        }
    }
    return 0;
}

static long   s_r14_str_total;

static long   s_r14_str_kind[TG_GK_COUNT];

static double s_r14_str_worst;

static int    s_r14_str_worst_span = -1;

static int    s_r14_str_worst_kind = TG_GK_OTHER;

static int    s_r14_str_logged;

static void tg_r14_coast_audit_mesh(const TG_NodeList *nl, const unsigned char *b,
                                    size_t off, size_t mlen, int kind)
{
    const size_t vtxoff = (size_t)tg_rd_u32(b + off + 0x30);
    const unsigned vtxcnt = tg_rd_u32(b + off + 0x08);
    const int tag = (int)tg_rd_u16(b + off + 0x02);
    double ox = 0.0, oy = 0.0, oz = 0.0;
    unsigned vi;
    int w, cap = 24;

    if (!td5_env_flag_off("TD5RE_R14_COAST_REPORT")) return;   /* default OFF */
    if (vtxcnt == 0 || vtxcnt > 65536) return;
    if (tg_r9_water_table_stale(nl)) tg_r9_water_table_build(nl);
    if (tag) {
        ox = (double)tg_rd_f32(b + off + 0x1C) / 256.0;
        oy = (double)tg_rd_f32(b + off + 0x20) / 256.0;
        oz = (double)tg_rd_f32(b + off + 0x24) / 256.0;
    }

    for (w = 0; w < s_r9_wet_count; w++) {
        const int s = s_r9_wet_span[w];
        const TG_Node *n0, *n1;
        /* [CRASH] Second line of defence for the same defect the freshness key
         * above fixes: a span index is only ever read together with its far
         * node, so a table entry that no longer has one is skipped, not
         * dereferenced. Cheap, and it cannot mask a real hit -- inside a valid
         * table every entry passes. */
        if (s < 0 || s + 1 >= nl->count) continue;
        n0 = &nl->v[s]; n1 = &nl->v[s + 1];
        const double surf = s_r9_wet_surf[w];
        const double outer = s_r9_wet_out[w], inner = s_r9_wet_in[w];
        const double side = s_r9_wet_side[w];
        const double ax = n1->x - n0->x, az = n1->z - n0->z;
        const double len = sqrt(ax * ax + az * az);
        double hi = -1e30, lo = 1e30, latsum = 0.0;
        int nin = 0;

        /* Vertices of THIS mesh that lie inside THIS span's water rectangle. */
        for (vi = 0; vi < vtxcnt; vi++) {
            const unsigned char *vp = b + off + vtxoff
                                    + (size_t)vi * TD5_TG_VTX_SIZE;
            double vx, vy, vz, dx, dz;
            if ((size_t)(vp + 12 - b) > off + mlen) break;
            vx = ox + (double)tg_rd_f32(vp);
            vy = oy + (double)tg_rd_f32(vp + 4);
            vz = oz + (double)tg_rd_f32(vp + 8);
            if (!tg_r13_wet_point(n0, len, inner, outer, side, vx, vz)) continue;
            dx = vx - n0->x; dz = vz - n0->z;
            latsum += dx * n0->tz - dz * n0->tx;
            if (vy > hi) hi = vy;
            if (vy < lo) lo = vy;
            nin++;
        }
        if (nin < 2) continue;
        /* STRADDLE: geometry on both sides of the surface plane. */
        if (!(hi > surf + TD5_TG_R14_STRADDLE &&
              lo < surf - TD5_TG_R14_STRADDLE)) continue;
        s_r14_str_total++;
        s_r14_str_kind[kind]++;
        if (hi - surf > s_r14_str_worst) {
            s_r14_str_worst = hi - surf;
            s_r14_str_worst_span = s;
            s_r14_str_worst_kind = kind;
        }
        /* A window pins the dump to the span the user is standing on; without
         * one the first two dozen lines are always the FIRST bridge run and the
         * reported span is never the one the complaint is about. Inside a
         * window the cap is wide, because the point is the whole picture there. */
        {
            const int w0 = td5_env_int("TD5RE_R14_COAST_SPAN0", -1, -1, 100000);
            const int w1 = td5_env_int("TD5RE_R14_COAST_SPAN1", -1, -1, 100000);
            if (w0 >= 0 && (s < w0 || s > w1)) return;
            cap = (w0 >= 0) ? 400 : 24;
        }
        if (s_r14_str_logged < cap) {
            s_r14_str_logged++;
            TD5_LOG_W(LOG_TAG, "R14COAST straddle: %s span %d lat %+.0f "
                      "(%s) y %.0f..%.0f surf %.0f up %.0f down %.0f",
                      k_guard_kind_name[kind], s, latsum / (double)nin,
                      (latsum >= 0.0) ? "LEFT" : "RIGHT",
                      lo, hi, surf, hi - surf, surf - lo);
        }
        return;                       /* one report per mesh */
    }
}

void tg_r14_coast_report(void)
{
    int k;
    if (!td5_env_flag_off("TD5RE_R14_COAST_REPORT")) return;
    TD5_LOG_I(LOG_TAG, "R14COAST: %ld meshes straddle a water surface; "
              "worst %s at span %d, %.0f above",
              s_r14_str_total,
              (s_r14_str_worst_span >= 0)
                  ? k_guard_kind_name[s_r14_str_worst_kind] : "-",
              s_r14_str_worst_span, s_r14_str_worst);
    for (k = 0; k < TG_GK_COUNT; k++)
        if (s_r14_str_kind[k])
            TD5_LOG_I(LOG_TAG, "R14COAST   %-12s %ld",
                      k_guard_kind_name[k], s_r14_str_kind[k]);
}

/* ================= [R15 PAIR] SCENERY-vs-SCENERY ARBITRATION ================
 * "this guardrail is clipping over the main road as well as this building"
 * (R15 items 10 + 11: a FENCE quad and a WALL_LOW flank in the same entry).
 *
 * WHY THE EXISTING GUARDS CANNOT SEE THIS, measured rather than assumed. The
 * on-road guard DOES test the fence -- TG_GK_BLOCK is TG_GKC_SCENERY -- and
 * does not reject it, so the road half of the report is an intrusion below
 * TD5_TG_GUARD_PEN. The building half is invisible to every guard in this file
 * by construction: both meshes stand OUTSIDE the carriageway and overlap EACH
 * OTHER, and the only pairwise geometry test that existed was
 * tg_validate_geometry_safety, which compares road centrelines. The R15 lateral
 * authority cannot express it either -- both are legally placed at their own
 * (span, side) laterals; they collide in world space, not in the lateral model.
 *
 * So this is the one case in the round that genuinely needs mesh-vs-mesh, and
 * it goes HERE for the reason the R9 water audit states: this is the one place
 * that sees the ASSEMBLED bytes of every mesh in an entry together with the
 * KIND that produced it, so a future emitter is arbitrated for free.
 *
 * FOUR DELIBERATE NARROWINGS, because a pass that DROPS geometry is far more
 * dangerous than one that measures it:
 *   1. ONLY FOUR KINDS participate (building / city / cross / block). Road,
 *      deck, terrain, water and the rest are never tested and never dropped.
 *   2. BILLBOARDS ARE EXCLUDED (tag != 0). A tree or a prop is camera-facing,
 *      so it has no fixed footprint and overlapping a wall is what it is FOR.
 *   3. PEERS ARE NEVER ARBITRATED. Two meshes of equal priority (a sidewalk and
 *      a crossstreet, both TG_GK_CITY) leave each other alone -- with equal
 *      authority there is no principled loser, and picking one by index would
 *      make the output depend on emit order.
 *   4. DEEP OVERLAP ONLY, in ALL THREE axes. Abutting is legal and common: a
 *      pavement arm bounds a flank wall, a facade stands on a kerb. The XZ
 *      threshold is ~40% of a lane; the Y threshold is separate and smaller
 *      because scenery is thin vertically -- at 600 a 520-high fence railing
 *      could never be caught at all, and at 200 a 130-high kerb slab still
 *      cannot be, which is exactly the discrimination wanted.
 * Sweep-and-prune on X, not the naive O(n^2): the per-entry budget is 384
 * meshes, so all-pairs would be ~73M tests across a track against a 0.5 s
 * streamed build.
 * ========================================================================== */
#define TD5_TG_PAIR_PEN_XZ  600.0   /* min horizontal interpenetration to act */
#define TD5_TG_PAIR_PEN_Y   200.0   /* min vertical; scenery is thin          */

/* [R15 PAIR] AN AABB ALONE IS NOT A FOOTPRINT, and measuring proved it before
 * this shipped. A first cut that acted on box-vs-box overlap dropped 299 meshes
 * on the reported seed -- 220 of them `city` -- at consecutive spans 0,1,2,3,4,
 * which is the signature of a systematic false positive rather than occasional
 * clipping. The reason is the geometry this generator makes: a crossstreet quad
 * runs up to 22800 units OUTWARD on a diagonal, so its axis-aligned box sweeps
 * a huge region and "overlaps" facades tens of thousands of units away that it
 * never touches. That is the same lesson the R8 guard already paid for when it
 * replaced vertex sampling with per-quad area coverage.
 *
 * So the box is kept only as a cheap PREFILTER for the sweep, and a pair that
 * survives it is confirmed by a real per-QUAD separating-axis test in XZ, which
 * also yields the penetration depth the threshold is applied to. */
typedef struct {
    double x0, x1, y0, y1, z0, z1;
    size_t off, mlen;
    int    idx, kind, pri, nq;
} TG_PairBox;

/* Penetration depth of two convex XZ quads, 0 when a separating axis exists. */
static double tg_pair_sat_xz(const double *A, const double *B)
{
    double best = 1e300;
    int poly, e, i;

    for (poly = 0; poly < 2; poly++) {
        const double *P = poly ? B : A;
        for (e = 0; e < 4; e++) {
            const double x0 = P[e * 2],           z0 = P[e * 2 + 1];
            const double x1 = P[((e + 1) & 3) * 2], z1 = P[((e + 1) & 3) * 2 + 1];
            double nx = -(z1 - z0), nz = (x1 - x0);
            double len = sqrt(nx * nx + nz * nz);
            double amin = 1e300, amax = -1e300, bmin = 1e300, bmax = -1e300, ov;
            if (len < 1e-9) continue;
            nx /= len; nz /= len;
            for (i = 0; i < 4; i++) {
                const double pa = A[i * 2] * nx + A[i * 2 + 1] * nz;
                const double pb = B[i * 2] * nx + B[i * 2 + 1] * nz;
                if (pa < amin) amin = pa;
                if (pa > amax) amax = pa;
                if (pb < bmin) bmin = pb;
                if (pb > bmax) bmax = pb;
            }
            ov = ((amax < bmax) ? amax : bmax) - ((amin > bmin) ? amin : bmin);
            if (ov <= 0.0) return 0.0;          /* separated: done */
            if (ov < best) best = ov;
        }
    }
    return (best < 1e299) ? best : 0.0;
}

/* [R15 PAIR] AND AN AREA TEST CANNOT SEE A FENCE, which the first SAT cut
 * proved: it dropped 0 meshes over 985 pair tests. A railing panel is a
 * VERTICAL quad -- its four vertices collapse to TWO distinct XZ points -- so
 * its footprint has zero area and tg_pair_sat_xz returns "separated" for it
 * every time, by construction. Exactly the mesh items 10/11 are about.
 *
 * So a quad is classified by its XZ AREA and the right test is chosen:
 *   AREA vs AREA     -> separating axis, penetration depth (a wall in a wall).
 *   SEGMENT vs AREA  -> clip the panel's ground line against the polygon's
 *                       half-planes and measure how much of it lies INSIDE.
 *                       That is the honest reading of "the guardrail is driven
 *                       600 units into this building".
 *   SEGMENT vs SEGMENT -> not judged. Two thin panels crossing is a line-line
 *                       case with no meaningful depth, and guessing one would
 *                       be the false-positive trap all over again. */
#define TD5_TG_PAIR_FLAT  10000.0   /* XZ area below this is a flat panel */

static double tg_pair_xz_area(const double *Q)
{
    double a = 0.0;
    int e;
    for (e = 0; e < 4; e++) {
        const int f = (e + 1) & 3;
        a += Q[e * 2] * Q[f * 2 + 1] - Q[f * 2] * Q[e * 2 + 1];
    }
    return fabs(a) * 0.5;
}

static void tg_pair_chord(const double *Q, double *p0, double *p1)
{
    double best = -1.0;
    int i, j;
    p0[0] = Q[0]; p0[1] = Q[1]; p1[0] = Q[2]; p1[1] = Q[3];
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            const double dx = Q[j * 2] - Q[i * 2];
            const double dz = Q[j * 2 + 1] - Q[i * 2 + 1];
            const double d  = dx * dx + dz * dz;
            if (d > best) {
                best = d;
                p0[0] = Q[i * 2]; p0[1] = Q[i * 2 + 1];
                p1[0] = Q[j * 2]; p1[1] = Q[j * 2 + 1];
            }
        }
    }
}

/* Length of segment p0->p1 lying inside convex quad Q (Liang-Barsky against
 * the quad's half-planes; winding taken from the signed area so the inward
 * normal is right whichever way the emitter wound it). */
static double tg_pair_seg_in_quad(const double *p0, const double *p1,
                                  const double *Q)
{
    const double dx = p1[0] - p0[0], dz = p1[1] - p0[1];
    double t0 = 0.0, t1 = 1.0, sarea = 0.0;
    double sgn;
    int e;

    for (e = 0; e < 4; e++) {
        const int f = (e + 1) & 3;
        sarea += Q[e * 2] * Q[f * 2 + 1] - Q[f * 2] * Q[e * 2 + 1];
    }
    if (fabs(sarea) < 1e-6) return 0.0;
    sgn = (sarea > 0.0) ? 1.0 : -1.0;

    for (e = 0; e < 4; e++) {
        const int f = (e + 1) & 3;
        const double ex = Q[f * 2] - Q[e * 2], ez = Q[f * 2 + 1] - Q[e * 2 + 1];
        double nx = -ez * sgn, nz = ex * sgn;
        double len = sqrt(nx * nx + nz * nz), num, den;
        if (len < 1e-9) continue;
        nx /= len; nz /= len;
        num = (p0[0] - Q[e * 2]) * nx + (p0[1] - Q[e * 2 + 1]) * nz;
        den = dx * nx + dz * nz;
        if (fabs(den) < 1e-12) {
            if (num < 0.0) return 0.0;      /* parallel and outside */
            continue;
        }
        {
            const double t = -num / den;
            if (den > 0.0) { if (t > t0) t0 = t; }
            else           { if (t < t1) t1 = t; }
        }
        if (t0 > t1) return 0.0;
    }
    return (t1 - t0) * sqrt(dx * dx + dz * dz);
}

/* The dispatcher: how deeply do these two quads interpenetrate in XZ? */
static double tg_pair_xz_depth(const double *A, const double *B)
{
    const double aa = tg_pair_xz_area(A), ab = tg_pair_xz_area(B);
    const int fa = (aa < TD5_TG_PAIR_FLAT), fb = (ab < TD5_TG_PAIR_FLAT);
    double s0[2], s1[2];

    if (!fa && !fb) return tg_pair_sat_xz(A, B);
    if (fa && fb)   return 0.0;
    if (fa) { tg_pair_chord(A, s0, s1); return tg_pair_seg_in_quad(s0, s1, B); }
    tg_pair_chord(B, s0, s1);
    return tg_pair_seg_in_quad(s0, s1, A);
}

/* [R15 PAIR] VALIDATE THE DETECTOR BEFORE TRUSTING ITS NULL RESULT.
 * The census reports 0 intersecting quad pairs on the reported seed, and a test
 * that always answers "no" is indistinguishable from one that correctly finds
 * nothing. So three cases with hand-computed answers are run against the very
 * same entry point the guard uses, and the numbers are logged rather than
 * asserted -- if a future change breaks the geometry the log says so on the
 * next build. TD5RE_R15_PAIR_SELFTEST=1 (default OFF, it is a fixed cost with
 * nothing to say on a normal run). */
static void tg_pair_selftest(void)
{
    /* 1. AREA vs AREA. Two 1000x1000 squares offset 400 in X: the minimum
     *    separating-axis penetration is the X overlap, 600. */
    static const double a1[8] = {    0,0, 1000,0, 1000,1000,    0,1000 };
    static const double b1[8] = {  400,0, 1400,0, 1400,1000,  400,1000 };
    /* 2. SEGMENT vs AREA. A vertical panel's footprint is the line x=500 from
     *    z=-500 to z=1500 (verts wound base,base,top,top so XZ repeats).
     *    Clipped to the unit square's z range [0,1000] that is 1000 inside. */
    static const double a2[8] = {  500,-500, 500,1500, 500,1500, 500,-500 };
    /* 3. DISJOINT. Nothing in common, must be exactly 0. */
    static const double b3[8] = { 5000,0, 6000,0, 6000,1000, 5000,1000 };

    if (!td5_env_flag_off("TD5RE_R15_PAIR_SELFTEST")) return;
    TD5_LOG_W(LOG_TAG, "[R15 PAIR SELFTEST] area/area=%.0f (expect 600), "
              "segment/area=%.0f (expect 1000), disjoint=%.0f (expect 0), "
              "flat-area(panel)=%.0f (expect 0), area(square)=%.0f "
              "(expect 1000000)",
              tg_pair_xz_depth(a1, b1),
              tg_pair_xz_depth(a2, b1),
              tg_pair_xz_depth(a1, b3),
              tg_pair_xz_area(a2),
              tg_pair_xz_area(a1));
}

/* Read quad q of a mesh into xz[8] + its y range. 0 when q is out of range. */
static int tg_pair_quad(const unsigned char *b, size_t off, size_t mlen, int q,
                        double *xz, double *y0, double *y1)
{
    const unsigned int vtxoff = tg_rd_u32(b + off + 0x30);
    int i;
    for (i = 0; i < 4; i++) {
        const unsigned char *vp = b + off + vtxoff
                                + (size_t)(q * 4 + i) * TD5_TG_VTX_SIZE;
        double vy;
        if ((size_t)(vp + 12 - b) > off + mlen) return 0;
        xz[i * 2]     = (double)tg_rd_f32(vp);
        xz[i * 2 + 1] = (double)tg_rd_f32(vp + 8);
        vy = (double)tg_rd_f32(vp + 4);
        if (!i) {
            *y0 = *y1 = vy;
        } else {
            if (vy < *y0) *y0 = vy;
            if (vy > *y1) *y1 = vy;
        }
    }
    return 1;
}

static long s_r15_pair_drop;
static long s_r15_pair_tests;
static long s_r15_pair_kind[TG_GK_COUNT];
static long s_r15_pair_isect;      /* quad pairs that intersect AT ALL      */
static long s_r15_pair_h[4];       /* depth >= 100 / 200 / 400 / 600        */
static double s_r15_pair_maxd;     /* deepest interpenetration seen         */

/* Higher wins. 0 = does not participate at all. Ordered by how structural the
 * element is: massing outranks street furniture, so a railing yields to the
 * wall it is driven through rather than the other way about. */
static int tg_pair_pri(int kind)
{
    switch (kind) {
    case TG_GK_BUILDING: return 40;
    case TG_GK_CITY:     return 35;
    case TG_GK_CROSS:    return 30;
    case TG_GK_BLOCK:    return 20;
    default:             return 0;
    }
}

static int tg_pair_box(const unsigned char *b, size_t off, size_t mlen,
                       TG_PairBox *o)
{
    const int tag = (int)tg_rd_u16(b + off + 0x02);
    const unsigned int vtxcnt = tg_rd_u32(b + off + 0x08);
    const unsigned int vtxoff = tg_rd_u32(b + off + 0x30);
    unsigned int vi;
    int n = 0;

    if (tag) return 0;                      /* narrowing 2: no billboards */
    for (vi = 0; vi < vtxcnt; vi++) {
        const unsigned char *vp = b + off + vtxoff + (size_t)vi * TD5_TG_VTX_SIZE;
        double vx, vy, vz;
        if ((size_t)(vp + 12 - b) > off + mlen) break;
        vx = (double)tg_rd_f32(vp);
        vy = (double)tg_rd_f32(vp + 4);
        vz = (double)tg_rd_f32(vp + 8);
        if (!n) {
            o->x0 = o->x1 = vx;
            o->y0 = o->y1 = vy;
            o->z0 = o->z1 = vz;
        } else {
            if (vx < o->x0) o->x0 = vx;
            if (vx > o->x1) o->x1 = vx;
            if (vy < o->y0) o->y0 = vy;
            if (vy > o->y1) o->y1 = vy;
            if (vz < o->z0) o->z0 = vz;
            if (vz > o->z1) o->z1 = vz;
        }
        n++;
    }
    o->off = off; o->mlen = mlen; o->nq = n / 4;
    return n >= 4;
}

static double tg_pair_ov(double a0, double a1, double b0, double b1)
{
    const double lo = (a0 > b0) ? a0 : b0;
    const double hi = (a1 < b1) ? a1 : b1;
    return hi - lo;
}

/* Fills drop[] (indexed by the ORIGINAL mesh index) with the losers. */
static void tg_pair_arbitrate(const unsigned char *b, size_t buflen,
                              const size_t *moff, const int *ord, int nmesh,
                              unsigned char *drop)
{
    static TG_PairBox box[TD5_TG_GUARD_KEPT_MAX];
    static int sx[TD5_TG_GUARD_KEPT_MAX];
    int nb = 0, i, k;

    if (!td5_env_flag_on("TD5RE_R15_PAIR")) return;
    if (nmesh <= 1 || nmesh > TD5_TG_GUARD_KEPT_MAX) return;

    for (i = 0; i < nmesh; i++) {
        const int oi = ord[i];
        const size_t off = moff[oi];
        size_t mlen;
        int kind, mark_si = -1, pri;
        TG_PairBox bx;
        if (off >= buflen) continue;
        mlen = tg_guard_mesh_len(b, off, buflen);
        if (mlen == 0 || off + mlen > buflen) continue;
        kind = tg_guard_kind_of(off, &mark_si);
        pri  = tg_pair_pri(kind);
        if (!pri) continue;                 /* narrowing 1 */
        if (!tg_pair_box(b, off, mlen, &bx)) continue;
        bx.idx = oi; bx.kind = kind; bx.pri = pri;
        box[nb++] = bx;
    }
    if (nb <= 1) return;

    for (i = 0; i < nb; i++) sx[i] = i;
    for (i = 1; i < nb; i++) {              /* insertion sort by x0 */
        const int key = sx[i];
        int m = i - 1;
        while (m >= 0 && box[sx[m]].x0 > box[key].x0) { sx[m + 1] = sx[m]; m--; }
        sx[m + 1] = key;
    }

    for (i = 0; i < nb; i++) {
        const TG_PairBox *A = &box[sx[i]];
        for (k = i + 1; k < nb; k++) {
            const TG_PairBox *B = &box[sx[k]];
            int lose;
            int qa, qb, hit = 0;
            if (B->x0 > A->x1) break;       /* sweep: sorted, so we are done */
            if (A->pri == B->pri) continue; /* narrowing 3 */
            /* PREFILTER only -- see the block comment. A box pair that fails
             * here certainly does not touch; one that passes still has to be
             * confirmed, because a 22800-long diagonal quad's box is mostly
             * empty air. */
            if (tg_pair_ov(A->x0, A->x1, B->x0, B->x1) <= 0.0) continue;
            if (tg_pair_ov(A->z0, A->z1, B->z0, B->z1) <= 0.0) continue;
            if (tg_pair_ov(A->y0, A->y1, B->y0, B->y1) < TD5_TG_PAIR_PEN_Y)
                continue;
            s_r15_pair_tests++;
            /* CONFIRM per quad, in the plane the collision actually happens in.
             * Capped so a pathological mesh cannot make this quadratic. */
            for (qa = 0; qa < A->nq && qa < 16 && !hit; qa++) {
                double az[8], ay0, ay1;
                if (!tg_pair_quad(b, A->off, A->mlen, qa, az, &ay0, &ay1))
                    break;
                for (qb = 0; qb < B->nq && qb < 16; qb++) {
                    double bz[8], by0, by1;
                    if (!tg_pair_quad(b, B->off, B->mlen, qb, bz, &by0, &by1))
                        break;
                    double d;
                    if (tg_pair_ov(ay0, ay1, by0, by1) < TD5_TG_PAIR_PEN_Y)
                        continue;
                    d = tg_pair_xz_depth(az, bz);
                    /* [R15 PAIR] Distribution, not just the verdict: without it
                     * a "0 dropped" result cannot be told apart from "the test
                     * never fires". */
                    if (d > 0.0) {
                        s_r15_pair_isect++;
                        if (d > s_r15_pair_maxd) s_r15_pair_maxd = d;
                        if (d >= 100.0) s_r15_pair_h[0]++;
                        if (d >= 200.0) s_r15_pair_h[1]++;
                        if (d >= 400.0) s_r15_pair_h[2]++;
                        if (d >= 600.0) s_r15_pair_h[3]++;
                    }
                    if (d >= TD5_TG_PAIR_PEN_XZ) {
                        hit = 1;
                        break;
                    }
                }
            }
            if (!hit) continue;             /* narrowing 4 */
            lose = (A->pri < B->pri) ? sx[i] : sx[k];
            if (!drop[box[lose].idx]) {
                drop[box[lose].idx] = 1;
                s_r15_pair_drop++;
                s_r15_pair_kind[box[lose].kind]++;
            }
        }
    }
}

int tg_guard_validate_entry(const TG_NodeList *nl, int ring, int s0,
                                   int ns, TG_Buf *meshes, size_t *moff,
                                   int *pnmesh)
{
    const int nmesh = *pnmesh;
    unsigned char *b = meshes->b;
    /* Per KEPT mesh, was it exempt (authored on/over the road)? Needed for the
     * residual self-check, which must count only NON-exempt on-road meshes --
     * exempt ranges are original byte offsets and stop mapping once compaction
     * moves the bytes. */
    static unsigned char kept_exempt[TD5_TG_GUARD_KEPT_MAX];  /* policy class */
    /* [R10] Compaction order. The write cursor below only stays inside the
     * allocation if the meshes are walked in ASCENDING byte order, so the walk
     * uses a sorted permutation of moff[] rather than moff[]'s own order, and
     * the surviving offsets are written back in the ORIGINAL order (draw order
     * is part of the block's meaning; compaction order is not). When moff[] is
     * already ascending -- which it is once an emitter records its offsets as
     * it appends -- ord[j] == j and this is byte-for-byte the old walk. */
    static int    ord[TD5_TG_GUARD_KEPT_MAX];      /* walk order (by offset)   */
    static size_t newoff[TD5_TG_GUARD_KEPT_MAX];   /* per ORIGINAL index       */
    static unsigned char gone[TD5_TG_GUARD_KEPT_MAX];
    /* [R15 PAIR] Losers of the scenery-vs-scenery pass, decided before the walk
     * below so its keep/compact logic stays a single forward pass. */
    static unsigned char pair_drop[TD5_TG_GUARD_KEPT_MAX];
    static unsigned char cls_of[TD5_TG_GUARD_KEPT_MAX];
    static unsigned char kind_of[TD5_TG_GUARD_KEPT_MAX];  /* [PICK] emitter kind */
    int win_lo, win_hi, i, j, nn = 0, rejected = 0;
    size_t w = 0;

    if (!tg_guard_enabled() || nmesh <= 0 || ring < 3) return 0;
    if (s0 >= ring) return 0;
    /* The per-entry mesh budget is well under this; refusing outright beats
     * compacting a buffer we cannot bookkeep. */
    if (nmesh > TD5_TG_GUARD_KEPT_MAX) return 0;

    win_lo = s0 - TD5_TG_GUARD_WINDOW; if (win_lo < 0) win_lo = 0;
    win_hi = s0 + ns - 1 + TD5_TG_GUARD_WINDOW;
    if (win_hi > ring - 1) win_hi = ring - 1;
    if (win_hi < win_lo) return 0;

    for (i = 0; i < nmesh; i++) { ord[i] = i; gone[i] = 0; newoff[i] = moff[i]; }
    for (i = 1; i < nmesh; i++) {          /* insertion sort, near-sorted input */
        const int key = ord[i];
        int k = i - 1;
        while (k >= 0 && moff[ord[k]] > moff[key]) { ord[k + 1] = ord[k]; k--; }
        ord[k + 1] = key;
        if (k + 1 != i) s_guard_unsorted++;
    }

    /* [R15 PAIR] Decide the scenery-vs-scenery losers up front. It needs the
     * sorted order (its sweep depends on it) and it must run BEFORE the walk,
     * because that walk compacts bytes as it goes and cannot revisit a mesh it
     * has already written. */
    memset(pair_drop, 0, (size_t)nmesh);
    tg_pair_arbitrate(b, meshes->len, moff, ord, nmesh, pair_drop);

    for (j = 0; j < nmesh; j++) {
        const int oi = ord[j];
        const size_t off = moff[oi];
        /* The next mesh IN BYTE ORDER, which is what bounds this one. */
        const size_t nxt = (j + 1 < nmesh) ? moff[ord[j + 1]] : meshes->len;
        size_t mlen = (off <= meshes->len) ? tg_guard_mesh_len(b, off, meshes->len) : 0;
        int keep = 1, exempt, kind = TG_GK_OTHER, cls = TG_GKC_SCENERY;
        int onroad = 0;                        /* [R14 GENPERF] timed separately */
        int mark_si = -1;
        double depth = 0.0;
        int si = -1;

        if (mlen == 0 || off + mlen > meshes->len) {
            /* Unparseable / spans to end: keep verbatim, do not risk corruption.
             * [R10] `nxt` is the next offset IN BYTE ORDER, so this subtraction
             * can no longer underflow into a ~2^64 length the way it did when
             * moff[] was walked in its recorded (possibly descending) order. */
            mlen = (nxt > off) ? nxt - off : 0;
            exempt = 1;
        } else {
            kind = tg_guard_kind_of(off, &mark_si);
            cls  = (int)k_guard_kind_class[kind];
            /* R7 fallback (TD5RE_R8_GUARD=0): the two classes R8 introduced
             * collapse back to what R7 did -- a decal was ordinary scenery that
             * simply never tripped the vertex test, and the skirt/water/coast
             * were unconditionally exempt. */
            if (!tg_guard_r8() && cls == TG_GKC_DECAL) cls = TG_GKC_SCENERY;
            if (!tg_guard_r8() && cls == TG_GKC_UNDER) cls = TG_GKC_EXEMPT;
            /* [R10 SPAN66] A/B pin: with the knob off, furniture is judged by
             * exactly the rule it was judged by before this round. */
            if (!tg_r10_xstreet_guard() && cls == TG_GKC_FURNITURE)
                cls = TG_GKC_SCENERY;
            exempt = (cls == TG_GKC_EXEMPT);
            /* [R8] SPAN-SCOPED EXEMPTION. An exempt mark licenses geometry that
             * crosses the carriageway AT THE SPAN IT WAS EMITTED FOR. Test the
             * mesh as plain SCENERY first: if it does intrude, the licence only
             * stands when the intrusion is near the marking span. Anything
             * further is an unrelated mesh that happened to land inside an
             * exempt byte range, and it is validated normally. */
            if (exempt && tg_guard_r8() && mark_si >= 0 &&
                k_guard_kind_local[kind] &&
                tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen, win_lo, win_hi,
                                          TG_GKC_SCENERY, &depth, &si)) {
                int d = si - mark_si; if (d < 0) d = -d;
                if (d > TD5_TG_GUARD_EX_SPANS) {
                    cls = TG_GKC_SCENERY;
                    exempt = 0;
                    s_guard_ex_scope_hits++;
                }
            }
            depth = 0.0; si = -1;
            /* [R14 GENPERF] The on-road guard's own cost, separated from the
             * over-water audit below: they are the two per-mesh passes and only
             * a measurement says which one the round paid for. */
            s_tg_guard_t0 = td5_plat_time_us();
            onroad = (!exempt &&
                      tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen,
                                                win_lo, win_hi, cls,
                                                &depth, &si));
            s_tg_zone_us[TG_ZONE_GUARD_ROAD] += td5_plat_time_us() - s_tg_guard_t0;
            s_tg_zone_n[TG_ZONE_GUARD_ROAD]++;
            if (onroad) {
                keep = 0;
                rejected++;
                s_guard_rejects++;
                s_guard_rej_kind[kind]++;
                tg_acct(TG_ACCT_GUARD_REJECT, si);
                tg_acct(TG_ACCT_R8_GUARD, si);
                if (rejected <= 8)
                    TD5_LOG_W(LOG_TAG, "on-road guard: rejected %s mesh at span "
                              "%d (coverage %.0f, %u verts, marked @%d)",
                              k_guard_kind_name[kind], si, depth,
                              tg_rd_u32(b + off + 0x08), mark_si);
            }
        }

        /* [R9 BRIDGE item 10] OVER-WATER AUDIT. "On span 1039 there's buildings
         * in the background that are over the water."
         *
         * A frame proves one instance; this is the CLASS test, and it runs here
         * because this is the one place in the generator that sees the ASSEMBLED
         * bytes of every mesh together with the KIND that produced it. Any kept
         * mesh with a vertex inside a bridge run's river rectangle and above its
         * surface is massing standing on water, whatever emitter made it -- so a
         * future emitter is audited for free, exactly like the on-road guard.
         * Counted per kind and per span; reported by tg_r9_bridge_report. */
        if (keep && mlen > 0 && off + mlen <= meshes->len) {
            int wet;
            s_tg_wet_t0 = td5_plat_time_us();      /* [R14 GENPERF] */
            wet = tg_r9_water_audit_mesh(nl, b, off, mlen, kind);
            s_tg_zone_us[TG_ZONE_GUARD_WATER] += td5_plat_time_us() - s_tg_wet_t0;
            s_tg_zone_n[TG_ZONE_GUARD_WATER]++;
            if (wet) {
                keep = 0;
                rejected++;
            }
        }
        /* [R15 PAIR items 10 + 11] The scenery-vs-scenery verdict, decided
         * before this walk started. Applied here, alongside the water audit,
         * so a mesh condemned by it goes through the SAME drop bookkeeping --
         * including tg_r13_faces_dropped below, which the frontage census
         * depends on being told about every building that does not ship. */
        if (keep && pair_drop[oi]) {
            keep = 0;
            rejected++;
            if (s_r15_pair_drop <= 8)
                TD5_LOG_W(LOG_TAG, "pair guard: dropped %s mesh at span %d "
                          "(overlapped a higher-priority neighbour)",
                          k_guard_kind_name[kind], mark_si);
        }
        /* [R14 COAST item 5a] The straddle census -- meshes that CROSS the
         * water surface rather than stand over it. Report-only; see the
         * function for why the R9 audit above cannot see these. */
        if (keep && mlen > 0 && off + mlen <= meshes->len)
            tg_r14_coast_audit_mesh(nl, b, off, mlen, kind);
        /* [R13 FACES item 6] Whichever of the two passes above condemned it, a
         * dropped BUILDING has to leave the frontage census, or the census keeps
         * reporting the run end the emitter meant to build rather than the one
         * the strip carries. */
        if (!keep && kind == TG_GK_BUILDING) tg_r13_faces_dropped(mark_si);

        /* [R8] DIAG: dump every mesh of one entry with its measured footprint.
         * This is the instrument the R8 diagnosis was made with -- it is what
         * showed that the surviving objects at spans 187/627/686 were NOT
         * height-gated and NOT exempt, but wide quads whose corners all sat
         * outside the road. Kept so the next round re-measures instead of
         * inheriting this conclusion in turn. */
        {
            const int dsp = tg_guard_diag_span();
            /* Report by WHERE THE MESH LANDS, not by which entry emitted it.
             * Scenery for span 184 routinely reaches span 187, so dumping only
             * the entry that contains the complaint span would miss the very
             * mesh the user is looking at. TD5RE_R8_GUARD_AUDIT=1 instead dumps
             * EVERY mesh on the whole strip that covers the carriageway at all,
             * whatever its class -- the sweep the acceptance bar asks for, and
             * the only way to see what the exemptions are currently licensing. */
            const int audit = td5_env_flag_off("TD5RE_R8_GUARD_AUDIT");
            if ((audit || (dsp > 0 && dsp >= win_lo && dsp <= win_hi))
                && mlen > 0 && off + mlen <= meshes->len) {
                TG_GuardHit h;
                int badv = tg_guard_mesh_scan(nl, ring, b, off, mlen, win_lo,
                                              win_hi, cls, &h);
                int d = (dsp > 0) ? h.si - dsp : 0;
                if (d < 0) d = -d;
                if (audit ? (h.intr > TD5_TG_GUARD_PEN
                             && tg_guard_quad_bad(TG_GKC_SCENERY, h.intr,
                                                  h.dy_lo, h.dy_hi))
                          : (d <= 2 && h.intr > -1e299))
                    TD5_LOG_I(LOG_TAG, "guard-diag: entry@%d mesh %d kind=%s "
                              "cls=%d mark_si=%d verts=%u intr=%.0f si=%d "
                              "dy=[%.0f,%.0f] %s", s0, oi,
                              k_guard_kind_name[kind], cls, mark_si,
                              tg_rd_u32(b + off + 0x08), h.intr, h.si, h.dy_lo,
                              h.dy_hi, badv ? "REJECT" : "keep");
            }
        }

        /* [PICK] Remember this mesh's kind by ORIGINAL index; the survivor
         * write-back below records it against the FINAL slot. */
        kind_of[oi] = (unsigned char)kind;

        /* Drop the bytes only on a real rejection AND when NOT in report-only
         * mode; otherwise (accepted, exempt, unparseable, or report-only) keep
         * the mesh, compacting it left over any gap left by earlier drops. */
        if (keep || tg_guard_report_only()) {
            /* [R10] LAST LINE OF DEFENCE. Ascending order already guarantees
             * w <= off, so this can only fire if moff[] carried something the
             * sort could not repair (overlapping or duplicate ranges). Skipping
             * the mesh costs one piece of scenery; letting the cursor past the
             * allocation smashes the next heap block's header and kills the
             * process inside free() with no diagnostic at all. */
            if (w + mlen > meshes->len) {
                s_guard_unsafe++;
                gone[oi] = 1;
            } else {
                if (w != off) memmove(b + w, b + off, mlen);
                /* Record the POLICY CLASS, not a bare exempt bit: the residual
                 * self-check has to judge each kept mesh by the same rule the
                 * drop pass did, and a decal legally covers the road. */
                cls_of[oi] = (unsigned char)(exempt ? TG_GKC_EXEMPT : cls);
                newoff[oi] = w;
                w += mlen;
            }
        } else {
            gone[oi] = 1;
        }
    }

    /* Write the survivors back in the ORIGINAL recorded order (see the note on
     * `ord` above): compaction order is a byte-layout detail, draw order is not. */
    for (i = 0; i < nmesh; i++) {
        if (gone[i]) continue;
        kept_exempt[nn] = cls_of[i];
        /* [PICK] tag the FINAL slot nn with this survivor's emitter kind. */
        tg_meshtag_set(s0 / TD5_TG_SPANS_PER_ENTRY, nn, (int)kind_of[i]);
        moff[nn++] = newoff[i];
    }

    meshes->len = w;
    *pnmesh = nn;

    /* Residual self-check: re-validate the kept NON-exempt meshes. This is the
     * class-level cleanliness proof -- after the drop, zero non-exempt geometry
     * may stand on the road. A non-zero residual is a parse/compaction bug (not
     * a policy call) and is surfaced in the build summary. In report-only mode
     * the rejected meshes were kept, so a residual there equals the reject count
     * and is expected. */
    if (!tg_guard_report_only()) {
        for (i = 0; i < nn; i++) {
            const size_t off = moff[i];
            size_t mlen = tg_guard_mesh_len(b, off, meshes->len);
            double depth = 0.0;
            int si = -1;
            int cls = (i < TD5_TG_GUARD_KEPT_MAX) ? (int)kept_exempt[i]
                                                  : TG_GKC_SCENERY;
            if (cls == TG_GKC_EXEMPT) continue;
            if (mlen == 0 || off + mlen > meshes->len) continue;
            if (tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen, win_lo, win_hi,
                                          cls, &depth, &si))
                s_guard_residual++;
        }
    }

    return rejected;
}

/* [R15] Per-module half of the round-15 report. Split out of the single
 * tg_r15_sky_report the work was first written against: after the trackgen
 * split its counters live in four different modules, and a file-static
 * cannot be read from another translation unit. One report per owning
 * module keeps the counters static where they belong.  */
void tg_r15_pair_report(void)
{
    {   /* [R15 PAIR items 10+11] per-kind, so "did this start eating city or
         * buildings" is a number rather than a screenshot. */
        char kb[256];
        int ki, kn = 0;
        kb[0] = '\0';
        for (ki = 0; ki < TG_GK_COUNT; ki++) {
            if (!s_r15_pair_kind[ki]) continue;
            kn += snprintf(kb + kn, sizeof(kb) - (size_t)kn, "%s%s:%ld",
                           kn ? " " : "", k_guard_kind_name[ki],
                           s_r15_pair_kind[ki]);
            if (kn >= (int)sizeof(kb) - 1) break;
        }
        TD5_LOG_I(LOG_TAG, "[R15 PAIR items 10+11] scenery-vs-scenery: %ld "
                  "mesh(es) dropped over %ld pair test(s) [%s] (knob "
                  "TD5RE_R15_PAIR=%s, pen xz=%.0f y=%.0f)", s_r15_pair_drop,
                  s_r15_pair_tests, kn ? kb : "-",
                  td5_env_flag_on("TD5RE_R15_PAIR") ? "on" : "off",
                  TD5_TG_PAIR_PEN_XZ, TD5_TG_PAIR_PEN_Y);
        tg_pair_selftest();
        TD5_LOG_I(LOG_TAG, "[R15 PAIR] interpenetration census: %ld quad pair(s) "
                  "intersect, deepest %.0f; depth >=100:%ld >=200:%ld >=400:%ld "
                  ">=600:%ld", s_r15_pair_isect, s_r15_pair_maxd,
                  s_r15_pair_h[0], s_r15_pair_h[1], s_r15_pair_h[2],
                  s_r15_pair_h[3]);
    }
}
