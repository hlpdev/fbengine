#pragma once

// What the shader area gives drawing: each shader's code, by the number a draw's pipeline key has.

#include "gpu/shader.hpp"

#include <cstdint>

namespace fbe::shaders_own {

/// The number in a pipeline's key of the code a shader has now, from 1; 0 for none, or a freed one.
std::uint32_t code_of(std::uint64_t shader);

/// The code a pipeline's key numbers, which is kept until the DLL's unloaded, since a pipeline is
/// made of it whenever it's first asked for; null for 0.
const gpu::shader_t* shader_of(std::uint32_t code);

/// Lets go of a hold on a shader, as a material freed does.
void release(std::uint64_t shader);

/// Takes a hold on a shader, as a material cloned does.
void retain(std::uint64_t shader);

} // namespace fbe::shaders_own
