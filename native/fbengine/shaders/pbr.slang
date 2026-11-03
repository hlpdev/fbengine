// glTF's metal-rough shading, which a material's shaders import: a surface's normal from its normal
// map, what a punctual light adds, and what an environment adds, read from the octahedral textures
// environment.slang bakes. Everything's in linear light, and directions point away from the surface.
module pbr;

import bindless;

static const float PI = 3.14159265358979;
/// The smallest GGX alpha, so a mirror's highlight stays finite: a roughness of 0.045.
static const float SMOOTHEST = 0.002;
/// How short a vector's square has to be for it to have no direction to speak of.
static const float DEGENERATE = 1e-8;

/// What a surface is at a point, as glTF's metal-rough material says.
public struct Surface
{
    public float3 baseColor;
    public float metallic;
    public float roughness;
    /// The shading normal, unit length.
    public float3 normal;
    /// How much of an environment's light reaches it, 1 for all.
    public float occlusion;
    public float3 emissive;
}

/// An environment's textures, by their indices in the bindless set, as fbe_environment.h bakes them:
/// the diffuse light, the specular light with roughness across its levels, and the split sum's
/// table; the sampler to read them through, which has no LOD bias; its specular's highest level to
/// read; and what its light's multiplied by.
public struct Environment
{
    public uint irradiance;
    public uint specular;
    public uint table;
    public uint sampler;
    public float levels;
    public float intensity;
}

/// The shading normal from a tangent-space normal map's texel, as stored, 0 to 1: `normal` and
/// `tangent` are the interpolated vertex's, whose tangent.w says which way its bitangent goes, as
/// MikkTSpace and glTF have it, +Y up; `scale` scales the map's X and Y, as glTF's normalTexture's
/// does. A tangent along the normal or of no length, as a model can have, or a texel with no
/// direction, leaves the normal as it is.
public float3 normalFromMap(float3 normal, float4 tangent, float3 texel, float scale = 1.0)
{
    const float3 n = normalize(normal);
    const float3 along = tangent.xyz - (n * dot(n, tangent.xyz));
    const float alongSquared = dot(along, along);
    if (alongSquared < DEGENERATE)
    {
        return n;
    }
    const float3 t = along / sqrt(alongSquared);
    const float3 b = cross(n, t) * tangent.w;
    const float3 mapped = ((texel * 2.0) - 1.0) * float3(scale, scale, 1.0);
    const float3 turned = (t * mapped.x) + (b * mapped.y) + (n * mapped.z);
    const float turnedSquared = dot(turned, turned);
    return turnedSquared < DEGENERATE ? n : turned / sqrt(turnedSquared);
}

/// A tangent for a surface with none, along its UVs' u, from how its place and UVs change over a
/// pixel, its w the way its bitangent goes against their v, as glTF's +Y is: as good as a pixel's
/// triangle says, which is flat, so its seams show where a model's own wouldn't. No tangent at all,
/// as a triangle whose UVs don't change has.
public float4 tangentFromDerivatives(float3 normal, float3 place, float2 uv)
{
    const float3 placeAcross = ddx(place);
    const float3 placeDown = ddy(place);
    const float2 uvAcross = ddx(uv);
    const float2 uvDown = ddy(uv);
    const float across = (uvAcross.x * uvDown.y) - (uvDown.x * uvAcross.y);
    if (abs(across) < 1e-20)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float3 tangent = ((placeAcross * uvDown.y) - (placeDown * uvAcross.y)) / across;
    const float3 alongV = ((placeDown * uvAcross.x) - (placeAcross * uvDown.x)) / across;
    return float4(tangent, dot(cross(normal, tangent), alongV) < 0.0 ? 1.0 : -1.0);
}

/// How much of the light Fresnel reflects at normal incidence: 4% for a dielectric, its base color
/// for a metal.
float3 reflectanceOf(Surface surface)
{
    return lerp(float3(0.04), surface.baseColor, surface.metallic);
}

/// GGX's distribution of normals, at `alpha`, the roughness squared.
public float ggxDistribution(float nDotH, float alpha)
{
    const float squared = alpha * alpha;
    const float across = (nDotH * nDotH * (squared - 1.0)) + 1.0;
    return squared / (PI * across * across);
}

/// Smith's height-correlated visibility for GGX: its geometry over 4 nDotL nDotV.
public float smithVisibility(float nDotL, float nDotV, float alpha)
{
    const float squared = alpha * alpha;
    const float viewed = nDotL * sqrt((nDotV * nDotV * (1.0 - squared)) + squared);
    const float lit = nDotV * sqrt((nDotL * nDotL * (1.0 - squared)) + squared);
    const float both = viewed + lit;
    return both > 0.0 ? 0.5 / both : 0.0;
}

/// What a light from `toLight`, a unit vector, adds where its `radiance` arrives (its color and
/// intensity, attenuated), seen from `view`, a unit vector: glTF's BRDF, Lambert's diffuse and GGX's
/// specular, Fresnel by Schlick, with Smith's height-correlated visibility, as Khronos's viewer has
/// it, where its specification writes the separable form. A light straight against the view has no
/// halfway, and takes the normal's.
public float3 shadePunctual(Surface surface, float3 view, float3 toLight, float3 radiance)
{
    const float nDotL = saturate(dot(surface.normal, toLight));
    if (nDotL <= 0.0)
    {
        return float3(0.0);
    }
    const float3 both = view + toLight;
    const float bothSquared = dot(both, both);
    const float3 halfway = bothSquared < DEGENERATE ? surface.normal : both / sqrt(bothSquared);
    const float nDotV = saturate(dot(surface.normal, view));
    const float nDotH = saturate(dot(surface.normal, halfway));
    const float vDotH = saturate(dot(view, halfway));
    const float alpha = max(surface.roughness * surface.roughness, SMOOTHEST);
    const float3 f0 = reflectanceOf(surface);
    const float3 fresnel = f0 + ((1.0 - f0) * pow(1.0 - vDotH, 5.0));
    const float3 diffuse = (1.0 - fresnel) * surface.baseColor * (1.0 - surface.metallic) / PI;
    const float3 specular = fresnel * smithVisibility(nDotL, nDotV, alpha) * ggxDistribution(nDotH, alpha);
    return (diffuse + specular) * radiance * nDotL;
}

/// `direction`, a unit vector, on the octahedral map: 0 to 1 across and down.
public float2 octahedralOf(float3 direction)
{
    const float3 onto = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    float2 at = onto.xz;
    if (onto.y < 0.0)
    {
        const float2 signs = float2(at.x >= 0.0 ? 1.0 : -1.0, at.y >= 0.0 ? 1.0 : -1.0);
        at = (1.0 - abs(at.yx)) * signs;
    }
    return (at * 0.5) + 0.5;
}

/// The unit vector at a place on the octahedral map, 0 to 1 across and down: +Y (up) at its middle,
/// and -Y at its corners. Its edges fold: a point on one is the same direction as its mirror about
/// that edge's middle.
public float3 directionOfOctahedral(float2 at)
{
    const float2 from = (at * 2.0) - 1.0;
    const float up = 1.0 - abs(from.x) - abs(from.y);
    float2 across = from;
    if (up < 0.0)
    {
        const float2 signs = float2(from.x >= 0.0 ? 1.0 : -1.0, from.y >= 0.0 ? 1.0 : -1.0);
        across = (1.0 - abs(from.yx)) * signs;
    }
    return normalize(float3(across.x, up, across.y));
}

/// An octahedral texture in `direction` at a whole `level`. Its texels are baked with the first and
/// last of each row and column on the map's edges, where they meet their mirrors, so reading between
/// texel middles is the same on either side of an edge, and filtering needs no border.
float3 octahedralAt(uint texture, uint sampler, float3 direction, float level)
{
    const float2 size = max(textureSize(texture) / exp2(level), float2(1.0));
    const float2 at = ((octahedralOf(direction) * (size - 1.0)) + 0.5) / size;
    return sampleTextureLevel(texture, sampler, at, level).rgb;
}

/// What an environment adds for a surface seen from `view`, a unit vector, its occlusion taken into
/// account: the split sum of its specular light, and its diffuse light, with the energy a single
/// scatter loses given back, as Fdez-Agüera's approximation does, so a white furnace is white at any
/// roughness.
public float3 shadeEnvironment(Surface surface, float3 view, Environment environment)
{
    const float nDotV = saturate(dot(surface.normal, view));
    const float3 mirrored = reflect(-view, surface.normal);
    const float level = saturate(surface.roughness) * environment.levels;
    const float lower = floor(level);
    const float upper = min(lower + 1.0, environment.levels);
    const float3 specularLight =
        lerp(octahedralAt(environment.specular, environment.sampler, mirrored, lower),
             octahedralAt(environment.specular, environment.sampler, mirrored, upper), level - lower);
    const float3 diffuseLight = octahedralAt(environment.irradiance, environment.sampler, surface.normal, 0.0);
    const float2 split = sampleTextureLevel(environment.table, environment.sampler,
                                            float2(nDotV, saturate(surface.roughness)), 0.0)
                             .rg;

    const float3 f0 = reflectanceOf(surface);
    // Fresnel averaged over the lobe, wider the rougher, by Fdez-Agüera's fit.
    const float3 grazing = max(float3(1.0 - surface.roughness), f0) - f0;
    const float3 fresnel = f0 + (grazing * pow(1.0 - nDotV, 5.0));
    const float3 single = (fresnel * split.x) + split.y;
    const float lost = 1.0 - (split.x + split.y);
    const float3 average = f0 + ((1.0 - f0) / 21.0);
    // A table that isn't baked yet loses everything, and white metal would divide by nothing.
    const float3 multiple = lost * single * average / max(1.0 - (average * lost), 1e-4);
    const float3 diffuse = surface.baseColor * (1.0 - surface.metallic) * (1.0 - single - multiple);
    const float3 light = (single * specularLight) + ((multiple + diffuse) * diffuseLight);
    return light * surface.occlusion * environment.intensity;
}
