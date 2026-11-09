// What's asked of it is its first parameter: its x picks what it does.
float4 pass(float2 uv, float4 color, float depth)
{
    const float which = parameter(0).x;
    if (which == 1.0)
    {
        // Halved, to say which came first with an inverting one.
        return float4(color.rgb * 0.5, color.a);
    }
    if (which == 2.0)
    {
        // Its depth a tenth of a meter to one, in linear light.
        return float4(depth * 0.1, 0.0, 0.0, 1.0);
    }
    if (which == 3.0)
    {
        return parameter(1);
    }
    if (which == 4.0)
    {
        return layer(0, uv);
    }
    if (which == 5.0)
    {
        // Its UV.
        return float4(uv, 0.0, 1.0);
    }
    if (which == 6.0)
    {
        return colorAt(uv + float2(pixelSize().x * 8.0, 0.0));
    }
    if (which == 7.0)
    {
        // A hard edge down its middle, whatever's drawn, for what softens one after it.
        return uv.x < 0.5 ? float4(0.0, 0.0, 0.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
    }
    if (which == 8.0)
    {
        // Half a pixel over: between it and the next.
        return colorAt(uv + float2(pixelSize().x * 0.5, 0.0));
    }
    if (which == 9.0)
    {
        return parameter(8);
    }
    if (which == 10.0)
    {
        return layer(8, uv);
    }
    if (which == 11.0)
    {
        return layer(1, uv);
    }
    // Inverted, premultiplied.
    return float4(color.a - color.rgb, color.a);
}
