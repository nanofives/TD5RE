/**
 * ps_light.hlsl - Deferred dynamic-light pass (screen-space)
 *
 * Fullscreen additive pass run after the opaque world geometry. For each pixel
 * it samples the scene depth buffer, reconstructs the pixel's WORLD position,
 * and accumulates the contribution of every dynamic light (headlights, etc.),
 * writing the summed light additively (ONE/ONE) onto the scene colour target.
 *
 * This is the real "intermediate lighting layer": because it lights the actual
 * visible surface per pixel, the light conforms to the road, cars and walls,
 * occludes correctly (a pixel is at its true surface), and scales to many lights
 * with no geometry hacks.
 *
 * Reconstruction (inverse of the port's software transform + the pre-transformed
 * VS, which stores NDC.z = depth_z = (view_z - depthBias)/depthScale):
 *   view_z = D * depthScale + depthBias
 *   view_x = -(sx - centerX) * view_z / focal      (sx = pane-relative pixel X)
 *   view_y = -(sy - centerY) * view_z / focal
 *   world  = camPos + view_x*right + view_y*up + view_z*forward   (basis orthonormal)
 */

#define LIGHT_MAX 32

cbuffer LightCB : register(b0)
{
    float4 camPosFocal;    /* xyz = camera world pos, w = focal length         */
    float4 rightCx;        /* xyz = camera right,   w = viewport center X       */
    float4 upCy;           /* xyz = camera up,      w = viewport center Y       */
    float4 fwdDepthScale;  /* xyz = camera forward, w = depth scale (195000)    */
    float4 misc;           /* x = depth bias(64), y = light count, z = vpX, w = vpY */
    /* [P2] x = occlusion march steps (0 = off), y = pane width, z = pane height */
    float4 ext;
    /* per light, 3 float4: [0]=pos.xyz,range  [1]=color.rgb,intensity  [2]=dir.xyz,coneCos */
    float4 lights[LIGHT_MAX * 3];
};

Texture2D    depthTex : register(t0);
/* [lighting rework P0] G-buffer written by the ps_*_g variants during the
 * opaque world pass: .rgb = world normal biased 0..1, .a = material id / 255.
 * a == 0 => no G-buffer data at this pixel (legacy no-normal fallback). Left
 * unbound when the G-buffer is inactive — Load then returns 0 = fallback. */
Texture2D    gbufTex  : register(t1);
SamplerState samp     : register(s0);   /* unused (Load), kept for signature parity */

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    int2 px = int2(input.pos.xy);
    float D = depthTex.Load(int3(px, 0)).r;      /* scene depth, [0,1] */

    /* Cleared/sky pixels sit at the far plane — nothing to light. */
    if (D >= 0.99999)
        discard;

    float focal      = camPosFocal.w;
    float depthScale = fwdDepthScale.w;
    float depthBias  = misc.x;
    int   count      = (int)misc.y;

    float vz = D * depthScale + depthBias;
    float sx = input.pos.x - misc.z;             /* pane-relative pixel coords */
    float sy = input.pos.y - misc.w;
    float vx = -(sx - rightCx.w) * vz / focal;
    float vy = -(sy - upCy.w)    * vz / focal;

    float3 world = camPosFocal.xyz
                 + vx * rightCx.xyz
                 + vy * upCy.xyz
                 + vz * fwdDepthScale.xyz;

    /* [lighting rework P0] Surface normal from the G-buffer (matid 0 = none). */
    float4 gb = gbufTex.Load(int3(px, 0));
    bool   hasN = (gb.a > 0.001);
    float3 N = float3(0.0, 1.0, 0.0);
    if (hasN)
    {
        N = gb.rgb * 2.0 - 1.0;
        float nl = length(N);
        if (nl > 0.001) N /= nl; else hasN = false;
    }

    float3 accum = float3(0.0, 0.0, 0.0);

    [loop]
    for (int k = 0; k < count; k++)
    {
        float4 pr = lights[k * 3 + 0];   /* pos.xyz, range          */
        float4 ci = lights[k * 3 + 1];   /* color.rgb, intensity    */
        float4 dc = lights[k * 3 + 2];   /* dir.xyz, coneCos        */

        float3 toL  = pr.xyz - world;    /* surface -> light */
        float  dist = length(toL);
        float  range = pr.w;
        if (dist >= range)
            continue;

        /* Smooth distance attenuation (bright near, 0 at range). */
        float atten = 1.0 - dist / range;
        atten = atten * atten;

        /* Spotlight cone (dc.w = cos(outer half-angle); w <= -1 => omni point). */
        float cone = 1.0;
        if (dc.w > -0.5)
        {
            float3 Ldir = toL / max(dist, 0.001);   /* surface -> light  */
            float  cd   = dot(-Ldir, dc.xyz);        /* light -> surface . beam dir */
            /* soft edge from the outer cone cos to 1.0 (beam centre). */
            cone = saturate((cd - dc.w) / (1.0 - dc.w));
            cone = cone * cone;
        }

        /* [lighting rework P0] Lambert term with a soft wrap (mirrors the CPU
         * per-vertex bump's 0.85/0.15 split) so grazing surfaces still catch a
         * little pool light. Pixels without G-buffer data keep the legacy
         * orientation-blind behavior. */
        float ndotl = 1.0;
        if (hasN)
        {
            float3 Lv = toL / max(dist, 0.001);
            ndotl = saturate(dot(N, Lv)) * 0.85 + 0.15;
        }

        /* [P2] Light-occlusion march: step from the surface toward the light
         * through the depth buffer; a blocking surface drops the contribution
         * to a small leak factor (so headlights no longer light THROUGH cars
         * and walls). Screen-space: off-screen blockers can't occlude. */
        float vis = 1.0;
        int osteps = (int)ext.x;
        if (osteps > 0)
        {
            float3 Ld = toL / max(dist, 0.001);
            /* per-pixel stratified jitter — see ps_shadow.hlsl rationale */
            float jit = frac(sin(dot(float2(px), float2(12.9898, 78.233))) * 43758.5453);
            [loop]
            for (int s = 1; s <= osteps; s++)
            {
                float t = dist * (((float)s - jit) / (float)(osteps + 1));
                float3 P = world + Ld * t;
                float3 dd = P - camPosFocal.xyz;
                float pvz = dot(dd, fwdDepthScale.xyz);
                if (pvz <= 1.0) continue;
                float pvx = dot(dd, rightCx.xyz);
                float pvy = dot(dd, upCy.xyz);
                float psx = -pvx * focal / pvz + rightCx.w;
                float psy = -pvy * focal / pvz + upCy.w;
                if (psx < 0.5 || psx >= ext.y - 0.5 || psy < 0.5 || psy >= ext.z - 0.5)
                    break;
                float sd = depthTex.Load(int3(int(psx + misc.z), int(psy + misc.w), 0)).r;
                if (sd >= 0.99999) continue;
                float svz = sd * depthScale + depthBias;
                float dz = pvz - svz;
                if (dz > 6.0 && dz < 500.0)
                {
                    vis = 0.15;            /* leak a little (soft look) */
                    break;
                }
            }
        }

        accum += ci.rgb * (ci.w * atten * cone * ndotl * vis);
    }

    return float4(accum, 1.0);
}
