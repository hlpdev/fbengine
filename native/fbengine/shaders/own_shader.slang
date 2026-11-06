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
// which takes each vertex of what's drawn but particles, morphed and skinned, in its own space, and
// of its shadow and its prepass, in meters and radians, never °. The view culls by the Geometry's
// bounds, not where this moves it, so a Geometry moved further than they reach wants a SetCullBox; a
// Terrain has none, and is culled where its heights have it, so moving one stays near them; and an
// Instances is culled by where each of its instances is, moved in its own space before its matrix,
// so the same.
//
// Whatever `shade` returns goes on as the scene's own color does: its alpha to the material's cutoff,
// then fog and blend. Colors are in linear light, whatever the material's shading and whichever way
// its camera draws: one in stored values stores what they encode to. A material's blend
// doesn't look at what `shade` returns: BLEND AUTO goes by the material's color and textures, so a
// shader whose alpha is to blend sets BLEND ALPHA on it. A shadow's cut out by the material's own
// alpha, as `shade` can light what it draws, and a shadow's pass has no lights. A camera's prepass,
// for ambient occlusion and temporal antialiasing, runs `shade` too, where the material has a cutoff,
// so what's cut there by what it returns has none of either; a `discard` of its own isn't seen there,
// and in that pass what it's given has no ambient occlusion in it.
//
// It reads only what's here. A texture of its own, or a global that isn't `static`, is something the
// scene's pipelines don't bind, which CookShader refuses: parameters and a material's layers are how
// a game gives it things.

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

    // A parameter Material.SetParameter set, the last for an index past them.
    float4 parameter(uint index)
    {
        return push.camera.materials[material].parameters[min(index, 7)];
    }

    // Layer `index`'s texture, of those the material has, at a UV, as a layer's own UVs don't move
    // it; white for one it hasn't.
    float4 texture(uint index, float2 uv)
    {
        if (index >= layerCount)
        {
            return float4(1.0, 1.0, 1.0, 1.0);
        }
        const Layer layer = push.camera.materials[material].layers[index];
        return sampleTextureBiased(layer.texture, layer.sampler, uv, push.camera.lodBias);
    }

    // Where it is in the world.
    float3 world()
    {
        const float4 rows[3] = push.camera.worldRows;
        return float3(dot(rows[0].xyz, place) + rows[0].w, dot(rows[1].xyz, place) + rows[1].w,
                      dot(rows[2].xyz, place) + rows[2].w);
    }

    // Its normal, in the world.
    float3 worldNormal()
    {
        const float4 rows[3] = push.camera.worldRows;
        return float3(dot(rows[0].xyz, normal), dot(rows[1].xyz, normal), dot(rows[2].xyz, normal));
    }

    // The light that reaches here on a normal, per pixel and in linear light, whatever the material's
    // shading.
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

    // A parameter Material.SetParameter set, the last for an index past them.
    float4 parameter(uint index)
    {
        return push.camera.materials[material].parameters[min(index, 7)];
    }
};
