// Flip's present: the screen drawn into its area of the window's image, which the viewport is, by
// a triangle covering it, read through its sampler, nearest or smooth; encoded as sRGB here when
// the window's image is stored as written, not as sRGB.

import bindless;

struct Present
{
    uint texture;
    uint sampler;
    uint encode;
};

[[vk::push_constant]]
ConstantBuffer<Present> present;

struct Corner
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    // Corners at (0, 0), (2, 0), and (0, 2) in UV, so the viewport's square is inside it.
    const float2 uv = float2(float((index << 1) & 2), float(index & 2));
    Corner corner;
    corner.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    corner.uv = uv;
    return corner;
}

float3 toSrgb(float3 linear)
{
    return select(linear <= 0.0031308, linear * 12.92, 1.055 * pow(linear, 1.0 / 2.4) - 0.055);
}

[shader("fragment")]
float4 fragmentMain(Corner corner) : SV_Target
{
    float3 color = sampleTexture(present.texture, present.sampler, corner.uv).rgb;
    if (present.encode != 0)
    {
        color = toSrgb(saturate(color));
    }
    return float4(color, 1.0);
}
