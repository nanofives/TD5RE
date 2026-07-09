# DA-T5: RenderHudRadialPulseOverlay deep audit (0x00439E60)

Target: `TD5_d3d.exe!RenderHudRadialPulseOverlay` (928B, 0x00439E60..0x0043A200).
Port stub: `td5mod/src/td5re/td5_render.c:5818  td5_render_radial_pulse(float dt)`.

Ghidra session: TD5_pool0 read-only (slot 3 acquire returned stale; existing
read-only session reused).

## Section A — Orig algorithm

Per-frame: while `s_radial_pulse_progress >= 0.0f` (i.e. armed), the overlay
renders a 5-petal "flower" centered at screen origin and advances both the
HUD-side phase and the renderer-side anim-state accumulator. Each call:

1. **Gate** (0x439e60..0x439e7d): if `phase < _DAT_0045d624` (0.0f), return.
   In port terms: only run when `s_radial_pulse_progress >= 0.0f`.
2. **Capture base angle** (0x439e87): `iVar2 = (int)_g_hudRadialPulseAnimState`
   via `__ftol` (truncate-toward-zero on the 32-bit float at [0x004B08C0]).
   This is the rotating base 12-bit-angle integer (top byte ignored).
3. **Phase advance** (0x439e9a..0x439eb4): if `phase < _DAT_0045d714` (3000.0f),
   `phase += dt * _DAT_0045d710` (dt * 4.2f). Capped, never wraps.
4. **Alpha** (0x439eba..0x439ee0): `iVar3 = (int)(phase * _DAT_0045d70c)` =
   `(int)(phase * 0.31875f)`; clamp to [0, 255]. So alpha saturates once
   `phase >= ~800`.
5. **Per-frame radius** (0x439ee2..0x439ef6): `fVar1 = g_renderWidthF * phase *
   _DAT_0045d64c` (= width * phase * 0.00625f).
6. **Loop 5 iterations** (0x439efa..0x439f69), each writes 4 floats into
   `local_50[20]` and decrements `iVar2 -= 0x33332`:
   - Sample 1 ("inner ring", angle a = iVar2 >> 8):
     `local_50[i+0]  = cos(a) * fVar1 * 0.5f`  (X inner)
     `local_50[i+0x28] = sin(a) * fVar1 * 0.5f` (Y inner)
   - Sample 2 ("outer ring", angle b = (iVar2 - 0x19999) >> 8):
     `local_50[i+0x04] = cos(b) * fVar1`       (X outer)
     `local_50[i+0x2c] = sin(b) * fVar1`       (Y outer)
   - `iVar2 -= 0x33332` (full 1/5 turn step), `i += 8` (advance 2 floats).
   Step 0x33332 ≈ 1/5 of 0x100000 (12-bit angle * 256). Sub-step 0x19999 =
   half of full step → outer-ring vertex sits at angle bisector.
7. **5 × BuildSpriteQuadTemplate(mode=5 = GEOMETRY|COLOR)** writing into
   four scratch quad records gHudFadeQuadTemplateArray (0x004B0C08),
   DAT_004B0CC0, DAT_004B0D78, DAT_004B0E30, DAT_004B0EE8. Each quad has
   4 vertices (V0/V1/V2/V3); the BSQT slot mapping pushes them as:

     V0 = (X[k+0], Y[k+0], Z=153.6)   inner @ angle a_k
     V1 = (X[k+1], Y[k+1], Z=153.6)   outer @ bisector
     V2 = (X[k+2], Y[k+2], Z=153.6)   inner @ angle a_{k+1} (next petal start)
     V3 = (0,      0,      Z=153.6)   center origin

   ARGB color = `0xFF000000 | iVar3 * 0x10101` (grayscale, alpha=0xFF).
   The C0 sentinel at `local_bc = 5` is the `mode_flags` field (BSQT_GEOMETRY
   | BSQT_COLOR; no UV → diffuse-only flat-shaded quad).
8. **5 × SubmitImmediateTranslucentPrimitive** flushing the four scratch quads
   (one is the first quad: `gHudFadeQuadTemplateArray @ 0x004B0C08`, then the
   four DAT_004B0CCC0/D78/E30/EE8 slots — 5 quads total).
9. **Anim-state accumulator** (0x43a1da..0x43a1fa): `_g_hudRadialPulseAnimState
   += dt * _DAT_0045d708` (= dt * 3328.0f). Stored back to [0x004B08C0].
   The bottom 12 bits of the integer cast of this state become the next
   frame's base angle, so the flower rotates at ~3328 angle-units/sec.

## Section B — Vertex layout

10 vertex positions per call, stored as `(X[10], Y[10])` in `local_50[20]`:

    Index k      Angle (12-bit units)  Radius     Role
    0 (inner)    iVar2_initial         fVar1*0.5  petal_0 start
    1 (outer)    iVar2 - 0x19999       fVar1      petal_0 outer
    2 (inner)    iVar2 - 0x33332       fVar1*0.5  petal_0 end / petal_1 start
    3 (outer)    iVar2 - 0x4CCCB       fVar1      petal_1 outer
    4 (inner)    iVar2 - 0x66664       fVar1*0.5  petal_1 end / petal_2 start
    5 (outer)    iVar2 - 0x7FFFD       fVar1      petal_2 outer
    6 (inner)    iVar2 - 0x99996       fVar1*0.5  petal_2 end / petal_3 start
    7 (outer)    iVar2 - 0xB332F       fVar1      petal_3 outer
    8 (inner)    iVar2 - 0xCCCC8       fVar1*0.5  petal_3 end / petal_4 start
    9 (outer)    iVar2 - 0xE6661       fVar1      petal_4 outer

Total swept angle = 5 × 0x33332 = 0x0FFFBA ≈ 0x100000 (full 12-bit circle).
Outer-ring radius is **2×** the inner-ring radius (the comment was correct;
the "_g_halfFloatConstant" name confirms it's 0.5f applied to the inner).

5 quads with shared inner-ring vertices and center origin:

    Quad  V0          V1          V2          V3
    0     (X0,Y0) in  (X1,Y1) out (X2,Y2) in  (0,0) center
    1     (X2,Y2) in  (X3,Y3) out (X4,Y4) in  (0,0) center
    2     (X4,Y4) in  (X5,Y5) out (X6,Y6) in  (0,0) center
    3     (X6,Y6) in  (X7,Y7) out (X8,Y8) in  (0,0) center
    4     (X8,Y8) in  (X9,Y9) out (X0,Y0) in  (0,0) center   ← wraps

Each quad draws as two triangles (V0,V1,V2)+(V0,V2,V3) — a petal wedge with
center pinch and an outward-pointing bisector vertex.

## Section C — Phase / anim-state lifecycle

- `s_radial_pulse_progress` (port td5_hud.c:151, orig [0x004B0FA0]):
  initialised to -1.0f at HUD init (line 3438) and on first race-start (line
  1255). When `td5_hud_render_overlays()` calls `td5_render_radial_pulse(dt)`
  inside the per-view loop (td5_hud.c:2504), the renderer:
    - skips when `progress < 0.0f` (orig gate)
    - increments `progress += dt * 4.2f` until `progress >= 3000.0f`
- `_g_hudRadialPulseAnimState` (orig [0x004B08C0]): independent rotating
  base-angle accumulator. Not present in port (no static today). Must be
  added; survives across re-arms (orig never resets it, just keeps integrating
  `dt * 3328.0f`).
- Re-arm: external code writes `progress = 0.0f` at the cue (race-start /
  lap / finish); the gate flips on, alpha ramps up from 0, radius grows, then
  alpha saturates and radius continues to grow until cap.

## Section D — Constants table

Values read directly from TD5_d3d.exe with `memory_read`:

    Symbol                       Address       Hex bytes (LE)  IEEE-754
    _DAT_0045d624 (gate)         0x0045d624    00 00 00 00      0.0f
    _DAT_0045d708 (anim incr)    0x0045d708    00 00 50 45      3328.0f
    _DAT_0045d70c (phase→alpha)  0x0045d70c    33 33 a3 3e      0.31875f
    _DAT_0045d710 (phase incr)   0x0045d710    66 66 86 40      4.2f
    _DAT_0045d714 (phase cap)    0x0045d714    00 80 3b 45      3000.0f
    _DAT_0045d64c (radius)       0x0045d64c    cd cc cc 3a      0.00625f (1/160)
    _DAT_0045d5d0 (0.5f mult)    0x0045d5d0    00 00 00 3f      0.5f
    Z constant (literal)         imm           9a 19 00 43      128.1f (0x4300199a)
    g_renderWidthF               0x004aaf08    00 00 00 00      (init by viewport)

Constant `0x4300199a` shows up as 4× immediate writes into the per-vertex Z
slot. `128.1f` (≈128) is the standard HUD-overlay Z depth used throughout the
HUD pipeline.

Inner-loop step magic: outer 12-bit-angle step = 0x33332 / 256 ≈ 819.2 (≈ 4096/5
= 819.2 — exactly 1/5 of a full 12-bit circle). Half-step 0x19999 / 256 ≈
409.6 (bisector). The `>> 8` (`SAR EDI, 0x8`) recovers the actual 12-bit
angle used to index the cos/sin LUTs.

## Section E — Port-ready C implementation

This is a drop-in replacement for the stub at td5_render.c:5818. It depends
on a small accessor pair in td5_hud.c to read/advance `s_radial_pulse_progress`
(currently static). Both are listed.

### E.1 — Add to td5_hud.c (after the existing radial_pulse static at line 151):

```c
/* Accessors so td5_render_radial_pulse can read/update the HUD's pulse phase
 * without exposing the static. Matches orig [0x004B0FA0] semantics. */
float td5_hud_radial_pulse_get(void)        { return s_radial_pulse_progress; }
void  td5_hud_radial_pulse_set(float value) { s_radial_pulse_progress = value; }
```

And in the corresponding header (td5_hud.h or td5_render.h prototype block):

```c
float td5_hud_radial_pulse_get(void);
void  td5_hud_radial_pulse_set(float value);
```

### E.2 — Replace td5_render.c:5818 stub with:

```c
/* RenderHudRadialPulseOverlay @ 0x00439E60 — 5-petal translucent pulse ring.
 * Drawn at center origin (V3=0,0); 10 ring vertices alternating inner/outer
 * (outer = 2× inner radius); rotating base angle accumulated in s_radial_pulse_anim.
 * Constants taken from TD5_d3d.exe data segment (DA-T5 deep audit 2026-05-22).
 * [CONFIRMED @ 0x00439E60..0x0043A200] */
static float s_radial_pulse_anim;  /* orig [0x004B08C0] _g_hudRadialPulseAnimState */

void td5_render_radial_pulse(float dt)
{
    float phase = td5_hud_radial_pulse_get();

    /* Gate: orig FCOMP [0x0045d624] (= 0.0f). Skip when phase < 0. */
    if (phase < 0.0f) return;

    /* Snapshot base angle from anim-state accumulator (truncate-toward-zero,
     * matches orig __ftol). */
    int base_angle = (int)s_radial_pulse_anim;

    /* Phase advance (capped at 3000.0f). */
    if (phase < 3000.0f) {
        phase += dt * 4.2f;
        td5_hud_radial_pulse_set(phase);
    }

    /* Alpha = clamp(phase * 0.31875f, 0, 255). */
    int alpha = (int)(phase * 0.31875f);
    if (alpha < 0)        alpha = 0;
    else if (alpha > 255) alpha = 255;

    /* Per-frame radius (orig fVar1). Note g_renderWidthF is the viewport width
     * in pixels; constant 0.00625f = 1/160. At width=640 px, phase=800 (alpha
     * saturated), fVar1 = 640*800*0.00625 ≈ 3200; this is a screen-space
     * radius in HUD units before the * 0.5 inner / * 1.0 outer multiply. */
    float fVar1 = g_renderWidthF * phase * 0.00625f;

    /* 10 ring vertices: X[0..9] then Y[0..9]. Even k = inner (radius*0.5),
     * odd k = outer (radius*1.0). Angle a_inner = (base_angle - k*0x33332) >> 8,
     * angle a_outer = a_inner - (0x19999 >> 8). */
    float vx[10], vy[10];
    int a = base_angle;
    for (int k = 0; k < 10; k += 2) {
        unsigned int a_inner = ((unsigned int)a) >> 8;
        unsigned int a_outer = ((unsigned int)(a - 0x19999)) >> 8;
        vx[k]     = CosFloat12bit(a_inner) * fVar1 * 0.5f;
        vy[k]     = SinFloat12bit((int)a_inner) * fVar1 * 0.5f;
        vx[k + 1] = CosFloat12bit(a_outer) * fVar1;
        vy[k + 1] = SinFloat12bit((int)a_outer) * fVar1;
        a -= 0x33332;
    }

    /* Grayscale ARGB: alpha=0xFF, RGB = alpha*0x10101. */
    uint32_t color = 0xFF000000u | (uint32_t)(alpha * 0x10101);

    /* Scratch quad buffers (orig DAT_004B0C08 / 0CC0 / 0D78 / 0E30 / 0EE8). */
    static uint8_t s_pulse_quads[5][0xB8];  /* same stride as TD5_RenderSpriteQuad */
    static const int idx_table[5][3] = {
        {0, 1, 2}, {2, 3, 4}, {4, 5, 6}, {6, 7, 8}, {8, 9, 0},
    };

    /* Build & submit one petal at a time. mode_flags=5 (GEOMETRY|COLOR) maps to
     * orig flag mask via TD5_BSQT_RAW_FLAGS so the legacy "do everything"
     * fallback doesn't kick in. */
    for (int q = 0; q < 5; q++) {
        int i0 = idx_table[q][0];
        int i1 = idx_table[q][1];
        int i2 = idx_table[q][2];

        struct {
            void     *dest;
            int       mode;
            float     scr_x[4];
            float     scr_y[4];
            float     depth_z[4];
            float     tex_u[4];
            float     tex_v[4];
            uint32_t  diffuse[4];
            int       texture_page;
            int       pad;
        } p;

        p.dest = &s_pulse_quads[q];
        p.mode = TD5_BSQT_RAW_FLAGS | TD5_BSQT_GEOMETRY | TD5_BSQT_COLOR;

        /* Slot mapping mirrors orig: V0=scr[0], V3=scr[1], V2=scr[2], V1=scr[3]
         * (per td5_render_build_sprite_quad). We feed:
         *   slot 0 (→V0) = inner_start
         *   slot 1 (→V3) = center
         *   slot 2 (→V2) = inner_end
         *   slot 3 (→V1) = outer_bisector
         * to recreate orig's V0=in_start, V1=out, V2=in_end, V3=center. */
        p.scr_x[0] = vx[i0];     p.scr_y[0] = vy[i0];      /* inner start */
        p.scr_x[1] = 0.0f;        p.scr_y[1] = 0.0f;        /* center */
        p.scr_x[2] = vx[i2];     p.scr_y[2] = vy[i2];      /* inner end */
        p.scr_x[3] = vx[i1];     p.scr_y[3] = vy[i1];      /* outer bisector */

        for (int v = 0; v < 4; v++) {
            p.depth_z[v] = 128.1f;   /* orig immediate 0x4300199a */
            p.diffuse[v] = color;
            p.tex_u[v] = 0.0f;
            p.tex_v[v] = 0.0f;
        }
        p.texture_page = 0;
        p.pad = 0;

        td5_render_build_sprite_quad((int *)&p);
        td5_render_submit_translucent_hud((uint16_t *)&s_pulse_quads[q]);
    }

    /* Anim-state accumulator: dt * 3328.0f added to rotating base angle. */
    s_radial_pulse_anim += dt * 3328.0f;
}
```

### E.3 — Behavior verification checklist

- Gate matches orig (no draw while progress < 0).
- Phase reaches 0.31875 * 800 ≈ 255 cap and stays.
- Radius grows linearly to width * 3000 * 0.00625 = width * 18.75 at cap
  (very large; visible-only because progress is reset by external trigger
  before reaching the cap in normal play).
- 5-fold rotational symmetry with one outer-ring vertex per petal bisector.
- Anim state rotates the entire flower at 3328 angle-units / sec.
- Alpha is in the ARGB high byte = 0xFF (constant 0xFF000000). Note the
  "alpha" in the original byte-0 slot is actually B in D3DCOLOR ARGB, and the
  grayscale tile `(alpha<<16)|(alpha<<8)|alpha` makes the petal a uniform
  gray fading from black to white as phase grows.

### E.4 — Outstanding risks / non-1:1 items

1. The orig writes the same color into the BSQT param `diffuse[4]` array;
   port's BSQT mapping (v0=src[0], v1=src[3], v2=src[2], v3=src[1]) reorders
   slots — since all four colors are identical, the reorder is a no-op.
2. `g_renderWidthF` value at frame time may differ between port and orig
   (port initialises in render_init; orig sets in viewport setup); if so the
   pulse radius will scale differently. Verify by reading `g_renderWidthF` at
   call time matches `_g_renderWidthF` in orig (Frida probe).
3. The 5th quad's V3 (last petal) reuses V0 of quad 0 — confirmed by
   `local_b8 = local_50[0]` at the 5th BuildSpriteQuadTemplate call site
   (0x43a144..0x43a1a3).
4. Z=128.1f is hard-coded; the port's HUD overlay pipeline accepts this
   value via depth_z[]. If the port's HUD depth sort uses a different range
   the petals may need a renormalised value.
