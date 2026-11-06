// What's asked of it is its first parameter: its x picks what it draws; its second parameter is how
// far it moves its vertices, and its fourth's x how far along their normals; its sixth, with a w,
// the normal it bends a PBR one to, in tangent space, and with none, the one it has.
#define FBE_MOVES_VERTICES
#define FBE_BENDS_NORMALS

float3 bend(Surface surface)
{
    const float4 to = surface.parameter(5);
    // With a w of 2, toward +x where what it's given faces the camera, and -x where it doesn't.
    if (to.w == 2.0)
    {
        return surface.normal.z > 0.0 ? to.xyz : float3(-to.x, to.y, to.z);
    }
    return to.w != 0.0 ? to.xyz : float3(0.0, 0.0, 1.0);
}

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    // Up by its fifth parameter's x times how far across its instance is.
    position += vertex.parameter(1).xyz + normal * vertex.parameter(3).x +
                float3(0.0, vertex.place.x * vertex.parameter(4).x, 0.0);
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
