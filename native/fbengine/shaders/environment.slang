// Baking an environment from an equirectangular image, in a full-screen triangle, into what pbr.slang
// reads: its diffuse light, its specular light for a roughness, each octahedral, and the split sum's
// table. Hammersley's points make it the same every time; each reads the image's mipmaps as far as
// its sample's share of the sphere covers, as Křivánek and Colbert's filtered importance sampling
// does, so a few samples aren't noisy.

import bindless;
import pbr;

static const float PI = 3.14159265358979;

struct Bake
{
    uint source;
    uint sampler;
    /// The roughness of the specular light being baked.
    float roughness;
    /// How many samples each texel takes.
    uint samples;
    /// The size of the level being drawn, in texels, whose first and last are on the map's edges.
    float size;
};

[[vk::push_constant]]
ConstantBuffer<Bake> bake;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

/// The direction of the octahedral map's texel a pixel is.
float3 directionOfPixel(float4 position)
{
    return directionOfOctahedral((position.xy - 0.5) / max(bake.size - 1.0, 1.0));
}

/// Hammersley's `index`th of `count` points.
float2 hammersley(uint index, uint count)
{
    return float2((float(index) + 0.5) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

/// A frame around `normal`, unit length.
float3x3 frameOf(float3 normal)
{
    const float3 other = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 across = normalize(cross(other, normal));
    return float3x3(across, cross(normal, across), normal);
}

/// The source image in `direction`, y up, blurred as far as a sample of `share` of the sphere covers.
float3 sourceIn(float3 direction, float share)
{
    const float2 size = textureSize(bake.source);
    const float maximum = float(textureLevels(bake.source)) - 1.0;
    const float texel = 4.0 * PI / (size.x * size.y);
    const float level = clamp(0.5 * log2(max(share / texel, 1e-6)), 0.0, maximum);
    const float rows = size.y / exp2(ceil(level));
    // Across it repeats, so it joins itself; down it stops at the poles, which repeating would wrap.
    // At a pole there's no angle, which atan2 leaves to the GPU: every side of it reads one column.
    const bool pole = direction.x == 0.0 && direction.z == 0.0;
    const float across = pole ? 0.5 : (atan2(direction.z, direction.x) / (2.0 * PI)) + 0.5;
    const float down = clamp(acos(clamp(direction.y, -1.0, 1.0)) / PI, 0.5 / rows, 1.0 - (0.5 / rows));
    return min(sampleTextureLevel(bake.source, bake.sampler, float2(across, down), level).rgb, float3(1e6));
}

/// The diffuse light for a normal: the light around it weighted by the cosine, over pi, so an even
/// environment's is its own color.
[shader("fragment")]
float4 irradianceMain(float4 position : SV_Position) : SV_Target
{
    const float3x3 frame = frameOf(directionOfPixel(position));
    float3 sum = float3(0.0);
    for (uint index = 0; index < bake.samples; ++index)
    {
        const float2 xi = hammersley(index, bake.samples);
        const float angle = 2.0 * PI * xi.y;
        const float radius = sqrt(xi.x);
        const float up = sqrt(max(1.0 - xi.x, 0.0));
        const float3 direction = mul(float3(radius * cos(angle), radius * sin(angle), up), frame);
        const float pdf = max(up, 1e-4) / PI;
        sum += sourceIn(direction, 1.0 / (float(bake.samples) * pdf));
    }
    return float4(sum / float(bake.samples), 1.0);
}

/// A half vector of GGX's distribution around +Z, at `alpha`, the roughness squared.
float3 ggxHalfway(float2 xi, float alpha)
{
    const float angle = 2.0 * PI * xi.y;
    const float upSquared = (1.0 - xi.x) / (1.0 + (((alpha * alpha) - 1.0) * xi.x));
    const float up = sqrt(upSquared);
    const float radius = sqrt(max(1.0 - upSquared, 0.0));
    return float3(radius * cos(angle), radius * sin(angle), up);
}

/// The specular light for a roughness, seen along the normal, as the split sum takes it; at no
/// roughness, the source itself.
[shader("fragment")]
float4 specularMain(float4 position : SV_Position) : SV_Target
{
    const float3 normal = directionOfPixel(position);
    if (bake.roughness <= 0.0)
    {
        return float4(sourceIn(normal, 0.0), 1.0);
    }
    const float3x3 frame = frameOf(normal);
    const float alpha = bake.roughness * bake.roughness;
    float3 sum = float3(0.0);
    float weights = 0.0;
    for (uint index = 0; index < bake.samples; ++index)
    {
        const float3 halfway = ggxHalfway(hammersley(index, bake.samples), alpha);
        const float3 local = (2.0 * halfway.z * halfway) - float3(0.0, 0.0, 1.0);
        if (local.z > 0.0)
        {
            const float pdf = max(ggxDistribution(halfway.z, alpha) * 0.25, 1e-4);
            sum += sourceIn(mul(local, frame), 1.0 / (float(bake.samples) * pdf)) * local.z;
            weights += local.z;
        }
    }
    return float4(sum / max(weights, 1e-4), 1.0);
}

/// The split sum's table, nDotV across and roughness down: what Fresnel's first term's multiplied
/// by, and what's added to it.
[shader("fragment")]
float4 tableMain(float4 position : SV_Position) : SV_Target
{
    const float nDotV = position.x / bake.size;
    const float roughness = position.y / bake.size;
    const float alpha = max(roughness * roughness, 0.002);
    const float3 view = float3(sqrt(max(1.0 - (nDotV * nDotV), 0.0)), 0.0, nDotV);
    float2 sum = float2(0.0);
    for (uint index = 0; index < bake.samples; ++index)
    {
        const float3 halfway = ggxHalfway(hammersley(index, bake.samples), alpha);
        const float vDotH = saturate(dot(view, halfway));
        const float3 local = (2.0 * vDotH * halfway) - view;
        if (local.z > 0.0)
        {
            const float visible = 4.0 * smithVisibility(local.z, nDotV, alpha) * local.z * vDotH /
                                  max(halfway.z, 1e-4);
            const float fresnel = pow(1.0 - vDotH, 5.0);
            sum += float2((1.0 - fresnel) * visible, fresnel * visible);
        }
    }
    // Near grazing, the samples' noise can come to more light than there is.
    const float2 table = sum / float(bake.samples);
    return float4(table / max(table.x + table.y, 1.0), 0.0, 1.0);
}
