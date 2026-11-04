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
    /// The viewport bloom came of, in pixels: from, and how much.
    float4 bloomRect;
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

/// A color through the color decision list: slope, offset, power, then saturation, on what's after
/// the tonemap. What overflows its power is as bright as there is, not a NaN through its luma.
float3 graded(float3 color)
{
    color = wholeLight(pow(max((color * tonemap.slope.rgb) + tonemap.offset.rgb, float3(0.0)), tonemap.power.rgb));
    const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    return max(lerp(float3(luma), color, tonemap.slope.a), float3(0.0));
}

/// Light through the exposure and the tonemap, and graded.
float3 throughGrade(float3 light)
{
    float3 color = wholeLight(wholeLight(light) * tonemap.exposure);
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
    return graded(color);
}

/// What comes of light that's all a pixel has.
float3 whole(float3 light)
{
    return wholeLight(pow(throughGrade(light), 1.0 / tonemap.gamma));
}

/// What comes of light added over what's under it, which light of 0 leaves as it is, whatever a
/// grade's offset does to what's there.
float3 added(float3 light)
{
    const float3 lifted = max(throughGrade(light) - graded(float3(0.0)), float3(0.0));
    return wholeLight(pow(lifted, 1.0 / tonemap.gamma));
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    const float2 uv = position.xy / tonemap.size;
    const float4 texel = sampleTextureLevel(tonemap.image, tonemap.sampler, uv, 0.0);
    const float alpha = isNotANumber(texel.a) ? 0.0 : saturate(texel.a);
    const float3 light = wholeLight(texel.rgb);
    float3 glow = float3(0.0);
    if (tonemap.bloomIntensity > 0.0)
    {
        const float2 across = (position.xy - tonemap.bloomRect.xy) / tonemap.bloomRect.zw;
        glow = sampleTextureLevel(tonemap.bloom, tonemap.bloomSampler, across, 0.0).rgb * tonemap.bloomIntensity;
    }
    // None of a surface, it's light added over what's under it, as what adds is.
    if (alpha <= 0.0)
    {
        return float4(added(light + glow), 0.0);
    }
    if (alpha >= 1.0)
    {
        return float4(whole(light + glow), 1.0);
    }
    // What's blended over nothing is its color times its alpha, and a curve's of its color, so it's
    // what's under the alpha that goes through it; and bloom's a layer of its own over it all, that's
    // the same on the surface as off it.
    return float4((whole((light / alpha) + glow) * alpha) + (added(glow) * (1.0 - alpha)), alpha);
}
