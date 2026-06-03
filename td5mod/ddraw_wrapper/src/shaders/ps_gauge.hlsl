/**
 * ps_gauge.hlsl - Procedural analog tachometer/speedometer dial (in-race HUD).
 *
 * Analytic SDF reproduction of the original TD5 gauge (tpage4 SPEEDO art), crisp
 * at any resolution. VectorUI only; the baked-texture path is the fallback.
 *
 * Faithful to the reference art:
 *  - a SEMI-TRANSPARENT dark circular background disc (NO hard border line),
 *    subtly shaded (rim lighter, centre darker + top sheen) for depth;
 *  - a concentric INNER circle with a beveled (top-lit) edge -> 3D recessed look;
 *  - an ARC of fine tick "teeth" (subdivisions) sweeping the dial;
 *  - the top portion of the sweep is a RED zone: its teeth are RED and a RED arc
 *    band runs along the rim there;
 *  - a small centre pivot hub.
 * The needle is drawn separately by the HUD (only it animates).
 *
 * Angles are screen-convention radians (0 = +X, CW, screen Y-down). Everything
 * is computed in the quad's LOCAL PIXEL space (uv * g_quadPx), drawn 1:1 in
 * screen px, so a fixed ~1px AA band is resolution-independent crispness.
 *
 * b0 stays FogParams (ps_common); params come from the b1 constant buffer.
 */

#include "ps_common.hlsli"

cbuffer GaugeParams : register(b1)
{
    float2 g_quadPx;     /* quad size px (uv -> local px) */
    float2 g_center;     /* dial centre in local px */
    float  g_radius;     /* outer disc radius px */
    float  g_innerR;     /* inner 3D circle radius px (0 => none) */
    float  g_sweepStart; /* first tick angle (rad) */
    float  g_sweepEnd;   /* last tick angle (rad) */
    float  g_tickCount;  /* number of ticks (>=2) */
    float  g_majorEvery; /* every Nth tick is major */
    float  g_majorLen;   /* major tick length px */
    float  g_minorLen;   /* minor tick length px */
    float  g_tickOut;    /* tick outer radius px */
    float  g_redStart;   /* red zone start angle (rad); ticks >= this are red */
    float  g_redEnd;     /* red zone end angle (rad); <= start => no red zone */
    float  g_pivotPx;    /* centre pivot radius px (0 => none) */
    float  g_rimRedPx;   /* red rim arc thickness px */
    float  g_pad0; float g_pad1; float g_pad2;
    float4 g_face;       /* outer disc rgba (semi-transparent) */
    float4 g_inner;      /* inner disc rgba */
    float4 g_tick;       /* white tick rgba */
    float4 g_red;        /* red teeth + red rim rgba */
    float4 g_pivot;      /* pivot hub rgba */
};

/* Unsigned distance from p to segment ab. */
float sd_seg(float2 p, float2 a, float2 b)
{
    float2 pa = p - a, ba = b - a;
    float  h  = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-5));
    return length(pa - ba * h);
}

/* Coverage of a signed distance edge (d<0 inside). ~1px AA band. */
float cov(float d) { return saturate(0.5 - d); }

/* Premultiplied "src over"; c straight rgb, a coverage. */
void over(inout float3 acc, inout float aA, float3 c, float a)
{
    acc = c * a + acc * (1.0 - a);
    aA  = a     + aA  * (1.0 - a);
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float2 p = input.uv * g_quadPx;
    float2 d = p - g_center;
    float  r = length(d);
    float  upness = -d.y / max(g_radius, 1.0);   /* +1 at top, -1 at bottom */

    float3 acc = float3(0.0, 0.0, 0.0);
    float  aA  = 0.0;

    /* --- 1. outer semi-transparent disc, subtly shaded (depth, no border) --- */
    {
        float cd     = cov(r - g_radius);
        float radial = 1.0 - 0.20 * saturate(1.0 - r / g_radius); /* centre darker */
        float sheen  = 1.0 + 0.14 * upness;                       /* top lighter   */
        over(acc, aA, g_face.rgb * radial * sheen, g_face.a * cd);
    }

    /* --- 2. inner concentric circle for the 3D (recessed) look --- */
    if (g_innerR > 0.0)
    {
        float cdi  = cov(r - g_innerR);
        float dome = 1.0 + 0.45 * (-d.y / max(g_innerR, 1.0));     /* top-lit dome */
        over(acc, aA, g_inner.rgb * dome, g_inner.a * cdi);
        /* beveled edge ring: highlight on top, shadow on the bottom */
        float edge = cov(abs(r - g_innerR) - 1.5);
        float t    = saturate(upness * 0.5 + 0.5);                 /* 1 top, 0 bottom */
        over(acc, aA, lerp(float3(0,0,0), float3(1,1,1), t), edge * 0.35);
    }

    /* --- 3. red rim arc band along the outer edge over the red zone --- */
    if (g_redEnd > g_redStart && g_rimRedPx > 0.0)
    {
        float r_rim = g_radius - g_rimRedPx * 0.5 - 1.0;
        float dr = 1e9;
        const int RN = 12;
        float2 prev = g_center + r_rim * float2(cos(g_redStart), sin(g_redStart));
        [loop] for (int j = 1; j <= RN; ++j)
        {
            float a = lerp(g_redStart, g_redEnd, (float)j / (float)RN);
            float2 cur = g_center + r_rim * float2(cos(a), sin(a));
            dr = min(dr, sd_seg(p, prev, cur));
            prev = cur;
        }
        over(acc, aA, g_red.rgb, g_red.a * cov(dr - g_rimRedPx * 0.5));
    }

    /* --- 4. tick teeth: white in the main zone, RED in the red zone --- */
    {
        int   n   = (int)(g_tickCount + 0.5);
        int   mev = max((int)(g_majorEvery + 0.5), 1);
        float dtw = 1e9;   /* white ticks */
        float dtr = 1e9;   /* red ticks   */
        [loop] for (int i = 0; i < n; ++i)
        {
            float frac  = (n > 1) ? (float)i / (float)(n - 1) : 0.0;
            float a     = lerp(g_sweepStart, g_sweepEnd, frac);
            bool  major = (i % mev) == 0;
            float len   = major ? g_majorLen : g_minorLen;
            float hw    = major ? 1.0 : 0.6;
            float2 dir  = float2(cos(a), sin(a));
            float2 pa   = g_center + g_tickOut * dir;
            float2 pb   = g_center + (g_tickOut - len) * dir;
            float  dd   = sd_seg(p, pa, pb) - hw;
            if (g_redEnd > g_redStart && a >= g_redStart - 1e-4)
                dtr = min(dtr, dd);
            else
                dtw = min(dtw, dd);
        }
        over(acc, aA, g_tick.rgb, g_tick.a * cov(dtw));
        over(acc, aA, g_red.rgb,  g_red.a  * cov(dtr));
    }

    /* --- 5. centre pivot hub --- */
    if (g_pivot.a > 0.0 && g_pivotPx > 0.0)
        over(acc, aA, g_pivot.rgb, g_pivot.a * cov(r - g_pivotPx));

    if (aA <= 0.0)
        discard;

    return float4(acc / aA, aA);  /* premultiplied -> straight for SRC_ALPHA blend */
}
