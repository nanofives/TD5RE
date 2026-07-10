/* ========================================================================
 * td5_render_pipeline.c -- Trig, matrix/vector ops, projection pipeline
 *
 * Split out of td5_render.c (P1-C step 3, 2026-07-02). Contents, in original
 * core order (all migrated from td5re_stubs.c originally):
 *   - 12-bit trigonometry (Cos/SinFloat12bit + LUT bake)
 *   - Matrix / vector operations (rotation builders, transforms)
 *   - Render pipeline helpers (world->view transform, projection,
 *     LoadRenderRotationMatrix and friends)
 * Cross-TU seam: td5_render_internal.h (PRIVATE); the trig/matrix builder
 * decls were already in the seam header (step-1 hoist) and
 * LoadRenderRotationMatrix is public in td5_render.h.
 * ======================================================================== */

#include "td5_render.h"
#include "td5_camera.h"
#include "td5_platform.h"
#include "td5_rcmd.h"   /* Phase B render-transform: per-pane CPU command recording */
#include "td5_profile.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_asset.h"
#include "td5_save.h"
#include "td5_vfx.h"
#include "td5_arcade.h"   /* ARCADE power-up pad / hazard world billboards */
#include "td5_damage.h"   /* [CAR DAMAGE] per-vertex deformation deltas */
#include "td5_ai.h"
#include "td5_light.h"    /* [DYNAMIC LIGHTS] world-space point-light registry */
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors */
#include "td5re.h"

#include "../../../re/include/td5_actor_struct.h"

#include "td5_render_internal.h"  /* PRIVATE core<->effects seam */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>

/* ======== [split] trig + matrix + pipeline helpers (moved verbatim from td5_render.c) ======== */
/* ========================================================================
 * 12-bit Trigonometry (migrated from td5re_stubs.c)
 *
 * Original game uses a lookup table populated once by
 * BuildSinCosLookupTables @ 0x0040A650 from FCOS in 80-bit x87 then stored
 * as 32-bit float (g_sinCosFloatTable @ 0x00488984, 5120 entries; covers a
 * 4096-step circle plus an extra quarter turn of padding).
 *
 *   CosFloat12bit(arg) : return LUT[(arg) & 0xFFF]
 *   SinFloat12bit(arg) : return LUT[(arg - 0x400) & 0xFFF]
 *
 * To match byte-for-byte, the port now also computes the LUT once at startup
 * (mirroring the original's two-step FILD/FMUL(2π)/FMUL(1/4096) x87 chain via
 * long double on i386 MinGW) and indexes it for every call. Computing live
 * with cos()/sin() was the previous behavior and leaked LSB drift relative
 * to the original's static LUT.
 *
 * Audit: re/analysis/pilot_trig_audit.md
 * ======================================================================== */


#define TD5_TRIG_LUT_SIZE 0x1400  /* 5120, matches original */

static float  s_cosFloatTable[TD5_TRIG_LUT_SIZE];
static int    s_cosFixedTable[TD5_TRIG_LUT_SIZE];
static int    s_trig_lut_built = 0;

/* Reference LUT extracted byte-for-byte from a running TD5_d3d.exe instance
 * via tools/frida_pool3_trig_dump.js (one-shot read of g_sinCosFloatTable
 * @ 0x00488984, 5120 entries × 4 bytes = 20 KB). Using the original's exact
 * bits avoids the residual ±1 ULP drift seen even when port-side FCOS,
 * constants, and FPU PC are all matched — likely due to a remaining x87
 * micro-state difference (FTOP, register pollution from MinGW startup math
 * before the LUT is built). The dump is the only way to guarantee byte
 * equality and is also faster than computing the LUT at startup.
 *
 * To regenerate after a runtime LUT change in the original:
 *   1. python re/tools/quickrace/td5_quickrace.py --no-ini ... \
 *          --extra-script tools/frida_pool3_trig_dump.js
 *   2. python -c "import struct; ..." > td5_trig_lut_data.c (see runbook).
 */
extern const uint32_t td5_trig_lut_bits[TD5_TRIG_LUT_SIZE];

static void td5_trig_build_lut(void) {
    /* Copy the embedded LUT bytes into the float LUT. The C-level cast via a
     * union pun is byte-exact. */
    for (int i = 0; i < TD5_TRIG_LUT_SIZE; i++) {
        union { uint32_t u; float f; } pun;
        pun.u = td5_trig_lut_bits[i];
        s_cosFloatTable[i] = pun.f;
    }
    /* The int (FP12 fixed-point) LUT — g_sinCosLut_fixed12 in the original — is
     * derived from the float LUT by `lrintf(float * 4096.0f)` using FISTP
     * semantics (round-to-nearest-even). Build it here so CosFixed12bit /
     * SinFixed12bit stay byte-faithful to the original's int LUT as well.
     * Note: doing this via FISTP under PC=64 matches the original because
     * the source float is already byte-equal. */
    static const float SCALE_F = 4096.0f;
    unsigned short saved_cw = 0, new_cw = 0;
    __asm__ volatile ("fnstcw %0" : "=m" (saved_cw));
    new_cw = (unsigned short)((saved_cw & 0xfcffu) | 0x0300u);
    __asm__ volatile ("fldcw %0" : : "m" (new_cw));
    for (int i = 0; i < TD5_TRIG_LUT_SIZE; i++) {
        float v = s_cosFloatTable[i];
        int   out;
        __asm__ volatile (
            "flds     %[in]       \n\t"
            "fmuls    %[scale]    \n\t"
            "fistpl   %[out]      \n\t"
            : [out] "=m" (out)
            : [in] "m" (v),
              [scale] "m" (SCALE_F)
            : "st", "memory"
        );
        s_cosFixedTable[i] = out;
    }
    __asm__ volatile ("fldcw %0" : : "m" (saved_cw));

    s_trig_lut_built = 1;
}

static inline void td5_trig_ensure_lut(void) {
    if (!s_trig_lut_built) td5_trig_build_lut();
}

/* [CONFIRMED @ 0x0040A6A0] Byte-faithful with orig CosFloat12bit.
 * L5 promotion 2026-05-18 (small-tier sweep). 4-instr listing match:
 * AND angle, 0xfff; FLD float [base + idx*4]. Port reads s_cosFloatTable
 * (built from FPU cos at td5_trig_build_lut) at the same index. */
float CosFloat12bit(unsigned int angle) {
    td5_trig_ensure_lut();
    unsigned int idx = angle & 0xFFFu;
    float v = s_cosFloatTable[idx];
    return v;
}

/* [CONFIRMED @ 0x0040A6C0] Byte-faithful with orig SinFloat12bit.
 * L5 promotion 2026-05-18 (small-tier sweep). 5-instr listing match:
 * ADD EAX, 0xfffffc00 (32-bit signed wrap = sin via cos(angle-pi/2));
 * AND 0xfff; FLD float [s_cosFloatTable + idx*4]. */
float SinFloat12bit(int angle) {
    td5_trig_ensure_lut();
    /* Match the original's `ADD EAX, 0xfffffc00` (32-bit signed wrap), then
     * AND 0xfff. */
    unsigned int shifted = ((unsigned int)angle) + 0xfffffc00u;
    unsigned int idx = shifted & 0xFFFu;
    float v = s_cosFloatTable[idx];
    return v;
}

int CosFixed12bit(unsigned int angle) {
    td5_trig_ensure_lut();
    return s_cosFixedTable[angle & 0xFFFu];
}

int SinFixed12bit(int angle) {
    td5_trig_ensure_lut();
    unsigned int shifted = ((unsigned int)angle) + 0xfffffc00u;
    return s_cosFixedTable[shifted & 0xFFFu];
}

/* AngleFromVector12 LUT — literal port of DAT_00463214 from TD5_d3d.exe.
 *
 * 1024 entries encode round(atan(i/1024) * 2048 / pi) for i in [0, 1023], range
 * [0, 511]. Entry 1024 (value 0x200 = 512) sits past the declared array and is
 * silently read by the original when param_1==param_2>0 (the diagonal of
 * octant 1 produces idx=1024 exactly). The original binary's memory at
 * 0x00463A14 holds 0x00 0x02 which decodes to 0x0200 — mathematically
 * atan(1.0)*2048/pi=512 — and we mirror that here. */
static const int16_t k_angle_from_vector12_lut[1026] = {
       0,    1,    1,    2,    3,    3,    4,    4,    5,    6,    6,    7,    8,    8,    9,   10,
      10,   11,   11,   12,   13,   13,   14,   15,   15,   16,   17,   17,   18,   18,   19,   20,
      20,   21,   22,   22,   23,   24,   24,   25,   25,   26,   27,   27,   28,   29,   29,   30,
      31,   31,   32,   32,   33,   34,   34,   35,   36,   36,   37,   38,   38,   39,   39,   40,
      41,   41,   42,   43,   43,   44,   44,   45,   46,   46,   47,   48,   48,   49,   50,   50,
      51,   51,   52,   53,   53,   54,   55,   55,   56,   57,   57,   58,   58,   59,   60,   60,
      61,   62,   62,   63,   63,   64,   65,   65,   66,   67,   67,   68,   69,   69,   70,   70,
      71,   72,   72,   73,   74,   74,   75,   75,   76,   77,   77,   78,   79,   79,   80,   80,
      81,   82,   82,   83,   84,   84,   85,   85,   86,   87,   87,   88,   89,   89,   90,   90,
      91,   92,   92,   93,   94,   94,   95,   95,   96,   97,   97,   98,   99,   99,  100,  100,
     101,  102,  102,  103,  104,  104,  105,  105,  106,  107,  107,  108,  108,  109,  110,  110,
     111,  112,  112,  113,  113,  114,  115,  115,  116,  117,  117,  118,  118,  119,  120,  120,
     121,  121,  122,  123,  123,  124,  125,  125,  126,  126,  127,  128,  128,  129,  129,  130,
     131,  131,  132,  132,  133,  134,  134,  135,  136,  136,  137,  137,  138,  139,  139,  140,
     140,  141,  142,  142,  143,  143,  144,  145,  145,  146,  146,  147,  148,  148,  149,  149,
     150,  151,  151,  152,  152,  153,  154,  154,  155,  156,  156,  157,  157,  158,  159,  159,
     160,  160,  161,  161,  162,  163,  163,  164,  164,  165,  166,  166,  167,  167,  168,  169,
     169,  170,  170,  171,  172,  172,  173,  173,  174,  175,  175,  176,  176,  177,  178,  178,
     179,  179,  180,  180,  181,  182,  182,  183,  183,  184,  185,  185,  186,  186,  187,  188,
     188,  189,  189,  190,  190,  191,  192,  192,  193,  193,  194,  195,  195,  196,  196,  197,
     197,  198,  199,  199,  200,  200,  201,  202,  202,  203,  203,  204,  204,  205,  206,  206,
     207,  207,  208,  208,  209,  210,  210,  211,  211,  212,  212,  213,  214,  214,  215,  215,
     216,  216,  217,  218,  218,  219,  219,  220,  220,  221,  222,  222,  223,  223,  224,  224,
     225,  225,  226,  227,  227,  228,  228,  229,  229,  230,  231,  231,  232,  232,  233,  233,
     234,  234,  235,  236,  236,  237,  237,  238,  238,  239,  239,  240,  241,  241,  242,  242,
     243,  243,  244,  244,  245,  246,  246,  247,  247,  248,  248,  249,  249,  250,  250,  251,
     252,  252,  253,  253,  254,  254,  255,  255,  256,  256,  257,  258,  258,  259,  259,  260,
     260,  261,  261,  262,  262,  263,  263,  264,  265,  265,  266,  266,  267,  267,  268,  268,
     269,  269,  270,  270,  271,  272,  272,  273,  273,  274,  274,  275,  275,  276,  276,  277,
     277,  278,  278,  279,  279,  280,  281,  281,  282,  282,  283,  283,  284,  284,  285,  285,
     286,  286,  287,  287,  288,  288,  289,  289,  290,  290,  291,  291,  292,  293,  293,  294,
     294,  295,  295,  296,  296,  297,  297,  298,  298,  299,  299,  300,  300,  301,  301,  302,
     302,  303,  303,  304,  304,  305,  305,  306,  306,  307,  307,  308,  308,  309,  309,  310,
     310,  311,  311,  312,  312,  313,  313,  314,  314,  315,  315,  316,  316,  317,  317,  318,
     318,  319,  319,  320,  320,  321,  321,  322,  322,  323,  323,  324,  324,  325,  325,  326,
     326,  327,  327,  328,  328,  329,  329,  330,  330,  331,  331,  332,  332,  333,  333,  334,
     334,  335,  335,  335,  336,  336,  337,  337,  338,  338,  339,  339,  340,  340,  341,  341,
     342,  342,  343,  343,  344,  344,  345,  345,  346,  346,  346,  347,  347,  348,  348,  349,
     349,  350,  350,  351,  351,  352,  352,  353,  353,  354,  354,  354,  355,  355,  356,  356,
     357,  357,  358,  358,  359,  359,  360,  360,  360,  361,  361,  362,  362,  363,  363,  364,
     364,  365,  365,  366,  366,  366,  367,  367,  368,  368,  369,  369,  370,  370,  371,  371,
     371,  372,  372,  373,  373,  374,  374,  375,  375,  375,  376,  376,  377,  377,  378,  378,
     379,  379,  379,  380,  380,  381,  381,  382,  382,  383,  383,  383,  384,  384,  385,  385,
     386,  386,  387,  387,  387,  388,  388,  389,  389,  390,  390,  390,  391,  391,  392,  392,
     393,  393,  393,  394,  394,  395,  395,  396,  396,  397,  397,  397,  398,  398,  399,  399,
     399,  400,  400,  401,  401,  402,  402,  402,  403,  403,  404,  404,  405,  405,  405,  406,
     406,  407,  407,  408,  408,  408,  409,  409,  410,  410,  410,  411,  411,  412,  412,  413,
     413,  413,  414,  414,  415,  415,  415,  416,  416,  417,  417,  417,  418,  418,  419,  419,
     419,  420,  420,  421,  421,  422,  422,  422,  423,  423,  424,  424,  424,  425,  425,  426,
     426,  426,  427,  427,  428,  428,  428,  429,  429,  430,  430,  430,  431,  431,  432,  432,
     432,  433,  433,  434,  434,  434,  435,  435,  435,  436,  436,  437,  437,  437,  438,  438,
     439,  439,  439,  440,  440,  441,  441,  441,  442,  442,  442,  443,  443,  444,  444,  444,
     445,  445,  446,  446,  446,  447,  447,  447,  448,  448,  449,  449,  449,  450,  450,  451,
     451,  451,  452,  452,  452,  453,  453,  454,  454,  454,  455,  455,  455,  456,  456,  457,
     457,  457,  458,  458,  458,  459,  459,  459,  460,  460,  461,  461,  461,  462,  462,  462,
     463,  463,  464,  464,  464,  465,  465,  465,  466,  466,  466,  467,  467,  468,  468,  468,
     469,  469,  469,  470,  470,  470,  471,  471,  471,  472,  472,  473,  473,  473,  474,  474,
     474,  475,  475,  475,  476,  476,  476,  477,  477,  478,  478,  478,  479,  479,  479,  480,
     480,  480,  481,  481,  481,  482,  482,  482,  483,  483,  483,  484,  484,  484,  485,  485,
     486,  486,  486,  487,  487,  487,  488,  488,  488,  489,  489,  489,  490,  490,  490,  491,
     491,  491,  492,  492,  492,  493,  493,  493,  494,  494,  494,  495,  495,  495,  496,  496,
     496,  497,  497,  497,  498,  498,  498,  499,  499,  499,  500,  500,  500,  501,  501,  501,
     502,  502,  502,  503,  503,  503,  504,  504,  504,  505,  505,  505,  506,  506,  506,  507,
     507,  507,  508,  508,  508,  508,  509,  509,  509,  510,  510,  510,  511,  511,  511,  512,
    /* index 1024 -- silent past-end read by octant 1's diagonal (p1==p2>0).
     * Original 0x00463A14 holds 0x0200 = 512 = round(atan(1)*2048/pi). */
    512,
    /* index 1025 -- silent past-end read by octants 5/6 only for the
     * degenerate inputs (-1,-1) and (-1,+1). Original 0x00463A16 = 0x0000. */
    0
};


int AngleFromVector12(int x, int z) {
    /* Literal port of 0x0040A720 AngleFromVector12 from TD5_d3d.exe.
     * Convention: x = param_1 (e.g. dx, horizontal), z = param_2 (dz, vertical).
     * Returns 12-bit angle (0..0xFFF) measured CW from +z axis.
     *
     * Implementation mirrors the listing octant-by-octant. The LUT-index
     * trick `&DAT_00463214 + idx * -2` in the assembly is replicated as
     * `k_angle_from_vector12_lut[-idx]` for the negative-quotient branches.
     *
     * Acceptance: byte-faithful with the original LUT — see pilot_0040A720_audit.md. */
    const int param_1 = x;
    const int param_2 = z;
    int ret;

    if (param_1 == 0 && param_2 == 0) {
        ret = 0;
    } else if (param_1 >= 0) {
        if (param_2 > 0) {
            if (param_1 < param_2) {
                /* OCTANT 0: idx = (p1*1024 + p2/2)/p2 ∈ [0, 1024) */
                int idx = (param_1 * 1024 + (param_2 >> 1)) / param_2;
                ret = k_angle_from_vector12_lut[idx];
            } else {
                /* OCTANT 1: param_1 >= param_2 > 0 → idx = (p2*1024 + p1/2)/p1 ∈ [0, 1024]
                 * Sub-test mirrors the assembly: only reach here via JLE at 0040a743,
                 * and a separate JZ at 0040a75f returns 0 for param_1==0 (dead
                 * because we already ruled out (0,0) and we're in p2>0). */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_2 * 1024 + (param_1 >> 1)) / param_1;
                    ret = 0x400 - k_angle_from_vector12_lut[idx];
                }
            }
        } else {
            /* param_1 >= 0, param_2 <= 0 */
            int neg_p2 = -param_2;
            if (neg_p2 <= param_1) {
                /* OCTANT 2: param_1 > 0, param_2 <= 0, -p2 <= p1 (so |p2|<=p1).
                 *   0040a7af TEST ESI,ESI; JZ 0040a731  (if param_1==0, return 0)
                 *   0040a7b7 MOV EAX,ECX; SHL EAX,0xA    ; EAX = p2*1024 (<=0)
                 *   0040a7bc MOV ECX,ESI; SAR ECX,1      ; ECX = p1>>1 (>0)
                 *   0040a7c0 SUB EAX,ECX                  ; EAX = p2*1024 - (p1>>1) (<=0)
                 *   0040a7c3 IDIV ESI                     ; /param_1 (>0); quotient <= 0
                 *   0040a7cd SUB EDX,EAX where EDX=0x463214  ; LUT_base - 2*q → LUT[-q]
                 *   0040a7d2 ADD EAX,0x400
                 */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_2 * 1024 - (param_1 >> 1)) / param_1;
                    ret = 0x400 + k_angle_from_vector12_lut[-idx];
                }
            } else {
                /* OCTANT 3: param_1 > 0 (we'd have hit dead-corner if ==0),
                 * param_2 < 0, -p2 > p1. The JZ at 0040a78a tests ECX==0 (param_2):
                 * if both p2==0 AND fell into this branch, return 0 — but we're
                 * here only if -p2 > p1 ≥ 0, so p2<0 strictly. JZ is dead.
                 *
                 *   EAX = p1*1024 - (p2>>1)   ; p2<0 → -(p2>>1)>0 → num positive
                 *   EAX /= p2                  ; quotient negative
                 *   2*quotient subtracted from LUT_base → LUT[-quotient]
                 *   return 0x800 - LUT[-quotient] */
                if (param_2 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_1 * 1024 - (param_2 >> 1)) / param_2;
                    ret = 0x800 - k_angle_from_vector12_lut[-idx];
                }
            }
        }
    } else {
        /* param_1 < 0, branched at 0040a737 → 0040a7d8.
         *   0040a7d8 TEST ECX, ECX   ; param_2 sign
         *   0040a7da MOV EAX, ESI
         *   0040a7dc JLE 0040a828    ; if param_2 <= 0 → 0040a828
         */
        if (param_2 > 0) {
            /* param_1 < 0, param_2 > 0.
             *   0040a7de NEG EAX       ; EAX = -param_1 (>0)
             *   0040a7e0 CMP ECX, EAX  ; compares param_2 vs -param_1
             *   0040a7e2 JLE 0040a807  ; take if param_2 <= -param_1 → OCTANT 6
             */
            int neg_p1 = -param_1;
            if (param_2 > neg_p1) {
                /* OCTANT 7: param_1<0, param_2>0, param_2 > -param_1
                 *   0040a7e4 MOV EDX, ECX; SAR EDX,1  ; EDX = p2>>1
                 *   0040a7e8 MOV EAX, ESI; SHL EAX,0xA; EAX = p1*1024 (negative)
                 *   0040a7ed SUB EAX, EDX             ; EAX = p1*1024 - p2/2
                 *   0040a7ef CDQ; IDIV ECX            ; /param_2 (positive); quotient negative
                 *   0040a7f8 SHL EAX,1
                 *   0040a7fa SUB ECX, EAX  (ECX=0x463214) ; LUT[-quotient]
                 *   0040a7ff EAX = 0x1000
                 *   0040a804 SUB EAX, EDX
                 */
                int idx = (param_1 * 1024 - (param_2 >> 1)) / param_2;
                ret = 0x1000 - k_angle_from_vector12_lut[-idx];
            } else {
                /* OCTANT 6: param_1<0, param_2>0, param_2 <= -param_1.
                 *   0040a807 MOV EAX, ECX; SHL EAX,0xA  ; EAX = p2*1024 (positive)
                 *   0040a80c MOV ECX, ESI; SAR ECX,1    ; ECX = p1>>1 (negative)
                 *   0040a810 SUB EAX, ECX               ; EAX = p2*1024 - p1/2 (positive larger)
                 *   0040a813 IDIV ESI                   ; /param_1 (negative); quotient negative
                 *   0040a81b SHL EAX,1
                 *   0040a81d SUB EDX, EAX  (EDX=0x463214) ; LUT[-quotient]
                 *   0040a822 ADD EAX, 0xc00
                 */
                int idx = (param_2 * 1024 - (param_1 >> 1)) / param_1;
                ret = 0xc00 + k_angle_from_vector12_lut[-idx];
            }
        } else {
            /* param_1 < 0, param_2 <= 0. 0040a828:
             *   0040a828 MOV EDX, ECX
             *   0040a82a NEG EAX       ; EAX = -param_1 (>0)
             *   0040a82c NEG EDX       ; EDX = -param_2 (>=0)
             *   0040a82e CMP EDX, EAX  ; compares -p2 vs -p1
             *   0040a830 JLE 0040a857  ; take if -p2 <= -p1 → OCTANT 5
             */
            int neg_p1 = -param_1;
            int neg_p2 = -param_2;
            if (neg_p2 > neg_p1) {
                /* OCTANT 4: param_1<0, param_2<0, -p2 > -p1 (|p2|>|p1|).
                 * The JZ at 0040a834 fires when param_1==0 — but we're in p1<0, dead.
                 *   0040a83a MOV EAX, ESI; SHL EAX,0xA  ; EAX = p1*1024 (negative)
                 *   0040a83f MOV EDX, ECX; SAR EDX,1    ; EDX = p2>>1 (negative)
                 *   0040a843 ADD EAX, EDX               ; EAX = p1*1024 + p2/2 (very negative)
                 *   0040a846 IDIV ECX                   ; /param_2 (negative); quotient positive
                 *   0040a849 MOVSX EAX,[EAX*2+0x463214] ; LUT[+quotient]
                 *   0040a851 ADD EAX, 0x800
                 */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_1 * 1024 + (param_2 >> 1)) / param_2;
                    ret = 0x800 + k_angle_from_vector12_lut[idx];
                }
            } else {
                /* OCTANT 5: param_1<0, param_2<=0, -p2 <= -p1 (|p2|<=|p1|).
                 *   0040a857 MOV EAX, ECX; SHL EAX,0xA  ; EAX = p2*1024 (<=0)
                 *   0040a85c MOV ECX, ESI; SAR ECX,1    ; ECX = p1>>1 (negative)
                 *   0040a860 ADD EAX, ECX               ; EAX = p2*1024 + p1/2 (very negative)
                 *   0040a863 IDIV ESI                   ; /param_1 (negative); quotient positive
                 *   0040a866 MOVSX EDX,[EAX*2+0x463214] ; LUT[+quotient]
                 *   0040a86e EAX = 0xc00; SUB EAX, EDX
                 */
                int idx = (param_2 * 1024 + (param_1 >> 1)) / param_1;
                ret = 0xc00 - k_angle_from_vector12_lut[idx];
            }
        }
    }

    return ret;
}

float td5_cos_12bit(uint32_t angle) {
    return CosFloat12bit(angle);
}

float td5_sin_12bit(uint32_t angle) {
    return SinFloat12bit((int)angle);
}

/* ========================================================================
 * Matrix / Vector Operations (migrated from td5re_stubs.c)
 * ======================================================================== */

/* [CONFIRMED @ 0x0042DA10] Byte-faithful with orig MultiplyRotationMatrices3x3.
 * L5 audit 2026-05-18 (TD5_pool0 read-only):
 *   - Formula: C[i][j] = sum_k A[i*3+k] * B[k*3+j], identical to original
 *     row-major 3x3 multiply (see param_3[2] = A[0]*B[2]+A[1]*B[5]+A[2]*B[8]).
 *   - Alias safety: original loads ALL 48 source slots into temps before any
 *     write to param_3 (aliasing-safe). Port uses local tmp[9] buffer + memcpy
 *     — semantically identical for any aliasing pattern (A==out, B==out, or
 *     A==B==out as seen in td5_camera.c rotor chains).
 *   - Original computes float-only with FPU stack-order writes; port computes
 *     float-only via i,j,k triple loop. Same precision (single-precision IEEE).
 *   - Write order in original is non-sequential (param_3[2], 3, 4, 0, 5, 1,
 *     6, 7, 8) because of FPU register pressure; result identical after all
 *     stores commit. */
void MultiplyRotationMatrices3x3(float *A, float *B, float *out) {
    int i, j, k;
    float tmp[9];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            tmp[i*3+j] = 0.0f;
            for (k = 0; k < 3; k++)
                tmp[i*3+j] += A[i*3+k] * B[k*3+j];
        }
    memcpy(out, tmp, 9 * sizeof(float));
}

void TransformVector3ByBasis(float *matrix, void *vec, int *out) {
    /*
     * 0x42dbd0 -- Transform a short[3] vector by a 3x3 rotation matrix.
     *
     * [ARCH-DIVERGENCE: signature & output type; L5 sweep 2026-05-21]
     *   Orig 0x0042DBD0 disassembly (FLD/FMUL/FADDP/FSTP) writes 3 floats
     *   via FSTP — no truncation. Orig callers consume floats:
     *   RenderRaceActorForView, BuildSpecialActorOverlayQuads,
     *   ApplyMeshProjectionEffect, RenderTireTrackPool.
     *
     *   Port reuses this symbol with (float *m, short *v, int *out) plus a
     *   truncate-toward-zero (int) cast at the FSTP site. Camera-side port
     *   callers (UpdateVehicleRelativeCamera, UpdateTracksideCamera case 1/2)
     *   call THIS, whereas in the orig those same camera sites called
     *   ConvertFloatVec3ToIntVec3 @ 0x0042DB40 (__ftol + (int)(short) clamp).
     *
     *   Math sequence is identical: out[i] = m[i*3+0]*v[0] + m[i*3+1]*v[1]
     *   + m[i*3+2]*v[2]. Term-reorder in orig FPU stack produces equivalent
     *   IEEE single-precision result. Output type and short-clamp behavior
     *   diverge — see UpdateTracksideCamera/UpdateVehicleRelativeCamera
     *   headers for visual impact assessment.
     */
    short *v = (short *)vec;
    if (!out) return;
    if (!matrix || !v) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)v[0];
    float fy = (float)v[1];
    float fz = (float)v[2];

    out[0] = (int)(matrix[0] * fx + matrix[1] * fy + matrix[2] * fz);
    out[1] = (int)(matrix[3] * fx + matrix[4] * fy + matrix[5] * fz);
    out[2] = (int)(matrix[6] * fx + matrix[7] * fy + matrix[8] * fz);
}

/* [FIX 2026-05-24 OVERSIGHT: case_1_2_basis_transform; orig 0x0042DB40
 * ConvertFloatVec3ToIntVec3] Same math as TransformVector3ByBasis but each
 * output is __ftol-rounded then truncated to int16 via (int)(short) cast.
 * Orig camera sites (UpdateTracksideCamera case 1/2, UpdateVehicleRelativeCamera)
 * call THIS helper, not TransformVector3ByBasis. For |result| <= 32767 the
 * two match; the short-clamp is a safety net for overflow cases. */
void ConvertFloatVec3ToIntVec3(float *matrix, void *vec, int *out) {
    short *v = (short *)vec;
    if (!out) return;
    if (!matrix || !v) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)v[0];
    float fy = (float)v[1];
    float fz = (float)v[2];

    out[0] = (int)(short)(int)(matrix[0] * fx + matrix[1] * fy + matrix[2] * fz);
    out[1] = (int)(short)(int)(matrix[3] * fx + matrix[4] * fy + matrix[5] * fz);
    out[2] = (int)(short)(int)(matrix[6] * fx + matrix[7] * fy + matrix[8] * fz);
}

void BuildRotationMatrixFromAngles(float *out, short *angles) {
    /*
     * 0x42e1e0 -- Build a 3x3 rotation matrix from 12-bit Euler angles
     * (pitch, yaw, roll).  Uses the same axis convention as
     * BuildCameraBasisFromAngles: yaw -> pitch -> roll.
     *
     * angles[0] = pitch, angles[1] = yaw, angles[2] = roll
     * All in 12-bit fixed-point (0-4095 = 0-360 degrees).
     *
     * NOTE: The original binary's trig lookup at 0x40a6a0 is a cosine
     * table (table[0]=1), and 0x40a6c0 offsets by -1024 giving sine.
     * Our stubs label them backwards (SinFloat12bit=sin, CosFloat12bit=cos).
     * The matrix slot pattern was decompiled from the original where the
     * "first" trig call (func_A) returns cos.  We swap s/c here to match:
     *   s = CosFloat12bit (= original func_B = sin)
     *   c = SinFloat12bit (= original func_A = cos)
     * so the rest of the matrix construction stays correct.
     */
    float rot[9];
    float s, c;

    if (!out || !angles) return;

    /* Start with identity */
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
    out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;

    /* [FIX 2026-05-27 PM-12 — matrix rotation ORDER reversed]
     * Decomp of orig BuildRotationMatrixFromAngles @ 0x0042E1E0 (closed-form)
     * produces Ry(a1)·Rx(a0)·Rz(a2) (yaw·pitch·roll). Working out the elements:
     *   M[5]=-sin(a0)   M[8]=cos(a1)*cos(a0)   M[2]=sin(a1)*cos(a0)
     *   M[3]=sin(a2)*cos(a0)   M[4]=cos(a2)*cos(a0)
     * all match Ry·Rx·Rz exactly.
     *
     * The previous port applied Yaw then Pitch then Roll (each as out = rot·out),
     * which builds Rz·Rx·Ry — the REVERSE order. Same display_angles, same trig
     * helpers, but a different final matrix. Physics solvers (attitude_from_wheels)
     * use raw contacts so the numeric attitude matched the orig; but every render
     * of the car body and the wheel billboards uses this matrix, so the orig saw
     * Ry·Rx·Rz and the port saw Rz·Rx·Ry → the rendered orientation differed even
     * though every diagnostic that re-multiplied through the same wrong matrix
     * agreed with itself (the data-matches-but-visual-differs paradox).
     *
     * To match the orig, apply Roll FIRST, then Pitch, then Yaw:
     *   out = I
     *   out = Rz · out          (after roll block)
     *   out = Rx · out  = Rx·Rz (after pitch block)
     *   out = Ry · out  = Ry·Rx·Rz   ✓ matches orig
     */

    /* Roll (angles[2]): rotate around Z axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[2]);
    c = SinFloat12bit(angles[2]);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Pitch (angles[0]): rotate around X axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[0]);
    c = SinFloat12bit(angles[0]);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Yaw (angles[1]): rotate around Y axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[1]);
    c = SinFloat12bit(angles[1]);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);
}

/*
 * Static matrix loaded by LoadRenderRotationMatrix for use by
 * ConvertFloatVec3ToShortAngles.  This mirrors the original engine's
 * global at ~0x43DA80 target.
 */
static float s_loaded_render_matrix[12] = {
    1,0,0, 0,1,0, 0,0,1, 0,0,0
};

void ConvertFloatVec3ToShortAngles(short *in, short *out) {
    /*
     * 0x42e2e0 -- Transform a short[3] direction vector through the
     * currently loaded render rotation matrix and store the result as
     * short[3].  Despite the misleading name, this is a matrix*vector
     * transform, not a unit conversion.
     */
    if (!out) return;
    if (!in) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)in[0];
    float fy = (float)in[1];
    float fz = (float)in[2];

    out[0] = (short)(int)(s_loaded_render_matrix[0] * fx +
                          s_loaded_render_matrix[1] * fy +
                          s_loaded_render_matrix[2] * fz);
    out[1] = (short)(int)(s_loaded_render_matrix[3] * fx +
                          s_loaded_render_matrix[4] * fy +
                          s_loaded_render_matrix[5] * fz);
    out[2] = (short)(int)(s_loaded_render_matrix[6] * fx +
                          s_loaded_render_matrix[7] * fy +
                          s_loaded_render_matrix[8] * fz);
}

void LoadRenderRotationMatrix(float *matrix) {
    /*
     * 0x43da80 -- Load a rotation matrix (float[12] = 3x3 + translation)
     * into the static render matrix used by ConvertFloatVec3ToShortAngles.
     * Only the 3x3 rotation part (first 9 floats) is needed for the
     * direction transform; we copy all 12 for completeness.
     */
    if (!matrix) return;
    memcpy(s_loaded_render_matrix, matrix, 12 * sizeof(float));
}

/* ========================================================================
 * Render Pipeline Helpers (migrated from td5re_stubs.c)
 * ======================================================================== */

typedef struct TD5_RenderSpriteQuadParams {
    void     *dest;
    int       mode_flags;
    float     scr_x[4];
    float     scr_y[4];
    float     depth_z[4];
    float     tex_u[4];
    float     tex_v[4];
    uint32_t  diffuse[4];
    int       texture_page;
    int       reserved;
} TD5_RenderSpriteQuadParams;

typedef struct TD5_RenderSpriteQuad {
    int      geometry_ptr;
    int      vertex_count;
    float    v0_x, v0_y, v0_z, v0_rhw;
    uint32_t v0_color;
    float    v0_u, v0_v;
    float    v1_x, v1_y, v1_z, v1_rhw;
    uint32_t v1_color;
    float    v1_u, v1_v;
    float    v2_x, v2_y, v2_z, v2_rhw;
    uint32_t v2_color;
    float    v2_u, v2_v;
    float    v3_x, v3_y, v3_z, v3_rhw;
    uint32_t v3_color;
    float    v3_u, v3_v;
    float    tex_u0, tex_v0;
    float    tex_u1, tex_v1;
    float    quad_width;
    float    quad_height;
    float    texture_page;
    uint8_t  padding[0xB8 - 0x94];
} TD5_RenderSpriteQuad;

/* BuildSpriteQuadTemplate @ 0x00432BD0 — flag-driven sprite-quad writer.
 *
 * Orig dispatches on a per-bit mask (verified via Ghidra disasm 0x432BD0..
 * 0x432D5D, decomp 2026-05-18 from TD5_pool0 read-only):
 *
 *   flag 0x001 — GEOMETRY:  write 4× (sx, sy, rhw) using formula
 *                  sx = view_x * g_inverseProjectionDepth * z
 *                  sy = view_y * g_inverseProjectionDepth * z
 *                  rhw = z
 *                Writes hit byte offsets 0x14, 0x40, 0x6c, 0x98 in the orig
 *                184-byte quad buffer (44-byte vertex stride).
 *   flag 0x002 — UV:        write 4× (u, v) = src * (1/256) = DAT_004749d0
 *   flag 0x004 — COLOR:     write 4× (uint32) = src & 0xff
 *                Note: the `& 0xff` mask is intentional. In orig D3DCOLOR
 *                ARGB the low byte is the BLUE channel; combined with D3D3
 *                TSS SELECTARG2 (texture-only) it has no visible effect.
 *                Port's R8G8B8A8_UNORM + MODULATE shader DOES read diffuse.rgb,
 *                so the mask reproduces the visual outcome only by ALSO
 *                forcing diffuse_rgb≈0 (which the modulate shader would render
 *                as black). Port-correct behavior is to KEEP the full 32-bit
 *                color so the modulate shader passes the texture through; this
 *                is the existing behavior and remains for visual parity.
 *   flag 0x100 — OPCODE:    write WORD at byte 0 of quad
 *                  param[26] != 0 ? 6 : 3  (tri-strip vs tri-fan opcode)
 *   flag 0x200 — TEXPAGE:   write WORD at byte 2 of quad from low 16 bits
 *                  of param[27].
 *
 * Port adaptation (legacy callers):
 *   The 3 existing callers in td5_hud.c pass mode_flags=0 expecting
 *   "do everything". Map mode_flags=0 to TD5_BSQT_LEGACY_ALL (geom+UV+color+
 *   texpage). Map mode_flags=2 to TD5_BSQT_UV_ONLY for compatibility with the
 *   smoke-draw style. Any caller passing a value with bit 0x1000 set is
 *   treated as a raw orig-style bitmask (geom/UV/color/opcode/texpage).
 *
 * Port-side ARCH-DIVERGENCE: the port's TD5_RenderSpriteQuad layout differs
 * from orig's 44-byte-stride packed buffer — the port uses a (sx, sy, sz, rhw,
 * color, u, v) 7-float layout per vertex starting at offset 0x08. The flag
 * dispatch maps each orig field semantic onto the port layout. The opcode
 * field at port byte 0 is `geometry_ptr` (int); the texpage at byte 2 is the
 * high half of geometry_ptr — preserving orig's 32-bit-wide opcode|texpage
 * header would corrupt the port's pointer-based pipeline. To avoid that we
 * store opcode/texpage into reserved scratch instead. */

/* Orig flag bits (must match exact values). */
#define TD5_BSQT_GEOMETRY   0x001
#define TD5_BSQT_UV         0x002
#define TD5_BSQT_COLOR      0x004
#define TD5_BSQT_OPCODE     0x100
#define TD5_BSQT_TEXPAGE    0x200

/* Port-side dispatch bit: when set, treat mode_flags as a raw orig bitmask.
 * Otherwise apply the legacy compatibility mapping documented above. */
#define TD5_BSQT_RAW_FLAGS  0x1000

/* Legacy "do everything" mask used when callers pass mode_flags=0. */
#define TD5_BSQT_LEGACY_ALL (TD5_BSQT_GEOMETRY | TD5_BSQT_UV | TD5_BSQT_COLOR | TD5_BSQT_TEXPAGE)

void td5_render_build_sprite_quad(int *params) {
    const TD5_RenderSpriteQuadParams *src = (const TD5_RenderSpriteQuadParams *)params;
    TD5_RenderSpriteQuad *dst;
    unsigned int flags;
    float z, rhw;

    if (!src || !src->dest) {
        return;
    }

    dst = (TD5_RenderSpriteQuad *)src->dest;

    /* Resolve flag mask. Legacy paths use mode_flags ∈ {0, 2}; orig-faithful
     * callers may set TD5_BSQT_RAW_FLAGS and pass the orig 5-bit mask. */
    if ((unsigned int)src->mode_flags & TD5_BSQT_RAW_FLAGS) {
        flags = (unsigned int)src->mode_flags & ~(unsigned int)TD5_BSQT_RAW_FLAGS;
    } else if (src->mode_flags == 2) {
        flags = TD5_BSQT_UV;
    } else {
        flags = TD5_BSQT_LEGACY_ALL;
    }

    /* --- Geometry (orig flag 0x001) --- */
    if (flags & TD5_BSQT_GEOMETRY) {
        z = src->depth_z[0];
        rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;

        dst->geometry_ptr = 0;
        dst->vertex_count = 4;

        /* Slot mapping mirrors orig's storage order:
         *   src[0] → dst.v0   src[3] → dst.v1
         *   src[2] → dst.v2   src[1] → dst.v3   */
        dst->v0_x = src->scr_x[0]; dst->v0_y = src->scr_y[0];
        dst->v1_x = src->scr_x[3]; dst->v1_y = src->scr_y[3];
        dst->v2_x = src->scr_x[2]; dst->v2_y = src->scr_y[2];
        dst->v3_x = src->scr_x[1]; dst->v3_y = src->scr_y[1];

        dst->v0_z = dst->v1_z = dst->v2_z = dst->v3_z = z;
        dst->v0_rhw = dst->v1_rhw = dst->v2_rhw = dst->v3_rhw = rhw;

        dst->quad_width = src->scr_x[2] - src->scr_x[0];
        dst->quad_height = src->scr_y[1] - src->scr_y[0];
    }

    /* --- Color (orig flag 0x004) --- */
    if (flags & TD5_BSQT_COLOR) {
        /* Orig masks src & 0xff (D3DCOLOR low byte = blue channel) — see
         * function header for why port keeps the full 32-bit value. */
        dst->v0_color = src->diffuse[0];
        dst->v1_color = src->diffuse[3];
        dst->v2_color = src->diffuse[2];
        dst->v3_color = src->diffuse[1];
    }

    /* --- UV (orig flag 0x002) --- */
    if (flags & TD5_BSQT_UV) {
        dst->v0_u = src->tex_u[0]; dst->v0_v = src->tex_v[0];
        dst->v1_u = src->tex_u[3]; dst->v1_v = src->tex_v[3];
        dst->v2_u = src->tex_u[2]; dst->v2_v = src->tex_v[2];
        dst->v3_u = src->tex_u[1]; dst->v3_v = src->tex_v[1];

        dst->tex_u0 = src->tex_u[0];
        dst->tex_v0 = src->tex_v[0];
        dst->tex_u1 = src->tex_u[2];
        dst->tex_v1 = src->tex_v[2];
    }

    /* --- Texpage (orig flag 0x200) --- */
    if (flags & TD5_BSQT_TEXPAGE) {
        dst->texture_page = (float)src->texture_page;
    }

    /* --- Opcode (orig flag 0x100) ---
     * Orig stores: param[26] != 0 ? 6 : 3 as a WORD at byte 0 of the quad.
     * Port has no equivalent slot (geometry_ptr at byte 0 is a pointer used
     * by the port's batch pipeline). The opcode is not consumed by the port
     * pipeline, so silently drop the write but record that we honored the
     * flag. */
    (void)(flags & TD5_BSQT_OPCODE);
}

void td5_render_submit_translucent(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) {
        return;
    }

    /*
     * HUD translucent quads are already emitted as pre-transformed 0xB8 sprite
     * records. They are not TD5_PrimitiveCmd batches, so forwarding them into
     * td5_render_queue_translucent_batch() makes the batch parser read garbage
     * dispatch_type state and crash after the first frame.
     */
    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z = fdata[base + 2];
        verts[i].rhw = fdata[base + 3];
        verts[i].diffuse = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u = fdata[base + 5];
        verts[i].tex_v = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad using the POINT-filter preset, which
 * uses alpha_ref=1 instead of the LINEAR preset's 0x80. Needed for surfaces
 * that want fractional vertex-alpha transparency below 0x80 — primarily the
 * minimap background grid, whose tiles need to stay under ~50% opacity. */
void td5_render_submit_translucent_low_ref(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z  = fdata[base + 2];
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad using the HUD preset (LINEAR filter +
 * alpha_ref=1). Mirrors orig M2DX DXD3D::SetRenderState @ M2DX.dll 0x10001770
 * which sets D3DRS_ALPHAREF=0 + D3DRS_ALPHAFUNC=NOTEQUAL — i.e. discard only
 * fully-transparent pixels. The non-HUD TRANSLUCENT_LINEAR keeps alpha_ref=0x80
 * to prune bilinear fringes on world props; HUD widgets need the lower cutoff
 * to retain anti-aliased edges on digits/text. */
void td5_render_submit_translucent_hud(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z  = fdata[base + 2];
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Additive variant of submit_translucent_hud for the victory star pulse.
 * Uses TD5_PRESET_ADDITIVE_OVERLAY (ONE/ONE, z-test off) so the grayscale-ramp
 * petals (diffuse RGB = phase*0.319, alpha pinned 0xFF) read as a SEMI-TRANSPARENT
 * WHITE GLOW that brightens as phase grows: gray 0 adds nothing (invisible at
 * start) -> gray 255 adds full white (bright). The plain translucent HUD path
 * (SRCALPHA with alpha=0xFF) drew them as OPAQUE gray quads instead.
 * [user feedback 2026-05-30: star should be white, semi-transparent, brighter
 *  as the animation progresses. Matches orig's additive (type-3) petal path.] */
void td5_render_submit_additive_hud(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z  = fdata[base + 2];
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_OVERLAY);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad for world-space VFX (smoke, weather
 * streaks) so it is OCCLUDED by opaque geometry (walls, cars), matching the
 * original. Uses TD5_PRESET_ADDITIVE_WORLD: ONE/ONE additive blend with the
 * depth test ON (LEQUAL) and z-write off.
 *
 * Why depth-tested: RE of the original (Ghidra, 2026-06-01) shows queued
 * translucent primitives — including wheel smoke — are drawn by
 * FlushQueuedTranslucentPrimitives @0x00431340, which RunRaceFrame @0x0042b580
 * calls while the OPAQUE pass preset is still active (ZFUNC=LESSEQUAL, z-buffer
 * enabled). SetRaceRenderStatePreset @0x0040b070 never touches ZENABLE, which
 * stays TRUE scene-wide (proven by the SKY pass dodging occlusion via
 * ZFUNC=ALWAYS rather than disabling ZENABLE), so orig smoke is depth-tested.
 *
 * DEPTH SPACE: the renderer is uniformly LINEAR depth (no NDC stage). Smoke's
 * `sz` from project_vertex (line 498) is vz*(1/195000); opaque geometry writes
 * (vz-64)*(1/195000) (clip_and_submit_polygon, line 824). They differ ONLY by
 * the constant 64 NEAR_DEPTH_OFFSET, folded in below so the LEQUAL compare
 * against coplanar geometry is exact. td5_render_submit_tire_mark (below) is
 * the analogous depth-tested decal path (via TD5_PRESET_SHADOW). */
void td5_render_submit_translucent_world(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        /* Fold in the -64 NEAR_DEPTH_OFFSET that the opaque pass applies
         * (line 824) but the shared project_vertex (line 498) omits, so smoke
         * ties exactly with coplanar opaque geometry under the LEQUAL test. */
        verts[i].depth_z  = fdata[base + 2] - NEAR_DEPTH_OFFSET * DEPTH_NORMALIZE_INV;
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_WORLD);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad as a ground DECAL: TD5_PRESET_SHADOW
 * (z_test=LEQUAL, z_write=0, alpha_ref=1, SRCALPHA). Used for tire/skid marks
 * — they lie on the road and MUST be depth-tested against world geometry so
 * walls/props occlude them. The marks' `sz` comes from the same project_vertex
 * (linear vz/far_clip) the opaque pass uses, so the LEQUAL compare is valid
 * (unlike the smoke NDC-vs-linear issue). z_write=0 so overlapping marks in
 * the trail don't z-fight each other. [FIX 2026-05-28 tire-marks-through-walls] */
/* [2026-06-08 procedural FX] When the VFX layer is rendering tire marks through
 * the procedural ps_fx_decal shader, it sets this so the immediate submit below
 * uses the low-alpha-ref depth-tested preset (TRANSLUCENT_POINT_ZTEST, alpha_ref=1)
 * instead of TRANSLUCENT_ANISO (alpha_ref=0x80) — otherwise the decal's feathered
 * edges (alpha < 0.5) get alpha-tested away. Default 0 keeps the legacy textured
 * path byte-identical. */
static int s_tire_mark_fx_preset = 0;
void td5_render_set_tire_mark_fx_preset(int on) { s_tire_mark_fx_preset = on ? 1 : 0; }

void td5_render_submit_tire_mark(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        /* [FIX 2026-06-02 tire-through-car] Put the decal in the SAME depth space
         * as the opaque road/car (which write (vz-NEAR_DEPTH_OFFSET)*INV) and add a
         * small extra bias toward the camera so the mark wins the coplanar road
         * without z-fighting, while the car body (genuinely much closer) still
         * occludes it. The raw projected sz (fdata[base+2]) omitted the -64 the
         * opaque pass applies, so marks were depth-inconsistent and showed through
         * the car. */
        verts[i].depth_z  = fdata[base + 2]
                            - (NEAR_DEPTH_OFFSET + TIRE_DECAL_BIAS) * DEPTH_NORMALIZE_INV;
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    /* [FIX 2026-06-02 tire-through-car] Use a depth-tested translucent preset WITHOUT
     * the SHADOW preset's polygon_offset. That rasterizer DepthBias pulls decals
     * toward the camera (needed for the car's OWN shadow, coplanar under the car) and
     * was shoving the tire marks IN FRONT of the car body -> see-through. The marks
     * trail behind the car, so they only need to (a) win the coplanar road, handled by
     * the small vertex bias above, and (b) lose to the car, handled by the normal
     * LEQUAL test now that no rasterizer pull over-biases them. */
    td5_plat_render_set_preset(s_tire_mark_fx_preset ? TD5_PRESET_TRANSLUCENT_POINT_ZTEST
                                                     : TD5_PRESET_TRANSLUCENT_ANISO);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

void td5_render_set_clip_rect(float left, float right, float top, float bottom) {
    int ileft   = (int)left;
    int itop    = (int)top;
    int iright  = (int)(right + 0.5f);
    int ibottom = (int)(bottom + 0.5f);
    td5_plat_render_set_clip_rect(ileft, itop, iright, ibottom);
}

/* SetProjectionCenterOffset @ 0x0043E8E0 — writes cx/cy to the globals that
 * every projection formula reads (port: s_center_x / s_center_y).
 * Called per-frame by RunRaceFrame for each viewport, and by the minimap path
 * for the inset render. Restore to screen center after the minimap pass.
 * [CONFIRMED @ 0x0043E8E0] */
void td5_render_set_projection_center(float cx, float cy) {
    s_center_x = cx;
    s_center_y = cy;
}

/* RecomputeTracksideProjectionScale @ 0x0043E900 -- update frustum plane normals
 * when the trackside camera changes g_depthFovFactor (projection depth).
 * Formula mirrors original: h_len = sqrt(W^2*0.25 + depth^2), etc.
 * [CONFIRMED @ 0x0043E900] */
void td5_render_recompute_frustum_for_trackside(void) {
    extern int   g_depthFovFactor; /* camera.c: projection scale, 0x1000=identity */
    extern float g_projFovScale;   /* camera.c: 1/4096 */
    float depth = s_focal_length * (float)g_depthFovFactor * g_projFovScale;
    float w = (float)s_viewport_width;
    float h = (float)s_viewport_height;
    float h_len = sqrtf(w * w * 0.25f + depth * depth);
    float v_len = sqrtf(h * h * 0.25f + depth * depth);
    s_frustum_h_cos =  depth / h_len;
    s_frustum_h_sin = -(w / (h_len + h_len));
    s_frustum_v_cos =  depth / v_len;
    s_frustum_v_sin = -(h / (v_len + v_len));
}

/* [CONFIRMED @ 0x00439E60 RenderHudRadialPulseOverlay; DA-T5 impl 2026-05-22]
 * 5-petal translucent pulse ring drawn at viewport center on race transitions
 * (race-start, lap, finish). Ported from orig 928-byte RenderHudRadialPulseOverlay
 * per DA-T5 audit:
 *
 *   - Phase advance:  phase += dt * 4.2f  while phase < 3000.0f
 *   - Alpha:          clamp(phase * 0.31875f, 0, 255)
 *   - Radius:         viewport_width * phase * (1/160)
 *   - Anim accum:     s_radial_pulse_anim += dt * 3328.0f
 *
 * Vertex layout per petal (mirrors orig V0/V1/V2/V3 quad slots):
 *   V0 = inner_start (radius/2)
 *   V1 = outer_bisector (full radius, between V0 and V2)
 *   V2 = inner_end (radius/2)
 *   V3 = (0, 0) center
 *
 * Constants (from TD5_d3d.exe data segment per DA-T5):
 *   0x0045d624 = 0.0f      (phase gate)
 *   0x0045d708 = 3328.0f   (anim incr)
 *   0x0045d70c = 0.31875f  (phase→alpha)
 *   0x0045d710 = 4.2f      (phase incr)
 *   0x0045d714 = 3000.0f   (phase cap)
 *   0x0045d64c = 0.00625f  (radius = w * phase / 160)
 *   0x0045d5d0 = 0.5f      (inner ring multiplier)
 *   0x4300199a = 128.1f    (quad Z) */
static float s_radial_pulse_anim;  /* orig [0x004B08C0] _g_hudRadialPulseAnimState */

/* Mirror of td5_hud.c HUD_WHITE_TEX_PAGE — the 1x1 white texture page uploaded
 * during HUD init, used to render flat-color (untextured-equivalent) HUD quads
 * through the texture-modulating translucent path. */
#define TD5_HUD_WHITE_TEX_PAGE 899

void td5_render_radial_pulse(float dt)
{
    float phase = td5_hud_radial_pulse_get();

    /* Gate: orig FCOMP [0x0045d624] (= 0.0f). Skip when phase < 0. */
    if (phase < 0.0f) return;

    /* Snapshot base angle from anim-state accumulator (truncate-toward-zero). */
    int base_angle = (int)s_radial_pulse_anim;

    /* Phase advance (capped at 3000.0f). */
    if (phase < 3000.0f) {
        phase += dt * 4.2f;
        td5_hud_radial_pulse_set(phase);
    }

    /* Anim accumulator advances every frame (independent of phase). */
    s_radial_pulse_anim += dt * 3328.0f;

    /* Star opacity ramp. [user 2026-06-02: victory star looked "a little
     * pinkish"; should be white/neutral.] The DOMINANT cause was the missing
     * TD5_BSQT_TEXPAGE flag above (the petals modulated PAGE 0, a brown scene
     * atlas) — fixed there; with page 899 bound the petals are now pure white
     * (frame-dump verified 255,255,255). This ramp is a SECONDARY measure: the
     * petals are still drawn translucent (SRCALPHA), so at the old coefficient
     * 0.55 they only reached ~72% opacity (phase ~0..330 -> alpha max ~181),
     * letting the WARM finish-line scene bleed ~28% through the white star and
     * leaving a faint residual warm tint. [RE: RenderHudRadialPulseOverlay
     * @0x00439E60 — orig petals are OPAQUE (alpha 0xFF), so the original never
     * bleeds the scene.]
     *
     * [S26 2026-06-05 FINAL — user wants a SUBTLE star: "appear just like now at
     * the beginning, then as it's rotating it gets just a little bit more opaque
     * (like 20%) and just that".] So this is a deliberate, documented deviation
     * from the original's opaque petals: the star stays TRANSLUCENT the whole
     * time. It fades in faint (same early look as the prior ramp) and rises
     * GENTLY to a low ~20% peak (alpha ~51 = 0.20*255) by the end of the ~2.5s
     * victory hold, then holds there — never approaching the full-white-out the
     * user rejected, and with NO fade-out at the end (also rejected).
     *   alpha = phase * 0.16  (reaches ~51 at phase ~315 = end of hold)
     *   capped at 51 so it never exceeds ~20% opacity.
     * Tunable: STAR_ALPHA_SLOPE sets how quickly it intensifies; STAR_ALPHA_MAX
     * sets the peak opacity (51 = 20%, 64 = 25%, 255 = fully opaque). */
    const float STAR_ALPHA_SLOPE = 0.16f;   /* opacity gained per phase unit */
    const int   STAR_ALPHA_MAX   = 51;      /* peak alpha (~20% of 255) */
    int alpha = (int)(phase * STAR_ALPHA_SLOPE);
    if (alpha < 0)                 alpha = 0;
    else if (alpha > STAR_ALPHA_MAX) alpha = STAR_ALPHA_MAX;

    /* Per-frame radius. viewport_width * phase * coeff.
     * Orig coeff [CONFIRMED @ 0x439e60 RenderHudRadialPulseOverlay: _DAT_0045d64c
     *  = 0.0015625f = 1/640]. A prior port bug used 0.00625f (1/160) — 4x too
     *  large, ballooning the star ~4x too fast (user 2026-05-30 "animation too
     *  fast") — which was restored to the faithful 1/640.
     *
     * [S26 2026-06-05 — "make the star bigger ... covers more of the screen"]
     *  Measured end radius/viewport_width = 0.752 at 1/640; enlarge the linear
     *  coefficient to 0.0024f (= 1/417, ~1.54x the faithful value) so the star
     *  grows clearly larger while the growth stays LINEAR in phase (smooth, no
     *  jump). Deliberate, documented deviation from the faithful 1/640 per user
     *  request. Tunable: 0.0015625 = faithful, 0.0024 = enlarged (current).
     *
     * [S26 2026-06-05 FINAL — "the star stops growing at some point, otherwise it
     *  looks fine"] A radius CAP was briefly added to stop a full white-out, but
     *  the white-out came from the opacity (since dialed down to a translucent
     *  ~20% peak above), not the size. At ~20% opacity a large star just lets the
     *  scene show through, so the cap is unnecessary AND the user saw the growth
     *  visibly stop when it clamped. Cap removed: the radius now grows linearly
     *  with phase for the whole animation, never stalling. */
    float radius = (float)s_viewport_width * phase * 0.0024f;

    /* 10 ring vertices: even k = inner (radius*0.5), odd k = outer (radius).
     * Inner angle steps by -0x33332 (~72°) per pair; outer angle is inner - 0x19999. */
    float vx[10], vy[10];
    int a = base_angle;
    for (int k = 0; k < 10; k += 2) {
        unsigned int a_inner = ((unsigned int)a) >> 8;
        unsigned int a_outer = ((unsigned int)(a - 0x19999)) >> 8;
        vx[k]     = CosFloat12bit(a_inner) * radius * 0.5f;
        vy[k]     = SinFloat12bit((int)a_inner) * radius * 0.5f;
        vx[k + 1] = CosFloat12bit(a_outer) * radius;
        vy[k + 1] = SinFloat12bit((int)a_outer) * radius;
        a -= 0x33332;
    }

    /* White victory star with alpha fade-in: RGB pinned WHITE (0xFFFFFF),
     * alpha = the phase ramp. Drawn via the translucent (SRCALPHA) HUD path
     * below, so alpha 0 = invisible -> alpha 255 = bright opaque white.
     *
     * [user 2026-05-30: "star is black, should be white".] Two port bugs made
     * it read black: (1) the previous pass put the ramp in the RGB bytes (so
     * the star was near-black gray at low phase) and (2) submitted ADDITIVE,
     * where dark RGB adds ~nothing to the scene -> a faint/black flash.
     * Deliberate deviation from orig's gray-RGB-ramp/opaque-alpha at 0x439e60:
     * orig's gray ramp only reaches white at phase ~800 (off-screen radius),
     * so on-screen it always looks dark — constant-white + alpha-ramp delivers
     * the white glow the user expects while keeping the faithful translucent blend. */
    uint32_t color = ((uint32_t)alpha << 24) | 0x00FFFFFFu;

    /* Center the ring on the viewport. */
    float cx = (float)s_viewport_width * 0.5f;
    float cy = (float)s_viewport_height * 0.5f;

    /* Per-petal scratch quad buffers (orig DAT_004B0C08/CC0/D78/E30/EE8). */
    static uint8_t s_pulse_quads[5][0xB8];
    static const int idx_table[5][3] = {
        {0, 1, 2}, {2, 3, 4}, {4, 5, 6}, {6, 7, 8}, {8, 9, 0},
    };

    for (int q = 0; q < 5; q++) {
        int i0 = idx_table[q][0];  /* inner start */
        int i1 = idx_table[q][1];  /* outer bisector */
        int i2 = idx_table[q][2];  /* inner end */

        TD5_RenderSpriteQuadParams p;
        p.dest = &s_pulse_quads[q];
        /* [FIX 2026-06-02 star-pinkish] TD5_BSQT_TEXPAGE was MISSING here, so
         * build_sprite_quad silently dropped p.texture_page (899=white) and the
         * quad kept texture_page=0 -> the petals modulated PAGE 0 (an arbitrary
         * brown scene atlas), rendering the victory star muddy brown/pink instead
         * of white. A prior pass set p.texture_page=899 but forgot the flag that
         * makes build_sprite_quad honour it. Adding TD5_BSQT_TEXPAGE binds the
         * real 1x1 white page 899 -> white star (matches orig untextured petals). */
        p.mode_flags = TD5_BSQT_RAW_FLAGS | TD5_BSQT_GEOMETRY | TD5_BSQT_COLOR | TD5_BSQT_TEXPAGE;

        /* Slot mapping per td5_render_build_sprite_quad:
         *   src[0] → V0   src[3] → V1   src[2] → V2   src[1] → V3
         * Orig wants V0=inner_start, V1=outer_bisector, V2=inner_end, V3=center.
         * So src indices: 0→inner_start, 1→center, 2→inner_end, 3→outer_bisector. */
        p.scr_x[0] = cx + vx[i0]; p.scr_y[0] = cy + vy[i0];
        p.scr_x[1] = cx;          p.scr_y[1] = cy;
        p.scr_x[2] = cx + vx[i2]; p.scr_y[2] = cy + vy[i2];
        p.scr_x[3] = cx + vx[i1]; p.scr_y[3] = cy + vy[i1];

        for (int v = 0; v < 4; v++) {
            p.depth_z[v] = 128.1f;   /* orig immediate 0x4300199a */
            p.diffuse[v] = color;
            p.tex_u[v]   = 0.0f;
            p.tex_v[v]   = 0.0f;
        }
        /* Orig petals are UNTEXTURED flat color (tex_u=tex_v=0). The port's
         * translucent-HUD path always modulates by a bound texture, so bind the
         * 1x1 WHITE page (== td5_hud.c HUD_WHITE_TEX_PAGE 899, uploaded at HUD
         * init) — page 0 is an arbitrary atlas whose texel darkened the flat
         * white, contributing to the "black star". white*white = white. */
        p.texture_page = TD5_HUD_WHITE_TEX_PAGE;
        p.reserved     = 0;

        td5_render_build_sprite_quad((int *)&p);
        /* Translucent (SRCALPHA/INVSRCALPHA) so the white petals fade in by
         * vertex alpha — faithful to orig SubmitImmediateTranslucentPrimitive
         * @ 0x4315b0 (NOT additive). The previous additive path made the dark
         * low-phase color invisible/black. [user feedback 2026-05-30] */
        td5_render_submit_translucent_hud((uint16_t *)&s_pulse_quads[q]);
    }
}
