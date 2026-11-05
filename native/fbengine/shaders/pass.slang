// A game's full-screen pass, as Camera.AddPass runs one, and CookShader compiles it, when its file
// says `#define FBE_PASS` first. It writes
//
//     float4 pass(float2 uv, float4 color, float depth)
//     {
//         return float4(color.a - color.rgb, color.a);
//     }
//
// which gives each pixel of the camera's viewport, from `uv`, 0 to 1 across it from its top left, the
// `color` that's drawn there so far, in linear light after the tonemap and the grade, and the `depth`
// of what's drawn there, how far in front of the camera along its view, in meters, or its far for
// nothing. What it returns goes to the next pass, or the camera's target, as linear light.
//
// Both colors are premultiplied by their alpha, as a camera that doesn't clear has less than 1 where
// it draws nothing, and the last pass goes over what's under it so: its inverse is `color.a -
// color.rgb`, which stays 0 where nothing is, where `1 - color.rgb` would add white.
//
// It reads only what's here. A texture of its own, or a global that isn't `static`, is something the
// pipelines don't bind, which CookShader refuses: parameters and its Material's layers are how a game
// gives it things.

import bindless;

// A texture a pass's material has as a layer, and how it's sampled.
struct PassLayer
{
    uint texture;
    uint sampler;
};

// What a pass is run with: the texture's size in pixels; the samplers it reads what's drawn through;
// the viewport, in pixels; what takes a stored depth to one in meters; its material's layers, each at
// its own index, and a bit for each there is; and its parameters.
struct PassData
{
    float2 size;
    uint nearest;
    uint linear;
    float4 viewport;
    float depthOffset;
    float depthScale;
    uint perspective;
    uint layersThere;
    PassLayer layers[8];
    float4 parameters[8];
};

struct PassPush
{
    PassData* data;
    uint color;
    uint depth;
};

[[vk::push_constant]]
ConstantBuffer<PassPush> passPush;

// A parameter Material.SetParameter set on the pass's material, the last for an index past them.
float4 parameter(uint index)
{
    return passPush.data.parameters[min(index, 7)];
}

// Layer `index` of the pass's material's textures at a UV, as a layer's own UVs don't move it; white
// for one it hasn't.
float4 layer(uint index, float2 uv)
{
    if (index >= 8 || (passPush.data.layersThere & (1u << index)) == 0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    const PassLayer found = passPush.data.layers[index];
    return sampleTextureLevel(found.texture, found.sampler, uv, 0.0);
}

// Where a UV across the viewport is in what's drawn, 0 to 1 across all of it.
float2 drawnAt(float2 uv)
{
    PassData* data = passPush.data;
    return (data.viewport.xy + (uv * data.viewport.zw)) / data.size;
}

// What's drawn so far at a UV across the viewport, between its pixels.
float4 colorAt(float2 uv)
{
    return sampleTextureLevel(passPush.color, passPush.data.linear, drawnAt(uv), 0.0);
}

// How far in front of the camera what's drawn at a UV across the viewport is, in meters.
float depthAt(float2 uv)
{
    PassData* data = passPush.data;
    const float stored = sampleTextureLevel(passPush.depth, data.nearest, drawnAt(uv), 0.0).r;
    return data.perspective != 0 ? data.depthScale / (stored + data.depthOffset)
                                 : (data.depthScale - stored) / data.depthOffset;
}

// A pixel's size across the viewport, in UV, for what reads those beside it.
float2 pixelSize()
{
    return 1.0 / passPush.data.viewport.zw;
}

#include "fbe_game_pass.slang"

[shader("vertex")]
float4 passVertex(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 passFragment(float4 position : SV_Position) : SV_Target
{
    PassData* data = passPush.data;
    const float2 uv = (position.xy - data.viewport.xy) / data.viewport.zw;
    const float2 pixel = position.xy / data.size;
    return pass(uv, sampleTextureLevel(passPush.color, data.nearest, pixel, 0.0), depthAt(uv));
}
