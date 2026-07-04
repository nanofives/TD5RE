/**
 * ps_shadow.hlsl - Screen-space ray-marched sun shadows (lighting rework P2)
 *
 * Fullscreen MULTIPLICATIVE pass run after the opaque world geometry (before
 * the additive dynamic-light pass, so headlight pools are not darkened).
 * For each pixel: reconstruct the world position from scene depth (same math
 * as ps_light.hlsl), then ray-march toward the zone's dominant directional
 * light ("sun") through the depth buffer. If a nearer surface blocks the ray,
 * the pixel is in shadow and the pass outputs a grey < 1 which the MULT blend
 * (dst * src) applies to the scene colour.
 *
 * Occluders must be ON SCREEN (screen-space limitation, documented in
 * LIGHTING_REWORK_PLAN.md section 6): cars shadow the road, walls/buildings
 * shadow the road, cars shadow each other — whatever the camera can see.
 *
 * Self-shadow (acne) controls: the march starts offset from the surface, uses
 * a view-z depth bias, and a thickness window so thin/far occluders don't
 * produce infinite shadow streaks. When the G-buffer has a normal for the
 * pixel, surfaces facing AWAY from the sun are skipped entirely — their
 * vertex shading is already dark and multiplying again would double-darken.
 */

cbuffer ShadowCB : register(b0)
{
    float4 camPosFocal;   /* xyz = camera world pos, w = focal length          */
    float4 rightCx;       /* xyz = camera right,   w = viewport center X       */
    float4 upCy;          /* xyz = camera up,      w = viewport center Y       */
    float4 fwdDepthScale; /* xyz = camera forward, w = depth scale (195000)    */
    float4 misc;          /* x = depth bias(64), y = vpX, z = vpY, w = strength 0..1 */
    float4 sun;           /* xyz = surface->light dir (world, unit), w = max march dist (world units) */
    float4 params;        /* x = steps, y = thickness (view-z units), z = start offset (world units), w = pane width  */
    float4 params2;       /* x = pane height, yzw reserved                     */
};

Texture2D    depthTex : register(t0);
Texture2D    gbufTex  : register(t1);   /* normal+matid; may be unbound (0) */
SamplerState samp     : register(s0);   /* unused (Load) */

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    int2 px = int2(input.pos.xy);
    float D = depthTex.Load(int3(px, 0)).r;

    /* Sky / cleared pixels — nothing to shadow. */
    if (D >= 0.99999)
        discard;

    float focal      = camPosFocal.w;
    float depthScale = fwdDepthScale.w;
    float depthBias  = misc.x;
    float vpX        = misc.y;
    float vpY        = misc.z;
    float strength   = misc.w;
    float paneW      = params.w;
    float paneH      = params2.x;

    /* Reconstruct this pixel's world position (same math as ps_light). */
    float vz = D * depthScale + depthBias;
    float sx = input.pos.x - vpX;
    float sy = input.pos.y - vpY;
    float vx = -(sx - rightCx.w) * vz / focal;
    float vy = -(sy - upCy.w)    * vz / focal;
    float3 world = camPosFocal.xyz + vx * rightCx.xyz + vy * upCy.xyz + vz * fwdDepthScale.xyz;

    /* G-buffer normal (a==0 => unknown). Sun-backfacing AND grazing surfaces
     * skip: their shading is already dark, and grazing pixels are the worst
     * self-shadow (acne/speckle) offenders under the jittered march. */
    float4 gb = gbufTex.Load(int3(px, 0));
    if (gb.a > 0.001)
    {
        float3 N = gb.rgb * 2.0 - 1.0;
        if (dot(N, sun.xyz) <= 0.25)
            discard;
    }

    int   steps   = (int)params.x;
    float maxDist = sun.w;
    float thick   = params.y;
    float t0      = params.z;

    /* Per-pixel stratified jitter (hash of the pixel coord — constant across
     * frames, so no temporal shimmer). Without it the fixed step grid makes
     * one occluder's shadow repeat as several offset "echo" copies: adjacent
     * road pixels alternately hit/miss a thin occluder in a spatially
     * coherent pattern. Jitter decorrelates neighbours, turning the echoes
     * into a single dithered-edge shadow. */
    float jit = frac(sin(dot(float2(px), float2(12.9898, 78.233))) * 43758.5453);

    float occ = 0.0;
    [loop]
    for (int k = 1; k <= steps; k++)
    {
        float t = t0 + (maxDist - t0) * (((float)k - jit) / (float)steps);
        float3 P = world + sun.xyz * t;

        /* World -> view (basis rows are orthonormal). */
        float3 d = P - camPosFocal.xyz;
        float pvz = dot(d, fwdDepthScale.xyz);
        if (pvz <= 1.0)
            continue;                      /* behind / at camera — no info   */
        float pvx = dot(d, rightCx.xyz);
        float pvy = dot(d, upCy.xyz);

        /* View -> pane-relative screen pixel (inverse of the port projection). */
        float psx = -pvx * focal / pvz + rightCx.w;
        float psy = -pvy * focal / pvz + upCy.w;
        if (psx < 0.5 || psx >= paneW - 0.5 || psy < 0.5 || psy >= paneH - 0.5)
            break;                         /* ray left this pane — stop      */

        int2  sp = int2(int(psx + vpX), int(psy + vpY));
        float sceneD  = depthTex.Load(int3(sp, 0)).r;
        if (sceneD >= 0.99999)
            continue;                      /* sky along the ray — unoccluded */
        float sceneVz = sceneD * depthScale + depthBias;

        /* Occluded when the scene surface at this screen point is NEARER than
         * the ray sample (with a bias), but within the thickness window (so a
         * distant foreground object doesn't cast an infinite streak). */
        /* Depth bias grows with march distance — grazing self-hits far along
         * the ray were the speckle source on curved car bodies. */
        float dz = pvz - sceneVz;
        if (dz > 6.0 + 0.02 * t && dz < thick)
        {
            /* Billboard occluders (camera-facing tree/sprite quads) carry no
             * G-buffer data (matid 0): their quads reorient with the camera,
             * so their "shadows" would swing and morph while driving. Only
             * REAL geometry (matid > 0: roads, walls, cars) casts. */
            if (gbufTex.Load(int3(sp, 0)).a > 0.001)
            {
                /* Distance-softened: far occluders cast lighter shadows. */
                occ = 1.0 - 0.45 * (t / maxDist);
                break;
            }
        }
    }

    float shade = 1.0 - strength * occ;
    return float4(shade, shade, shade, 1.0);
}
