// What pbr.slang makes of a direction across an equirectangular image, as a color, for its tests to
// give it directions no map's texels are sure to reach on every GPU.

import pbr;

struct Poles
{
    float4 direction;
};

[[vk::push_constant]]
ConstantBuffer<Poles> poles;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(equirectangularAcross(poles.direction.xyz), 0.0, 0.0, 1.0);
}
