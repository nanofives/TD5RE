/**
 * vs_fullscreen.hlsl - Fullscreen triangle vertex shader
 *
 * Generates a fullscreen triangle from vertex ID (no vertex buffer needed).
 * Used with ps_composite for present blits and BltFast compositing.
 *
 * Vertex 0: (-1, -1) uv (0, 1)
 * Vertex 1: (-1,  3) uv (0,-1)
 * Vertex 2: ( 3, -1) uv (2, 1)
 *
 * The oversized triangle covers the entire viewport; the GPU clips it.
 */

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;

    /* Generate fullscreen triangle from vertex ID */
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv  = uv;

    return output;
}
