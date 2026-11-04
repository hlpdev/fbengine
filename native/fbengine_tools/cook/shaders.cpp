// What a game's shader is compiled with: the engine's shaders' sources, embedded by CMake, since
// nothing else of them is on a machine that has only the package.

#include "cook/fbe_cook.h"
#include "shader_sources.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace {

fbe::tools::shader_source_t const* source_at(std::int32_t index) {
    const std::span<const fbe::tools::shader_source_t> sources = fbe::tools::shader_sources();
    return index >= 0 && std::cmp_less(index, sources.size())
               ? &sources[static_cast<std::size_t>(index)]
               : nullptr;
}

} // namespace

extern "C" {

FBE_TOOLS_API int32_t fbe_cook_shader_source_count(void) {
    return static_cast<int32_t>(fbe::tools::shader_sources().size());
}

FBE_TOOLS_API const char* fbe_cook_shader_source_name(int32_t index) {
    const fbe::tools::shader_source_t* found = source_at(index);
    return found != nullptr ? found->name : "";
}

FBE_TOOLS_API const char* fbe_cook_shader_source_text(int32_t index) {
    const fbe::tools::shader_source_t* found = source_at(index);
    return found != nullptr ? found->text : "";
}

FBE_TOOLS_API const char* fbe_cook_shader_stamp(void) {
    return FBE_SHADER_STAMP;
}

FBE_TOOLS_API const char* fbe_cook_slang_version(void) {
    return FBE_SLANG_VERSION;
}

} // extern "C"
