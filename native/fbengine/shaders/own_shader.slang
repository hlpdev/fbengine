// What a material's own shader has: scene.slang includes this, then the game's file, only when
// fbengine_tools' CookShader compiles one. A game's file writes
//
//     float4 shade(Surface surface)
//     {
//         return surface.color * surface.parameter(0);
//     }
//
// and, for vertices of its own, says `#define FBE_MOVES_VERTICES` first and writes
//
//     void move(inout float3 position, inout float3 normal, MovingVertex vertex)
//
// which takes each vertex of a mesh or terrain, morphed and skinned, in its mesh's space, for what's
// drawn and its shadow; the view culls by the Geometry's bounds, not where this moves it, so a
// Geometry moved further than they reach wants a SetCullBox. Whatever `shade` returns goes on as the
// scene's own color does: to the material's alpha cutoff, fog, and blend. Colors are in linear light,
// whatever the material's shading. A shadow's cut out by the material's own alpha, as `shade` can
// light what it draws, and a shadow's pass has no lights.

// A pixel of what a material draws, as a shader's given it.
struct Surface
{
    // In the camera's view, x right, y up, looking down -z.
    float3 place;
    // Length 1, or 0 for what has none, facing the camera for a two-sided material's back.
    float3 normal;
    float2 uv0;
    float2 uv1;
    // The vertex's color, whether the material takes vertex colors or not.
    float4 vertexColor;
    // What the material would draw here without a shader of its own: its colors and layers, lit.
    float4 color;
    // The pixel, in the target's.
    float2 pixel;
    // Which of the camera's materials and draws it is, for what's below.
    uint material;

    // A parameter Material.SetParameter set.
    float4 parameter(uint index)
    {
        return push.camera.materials[material].parameters[index];
    }

    // Layer `index`'s texture, of those the material has, at a UV, as a layer's own UVs don't move
    // it; white for one it hasn't.
    float4 texture(uint index, float2 uv)
    {
        const Layer layer = push.camera.materials[material].layers[index];
        return index < layerCount ? sampleTexture(layer.texture, layer.sampler, uv)
                                  : float4(1.0, 1.0, 1.0, 1.0);
    }

    // The light that reaches here on a normal, as the scene's own lights it, in linear light.
    float3 light(float3 toward, float shininess)
    {
        return pixelLit(push.camera, pixel, place, toward, shininess);
    }
};

// A vertex a shader's moving, as it was made.
struct MovingVertex
{
    float2 uv0;
    float2 uv1;
    float4 color;
    uint material;

    float4 parameter(uint index)
    {
        return push.camera.materials[material].parameters[index];
    }
};
