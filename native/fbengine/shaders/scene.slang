// The scene's one pipeline family: every material's look, from the specialization constants its
// draw's pipeline key gives (fbe_render.h has the key, render/draw.cpp makes the constants), so any
// mix of them draws in one pass. Everything's read through addresses: the push constants give the
// camera's, which gives its draws', materials', and lights'; each draw, by its instance, gives its
// vertices'. Lighting is in the camera's view, x right, y up, looking down -z.
//
// PBR works in linear light, its color and layers its base color, shaded by pbr.slang. UNLIT and
// VERTEX are the fixed-function look, which worked on what was
// stored: their colors, textures, adds, doublings, and fog are all in encoded values, sRGB's, each
// stage clamped to 0 to 1, as its stages were, and only the color they end with is made linear for
// the target. A room's 50 doubled is 100, as it was, not linear light's 71.

import bindless;
import pbr;

// A pipeline's constants, render/draw.cpp's desc_of's order: its FBE_SHADING_; its FBE_BLEND_;
// whether it has an alpha cutoff, takes vertex colors, and is two-sided; its FBE_FOG_; how many
// layers it has, each layer's FBE_LAYER_ in four bits from the lowest, and each layer's UV set in
// a bit.
[vk::constant_id(0)] const uint shading = 0;
[vk::constant_id(1)] const uint blend = 1;
[vk::constant_id(2)] const uint alphaTest = 0;
[vk::constant_id(3)] const uint vertexColors = 0;
[vk::constant_id(4)] const uint twoSided = 0;
[vk::constant_id(5)] const uint fog = 0;
[vk::constant_id(6)] const uint layerCount = 0;
[vk::constant_id(7)] const uint layerBlends = 0;
[vk::constant_id(8)] const uint layerSets = 0;

static const uint shadingUnlit = 1;
static const uint shadingVertex = 2;
static const uint blendAdd = 3;
static const uint blendMultiply = 4;
static const uint blendOpaque = 1;
static const uint fogLinear = 1;
static const uint fogExponential = 2;
static const uint layerMultiply = 0;
static const uint layerAdd = 1;
static const uint layerAlpha = 2;
static const uint layerMultiply2 = 3;
static const uint layerBump = 4;
static const uint mapMetalRough = 0;
static const uint mapNormal = 1;
static const uint mapOcclusion = 2;
static const uint mapEmissive = 3;
static const uint lightDirectional = 0;
static const uint lightSpot = 2;
static const uint clustersAcross = 16;
static const uint clustersUp = 9;
static const uint clustersDeep = 24;
// How tight a highlight is, as fixed-function hardware's was.
static const float highlightPower = 100.0;
// A vertex's joint that's no bone, which leaves it as it is.
static const uint noBone = 0xFFFF;

// fbe_vertex_t, its floats loose, so nothing's aligned past them.
struct Vertex
{
    float position[3];
    float normal[3];
    float uv0[2];
    float uv1[2];
    float color[4];
};

// A vertex's joints: which of the palette's matrices, in two bytes each, and how much of each.
struct Joints
{
    uint joints[2];
    float weights[4];
};

// A vertex's tangent, its floats loose.
struct Tangent
{
    float xyzw[4];
};

// A draw: its matrix to the camera's view, what turns its normals there, its color and alpha, its
// vertices, its joints and palette if it's skinned, its tangents if it has them, its material's
// index, its own lights, and how many matrices its palette has, 0 if it isn't skinned.
struct Draw
{
    float4x4 toView;
    float4 normals[3];
    float4 color;
    Vertex* vertices;
    Joints* joints;
    float4x4* palette;
    Tangent* tangents;
    uint material;
    uint lightFirst;
    uint lightCount;
    uint jointCount;
    uint hasTangents;
    uint spare;
};

// A layer: its texture and sampler in the bindless set; how a bump layer scales the next layer's
// light; what turns its UVs, with, in the first row's w, whether sampling its texture decodes sRGB,
// and in the second's, whether it can't be sampled, so it leaves what's under it as it is; and what
// a bump layer's map shifts the next layer's UVs by.
struct Layer
{
    uint texture;
    uint sampler;
    float luminanceScale;
    float luminanceOffset;
    float4 uvAcross;
    float4 uvDown;
    float4 bump;
};

struct Material
{
    float4 color;
    float shininess;
    float cutoff;
    // Whether its own and its textures' alpha are left out, as FBE_BLEND_OPAQUE's are.
    uint noAlpha;
    uint spare;
    Layer layers[8];
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    float3 emissive;
    // Which of its maps it has, a bit for each, whose UV sets are in their bumps' x.
    uint maps;
    Layer mapLayers[4];
};

// fbe_draw_light_t, its floats loose.
struct Light
{
    uint handle[2];
    int type;
    float color[3];
    float position[3];
    float direction[3];
    float range;
    float innerCosine;
    float outerCosine;
    uint spare;
};

struct Cluster
{
    uint first;
    uint count;
};

// A camera's frame: how its view goes to clip space; its viewport in the target's pixels; its fog;
// the ambient light; how a depth finds its cluster; whether its view's mirrored across, as a
// reflection's is; how many of its lights, first, are directional; and its lists.
struct Camera
{
    float4x4 projection;
    float4 viewport;
    float4 fogColor;
    float fogStart;
    float fogEnd;
    float fogDensity;
    float sliceScale;
    float3 ambient;
    float sliceBias;
    uint perspective;
    uint flipped;
    uint directional;
    uint spare;
    Draw* draws;
    Material* materials;
    Light* lights;
    Cluster* clusters;
    uint* clusterLights;
    uint* drawLights;
    Environment environment;
    uint hasEnvironment;
    uint spareToo;
    // The rows of what turns its view's directions into the world's.
    float4 toWorld[3];
};

struct Push
{
    Camera* camera;
};

[[vk::push_constant]]
ConstantBuffer<Push> push;

struct Corner
{
    float4 position : SV_Position;
    float3 place : PLACE;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR;
    float3 lit : LIT;
    // Its tangent in the camera's view, w its bitangent's way; 0 in w for none.
    float4 tangent : TANGENT;
    nointerpolation uint draw : DRAW;
};

// Whether this pipeline's the fixed-function look, in encoded values.
bool fixedLook()
{
    return shading == shadingUnlit || shading == shadingVertex;
}

// Linear light as sRGB stores it, and back.
float3 toEncoded(float3 linear)
{
    const float3 positive = max(linear, float3(0.0, 0.0, 0.0));
    const float3 low = positive * 12.92;
    const float3 high = 1.055 * pow(positive, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return float3(positive.r <= 0.0031308 ? low.r : high.r, positive.g <= 0.0031308 ? low.g : high.g,
                  positive.b <= 0.0031308 ? low.b : high.b);
}

float3 toLinear(float3 encoded)
{
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return float3(encoded.r <= 0.04045 ? low.r : high.r, encoded.g <= 0.04045 ? low.g : high.g,
                  encoded.b <= 0.04045 ? low.b : high.b);
}

// A normal made whole, or none where it has none, as a sprite's: only ambient light lights that.
float3 whole(float3 normal)
{
    const float squared = dot(normal, normal);
    return squared > 0.0 ? normal / sqrt(squared) : normal;
}

// A light at a place: the way to it, and the light that reaches it. Per pixel, as glTF's
// KHR_lights_punctual has it, which ends at its range, as clusters need; per vertex, as
// fixed-function hardware's did, whole within its range and falling as range over distance past it,
// with no end, in encoded values.
void lightAt(Light light, float3 place, out float3 way, out float3 color)
{
    float falling = 1.0;
    if (light.type == int(lightDirectional))
    {
        way = -float3(light.direction[0], light.direction[1], light.direction[2]);
    }
    else
    {
        const float3 to = float3(light.position[0], light.position[1], light.position[2]) - place;
        const float squared = dot(to, to);
        const float apart = sqrt(squared);
        way = apart > 0.0 ? to / apart : float3(0.0, 0.0, 1.0);
        if (shading == shadingVertex)
        {
            falling = min(light.range / max(apart, 0.0001), 1.0);
        }
        else
        {
            const float part = apart / light.range;
            const float window = saturate(1.0 - part * part * part * part);
            falling = window * window / max(squared, 0.0001);
        }
        if (light.type == int(lightSpot))
        {
            const float3 shining = float3(light.direction[0], light.direction[1], light.direction[2]);
            const float inCone = saturate((dot(-way, shining) - light.outerCosine) /
                                          max(light.innerCosine - light.outerCosine, 0.001));
            falling *= inCone * inCone;
        }
    }
    const float3 linear = float3(light.color[0], light.color[1], light.color[2]);
    color = (shading == shadingVertex ? toEncoded(linear) : linear) * falling;
}

// What a light adds to a fixed-function vertex: what falls on it, and what its highlight adds.
void addLight(Light light, float3 place, float3 normal, float3 eye, float shininess,
              inout float3 diffuse, inout float3 highlight)
{
    float3 way;
    float3 color;
    lightAt(light, place, way, color);
    const float facing = saturate(dot(normal, way));
    diffuse += color * facing;
    if (shininess > 0.0 && facing > 0.0)
    {
        const float3 between = way + eye;
        const float length = dot(between, between);
        if (length > 0.0)
        {
            const float aligned = saturate(dot(normal, between / sqrt(length)));
            highlight += color * shininess * pow(aligned, highlightPower);
        }
    }
}

// The direction to the eye from a place, by how the camera projects.
float3 toEye(Camera* camera, float3 place)
{
    return camera.perspective != 0 ? normalize(-place) : float3(0.0, 0.0, 1.0);
}

// -1 if a matrix's turn and scale mirror what it's given, and 1 if they don't.
float mirroring(float4x4 m)
{
    return dot(cross(m[0].xyz, m[1].xyz), m[2].xyz) < 0.0 ? -1.0 : 1.0;
}

// A normal through a skin's matrix: the cofactors of its turn and scale, so a bone scaled along one
// axis leans a normal as it should, its sign the determinant's, which whole() then makes length 1.
float3 skinnedNormal(float4x4 skin, float3 normal)
{
    const float3 a = float3(skin[0][0], skin[1][0], skin[2][0]);
    const float3 b = float3(skin[0][1], skin[1][1], skin[2][1]);
    const float3 c = float3(skin[0][2], skin[1][2], skin[2][2]);
    const float3 across = cross(b, c);
    const float3 turned = across * normal.x + cross(c, a) * normal.y + cross(a, b) * normal.z;
    return whole(dot(a, across) < 0.0 ? -turned : turned);
}

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    Camera* camera = push.camera;
    const Draw draw = camera.draws[instance];
    const Vertex vertex = draw.vertices[index];
    float4 position = float4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0);
    float3 normal = float3(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
    const bool tangents = !fixedLook() && draw.hasTangents != 0;
    float4 tangent = float4(0.0, 0.0, 0.0, 0.0);
    if (tangents)
    {
        const Tangent stored = draw.tangents[index];
        tangent = float4(stored.xyzw[0], stored.xyzw[1], stored.xyzw[2], stored.xyzw[3]);
    }
    // Skinned before its own matrix, in its mesh's space, as the palette is.
    if (draw.jointCount != 0)
    {
        const Joints joints = draw.joints[index];
        float4x4 skin = float4x4(0.0);
        for (uint joint = 0; joint < 4; ++joint)
        {
            const uint which = (joints.joints[joint / 2] >> (16 * (joint % 2))) & 0xFFFF;
            const float weight = joints.weights[joint];
            if (which == noBone)
            {
                skin += float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
                                 0.0, 1.0) * weight;
            }
            else
            {
                // One past the palette is a mistake in a model, never a read past its end.
                skin += draw.palette[min(which, draw.jointCount - 1)] * weight;
            }
        }
        position = mul(skin, position);
        normal = skinnedNormal(skin, normal);
        // A bone that mirrors turns its bitangent with it, as it does its normal.
        tangent = float4(mul(skin, float4(tangent.xyz, 0.0)).xyz, tangent.w * mirroring(skin));
    }
    const float4 place = mul(draw.toView, position);
    Corner corner;
    corner.position = mul(camera.projection, place);
    corner.place = place.xyz;
    corner.normal = float3(dot(draw.normals[0].xyz, normal), dot(draw.normals[1].xyz, normal),
                           dot(draw.normals[2].xyz, normal));
    corner.uv0 = float2(vertex.uv0[0], vertex.uv0[1]);
    corner.uv1 = float2(vertex.uv1[0], vertex.uv1[1]);
    corner.color = float4(vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]);
    if (fixedLook())
    {
        corner.color.rgb = toEncoded(corner.color.rgb);
    }
    corner.draw = instance;
    corner.lit = float3(1.0, 1.0, 1.0);
    corner.tangent = tangents ? float4(mul(draw.toView, float4(tangent.xyz, 0.0)).xyz,
                                       tangent.w * mirroring(draw.toView))
                              : float4(0.0, 0.0, 0.0, 0.0);
    if (shading == shadingVertex)
    {
        // Lit here, by the draw's own lights, and clamped here, before any texture.
        const float3 unit = whole(corner.normal);
        const float3 eye = toEye(camera, place.xyz);
        const float shininess = camera.materials[draw.material].shininess;
        float3 diffuse = toEncoded(camera.ambient);
        float3 highlight = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < draw.lightCount; ++i)
        {
            addLight(camera.lights[camera.drawLights[draw.lightFirst + i]], place.xyz, unit, eye,
                     shininess, diffuse, highlight);
        }
        corner.lit = saturate(diffuse + highlight);
    }
    return corner;
}

// A map's UVs at a corner, and its texel there.
float2 mapUv(Layer map, Corner corner)
{
    const float3 uv = float3(map.bump.x != 0.0 ? corner.uv1 : corner.uv0, 1.0);
    return float2(dot(map.uvAcross.xyz, uv), dot(map.uvDown.xyz, uv));
}

float4 mapTexel(Layer map, Corner corner)
{
    return sampleTexture(map.texture, map.sampler, mapUv(map, corner));
}

// A surface's own tangent as a normal map's UVs go, which what turns them makes another: what its
// u and v are along, in terms of the UVs the tangent's of, so it's what a pixel makes of its UVs,
// its bitangent turned around by a turn that mirrors them.
float4 tangentThrough(Layer map, float3 normal, float4 tangent)
{
    const float2 u = map.uvAcross.xy;
    const float2 v = map.uvDown.xy;
    const float3 bitangent = cross(normal, tangent.xyz) * (tangent.w < 0.0 ? -1.0 : 1.0);
    const float across = (u.x * v.y) - (u.y * v.x);
    return float4(((tangent.xyz * v.y) + (bitangent * v.x)) / across,
                  (tangent.w < 0.0 ? -1.0 : 1.0) * (across < 0.0 ? -1.0 : 1.0));
}

// What a pixel looks like, shaded by pbr.slang: its base color by its environment and ambient light,
// each directional light, and its cluster's, what it emits on top.
float3 pixelShaded(Camera* camera, Material material, Corner corner, float3 baseColor, bool front)
{
    const float3 place = corner.place;
    const float3 eye = toEye(camera, place);
    float3 normal = whole(corner.normal);
    // How much light it gives back of what falls on it, which no more than all of it can be.
    const float3 reflecting = saturate(baseColor);
    float metallic = material.metallic;
    float roughness = material.roughness;
    float occlusion = 1.0;
    float3 emissive = material.emissive;
    const bool hasNormal = dot(normal, normal) > 0.0;
    if ((material.maps & (1 << mapMetalRough)) != 0)
    {
        const float4 texel = mapTexel(material.mapLayers[mapMetalRough], corner);
        roughness *= texel.g;
        metallic *= texel.b;
    }
    if ((material.maps & (1 << mapOcclusion)) != 0)
    {
        occlusion = 1.0 + (material.occlusionStrength * (mapTexel(material.mapLayers[mapOcclusion], corner).r - 1.0));
    }
    if ((material.maps & (1 << mapEmissive)) != 0)
    {
        emissive *= mapTexel(material.mapLayers[mapEmissive], corner).rgb;
    }
    if (hasNormal && (material.maps & (1 << mapNormal)) != 0)
    {
        const Layer map = material.mapLayers[mapNormal];
        // A surface's own tangents are its first UV set's.
        const float4 tangent = corner.tangent.w != 0.0 && map.bump.x == 0.0
                                   ? tangentThrough(map, normal, corner.tangent)
                                   : tangentFromDerivatives(normal, place, mapUv(map, corner));
        normal = normalFromMap(normal, tangent, mapTexel(map, corner).rgb, material.normalScale);
    }
    // Seen from behind, the whole frame's turned around, as its map says of its front.
    if (twoSided != 0 && !front)
    {
        normal = -normal;
    }
    // No normal, as a sprite's: only ambient light lights that.
    float3 color = reflecting * camera.ambient * occlusion;
    if (!hasNormal)
    {
        return color + emissive;
    }
    Surface surface;
    surface.baseColor = reflecting;
    surface.metallic = saturate(metallic);
    surface.roughness = saturate(roughness);
    surface.normal = normal;
    surface.occlusion = occlusion;
    surface.emissive = emissive;
    float2 across = (corner.position.xy - camera.viewport.xy) / camera.viewport.zw;
    if (camera.flipped != 0)
    {
        across.x = 1.0 - across.x;
    }
    const float depth = -place.z;
    const float part = camera.perspective != 0 ? log(max(depth, 0.000001)) * camera.sliceScale +
                                                     camera.sliceBias
                                               : depth * camera.sliceScale + camera.sliceBias;
    const uint column = uint(clamp(across.x * float(clustersAcross), 0.0, float(clustersAcross - 1)));
    const uint row = uint(clamp(across.y * float(clustersUp), 0.0, float(clustersUp - 1)));
    const uint slice = uint(clamp(part * float(clustersDeep), 0.0, float(clustersDeep - 1)));
    const Cluster cluster = camera.clusters[column + clustersAcross * (row + clustersUp * slice)];
    for (uint i = 0; i < cluster.count; ++i)
    {
        float3 way;
        float3 reaching;
        lightAt(camera.lights[camera.clusterLights[cluster.first + i]], place, way, reaching);
        color += shadePunctual(surface, eye, way, reaching);
    }
    // The directional lights, which no cluster lists, come first in the lights.
    for (uint i = 0; i < camera.directional; ++i)
    {
        float3 way;
        float3 reaching;
        lightAt(camera.lights[i], place, way, reaching);
        color += shadePunctual(surface, eye, way, reaching);
    }
    if (camera.hasEnvironment != 0)
    {
        // An environment's directions are the world's, and a surface's the view's.
        Surface inWorld = surface;
        inWorld.normal = float3(dot(camera.toWorld[0].xyz, normal), dot(camera.toWorld[1].xyz, normal),
                                dot(camera.toWorld[2].xyz, normal));
        const float3 eyeInWorld = float3(dot(camera.toWorld[0].xyz, eye), dot(camera.toWorld[1].xyz, eye),
                                         dot(camera.toWorld[2].xyz, eye));
        color += shadeEnvironment(inWorld, eyeInWorld, camera.environment);
    }
    return color + emissive;
}

[shader("fragment")]
float4 fragmentMain(Corner corner, bool front : SV_IsFrontFace) : SV_Target
{
    Camera* camera = push.camera;
    const Draw draw = camera.draws[corner.draw];
    const Material material = camera.materials[draw.material];
    const bool encoded = fixedLook();
    float4 current = material.color * draw.color;
    if (encoded)
    {
        current.rgb = toEncoded(material.color.rgb) * toEncoded(draw.color.rgb);
    }
    if (vertexColors != 0)
    {
        current *= corner.color;
    }
    if (material.noAlpha != 0)
    {
        current.a = draw.color.a;
    }
    if (encoded)
    {
        current.rgb *= corner.lit;
    }
    // Each layer over what's under it, the lit color first, as fixed-function stages were; PBR's
    // make its base color, which is lit after.
    float2 shift = float2(0.0, 0.0);
    float light = 1.0;
    for (uint i = 0; i < layerCount; ++i)
    {
        const Layer layer = material.layers[i];
        const uint kind = (layerBlends >> (4 * i)) & 15;
        const float2 raw = ((layerSets >> i) & 1) != 0 ? corner.uv1 : corner.uv0;
        const float3 uv = float3(raw, 1.0);
        const float2 turned = float2(dot(layer.uvAcross.xyz, uv), dot(layer.uvDown.xyz, uv)) + shift;
        float4 texel = sampleTexture(layer.texture, layer.sampler, turned);
        // A map's offsets and every fixed-function color are what's stored.
        if (layer.uvAcross.w != 0.0 && (encoded || kind == layerBump))
        {
            texel.rgb = toEncoded(texel.rgb);
        }
        // One that can't be sampled leaves what's under it as it is.
        if (layer.uvDown.w != 0.0)
        {
            const float none = kind == layerAdd ? 0.0 : (kind == layerBump ? 128.0 / 255.0 : 1.0);
            const float halved = kind == layerMultiply2 ? 128.0 / 255.0 : none;
            texel = float4(halved, halved, halved, kind == layerAlpha ? 0.0 : 1.0);
        }
        if (kind == layerBump)
        {
            // Shifts the next layer's UVs by its red and green, about middle gray's byte, through
            // its matrix, and scales its light by its blue.
            const float2 by = (floor(texel.rg * 255.0 + float2(0.5, 0.5)) - float2(128.0, 128.0)) / 127.0;
            shift = float2(layer.bump.x * by.x + layer.bump.z * by.y,
                           layer.bump.y * by.x + layer.bump.w * by.y);
            light = saturate(texel.b * layer.luminanceScale + layer.luminanceOffset);
            continue;
        }
        const float3 color = texel.rgb * light;
        shift = float2(0.0, 0.0);
        light = 1.0;
        if (kind == layerMultiply)
        {
            current = float4(current.rgb * color, current.a * texel.a);
        }
        else if (kind == layerMultiply2)
        {
            current = float4(current.rgb * color * 2.0, current.a * texel.a);
        }
        else if (kind == layerAdd)
        {
            current = float4(current.rgb + color, current.a * texel.a);
        }
        else if (kind == layerAlpha)
        {
            current.rgb = lerp(current.rgb, color, texel.a);
        }
        if (encoded)
        {
            current.rgb = saturate(current.rgb);
        }
    }
    if (material.noAlpha != 0)
    {
        current.a = draw.color.a;
    }
    // Before it's discarded, as maps' levels and a tangent's derivatives need their neighbors.
    if (!encoded)
    {
        current.rgb = pixelShaded(camera, material, corner, current.rgb, front);
    }
    if (alphaTest != 0 && current.a < material.cutoff)
    {
        discard;
    }
    if (fog != 0)
    {
        const float depth = -corner.place.z;
        float thick;
        if (fog == fogLinear)
        {
            thick = saturate((depth - camera.fogStart) / (camera.fogEnd - camera.fogStart));
        }
        else if (fog == fogExponential)
        {
            thick = 1.0 - exp(-camera.fogDensity * depth);
        }
        else
        {
            const float amount = camera.fogDensity * depth;
            thick = 1.0 - exp(-amount * amount);
        }
        // What adds fades to adding nothing and what multiplies to multiplying by white, not to
        // the fog's color, which what's behind them is already in.
        if (blend == blendAdd)
        {
            current.rgb *= 1.0 - thick;
        }
        else if (blend == blendMultiply)
        {
            current.rgb = lerp(current.rgb, float3(1.0, 1.0, 1.0), thick);
        }
        else
        {
            const float3 fogColor = encoded ? toEncoded(camera.fogColor.rgb) : camera.fogColor.rgb;
            current.rgb = lerp(current.rgb, fogColor, thick);
        }
    }
    if (blend == blendMultiply)
    {
        // Scaled by its alpha: none of it leaves what's behind as it is.
        current.rgb = lerp(float3(1.0, 1.0, 1.0), current.rgb, current.a);
    }
    if (encoded)
    {
        current.rgb = toLinear(saturate(current.rgb));
    }
    if (blend == blendOpaque)
    {
        current.a = 1.0;
    }
    return current;
}
