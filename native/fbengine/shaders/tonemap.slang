// What a camera's HDR image comes to in its target: its light through its exposure, a tonemap, and
// its gamma, by a triangle over its viewport, which the scissor is. A target stored as sRGB encodes
// what it's given, so all that's here is linear.

import bindless;

static const uint tonemapReinhard = 1;
static const uint tonemapAces = 2;
static const uint tonemapNeutral = 3;
/// The brightest a half float holds, what its infinity's taken as.
static const float brightest = 65504.0;

struct Tonemap
{
    float2 size;
    uint image;
    uint sampler;
    float exposure;
    float gamma;
    uint kind;
};

[[vk::push_constant]]
ConstantBuffer<Tonemap> tonemap;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

/// What a tonemap's given, as a number: no NaN, which is no light, nor anything past what's held.
float3 wholeLight(float3 light)
{
    return float3(isnan(light.r) ? 0.0 : clamp(light.r, 0.0, brightest),
                  isnan(light.g) ? 0.0 : clamp(light.g, 0.0, brightest),
                  isnan(light.b) ? 0.0 : clamp(light.b, 0.0, brightest));
}

/// Narkowicz's fit of ACES's filmic curve, as he gives it, with its exposure of 0.6.
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
    const float4 texel = sampleTextureLevel(tonemap.image, tonemap.sampler, position.xy / tonemap.size, 0.0);
    float3 color = wholeLight(texel.rgb) * tonemap.exposure;
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
    return float4(pow(max(color, float3(0.0)), 1.0 / tonemap.gamma), isnan(texel.a) ? 0.0 : texel.a);
}
