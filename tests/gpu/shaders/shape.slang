// A triangle of one color at a depth, which the target tests draw: its corners at `offset` plus
// (0, -1), (-1, 1), and (1, 1) times `size`, in clip space, at depth `depth`, reversed, 1 the
// nearest.

struct Shape
{
    float4 color;
    float2 offset;
    float size;
    float depth;
};

[[vk::push_constant]]
ConstantBuffer<Shape> shape;

static const float2 corners[3] = { float2(0.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    return float4(shape.offset + corners[index] * shape.size, shape.depth, 1.0);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return shape.color;
}
