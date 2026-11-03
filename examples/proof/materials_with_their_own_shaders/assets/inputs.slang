// What a shader's given: which of them the material's first parameter picks.
float4 shade(Surface surface)
{
    const float which = surface.parameter(0).x;
    if (which == 0.0)
    {
        return float4(surface.uv0, 0.0, 1.0);
    }
    if (which == 1.0)
    {
        return float4(surface.normal * 0.5 + float3(0.5, 0.5, 0.5), 1.0);
    }
    if (which == 2.0)
    {
        return surface.vertexColor;
    }
    if (which == 3.0)
    {
        return surface.color * surface.texture(0, surface.uv0);
    }
    if (which == 4.0)
    {
        return surface.texture(0, surface.uv0);
    }
    if (which == 6.0)
    {
        return float4(1.0, 1.0, 1.0, 0.5);
    }
    return float4(surface.light(surface.normal, 0.0), 1.0);
}
