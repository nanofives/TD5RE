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

    output.pos = float4(ndcX, ndcY, z, 1.0);
    output.diffuse  = input.diffuse;
    output.specular = input.specular;
    output.uv       = input.uv;
    output.depth    = input.pos.z;  /* for fog */

    return output;
}
