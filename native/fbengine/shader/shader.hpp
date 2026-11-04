#pragma once

// What the shader area gives drawing: each shader's code, by the number a draw's pipeline key has.

#include "gpu/shader.hpp"

#include <cstdint>

namespace fbe::shaders_own {

/// The number in a pipeline's key of the code a shader has now, from 1; 0 for none, a freed one, or
/// a full-screen pass's, which draws nothing a material does.
std::uint32_t code_of(std::uint64_t shader);

/// The number of the code a shader has now if it's a full-screen pass's, from 1, or 0.
std::uint32_t pass_code_of(std::uint64_t shader);

/// The code a pipeline's key numbers, which is kept until the DLL's unloaded, since a pipeline is
/// made of it whenever it's first asked for; null for 0, or one a pipeline couldn't be made of.
const gpu::shader_t* shader_of(std::uint32_t code);

/// That a pipeline couldn't be made of a code, whose draws go on as the scene's own, saying so
/// once.
void refuse(std::uint32_t code);

/// Lets go of a hold on a shader, as a material freed does.
void release(std::uint64_t shader);

/// Takes a hold on a shader, as a material cloned does.
void retain(std::uint64_t shader);

} // namespace fbe::shaders_own
