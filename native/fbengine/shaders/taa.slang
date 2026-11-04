// A camera's temporal antialiasing: what it's drawn now, each frame's a part of a pixel off the
// last, over what it had by the frames before, moved to where that is now and held to what's around
// it now, so what's gone from a pixel doesn't linger, by a triangle over its image.

import bindless;

struct Temporal
{
    float2 size;
    uint image;
    uint prepass;
    uint history;
    uint sampler;
    /// How much of what's now goes in; 1 takes nothing of what's been.
    float weight;
    /// Whether turning moves what's at no depth, as it does in a perspective view.
    uint perspective;
    /// The view's pixels: where it starts and how big; and those a target has of it, to and from.
    float4 viewport;
    int2 least;
    int2 most;
    /// The rows of what turns this view's directions to the last's; its projection across and up.
    float4 turn[3];
    float2 scale;
};

[[vk::push_constant]]
ConstantBuffer<Temporal> temporal;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

float luma(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

/// A color as YCoCg, whose box around what's near hugs it closer than red, green, and blue's.
float3 toYCoCg(float3 color)
{
    return float3(dot(color, float3(0.25, 0.5, 0.25)), dot(color, float3(0.5, 0.0, -0.5)),
                  dot(color, float3(-0.25, 0.5, -0.25)));
}

float3 fromYCoCg(float3 color)
{
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

/// What's in a pixel of what's drawn now, as numbers it can keep.
float4 nowAt(int2 at)
{
    const float4 texel = loadTexel(temporal.image, clamp(at, temporal.least, temporal.most));
    return float4(wholeLight(texel.rgb), isNotANumber(texel.a) ? 0.0 : saturate(texel.a));
}

/// What's been at a place, by Catmull-Rom's curve in five samples (Jimenez, 2016), which keeps it
/// from going soft as it's moved over and over.
float4 historyAt(float2 uv)
{
    const float2 place = uv * temporal.size;
    const float2 center = floor(place - float2(0.5, 0.5)) + float2(0.5, 0.5);
    const float2 part = place - center;
    const float2 squared = part * part;
    const float2 cubed = squared * part;
    const float2 first = squared - (0.5 * (cubed + part));
    const float2 second = (1.5 * cubed) - (2.5 * squared) + float2(1.0, 1.0);
    const float2 fourth = 0.5 * (cubed - squared);
    const float2 third = float2(1.0, 1.0) - first - second - fourth;
    const float2 middle = second + third;
    const float2 before = (center - float2(1.0, 1.0)) / temporal.size;
    const float2 between = (center + (third / middle)) / temporal.size;
    const float2 after = (center + float2(2.0, 2.0)) / temporal.size;
    const uint image = temporal.history;
    const uint sampler = temporal.sampler;
    const float4 sum =
        (sampleTextureLevel(image, sampler, float2(between.x, before.y), 0.0) * middle.x *
         first.y) +
        (sampleTextureLevel(image, sampler, float2(before.x, between.y), 0.0) * first.x *
         middle.y) +
        (sampleTextureLevel(image, sampler, between, 0.0) * middle.x * middle.y) +
        (sampleTextureLevel(image, sampler, float2(after.x, between.y), 0.0) * fourth.x *
         middle.y) +
        (sampleTextureLevel(image, sampler, float2(between.x, after.y), 0.0) * middle.x *
         fourth.y);
    return sum / ((middle.x * first.y) + (first.x * middle.y) + (middle.x * middle.y) +
                  (fourth.x * middle.y) + (middle.x * fourth.y));
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    const int2 at = int2(position.xy);
    const float4 now = nowAt(at);
    if (temporal.weight >= 1.0 || any(at < temporal.least) || any(at > temporal.most))
    {
        return now;
    }
    // What's around it now, and the nearest of it, whose motion is an edge's, on either side of it.
    float3 lowest = toYCoCg(now.rgb);
    float3 highest = lowest;
    float lowestAlpha = now.a;
    float highestAlpha = now.a;
    float nearest = 0.0;
    float2 moved = float2(0.0, 0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 other = clamp(at + int2(x, y), temporal.least, temporal.most);
            const float4 around = nowAt(other);
            const float3 color = toYCoCg(around.rgb);
            lowest = min(lowest, color);
            highest = max(highest, color);
            lowestAlpha = min(lowestAlpha, around.a);
            highestAlpha = max(highestAlpha, around.a);
            const float4 texel = loadTexel(temporal.prepass, other);
            if (texel.r > 0.0 && (nearest <= 0.0 || texel.r < nearest))
            {
                nearest = texel.r;
                moved = texel.ba;
            }
        }
    }
    const float2 part = (position.xy - temporal.viewport.xy) / temporal.viewport.zw;
    float2 then = part - moved;
    if (nearest <= 0.0 && temporal.perspective != 0)
    {
        // What's at no depth is as far as can be, so only turning moves it.
        const float2 clip = (part * 2.0) - float2(1.0, 1.0);
        const float3 way = float3(clip / temporal.scale, -1.0);
        const float3 turned = float3(dot(temporal.turn[0].xyz, way), dot(temporal.turn[1].xyz, way),
                                     dot(temporal.turn[2].xyz, way));
        if (turned.z >= 0.0)
        {
            return now;
        }
        then = (((turned.xy / -turned.z) * temporal.scale) + float2(1.0, 1.0)) * 0.5;
    }
    if (any(then < float2(0.0, 0.0)) || any(then > float2(1.0, 1.0)))
    {
        return now;
    }
    const float2 uv = ((then * temporal.viewport.zw) + temporal.viewport.xy) / temporal.size;
    const float4 before = historyAt(uv);
    // What's kept is only ever what's come of numbers.
    const float3 clamped = clamp(toYCoCg(before.rgb), lowest, highest);
    const float3 held = max(fromYCoCg(clamped), float3(0.0, 0.0, 0.0));
    const float heldAlpha = clamp(before.a, lowestAlpha, highestAlpha);
    // Each by how bright it isn't, so a pixel of a bright thing flickering by counts for little.
    const float nowPart = temporal.weight / (1.0 + luma(now.rgb));
    const float heldPart = (1.0 - temporal.weight) / (1.0 + luma(held));
    const float whole = nowPart + heldPart;
    return float4(((now.rgb * nowPart) + (held * heldPart)) / whole,
                  ((now.a * nowPart) + (heldAlpha * heldPart)) / whole);
}
