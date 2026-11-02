// A triangle with a color at each corner, from nothing but the vertex's index, which the golden
// image tests draw: red at the top, green at the bottom left, blue at the bottom right.

struct Corner
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

static const float2 positions[3] = { float2(0.0, -0.8), float2(-0.8, 0.8), float2(0.8, 0.8) };
static const float3 colors[3] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    Corner corner;
    corner.position = float4(positions[index], 0.0, 1.0);
    corner.color = colors[index];
    return corner;
}

[shader("fragment")]
float4 fragmentMain(Corner corner) : SV_Target
{
    return float4(corner.color, 1.0);
}
