// What a camera's HDR image comes to in its target: its light, with its bloom, through its exposure,
// a tonemap, its color grade, and its gamma, by a triangle over its viewport, which the scissor is. A
// target stored as sRGB encodes what it's given, so all that's here is linear.

import bindless;

static const uint tonemapReinhard = 1;
static const uint tonemapAces = 2;
static const uint tonemapNeutral = 3;
struct Tonemap
{
    float2 size;
    uint image;
    uint sampler;
    float exposure;
    float gamma;
    uint kind;
    uint bloom;
    uint bloomSampler;
    float bloomIntensity;
    float2 spare;
    /// The American Society of Cinematographers' color decision list: slope, with saturation in its
    /// w, offset, and power.
    float4 slope;
    float4 offset;
    float4 power;
};

[[vk::push_constant]]
ConstantBuffer<Tonemap> tonemap;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

/// Narkowicz's fit of ACES's filmic curve, after the exposure of 0.6 he says matches ACES's own, which
/// his fit doesn't have: light of 1 is 0.673.
float3 aces(float3 light)
{
    const float3 x = light * 0.6;
    return saturate((x * ((x * 2.51) + 0.03)) / ((x * ((x * 2.43) + 0.59)) + 0.14));
}

/// Khronos's PBR Neutral, which leaves a base color under its compression as it was.
float3 neutral(float3 light)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float least = min(light.r, min(light.g, light.b));
    const float offset = least < 0.08 ? least - (6.25 * least * least) : 0.04;
    float3 color = light - offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }
    const float left = 1.0 - startCompression;
    const float newPeak = 1.0 - (left * left / (peak + left - startCompression));
    color *= newPeak / peak;
    const float toWhite = 1.0 - (1.0 / ((desaturation * (peak - newPeak)) + 1.0));
    return lerp(color, float3(newPeak), toWhite);
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    const float2 uv = position.xy / tonemap.size;
    const float4 texel = sampleTextureLevel(tonemap.image, tonemap.sampler, uv, 0.0);
    const float alpha = isNotANumber(texel.a) ? 0.0 : saturate(texel.a);
    float3 light = wholeLight(texel.rgb);
    if (tonemap.bloomIntensity > 0.0)
    {
        light += sampleTextureLevel(tonemap.bloom, tonemap.bloomSampler, uv, 0.0).rgb * tonemap.bloomIntensity;
    }
    // What's blended over nothing is its color times its alpha, and a curve's of its color: so it's
    // what's under the alpha that goes through it, unless there's no alpha at all, as what adds has.
    const bool partly = alpha > 0.0 && alpha < 1.0;
    float3 color = wholeLight(wholeLight(partly ? light / alpha : light) * tonemap.exposure);
    if (tonemap.kind == tonemapReinhard)
    {
        color = color / (color + 1.0);
    }
    else if (tonemap.kind == tonemapAces)
    {
        color = aces(color);
    }
    else if (tonemap.kind == tonemapNeutral)
    {
        color = neutral(color);
    }
    color = pow(max((color * tonemap.slope.rgb) + tonemap.offset.rgb, float3(0.0)), tonemap.power.rgb);
    const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = max(lerp(float3(luma), color, tonemap.slope.a), float3(0.0));
    color = wholeLight(pow(color, 1.0 / tonemap.gamma));
    return float4(partly ? color * alpha : color, alpha);
}
