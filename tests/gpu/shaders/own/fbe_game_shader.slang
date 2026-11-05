// What's asked of it is its first parameter: its x picks what it draws; its second parameter is how
// far it moves its vertices, and its fourth's x how far along their normals.
#define FBE_MOVES_VERTICES

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    position += vertex.parameter(1).xyz + normal * vertex.parameter(3).x;
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
    if (which == 6.0)
    {
        // Cut by where it is, how it faces, and its vertex's alpha.
        const bool kept = surface.place.x < 0.0 && surface.normal.z > 0.5 && surface.vertexColor.a > 0.5;
        return float4(surface.parameter(2).rgb, kept ? 1.0 : 0.0);
    }
    if (which == 7.0)
    {
        // Cut by how it's lit.
        return float4(surface.parameter(2).rgb, surface.color.r > 0.2 ? 1.0 : 0.0);
    }
    if (which == 8.0)
    {
        // Its first layer, at a UV of its own: its fourth parameter's y of the surface's.
        return surface.texture(0, surface.uv0 * surface.parameter(3).y);
    }
    return surface.parameter(2);
}
