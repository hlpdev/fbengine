#pragma once

// An environment as shaders read it, for what draws with one.

#include <cstdint>

namespace fbe::environment {

/// What pbr.slang's Environment is, in a shader's push constants or a buffer.
struct shader_data_t {
    std::uint32_t irradiance = 0;
    std::uint32_t specular = 0;
    std::uint32_t table = 0;
    std::uint32_t sampler = 0;
    float levels = 0.0F;
    float intensity = 1.0F;
};

/// An environment's textures as a shader reads them; false, with fbe_error's why, if the handle
/// isn't one's or the GPU's stopped since it was baked.
bool shader_data(std::uint64_t environment, shader_data_t& into);

/// Sets the most samples a draw or a submit of a bake takes, 0 for what it is at first, and forgets
/// the most one's taken, which largest_draw says, for tests.
void set_sample_budget(std::uint64_t samples);
std::uint64_t largest_draw();

} // namespace fbe::environment
