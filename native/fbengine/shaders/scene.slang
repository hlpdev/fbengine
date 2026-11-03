// The scene's one pipeline family: every material's look, from the specialization constants its
// draw's pipeline key gives (fbe_render.h has the key, render/draw.cpp makes the constants), so any
// mix of them draws in one pass. Everything's read through addresses: the push constants give the
// camera's, which gives its draws', materials', and lights'; each draw, by its instance, gives its
// vertices'. Lighting is in the camera's view, x right, y up, looking down -z.
//
// PBR works in linear light. UNLIT and VERTEX are the fixed-function look, which worked on what was
// stored: their colors, textures, adds, doublings, and fog are all in encoded values, sRGB's, each
// stage clamped to 0 to 1, as its stages were, and only the color they end with is made linear for
// the target. A room's 50 doubled is 100, as it was, not linear light's 71.

import bindless;

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

// A draw: its matrix to the camera's view, what turns its normals there, its color and alpha, its
// vertices, its joints and palette if it's skinned, its material's index, its own lights, and how
// many matrices its palette has, 0 if it isn't skinned.
struct Draw
{
    float4x4 toView;
    float4 normals[3];
    float4 color;
    Vertex* vertices;
    Joints* joints;
    float4x4* palette;
    uint material;
    uint lightFirst;
    uint lightCount;
    uint jointCount;
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
    int shadow;
};

struct Cluster
{
    uint first;
    uint count;
};

// A shadow's tile: from the view to the light's clip space; its texels in the atlas, from and to; its
// biases, in depth, world units, and along the normal, in world units at a distance of 1, or a level
// light's anywhere; what a world unit's depth comes to in the tile's; where a cascade ends; whether
// it's level, a directional light's; and a texel of the atlas in UV.
struct Shadow
{
    float4x4 fromView;
    float4 rect;
    float bias;
    float normalBias;
    float depthPer;
    float split;
    uint level;
    float inverseSide;
    float spare0;
    float spare1;
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
    Shadow* shadows;
    uint shadowAtlas;
    uint shadowSpare;
    float4 worldRows[3];
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

// The light that reaches a place from a light, on a normal: what falls on it, and what its
// highlight adds. Per pixel, as glTF's KHR_lights_punctual has it, which ends at its range, as
// clusters need; per vertex, as fixed-function hardware's did, whole within its range and falling as
// range over distance past it, with no end, in encoded values.
void addLight(Light light, float3 place, float3 normal, float3 eye, float shininess, float reached,
              inout float3 diffuse, inout float3 highlight)
{
    float3 way;
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
    const float facing = saturate(dot(normal, way));
    const float3 linear = float3(light.color[0], light.color[1], light.color[2]);
    const float3 color = (shading == shadingVertex ? toEncoded(linear) : linear) * falling * reached;
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
                     shininess, 1.0, diffuse, highlight);
        }
        corner.lit = saturate(diffuse + highlight);
    }
    return corner;
}

// How much of a light reaches a place past what casts its shadow, from its tile: 0 to 1, a bilinear
// filter over the 4 by 4 texels around it, in the tile, each one compared.
float shadowIn(Camera* camera, Shadow shadow, float3 place, float3 normal, float distance)
{
    const float offset = shadow.normalBias * (shadow.level != 0 ? 1.0 : distance);
    const float4 clip = mul(shadow.fromView, float4(place + normal * offset, 1.0));
    if (clip.w <= 0.0)
    {
        return 1.0;
    }
    const float3 seen = clip.xyz / clip.w;
    if (seen.z < 0.0 || abs(seen.x) > 1.0 || abs(seen.y) > 1.0)
    {
        return 1.0;
    }
    const float size = shadow.rect.z - shadow.rect.x + 1.0;
    const float2 at = shadow.rect.xy + (seen.xy * 0.5 + float2(0.5, 0.5)) * size - float2(0.5, 0.5);
    const float2 base = floor(at);
    const float2 part = at - base;
    const float bias = shadow.level != 0 ? shadow.bias * shadow.depthPer
                                         : shadow.bias * shadow.depthPer / max(clip.w * clip.w, 0.000001);
    float reached = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        const float across = y == -1 ? 1.0 - part.y : (y == 2 ? part.y : 1.0);
        for (int x = -1; x <= 2; ++x)
        {
            const float along = x == -1 ? 1.0 - part.x : (x == 2 ? part.x : 1.0);
            const int2 texel = clamp(int2(base) + int2(x, y), int2(shadow.rect.xy), int2(shadow.rect.zw));
            const float nearest = loadTexel(camera.shadowAtlas, texel).r;
            reached += nearest > seen.z + bias ? 0.0 : across * along;
        }
    }
    return reached / 9.0;
}

// How much of a light reaches a place, by whichever of its tiles it's in.
float shadowOf(Camera* camera, Light light, float3 place, float3 normal)
{
    if (light.shadow < 0)
    {
        return 1.0;
    }
    uint first = uint(light.shadow);
    if (light.type == int(lightDirectional))
    {
        const float depth = -place.z;
        Shadow shadow = camera.shadows[first];
        while (shadow.level != 0 && depth >= shadow.split)
        {
            first += 1;
            shadow = camera.shadows[first];
        }
        if (shadow.level == 0)
        {
            return 1.0;
        }
        return shadowIn(camera, shadow, place, normal, 1.0);
    }
    const float3 from = place - float3(light.position[0], light.position[1], light.position[2]);
    const float apart = length(from);
    if (light.type == int(lightSpot))
    {
        return shadowIn(camera, camera.shadows[first], place, normal, apart);
    }
    // A point light's six, along the world's axes.
    const float3 world = float3(dot(camera.worldRows[0].xyz, from), dot(camera.worldRows[1].xyz, from),
                                dot(camera.worldRows[2].xyz, from));
    const float3 size = abs(world);
    uint face = world.z < 0.0 ? 5 : 4;
    if (size.x >= size.y && size.x >= size.z)
    {
        face = world.x < 0.0 ? 1 : 0;
    }
    else if (size.y >= size.z)
    {
        face = world.y < 0.0 ? 3 : 2;
    }
    return shadowIn(camera, camera.shadows[first + face], place, normal, apart);
}

// What lights a pixel per pixel: the ambient light, every directional light, and its cluster's.
float3 pixelLit(Camera* camera, float2 pixel, float3 place, float3 normal, float shininess)
{
    const float3 eye = toEye(camera, place);
    float3 diffuse = camera.ambient;
    float3 highlight = float3(0.0, 0.0, 0.0);
    float2 across = (pixel - camera.viewport.xy) / camera.viewport.zw;
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
        const Light light = camera.lights[camera.clusterLights[cluster.first + i]];
        addLight(light, place, normal, eye, shininess, shadowOf(camera, light, place, normal), diffuse,
                 highlight);
    }
    // The directional lights, which no cluster lists, come first in the lights.
    for (uint i = 0; i < camera.directional; ++i)
    {
        const Light light = camera.lights[i];
        addLight(light, place, normal, eye, shininess, shadowOf(camera, light, place, normal), diffuse,
                 highlight);
    }
    return diffuse + highlight;
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
    float3 lit = corner.lit;
    if (!encoded)
    {
        float3 normal = whole(corner.normal);
        if (twoSided != 0 && !front)
        {
            normal = -normal;
        }
        lit = pixelLit(camera, corner.position.xy, corner.place, normal, material.shininess);
    }
    current.rgb *= lit;
    // Each layer over what's under it, the lit color first, as fixed-function stages were.
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

// A shadow's tile: what's nearest the light, kept as its depth, drawn with the camera's projection the
// light's whole matrix to clip space, and each draw's matrix its own to the world.
struct ShadowCorner
{
    float4 position : SV_Position;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    nointerpolation uint draw : DRAW;
};

[shader("vertex")]
ShadowCorner shadowVertex(uint index : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    Camera* camera = push.camera;
    const Draw draw = camera.draws[instance];
    const Vertex vertex = draw.vertices[index];
    float4 position = float4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0);
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
                skin += draw.palette[min(which, draw.jointCount - 1)] * weight;
            }
        }
        position = mul(skin, position);
    }
    ShadowCorner corner;
    corner.position = mul(camera.projection, mul(draw.toView, position));
    // Nearer than the near plane, it still casts, flattened onto it.
    corner.position.z = min(corner.position.z, corner.position.w);
    corner.uv0 = float2(vertex.uv0[0], vertex.uv0[1]);
    corner.uv1 = float2(vertex.uv1[0], vertex.uv1[1]);
    corner.draw = instance;
    return corner;
}

[shader("fragment")]
float shadowFragment(ShadowCorner corner) : SV_Target
{
    if (alphaTest != 0)
    {
        // Cut out by its first layer, as a leaf's texture has it.
        Camera* camera = push.camera;
        const Draw draw = camera.draws[corner.draw];
        const Material material = camera.materials[draw.material];
        float alpha = material.color.a * draw.color.a;
        if (layerCount > 0)
        {
            const Layer layer = material.layers[0];
            const float2 raw = (layerSets & 1) != 0 ? corner.uv1 : corner.uv0;
            const float3 uv = float3(raw, 1.0);
            alpha *= sampleTexture(layer.texture, layer.sampler,
                                   float2(dot(layer.uvAcross.xyz, uv), dot(layer.uvDown.xyz, uv))).a;
        }
        if (alpha < material.cutoff)
        {
            discard;
        }
    }
    return corner.position.z;
}
