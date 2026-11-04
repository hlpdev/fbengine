// What's asked of it is its first parameter: its x picks what it draws, and its second parameter
// is how far it moves its vertices.
#define FBE_MOVES_VERTICES

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    position += vertex.parameter(1).xyz;
}

float4 shade(Surface surface)
{
    const float which = surface.parameter(0).x;
    if (which == 1.0)
    {
        return float4(surface.normal * 0.5 + float3(0.5, 0.5, 0.5), 1.0);
    }
    if (which == 2.0)
    {
        return surface.parameter(1000000);
    }
    if (which == 3.0)
    {
        return surface.texture(9, surface.uv0);
    }
    if (which == 4.0)
    {
        return float4(surface.light(surface.normal, 0.0), 1.0);
    }
    if (which == 5.0)
    {
        return surface.color;
    }
    return surface.parameter(2);
}
