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
