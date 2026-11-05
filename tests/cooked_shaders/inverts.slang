#define FBE_PASS

// What's drawn so far inverted, premultiplied.
float4 pass(float2 uv, float4 color, float depth)
{
    return float4(color.a - color.rgb, color.a);
}
