/**
 * vs_pretransformed.hlsl - Vertex shader for pre-transformed (XYZRHW) vertices
 *
 * TD5 submits all geometry as screen-space XYZRHW vertices (pixel coordinates).
 * This shader converts from screen-space pixels to D3D11 NDC clip space.
 *
 * Vertex layout (32 bytes, matches TD5_FVF = 0x1C4):
 *   float4 pos     : POSITION   (x, y, z, rhw) - screen pixels
 *   float4 diffuse : COLOR0     - per-vertex color (BGRA packed as UNORM)
 *   float4 specular: COLOR1     - specular color (unused by game, forced off)
 *   float2 uv      : TEXCOORD0  - texture coordinates
 */

cbuffer ViewportParams : register(b0)
{
    float viewportWidth;
    float viewportHeight;
    float2 _pad0;
};

struct VS_INPUT
{
    float4 pos      : POSITION;   /* x, y, z, rhw in screen pixels */
    float4 diffuse  : COLOR0;
    float4 specular : COLOR1;
    float2 uv       : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 pos      : SV_POSITION;
    float4 diffuse  : COLOR0;
    float4 specular : COLOR1;
    float2 uv       : TEXCOORD0;
    float  depth    : TEXCOORD1;  /* pass Z for fog computation in PS */
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;

    /* Convert screen-space pixel coords to D3D11 NDC:
     *   NDC.x = (pixel_x / width)  * 2 - 1       range [-1, +1]
     *   NDC.y = 1 - (pixel_y / height) * 2        range [+1, -1] (Y flipped)
     *   NDC.z = z (already in [0,1] from game)
     *   NDC.w = 1.0 (post-projection, no perspective divide needed)
     *
     * D3D11 half-pixel offset: D3D11 does NOT have the legacy Direct3D half-pixel offset,
     * so screen-space coordinates map directly without the -0.5 adjustment
     * that legacy Direct3D sometimes needs. The game's XYZRHW coords are pixel-centered. */
    float ndcX = (input.pos.x / viewportWidth)  *  2.0 - 1.0;
    float ndcY = (input.pos.y / viewportHeight) * -2.0 + 1.0;
    float z = saturate(input.pos.z);

    /* Use RHW (input.pos.w = 1/view_z) to reconstruct proper W for
     * perspective-correct interpolation.  Without this, the GPU does
     * affine (linear) interpolation causing warped geometry at screen edges.
     *
     * [FAR-SPAN WARP FIX 2026-08-30] The epsilon here is a divide-by-zero
     * guard ONLY -- it must never bind inside the legitimate depth range, or
     * every vertex past that depth collapses to the SAME w and the rasterizer
     * silently degrades to affine UV interpolation again (the exact failure
     * this reconstruction exists to prevent).
     *
     * The old value 0.0001 capped w at 10000, i.e. it bound at view_z > 10000
     * -- but s_far_clip is 195000 (td5_render_internal.h), so the clamp was
     * active across ~95% of the visible depth range.  Road/wall quads are
     * submitted whole with no distance subdivision (dispatch_projected_quad,
     * td5_render.c), so a single long span quad straddling that boundary got
     * PS1-style texture warping: straight lane lines and masonry courses
     * kinked and rippled, then snapped straight as the camera closed in and
     * the far vertex came back inside 10000.
     *
     * 1e-6 caps w at 1e6, which binds only past view_z = 1,000,000 -- over 5x
     * the far plane, so it can no longer trigger on real geometry, while
     * keeping w bounded (a much smaller epsilon would let float32 lose
     * precision in the z*w / w round-trip).
     *
     * 2D paths are unaffected: frontend/FMV/HUD submit rhw = 1.0 (w = 1), and
     * the one rhw = 0 producer (behind-near-clip verts in td5_render_effects.c)
     * is excluded from every emitted triangle by its ok[] gate. */
    float rhw = max(input.pos.w, 0.000001);
    float w = 1.0 / rhw;
    output.pos = float4(ndcX * w, ndcY * w, z * w, w);
    output.diffuse  = input.diffuse;
    output.specular = input.specular;
    output.uv       = input.uv;
    output.depth    = input.pos.z;  /* for fog */

    return output;
}
