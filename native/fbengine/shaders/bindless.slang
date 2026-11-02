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
