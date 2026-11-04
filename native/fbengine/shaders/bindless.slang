// The bindless set every pipeline reads textures through, set 0: the samplers, one for each filter
// and wrap, and every texture, by its index, the white one at 0. gpu/vulkan.hpp's sampler_binding
// and texture_binding say the same.
module bindless;

[[vk::binding(0, 0)]]
public SamplerState samplers[32];

[[vk::binding(1, 0)]]
public Texture2D textures[];

/// Texture `index` at `uv`, through sampler `sampler`.
public float4 sampleTexture(uint index, uint sampler, float2 uv)
{
    return textures[NonUniformResourceIndex(index)].Sample(samplers[NonUniformResourceIndex(sampler)], uv);
}

/// Texture `index` at `uv`, through sampler `sampler`, as if uv changed by `across` and `down` to
/// the next pixel across and down.
public float4 sampleTextureGrad(uint index, uint sampler, float2 uv, float2 across, float2 down)
{
    return textures[NonUniformResourceIndex(index)].SampleGrad(samplers[NonUniformResourceIndex(sampler)], uv,
                                                               across, down);
}

/// Texture `index`'s first level's texel at `at`, as it's stored.
public float4 loadTexel(uint index, int2 at)
{
    return textures[NonUniformResourceIndex(index)].Load(int3(at, 0));
}

/// Texture `index`'s first level's size, in texels.
public float2 textureSize(uint index)
{
    uint width;
    uint height;
    textures[NonUniformResourceIndex(index)].GetDimensions(width, height);
    return float2(width, height);
}

/// How many levels texture `index` has, its mipmaps and its first.
public uint textureLevels(uint index)
{
    uint width;
    uint height;
    uint levels;
    textures[NonUniformResourceIndex(index)].GetDimensions(0, width, height, levels);
    return levels;
}

/// Texture `index` at `uv`, level `level` of it, through sampler `sampler`.
public float4 sampleTextureLevel(uint index, uint sampler, float2 uv, float level)
{
    return textures[NonUniformResourceIndex(index)].SampleLevel(samplers[NonUniformResourceIndex(sampler)], uv, level);
}

/// Whether a float's NaN, or an infinity, by its bits, which a driver's fast math can't assume away.
public bool isNotANumber(float value)
{
    return (asuint(value) & 0x7FFFFFFF) > 0x7F800000;
}

public bool isInfinite(float value)
{
    return (asuint(value) & 0x7FFFFFFF) == 0x7F800000;
}

/// Light as a number: no NaN or light below none, which are none, nor past what a half float holds,
/// as an infinity's taken to be.
public float wholeLight(float light)
{
    const float brightest = 65504.0;
    return isNotANumber(light) ? 0.0 : (isInfinite(light) ? (light > 0.0 ? brightest : 0.0) : clamp(light, 0.0, brightest));
}

public float3 wholeLight(float3 light)
{
    return float3(wholeLight(light.r), wholeLight(light.g), wholeLight(light.b));
}
