// A camera's bloom: its HDR image's light past a threshold, blurred down a chain of smaller targets
// and back up, each a full-screen triangle, as Jimenez's Call of Duty's: thirteen samples down,
// which don't flicker as a bright pixel moves, and a tent up. The first level down weighs each of its
// groups by Karis's average, so one pixel far brighter than its neighbors, a firefly, counts for
// about as much as they do.

import bindless;

struct Bloom
{
    /// What's read's texel, in UVs.
    float2 texel;
    uint source;
    /// What's added to on the way up.
    uint below;
    uint sampler;
    /// Where light starts to bloom, and how far below it it fades in.
    float threshold;
    float knee;
    /// How much of what's below goes into this level, on the way up.
    float spread;
    /// Whether this is the first level, which takes what's past the threshold.
    uint first;
    uint spare;
    /// Where what's read is in the source, in UVs, from and how much of it: a camera's viewport, on
    /// the first level; and its texels' middles at either end, which nothing's read past.
    float2 from;
    float2 scale;
    float2 least;
    float2 most;
};

[[vk::push_constant]]
ConstantBuffer<Bloom> bloom;

struct Corner
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    Corner corner;
    corner.position = float4((at * 2.0) - 1.0, 0.0, 1.0);
    corner.uv = at;
    return corner;
}

/// A texel of the source as a number.
float3 sourceAt(float2 uv)
{
    return wholeLight(sampleTextureLevel(bloom.source, bloom.sampler, clamp(uv, bloom.least, bloom.most), 0.0).rgb);
}

/// How bright a color is, as the threshold goes by.
float brightness(float3 color)
{
    return max(color.r, max(color.g, color.b));
}

/// What of a color's past the threshold: all of what's past it, fading in over the knee below it.
float3 pastThreshold(float3 color)
{
    const float bright = brightness(color);
    const float soft = clamp(bright - bloom.threshold + bloom.knee, 0.0, 2.0 * bloom.knee);
    const float curve = soft * soft / ((4.0 * bloom.knee) + 0.0001);
    return color * (max(curve, bright - bloom.threshold) / max(bright, 0.0001));
}

/// Karis's average of four samples: each weighs less the brighter it is.
float3 groupOf(float3 a, float3 b, float3 c, float3 d)
{
    const float3 first[4] = { pastThreshold(a), pastThreshold(b), pastThreshold(c), pastThreshold(d) };
    float3 sum = float3(0.0);
    float weights = 0.0;
    for (uint at = 0; at < 4; ++at)
    {
        const float weight = 1.0 / (1.0 + brightness(first[at]));
        sum += first[at] * weight;
        weights += weight;
    }
    return sum / weights;
}

/// Half the size of the source: five overlapping groups of four, the middle's worth half of it all.
[shader("fragment")]
float4 downMain(Corner corner) : SV_Target
{
    const float2 uv = bloom.from + (corner.uv * bloom.scale);
    const float2 t = bloom.texel;
    const float3 a = sourceAt(uv + float2(-2.0, -2.0) * t);
    const float3 b = sourceAt(uv + float2(0.0, -2.0) * t);
    const float3 c = sourceAt(uv + float2(2.0, -2.0) * t);
    const float3 d = sourceAt(uv + float2(-2.0, 0.0) * t);
    const float3 e = sourceAt(uv);
    const float3 f = sourceAt(uv + float2(2.0, 0.0) * t);
    const float3 g = sourceAt(uv + float2(-2.0, 2.0) * t);
    const float3 h = sourceAt(uv + float2(0.0, 2.0) * t);
    const float3 i = sourceAt(uv + float2(2.0, 2.0) * t);
    const float3 j = sourceAt(uv + float2(-1.0, -1.0) * t);
    const float3 k = sourceAt(uv + float2(1.0, -1.0) * t);
    const float3 l = sourceAt(uv + float2(-1.0, 1.0) * t);
    const float3 m = sourceAt(uv + float2(1.0, 1.0) * t);
    if (bloom.first != 0)
    {
        const float3 kept = (groupOf(j, k, l, m) * 0.5) + ((groupOf(a, b, d, e) + groupOf(b, c, e, f) +
                                                            groupOf(d, e, g, h) + groupOf(e, f, h, i)) * 0.125);
        return float4(kept, 1.0);
    }
    const float3 kept = ((j + k + l + m) * 0.125) +
                        ((a + c + g + i) * 0.03125) + ((b + d + f + h) * 0.0625) + (e * 0.125);
    return float4(kept, 1.0);
}

/// A level on the way up: what the way down left here, with the tent over what's below spread in.
[shader("fragment")]
float4 upMain(Corner corner) : SV_Target
{
    const float2 uv = corner.uv;
    const float2 t = bloom.texel;
    const float3 here = sourceAt(uv);
    float3 tent = sampleTextureLevel(bloom.below, bloom.sampler, uv, 0.0).rgb * 4.0;
    for (int x = -1; x <= 1; x += 2)
    {
        for (int y = -1; y <= 1; y += 2)
        {
            tent += sampleTextureLevel(bloom.below, bloom.sampler, uv + (float2(x, y) * t), 0.0).rgb;
            tent += sampleTextureLevel(bloom.below, bloom.sampler, uv + (float2(x, 0) * t), 0.0).rgb;
            tent += sampleTextureLevel(bloom.below, bloom.sampler, uv + (float2(0, y) * t), 0.0).rgb;
        }
    }
    return float4(lerp(here, tent / 16.0, bloom.spread), 1.0);
}
