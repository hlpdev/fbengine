// A color of a material's own, whatever else it is.
float4 shade(Surface surface)
{
    return float4(surface.parameter(0).rgb, 1.0);
}
