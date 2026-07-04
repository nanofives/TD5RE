/**
 * ps_ssr.hlsl - Screen-space ray-marched reflections (lighting rework P3)
 *
 * Fullscreen ALPHA-BLENDED pass run after the shadow + dynamic-light passes
 * (so reflections include shadows and headlight pools). For each pixel with
 * G-buffer data: reflect the view ray about the surface normal and march the
 * reflected ray through the depth buffer; on a hit, sample the SCENE-COLOR
 * COPY at the hit point and blend it in weighted by the material's
 * reflectivity x Fresnel. Miss / off-pane / sky rays contribute nothing
 * (discard) — surfaces just keep their shaded colour.
 *
 * Material reflectivity comes from a small per-material-id LUT (reflA/reflB,
 * ids 0..7 — mirrors td5_material.c). Wet roads: up-facing pixels of material
 * id 1 (DEFAULT — track geometry) get an extra wetBoost when it rains.
 */

cbuffer SSRCB : register(b0)
{
    float4 camPosFocal;   /* xyz = camera world pos, w = focal length          */
    float4 rightCx;       /* xyz = camera right,   w = viewport center X       */
    float4 upCy;          /* xyz = camera up,      w = viewport center Y       */
    float4 fwdDepthScale; /* xyz = camera forward, w = depth scale (195000)    */
    float4 misc;          /* x = depth bias(64), y = vpX, z = vpY, w = wet boost */
    float4 params;        /* x = steps, y = max march dist, z = thickness, w = pane W */
    float4 params2;       /* x = pane H, y = master intensity, zw reserved     */
    float4 reflA;         /* base reflectivity for material ids 0..3           */
    float4 reflB;         /* base reflectivity for material ids 4..7           */
};

Texture2D    depthTex : register(t0);
Texture2D    gbufTex  : register(t1);
Texture2D    sceneTex : register(t2);   /* pre-pass copy of the scene colour */
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
    if (D >= 0.99999)
        discard;                                    /* sky */

    float4 gb = gbufTex.Load(int3(px, 0));
    int matid = (int)(gb.a * 255.0 + 0.5);
    if (matid <= 0)
        discard;                                    /* no G-buffer data */

    float3 N = gb.rgb * 2.0 - 1.0;
    float nl = length(N);
    if (nl < 0.001)
        discard;
    N /= nl;

    float focal      = camPosFocal.w;
    float depthScale = fwdDepthScale.w;
    float depthBias  = misc.x;
    float vpX        = misc.y;
    float vpY        = misc.z;
    float paneW      = params.w;
    float paneH      = params2.x;

    /* Reconstruct world position + view direction. */
    float vz = D * depthScale + depthBias;
    float sx = input.pos.x - vpX;
    float sy = input.pos.y - vpY;
    float vx = -(sx - rightCx.w) * vz / focal;
    float vy = -(sy - upCy.w)    * vz / focal;
    float3 world = camPosFocal.xyz + vx * rightCx.xyz + vy * upCy.xyz + vz * fwdDepthScale.xyz;
    float3 V = normalize(world - camPosFocal.xyz);

    float ndv = dot(N, -V);
    if (ndv <= 0.02)
        discard;                                    /* backfacing normal */

    /* Base reflectivity by material id (+ wet-road boost on up-facing
     * DEFAULT-material pixels — roads/pavements when it rains). */
    float base;
    if      (matid == 0) base = reflA.x;
    else if (matid == 1) base = reflA.y;
    else if (matid == 2) base = reflA.z;
    else if (matid == 3) base = reflA.w;
    else if (matid == 4) base = reflB.x;
    else if (matid == 5) base = reflB.y;
    else if (matid == 6) base = reflB.z;
    else                 base = reflB.w;
    /* Up-facing in POSITION space: world +Y is DOWN, so up = -Y. */
    if (matid == 1 && -N.y > 0.6)
        base += misc.w * saturate((-N.y - 0.6) / 0.4);

    float fresnel = pow(1.0 - ndv, 3.0);
    float w = base * (0.25 + 0.75 * fresnel) * params2.y;
    if (w < 0.02)
        discard;

    float3 R = reflect(V, N);

    int   steps   = (int)params.x;
    float maxDist = params.y;
    float thick   = params.z;

    float3 hitColor = float3(0.0, 0.0, 0.0);
    float  hitFade  = 0.0;

    /* per-pixel stratified jitter — see ps_shadow.hlsl rationale */
    float jit = frac(sin(dot(float2(px), float2(12.9898, 78.233))) * 43758.5453);

    [loop]
    for (int k = 1; k <= steps; k++)
    {
        /* Start 60 units out — a reflected ray leaving a curved body re-hits
         * its OWN geometry almost immediately, which showed up as dark noise
         * dots on light car paint (user feedback round 2). */
        float t = 60.0 + (maxDist - 60.0) * (((float)k - jit) / (float)steps);
        float3 P = world + R * t;

        float3 d = P - camPosFocal.xyz;
        float pvz = dot(d, fwdDepthScale.xyz);
        if (pvz <= 1.0)
            break;                                  /* reflected behind camera */
        float pvx = dot(d, rightCx.xyz);
        float pvy = dot(d, upCy.xyz);

        float psx = -pvx * focal / pvz + rightCx.w;
        float psy = -pvy * focal / pvz + upCy.w;
        if (psx < 0.5 || psx >= paneW - 0.5 || psy < 0.5 || psy >= paneH - 0.5)
            break;                                  /* left the pane */

        int2  sp = int2(int(psx + vpX), int(psy + vpY));
        float sceneD = depthTex.Load(int3(sp, 0)).r;
        if (sceneD >= 0.99999)
            continue;                               /* sky along the ray */
        float sceneVz = sceneD * depthScale + depthBias;

        /* Distance-scaled bias (same acne control as the shadow passes). */
        float dz = pvz - sceneVz;
        if (dz > 6.0 + 0.02 * t && dz < thick)
        {
            hitColor = sceneTex.Load(int3(sp, 0)).rgb;

            /* Edge fade: hits near the pane border pop as the camera moves —
             * fade them out over a 12% margin. Also fade by march distance. */
            float mx = min(psx, paneW - psx) / paneW;
            float my = min(psy, paneH - psy) / paneH;
            float edge = saturate(min(mx, my) / 0.12);
            float dist = 1.0 - 0.5 * ((float)k / (float)steps);
            hitFade = edge * dist;
            break;
        }
    }

    if (hitFade <= 0.01)
        discard;

    return float4(hitColor, w * hitFade);
}
