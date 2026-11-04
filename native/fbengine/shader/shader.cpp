// Shaders' C API: cooked files checked and kept, and which of a material's they are. Code is never
// let go of, since a pipeline's description names it and is made whenever it's first asked for; so
// a shader replaced has a new number, and what's kept is bounded by what a key's bits can number,
// which the same bytes loaded again don't add to.

#include "shader/shader.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/vulkan.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::shaders_own {

namespace {

/// What a cooked file starts with, then its version's length, in 4 bytes, its version, and its
/// stamp.
constexpr std::string_view magic = "FBESHADR";
constexpr std::string_view stamp = FBE_SHADER_STAMP;

/// The most of a file's version a message says, as it's whatever the file has.
constexpr std::size_t most_said = 32;

/// How many codes a pipeline key's 13 bits can number.
constexpr std::size_t most_codes = 8191;

constexpr std::uint32_t spirv_magic = 0x07230203;
constexpr std::uint32_t op_entry_point = 15;
constexpr std::uint32_t op_decorate = 71;
constexpr std::uint32_t decoration_binding = 33;
constexpr std::uint32_t decoration_set = 34;
constexpr std::uint32_t model_vertex = 0;
constexpr std::uint32_t model_fragment = 4;

/// What the scene's pipelines draw by, so one cooked without them is refused here, not at a draw.
struct entry_t {
    std::string_view name;
    std::uint32_t model;
};
constexpr std::array<entry_t, 5> entries{
    {
        { .name = "vertexMain", .model = model_vertex },
        { .name = "fragmentMain", .model = model_fragment },
        { .name = "particleVertex", .model = model_vertex },
        { .name = "shadowVertex", .model = model_vertex },
        { .name = "shadowFragment", .model = model_fragment },
    },
};

struct kept_code_t {
    std::vector<std::uint32_t> words;
    std::string name;
    gpu::shader_t shader;
    bool refused = false;
};

struct shader_t {
    std::uint32_t references = 0;
    std::uint32_t code = 0;
};

struct shaders_t {
    handle_slots_t slots{ SHADER_KIND };
    std::vector<shader_t> kept;
    std::deque<kept_code_t> codes;
};

shaders_t& all() {
    static shaders_t made;
    return made;
}

std::uint32_t slot_of(std::uint64_t handle) {
    const auto slot = all().slots.find(handle);
    if (!slot) {
        fail("the shader was freed");
        return scene::none;
    }
    return *slot;
}

/// Whether SPIR-V's words are whole instructions, with an entry point of `name` of `model`.
bool has_entry(std::span<const std::uint32_t> words, std::string_view name, std::uint32_t model) {
    std::size_t at = 5;
    while (at < words.size()) {
        const std::uint32_t count = words[at] >> 16U;
        const std::uint32_t code = words[at] & 0xFFFFU;
        if (count == 0 || count > words.size() - at) {
            return false;
        }
        if (code == op_entry_point && count > 3 && words[at + 1] == model) {
            const std::span<const std::uint32_t> named = words.subspan(at + 3, count - 3);
            const std::string_view found(reinterpret_cast<const char*>(named.data()),
                                         named.size_bytes());
            if (found.substr(0, found.find('\0')) == name) {
                return true;
            }
        }
        at += count;
    }
    return false;
}

/// Whether SPIR-V binds only what the scene's pipelines' layout has: the bindless set. A game's
/// global that isn't static is one more, or its own texture, which no pipeline could be made with.
bool binds_only_the_scene(std::span<const std::uint32_t> words) {
    std::size_t at = 5;
    while (at < words.size()) {
        const std::uint32_t count = words[at] >> 16U;
        if ((words[at] & 0xFFFFU) == op_decorate && count > 3) {
            const std::uint32_t kind = words[at + 2];
            const std::uint32_t value = words[at + 3];
            if ((kind == decoration_set && value != 0) ||
                (kind == decoration_binding && value != gpu::sampler_binding &&
                 value != gpu::texture_binding)) {
                return false;
            }
        }
        at += count;
    }
    return true;
}

/// A cooked file's SPIR-V, or nothing, with fbe_error saying why.
std::optional<std::vector<std::uint32_t>> words_of(const std::uint8_t* bytes, std::int64_t size) {
    if (bytes == nullptr || size < 0) {
        fail("a shader's bytes are given");
        return std::nullopt;
    }
    const std::span<const std::uint8_t> file(bytes, static_cast<std::size_t>(size));
    const std::size_t header = magic.size() + sizeof(std::uint32_t);
    if (file.size() < header || std::memcmp(file.data(), magic.data(), magic.size()) != 0) {
        fail("that isn't a cooked shader: a .slang file goes through fbengine_tools' CookShader, "
             "which a game's build step does");
        return std::nullopt;
    }
    std::uint32_t length = 0;
    std::memcpy(&length, file.subspan(magic.size()).data(), sizeof(length));
    if (length > file.size() - header) {
        fail("the cooked shader is cut short");
        return std::nullopt;
    }
    const std::string_view version(reinterpret_cast<const char*>(file.subspan(header).data()),
                                   length);
    if (version != FBE_VERSION) {
        fail(std::format("the shader is cooked for fbengine {}, and this is {}: cook it again",
                         version.substr(0, most_said), FBE_VERSION));
        return std::nullopt;
    }
    const std::span<const std::uint8_t> rest = file.subspan(header + length);
    if (rest.size() < stamp.size()) {
        fail("the cooked shader is cut short");
        return std::nullopt;
    }
    if (std::memcmp(rest.data(), stamp.data(), stamp.size()) != 0) {
        fail("the shader is cooked with other sources of fbengine's shaders: cook it again");
        return std::nullopt;
    }
    const std::span<const std::uint8_t> code = rest.subspan(stamp.size());
    if (code.size() % sizeof(std::uint32_t) != 0 || code.size() < 5 * sizeof(std::uint32_t)) {
        fail("the cooked shader's SPIR-V isn't whole");
        return std::nullopt;
    }
    std::vector<std::uint32_t> words(code.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != spirv_magic) {
        fail("the cooked shader's SPIR-V isn't SPIR-V");
        return std::nullopt;
    }
    for (const entry_t& entry : entries) {
        if (!has_entry(words, entry.name, entry.model)) {
            fail(std::format("the cooked shader's SPIR-V hasn't the scene's {}: cook it again",
                             entry.name));
            return std::nullopt;
        }
    }
    if (!binds_only_the_scene(words)) {
        fail(
            "the shader has what fbengine's pipelines don't bind, a texture or a global that isn't "
            "static: it reads only what a Surface and its material's parameters have");
        return std::nullopt;
    }
    return words;
}

/// The number of a checked file's words, kept unless they already are, or 0, with fbe_error saying
/// why.
std::uint32_t keep(std::vector<std::uint32_t> words, const char* name) {
    shaders_t& shaders = all();
    const auto found = std::ranges::find(shaders.codes, words, &kept_code_t::words);
    if (found != shaders.codes.end()) {
        return static_cast<std::uint32_t>(found - shaders.codes.begin()) + 1;
    }
    if (shaders.codes.size() >= most_codes) {
        fail("there have been more different shaders than a pipeline's key can number");
        return 0;
    }
    std::string named = name != nullptr ? name : "a game's shader";
    kept_code_t& kept = shaders.codes.emplace_back();
    kept.words = std::move(words);
    kept.name = std::move(named);
    kept.shader = { .name = kept.name, .code = kept.words };
    return static_cast<std::uint32_t>(shaders.codes.size());
}

} // namespace

std::uint32_t code_of(std::uint64_t shader) {
    if (shader == 0) {
        return 0;
    }
    const auto slot = all().slots.find(shader);
    return slot ? all().kept[*slot].code : 0;
}

const gpu::shader_t* shader_of(std::uint32_t code) {
    return code != 0 && code <= all().codes.size() && !all().codes[code - 1].refused
               ? &all().codes[code - 1].shader
               : nullptr;
}

void refuse(std::uint32_t code) {
    if (code != 0 && code <= all().codes.size() && !all().codes[code - 1].refused) {
        all().codes[code - 1].refused = true;
        // NOLINTNEXTLINE(modernize-use-std-print): that allocates, and needs MinGW's libstdc++exp.
        std::fprintf(stderr,
                     "fbengine: no pipeline could be made of %s, so what draws with it draws as a "
                     "material with no shader of its own does: %s\n",
                     all().codes[code - 1].name.c_str(), fbe_error());
    }
}

void release(std::uint64_t shader) {
    const auto slot = all().slots.find(shader);
    if (slot && --all().kept[*slot].references == 0) {
        all().slots.free(*slot);
    }
}

void retain(std::uint64_t shader) {
    const auto slot = all().slots.find(shader);
    if (slot) {
        ++all().kept[*slot].references;
    }
}

} // namespace fbe::shaders_own

using fbe::fail;
using fbe::scene::none;

namespace own = fbe::shaders_own;

extern "C" {

FBE_API const char* fbe_shader_stamp(void) {
    return FBE_SHADER_STAMP;
}

FBE_API bool fbe_shader_check(const uint8_t* bytes, int64_t size) try {
    return own::words_of(bytes, size).has_value();
} catch (const std::bad_alloc&) {
    return fail("out of memory");
}

FBE_API uint64_t fbe_shader_create(const uint8_t* bytes, int64_t size, const char* name) try {
    auto words = own::words_of(bytes, size);
    if (!words) {
        return 0;
    }
    const std::uint32_t code = own::keep(std::move(*words), name);
    if (code == 0) {
        return 0;
    }
    own::shaders_t& shaders = own::all();
    // Room first, as a slot made needs its place.
    shaders.kept.reserve(shaders.kept.size() + 1);
    const std::uint32_t slot = shaders.slots.make();
    if (slot == shaders.kept.size()) {
        shaders.kept.emplace_back();
    }
    shaders.kept[slot] = { .references = 1, .code = code };
    return shaders.slots.handle(slot);
} catch (const std::bad_alloc&) {
    fail("out of memory");
    return 0;
}

FBE_API bool fbe_shader_retain(uint64_t shader) {
    const std::uint32_t slot = own::slot_of(shader);
    if (slot == none) {
        return false;
    }
    ++own::all().kept[slot].references;
    return true;
}

FBE_API bool fbe_shader_release(uint64_t shader) {
    if (own::slot_of(shader) == none) {
        return false;
    }
    own::release(shader);
    return true;
}

FBE_API bool fbe_shader_alive(uint64_t shader) {
    return own::all().slots.find(shader).has_value();
}

FBE_API bool fbe_shader_replace(uint64_t shader, const uint8_t* bytes, int64_t size,
                                const char* name) try {
    const std::uint32_t slot = own::slot_of(shader);
    if (slot == none) {
        return false;
    }
    auto words = own::words_of(bytes, size);
    if (!words) {
        return false;
    }
    const std::uint32_t code = own::keep(std::move(*words), name);
    if (code == 0) {
        return false;
    }
    own::all().kept[slot].code = code;
    return true;
} catch (const std::bad_alloc&) {
    return fail("out of memory");
}

FBE_API bool fbe_material_set_shader(uint64_t material, uint64_t shader) {
    const std::uint32_t found = fbe::scene::material_slot(material);
    if (found == none || (shader != 0 && own::slot_of(shader) == none)) {
        return false;
    }
    fbe::scene::material_t& drawn = fbe::scene::scene().materials[found];
    if (shader != 0) {
        own::retain(shader);
    }
    if (drawn.shader != 0) {
        own::release(drawn.shader);
    }
    drawn.shader = shader;
    return true;
}

FBE_API uint64_t fbe_material_shader(uint64_t material) {
    const std::uint32_t found = fbe::scene::material_slot(material);
    return found != none ? fbe::scene::scene().materials[found].shader : 0;
}

FBE_API bool fbe_material_set_parameter(uint64_t material, int32_t index, float x, float y, float z,
                                        float w) {
    const std::uint32_t found = fbe::scene::material_slot(material);
    if (found == none) {
        return false;
    }
    if (index < 0 || index >= FBE_MATERIAL_PARAMETERS) {
        return fail("a material's parameters are 0 to 7");
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
        return fail("a material's parameters are numbers");
    }
    const std::array<float, 4> value{ x, y, z, w };
    fbe::scene::scene().materials[found].parameters.at(static_cast<std::size_t>(index)) = value;
    return true;
}

FBE_API bool fbe_material_parameter(uint64_t material, int32_t index, float* values) {
    const std::uint32_t found = fbe::scene::material_slot(material);
    if (found == none) {
        return false;
    }
    if (index < 0 || index >= FBE_MATERIAL_PARAMETERS || values == nullptr) {
        return fail("a material's parameters are 0 to 7");
    }
    std::ranges::copy(
        fbe::scene::scene().materials[found].parameters.at(static_cast<std::size_t>(index)),
        values);
    return true;
}

} // extern "C"
