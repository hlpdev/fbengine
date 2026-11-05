// A camera's fast approximate antialiasing, by a triangle over its target, after all else it does
// to its picture: where brightness changes sharply, what's drawn is moved along across the edge, by
// how much of an edge's length a pixel is from its nearer end, as FXAA's quality version has it
// (Lottes, 2009). It knows no more than the picture: no depth, no frames before.

import bindless;

/// The least contrast that's an edge, and the least as a part of the brightest around it.
static const float leastContrast = 0.0312;
static const float leastContrastPart = 0.125;
/// How far each step along an edge goes, in pixels.
static const uint stepCount = 12;
static const float steps[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

struct Fxaa
{
    float2 size;
    uint image;
    uint sampler;
};

[[vk::push_constant]]
ConstantBuffer<Fxaa> fxaa;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

// Whether its camera draws in stored values, as its image then has.
[vk::constant_id(0)] const uint storedValues = 0;

float4 colorAt(float2 uv)
{
    return sampleTextureLevel(fxaa.image, fxaa.sampler, uv, 0.0);
}

/// How bright a color looks, near enough, of its linear light or, in stored values, of those.
float brightness(float3 color)
{
    const float weighed = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0.0);
    return storedValues != 0 ? weighed : sqrt(weighed);
}

float brightnessAt(float2 uv)
{
    return brightness(colorAt(uv).rgb);
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    const float2 texel = float2(1.0, 1.0) / fxaa.size;
    const float2 uv = position.xy * texel;
    const float4 middleColor = colorAt(uv);
    const float middle = brightness(middleColor.rgb);
    const float up = brightnessAt(uv + float2(0.0, -texel.y));
    const float down = brightnessAt(uv + float2(0.0, texel.y));
    const float left = brightnessAt(uv + float2(-texel.x, 0.0));
    const float right = brightnessAt(uv + float2(texel.x, 0.0));
    const float most = max(middle, max(up, max(down, max(left, right))));
    const float least = min(middle, min(up, min(down, min(left, right))));
    const float contrast = most - least;
    if (contrast < max(leastContrast, most * leastContrastPart))
    {
        return middleColor;
    }
    const float upLeft = brightnessAt(uv + float2(-texel.x, -texel.y));
    const float upRight = brightnessAt(uv + float2(texel.x, -texel.y));
    const float downLeft = brightnessAt(uv + float2(-texel.x, texel.y));
    const float downRight = brightnessAt(uv + float2(texel.x, texel.y));
    // How much it stands out from all around it, for what's smaller than a pixel.
    const float sides = up + down + left + right;
    const float corners = upLeft + upRight + downLeft + downRight;
    const float around = ((2.0 * sides) + corners) / 12.0;
    const float apart = saturate(abs(around - middle) / contrast);
    const float eased = ((-2.0 * apart) + 3.0) * apart * apart;
    const float smallPart = eased * eased * 0.75;
    // Which way the edge goes: along x, if it changes more going up and down.
    const float changeDown = abs(upLeft + downLeft - (2.0 * left)) +
                             (2.0 * abs(up + down - (2.0 * middle))) +
                             abs(upRight + downRight - (2.0 * right));
    const float changeAcross = abs(upLeft + upRight - (2.0 * up)) +
                               (2.0 * abs(left + right - (2.0 * middle))) +
                               abs(downLeft + downRight - (2.0 * down));
    const bool alongX = changeDown >= changeAcross;
    const float before = alongX ? up : left;
    const float after = alongX ? down : right;
    const bool nearBefore = abs(before - middle) >= abs(after - middle);
    const float steepest = max(abs(before - middle), abs(after - middle));
    float across = alongX ? texel.y : texel.x;
    if (nearBefore)
    {
        across = -across;
    }
    const float2 along = alongX ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    const float2 onEdge = alongX ? uv + float2(0.0, across * 0.5) : uv + float2(across * 0.5, 0.0);
    // Along the edge each way, until it's left: where brightness is no more the edge's.
    const float edge = (middle + (nearBefore ? before : after)) * 0.5;
    const float enough = steepest * 0.25;
    float2 backward = onEdge;
    float2 forward = onEdge;
    float backwardEnd = 0.0;
    float forwardEnd = 0.0;
    bool backwardDone = false;
    bool forwardDone = false;
    for (uint i = 0; i < stepCount && !(backwardDone && forwardDone); ++i)
    {
        if (!backwardDone)
        {
            backward -= along * steps[i];
            backwardEnd = brightnessAt(backward) - edge;
            backwardDone = abs(backwardEnd) >= enough;
        }
        if (!forwardDone)
        {
            forward += along * steps[i];
            forwardEnd = brightnessAt(forward) - edge;
            forwardDone = abs(forwardEnd) >= enough;
        }
    }
    const float2 toBackward = uv - backward;
    const float2 toForward = forward - uv;
    const float backwardApart = alongX ? toBackward.x : toBackward.y;
    const float forwardApart = alongX ? toForward.x : toForward.y;
    const bool backwardNearer = backwardApart < forwardApart;
    // Moved only toward an end the edge turns away from this pixel at.
    const bool darker = middle < edge;
    const bool turns = ((backwardNearer ? backwardEnd : forwardEnd) < 0.0) != darker;
    const float length = backwardApart + forwardApart;
    const float nearEnd = min(backwardApart, forwardApart);
    const float byEnd = turns ? 0.5 - (nearEnd / length) : 0.0;
    const float moved = max(byEnd, smallPart) * across;
    const float2 taken = alongX ? uv + float2(0.0, moved) : uv + float2(moved, 0.0);
    return colorAt(taken);
}
