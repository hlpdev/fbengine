// A rectangle of a texture, which the golden image tests draw: two triangles from nothing but the
// vertex's index, at `rect` (left, top, right, bottom, in clip space), reading `uv` (left, top,
// right, bottom) of the texture through the bindless set.

import bindless;

struct Quad
{
    float4 rect;
    float4 uv;
    uint texture;
    uint sampler;
};

[[vk::push_constant]]
ConstantBuffer<Quad> quad;

struct Corner
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    const float2 at = corners[index];
    Corner corner;
    corner.position = float4(lerp(quad.rect.xy, quad.rect.zw, at), 0.0, 1.0);
    corner.uv = lerp(quad.uv.xy, quad.uv.zw, at);
    return corner;
}

[shader("fragment")]
float4 fragmentMain(Corner corner) : SV_Target
{
    return sampleTexture(quad.texture, quad.sampler, corner.uv);
}
