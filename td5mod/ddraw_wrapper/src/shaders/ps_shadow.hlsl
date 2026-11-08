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
    float4 params3;       /* [round UG] x = debug view, y = 1 if t1 is the real G-buffer */
};

Texture2D    depthTex : register(t0);
Texture2D    gbufTex  : register(t1);   /* normal+matid; may be unbound (0) */
SamplerState samp     : register(s0);   /* unused (Load) */

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

/* [PERSPECTIVE DEPTH 2026-08-03] recover linear view-Z from the perspective-
 * correct normalized depth (inverse of CPU td5_depth_persp). */
float depth_to_viewz(float d, float depthScale, float depthBias)
{
    float A = (depthBias + depthScale) / depthScale;
    return depthBias * A / (A - d);
}

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
    float vz = depth_to_viewz(D, depthScale, depthBias);
    float sx = input.pos.x - vpX;
    float sy = input.pos.y - vpY;
    float vx = -(sx - rightCx.w) * vz / focal;
    float vy = -(sy - upCy.w)    * vz / focal;
    float3 world = camPosFocal.xyz + vx * rightCx.xyz + vy * upCy.xyz + vz * fwdDepthScale.xyz;

    /* G-buffer normal (a==0 => unknown). Sun-backfacing AND grazing surfaces
     * skip: their shading is already dark, and grazing pixels are the worst
     * self-shadow (acne/speckle) offenders under the jittered march. */
    float4 gb = gbufTex.Load(int3(px, 0));
    int dbg = (int)params3.x;
    if (dbg == 1)   /* G-buffer presence: green = matid>0, magenta = none */
        return (gb.a > 0.001) ? float4(0.3, 1.0, 0.3, 1.0) : float4(1.0, 0.3, 1.0, 1.0);
    if (dbg == 4)   /* reconstruction check: world.y in 1000-unit bands */
    {
        float b = frac(world.y / 1000.0);
        return float4(b, 1.0 - b, 0.5, 1.0);
    }
    /* [round UG] With the real G-buffer bound (params3.y), a receiver with no
     * material id is a camera-facing billboard (tree crown, banner sprite):
     * marching from it self-hits its own quad and speckles. Leave it lit, the
     * same rule that already keeps billboards from CASTING. */
    if (params3.y > 0.5 && gb.a <= 0.001 && dbg != 2)
    {
        if (dbg == 5) return float4(0.0, 0.0, 1.0, 1.0);
        discard;
    }
    float ysign = (params3.z != 0.0) ? params3.z : 1.0;
    if (gb.a > 0.001 && dbg != 2)
    {
        float3 N = gb.rgb * 2.0 - 1.0;
        /* [round UG] Two-sided quads (banners, signs, fences) carry ONE normal;
         * seen from the back it points away from the camera. Face it toward
         * the viewer first (view vector taken into the normal's frame), else
         * a sunlit-looking back face marches through its own quad and the
         * whole sign reads as shadowed. */
        float3 V = camPosFocal.xyz - world;
        V.y *= ysign;
        if (dot(N, V) < 0.0) N = -N;
        if (dot(N, sun.xyz) <= 0.25)
        {
            if (dbg == 5) return float4(0.0, 0.0, 1.0, 1.0);   /* mask: faces away */
            discard;
        }
    }

    /* [round UG] params3.z = sign applied to sun.y for the MARCH only (the
     * receiver test above compares against the G-buffer normal, which lives in
     * the same frame as the sun the game passes). 0 = legacy (+1). */
    float3 msun = float3(sun.x, sun.y * ysign, sun.z);

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
        /* [round UG] steps packed toward the receiver (u^1.6): small casters
         * close by (car, bollards, rails) keep fine steps, tall far casters
         * (houses, walls) are reached with coarse ones, so the range can be
         * long enough for a low sun without more steps. */
        float t = t0 + (maxDist - t0) * pow(((float)k - jit) / (float)steps, 1.6);
        float3 P = world + msun * t;

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
        float sceneVz = depth_to_viewz(sceneD, depthScale, depthBias);

        /* Occluded when the scene surface at this screen point is NEARER than
         * the ray sample (with a bias), but within the thickness window (so a
         * distant foreground object doesn't cast an infinite streak). */
        /* Depth bias grows with march distance — grazing self-hits far along
         * the ray were the speckle source on curved car bodies. */
        float dz = pvz - sceneVz;
        /* [round UG] + a term in the receiver's view distance: the 24-bit
         * perspective depth loses precision with distance, and far road pixels
         * read their own neighbours as occluders (speckle near the horizon). */
        if (dz > 6.0 + 0.02 * t + 0.004 * vz && dz < thick)
        {
            /* Billboard occluders (camera-facing tree/sprite quads) carry no
             * G-buffer data (matid 0): their quads reorient with the camera,
             * so their "shadows" would swing and morph while driving. Only
             * REAL geometry (matid > 0: roads, walls, cars) casts. */
            if (dbg == 2 || gbufTex.Load(int3(sp, 0)).a > 0.001)
            {
                /* Distance-softened: far occluders cast lighter shadows.
                 * [round UG] and fade to 0 at the end of the march, so
                 * a long low-sun shadow (house, wall) ends in a gradient
                 * instead of a hard arc where the rays stop reaching it
                 * (last 30% of the range). */
                float f = t / maxDist;
                occ = (1.0 - 0.45 * f) * saturate((1.0 - f) / 0.3);
                break;
            }
        }
    }

    if (dbg == 5)   /* [round UG] opaque MASK for the ug-shadow-visible row: black = shadowed, white = lit */
        return (occ > 0.0) ? float4(0.0, 0.0, 0.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
    if (dbg == 2 || dbg == 3)   /* occlusion as red (2 = gbuffer filter off) */
        return (occ > 0.0) ? float4(1.0, 0.15, 0.15, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
    float shade = 1.0 - strength * occ;
    return float4(shade, shade, shade, 1.0);
}
