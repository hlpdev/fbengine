#pragma once

// A shader, compiled from Slang to SPIR-V as the engine builds and embedded in it: what the headers
// fbengine_add_shaders makes declare, one for each .slang file, by its name.

#include <cstdint>
#include <span>
#include <string_view>

namespace fbe::gpu {

/// A .slang file's SPIR-V, every entry point of it, as slangc made it.
struct shader_t {
    /// The file's name, without .slang.
    std::string_view name;
    std::span<const std::uint32_t> code;
};

} // namespace fbe::gpu
