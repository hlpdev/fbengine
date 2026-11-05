// A camera's ambient occlusion, from what its prepass left, by a triangle over its target: how much
// of the sky of a pixel's surface the surfaces around it leave open, 1 for all of it, then blurred
// across what's near in depth. After McGuire's obscurance: a spiral of samples of the depth around
// it, each taking off the cosine of how far it stands above the surface, less as it's further off,
// to none at its radius, so a flat surface is exactly open. Normals come from depth, which is all
// the prepass has, so a normal map's bumps have none.

import bindless;

static const uint sampleCount = 16;
/// How many times around a pixel its samples spiral, prime to their count so none line up.
static const float spiralTurns = 7.0;
static const int blurReach = 4;
/// The most of a view's height its samples reach, so those near a pixel aren't all spread past it.
static const float mostReach = 0.1;

struct Occlusion
{
    /// The view's pixels: where it starts and how big; and those a target has of it, to and from.
    float4 viewport;
    int2 least;
    int2 most;
    /// What a place's turned into its clip space by across and up, and the view's jitter.
    float2 scale;
    float2 jitter;
    float radius;
    uint perspective;
    uint frame;
    uint depth;
    uint source;
    uint spare;
    int2 way;
};

[[vk::push_constant]]
ConstantBuffer<Occlusion> occlusion;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

/// What the prepass left of a pixel's depth, 0 for nothing: render/prepass.cpp's red and green.
float depthAt(int2 at)
{
    const float4 texel = loadTexel(occlusion.depth, clamp(at, occlusion.least, occlusion.most));
    return texel.r * (1.0 + (texel.g / 1024.0));
}

/// A pixel's place in its view, at a depth.
float3 placeAt(int2 at, float depth)
{
    const float2 pixel = float2(at) + float2(0.5, 0.5) - occlusion.viewport.xy;
    const float2 part = pixel / occlusion.viewport.zw;
    const float2 clip = (part * 2.0) - float2(1.0, 1.0) - occlusion.jitter;
    const float2 across = clip / occlusion.scale;
    return float3(occlusion.perspective != 0 ? across * depth : across, -depth);
}

/// The way along a surface from a place to whichever neighbor along `way`, on each side, is nearest
/// in depth, so a surface's edge doesn't lean it toward what's behind.
float3 alongSurface(int2 at, int2 way, float3 place, float depth)
{
    // Off the view, there's no neighbor to go by.
    const int2 ahead = clamp(at + way, occlusion.least, occlusion.most);
    const int2 behind = clamp(at - way, occlusion.least, occlusion.most);
    const float forward = all(ahead == at) ? 0.0 : depthAt(ahead);
    const float backward = all(behind == at) ? 0.0 : depthAt(behind);
    if (forward > 0.0 && (backward <= 0.0 || abs(forward - depth) < abs(backward - depth)))
    {
        return placeAt(ahead, forward) - place;
    }
    return backward > 0.0 ? place - placeAt(behind, backward) : float3(0.0, 0.0, 0.0);
}

[shader("fragment")]
float occlusionMain(float4 position : SV_Position) : SV_Target
{
    const int2 at = int2(position.xy);
    const float depth = depthAt(at);
    if (depth <= 0.0)
    {
        return 1.0;
    }
    const float3 place = placeAt(at, depth);
    const float3 across = cross(alongSurface(at, int2(1, 0), place, depth),
                                alongSurface(at, int2(0, 1), place, depth));
    const float squared = dot(across, across);
    if (squared <= 0.0)
    {
        return 1.0;
    }
    float3 normal = across / sqrt(squared);
    const float3 toEye = occlusion.perspective != 0 ? -place : float3(0.0, 0.0, 1.0);
    if (dot(normal, toEye) < 0.0)
    {
        normal = -normal;
    }
    // How many pixels its radius is at this depth.
    const float radius = occlusion.radius;
    const float pixels = abs(occlusion.scale.y) * occlusion.viewport.w * 0.5 * radius;
    const float reach = min(occlusion.perspective != 0 ? pixels / depth : pixels,
                            occlusion.viewport.w * mostReach);
    if (reach < 1.0)
    {
        return 1.0;
    }
    // Interleaved gradient noise, which blurs out, and which each frame's jitter moves on.
    const float2 seed = position.xy + (5.588238 * float(occlusion.frame % 64));
    const float noise = frac(52.9829189 * frac(dot(seed, float2(0.06711056, 0.00583715))));
    // Past what rounding leaves of a place, as far off as it is.
    const float bias = (radius * 0.01) + (depth * 0.00001);
    const float squaredRadius = radius * radius;
    float sum = 0.0;
    for (uint i = 0; i < sampleCount; ++i)
    {
        const float part = (float(i) + 0.5) / float(sampleCount);
        const float angle = ((part * spiralTurns) + noise) * 6.28318530718;
        const int2 other = clamp(at + int2(float2(cos(angle), sin(angle)) * part * reach),
                                 occlusion.least, occlusion.most);
        const float otherDepth = depthAt(other);
        if (otherDepth <= 0.0)
        {
            continue;
        }
        const float3 to = placeAt(other, otherDepth) - place;
        const float apart = dot(to, to);
        const float above = max((dot(to, normal) - bias) / sqrt(max(apart, 1e-8)), 0.0);
        sum += above * max(1.0 - (apart / squaredRadius), 0.0);
    }
    return saturate(1.0 - (sum * 2.0 / float(sampleCount)));
}

[shader("fragment")]
float blurMain(float4 position : SV_Position) : SV_Target
{
    const int2 at = int2(position.xy);
    const float depth = depthAt(at);
    const float own = loadTexel(occlusion.source, at).r;
    if (depth <= 0.0)
    {
        return own;
    }
    float sum = own;
    float weights = 1.0;
    for (int i = -blurReach; i <= blurReach; ++i)
    {
        if (i == 0)
        {
            continue;
        }
        const int2 other = clamp(at + (occlusion.way * i), occlusion.least, occlusion.most);
        const float apart = abs(depthAt(other) - depth) / (occlusion.radius * 0.5);
        const float weight = exp(-float(i * i) / 8.0) * max(1.0 - apart, 0.0);
        sum += loadTexel(occlusion.source, other).r * weight;
        weights += weight;
    }
    return sum / weights;
}
