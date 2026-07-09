/* RaceParticleSpawnState — recovered from:
 *   SpawnVehicleSmokeSprite               @ 0x00429cf0
 *   SpawnVehicleSmokeVariant              @ 0x00429a30
 *   SpawnVehicleSmokePuffFromHardpoint    @ 0x0042a290
 *   ProjectRaceParticlesToView            @ 0x00429690
 *   DrawRaceParticleEffects               @ 0x00429720
 *   UpdateRaceParticleEffects             @ 0x00429790
 *
 * Pool base: g_raceParticlePoolBase @ 0x004a3170
 * Per-particle stride: 0x40 bytes (64) — confirmed by `pcVar4 += 0x40` walk in spawn
 * Pool layout: 6 view banks × 100 particles × 0x40 B = 0x12C00 (0x1900 per bank)
 *              [pool_base + view_bank*0x1900 + particle_idx*0x40]
 * Size: 0x40 bytes per entry
 * Source: Tier1-B 2026-05-22
 *
 * Cross-bank stride 0x1900 confirmed by `&DAT_004a318f + param_4 * 0x1900` in all 4 spawn fns.
 * Per-particle stride 0x40 confirmed by `puVar2 += 0x10` (in undefined4 units) = 0x40 bytes
 * in DrawRaceParticleEffects/UpdateRaceParticleEffects.
 *
 * Per-spawn defaults (cross-validated 3 spawn fns):
 *   SmokeSprite: lifetime=0x2000, initial_size=0x9000, initial_alpha=0x2080, life_mult=15
 *   SmokeVariant: lifetime=0x1800, initial_size=0x7000, initial_alpha=0x2080, life_mult=10
 *   SmokePuff: lifetime=0x600,  initial_size=0x4000, initial_alpha=0x26c0, life_mult=10
 *
 * Flag byte at +0x1f:
 *   bit 7 (0x80): active     (set on spawn; tested by Update before invoking update_fn)
 *   bit 6 (0x40): projected  (set by ProjectRaceParticlesToView after view-space xform)
 *   bit 5 (0x20): hidden     (skip-project gate at ProjectRaceParticlesToView)
 *   Render-gate: (flags & 0xc0) == 0xc0 — both active AND projected.
 */
typedef struct RaceParticleSpawnState {
    /* +0x00 */ unsigned char  phase;            /* smoke animation phase, rand() % 0x1f */
    /* +0x01 */ unsigned char  scratch_index;    /* index into g_raceParticleScratchTable (per-bank, 0..0x31) */
    /* +0x02 */ unsigned char  life_max;         /* (rand()&3+1) * (10 or 15) — used as ramp divisor */
    /* +0x03 */ unsigned char  hardpoint_id;     /* caller-supplied hardpoint/decal slot id */
    /* +0x04 */ unsigned short initial_size;     /* sprite half-extent fp8 (0x9000/0x7000/0x4000) */
    /* +0x06 */ short          size_rate;        /* per-tick size delta: -0x3000 / life_max */
    /* +0x08 */ unsigned short initial_alpha;    /* sprite alpha fp8 (0x2080/0x26c0) */
    /* +0x0a */ short          alpha_rate;       /* per-tick alpha delta: 0x1900 / life_max */
    /* +0x0c */ int            velocity_x;       /* fp8 world-space velocity X */
    /* +0x10 */ unsigned int   lifetime;         /* fp8 lifetime countdown (0x600/0x1800/0x2000) */
    /* +0x14 */ int            velocity_z;       /* fp8 world-space velocity Z */
    /* +0x18 */ unsigned int   _pad_18;          /* unused — no refs */
    /* +0x1c */ unsigned char  _pad_1c[3];       /* unused — no refs */
    /* +0x1f */ unsigned char  flags;            /* bit7=active, bit6=projected, bit5=hidden */
    /* +0x20 */ int            world_x;          /* fp8 world position X */
    /* +0x24 */ int            world_y;          /* fp8 world position Y */
    /* +0x28 */ int            world_z;          /* fp8 world position Z */
    /* +0x2c */ float          view_x;           /* post-xform X (written by TransformVec3ByRenderMatrixFull) */
    /* +0x30 */ float          view_y;
    /* +0x34 */ float          view_z;
    /* +0x38 */ void *         render_fn;        /* per-particle render callback (e.g. LAB_00429950) */
    /* +0x3c */ void *         update_fn;        /* per-particle update callback (e.g. LAB_004297d0) */
} RaceParticleSpawnState; /* size 0x40 */

/* type_define_c-ready definition above. Apply via ghidra-apply Wave 2E phase. */
