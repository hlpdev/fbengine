// A triangle whose corners a buffer holds, which the buffer tests draw: read through the buffer's
// address, from the push constants, by the vertex's index, in one color.

struct Points
{
    float2* corners;
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<Points> points;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    return float4(points.corners[index], 0.5, 1.0);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return points.color;
}
