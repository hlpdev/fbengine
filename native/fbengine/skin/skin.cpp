// Skins' C API: joints by name with their inverse binds, shared by the meshes skinned by them, each
// mesh's joints bound to entities under its root, and the palette it's drawn with.

#include "skin/skin.hpp"

#include "assets/loads.hpp"
#include "assets/model.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::anim {

namespace {

/// A skin: its joints' names and inverse binds, what holds it, and how many times it changed, so a
/// mesh bound to it knows to bind again.
struct skin_t {
    std::vector<std::string> joints;
    std::vector<scene::mat4_t> inverse_binds;
    std::uint32_t references = 0;
    std::uint64_t version = 0;
};

/// A mesh skinned by a skin, which it holds: the root its joints are found under, the skin's
/// version they were bound at, the entity of each joint, 0 for none, and its palette as it was
/// last worked out.
struct skinned_t {
    std::uint64_t mesh = 0;
    std::uint32_t skin = scene::none;
    std::uint64_t root = 0;
    std::uint64_t version = 0;
    std::vector<std::uint64_t> joints;
    std::vector<scene::mat4_t> palette;
};

struct skins_t {
    handle_slots_t slots{ SKIN_KIND };
    std::vector<skin_t> kept;
    std::vector<skinned_t> skinned;
    /// Whether the last skin from a model was refused for what's in its file.
    bool model_invalid = false;
};

skins_t& skins() {
    static skins_t made;
    return made;
}

std::uint32_t skin_slot(std::uint64_t handle) {
    return scene::live_slot(skins().slots, handle, "skin");
}

void release_skin(std::uint32_t slot) {
    skin_t& found = skins().kept[slot];
    if (--found.references == 0) {
        found = {};
        skins().slots.free(slot);
    }
}

/// The live mesh entity a handle is for, or none, with fbe_error saying so.
std::uint32_t mesh_slot(std::uint64_t handle) {
    const std::uint32_t slot = scene::entity_slot(handle);
    if (slot != scene::none && scene::scene().entities.kind[slot] != FBE_ENTITY_MESH) {
        fail("the entity isn't a mesh");
        return scene::none;
    }
    return slot;
}

skinned_t* skinned_of(std::uint64_t mesh) {
    std::vector<skinned_t>& skinned = skins().skinned;
    const auto found =
        std::ranges::find_if(skinned, [&](const skinned_t& each) { return each.mesh == mesh; });
    return found != skinned.end() ? &*found : nullptr;
}

/// Binds a skinned mesh's joints to the entities of their names under its root, if it's live.
void bind(skinned_t& skinned) {
    const skin_t& skin = skins().kept[skinned.skin];
    skinned.joints.assign(skin.joints.size(), 0);
    skinned.version = skin.version;
    const auto root = scene::scene().entities.slots.find(skinned.root);
    if (!root) {
        return;
    }
    const std::string& named = scene::scene().entities.name[*root];
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        skinned.joints[i] = named == skin.joints[i]
                                ? skinned.root
                                : fbe_entity_find(skinned.root, skin.joints[i].c_str());
    }
}

} // namespace

void copy_skins(const std::vector<std::pair<std::uint32_t, std::size_t>>& originals,
                const std::vector<std::uint32_t>& copies) {
    const scene::entities_t& entities = scene::scene().entities;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        // Run out of memory, a copy is skinned by nothing, and fbe_error says why.
        try {
            const skinned_t* original = skinned_of(entities.slots.handle(originals[i].first));
            if (original == nullptr) {
                continue;
            }
            skinned_t copy;
            copy.mesh = entities.slots.handle(copies[i]);
            copy.skin = original->skin;
            copy.root = original->root;
            const auto root = entities.slots.find(original->root);
            const auto copied = std::ranges::find_if(
                originals, [&](const auto& entity) { return root && entity.first == *root; });
            if (copied != originals.end()) {
                copy.root = entities.slots.handle(
                    copies[static_cast<std::size_t>(copied - originals.begin())]);
            }
            bind(copy);
            skins().skinned.push_back(std::move(copy));
            ++skins().kept[skins().skinned.back().skin].references;
        } catch (const std::bad_alloc&) {
            fail("out of memory");
        }
    }
}

void forget_skin(std::uint32_t mesh) {
    const std::uint64_t handle = scene::scene().entities.slots.handle(mesh);
    std::erase_if(skins().skinned, [&](const skinned_t& each) {
        if (each.mesh != handle) {
            return false;
        }
        release_skin(each.skin);
        return true;
    });
}

bool skin_joints_box(std::uint32_t mesh, const scene::tweened_t& tweened, scene::vec3_t& min,
                     scene::vec3_t& max) {
    skinned_t* found = skinned_of(scene::scene().entities.slots.handle(mesh));
    if (found == nullptr) {
        return false;
    }
    if (found->version != skins().kept[found->skin].version) {
        bind(*found);
    }
    const scene::entities_t& entities = scene::scene().entities;
    bool any = false;
    for (const std::uint64_t handle : found->joints) {
        const auto joint = entities.slots.find(handle);
        if (!joint) {
            continue;
        }
        const scene::vec3_t at = tweened.pose(*joint).matrix.axis(3);
        min = any ? scene::vec3_t(std::fmin(min.x, at.x), std::fmin(min.y, at.y),
                                  std::fmin(min.z, at.z))
                  : at;
        max = any ? scene::vec3_t(std::fmax(max.x, at.x), std::fmax(max.y, at.y),
                                  std::fmax(max.z, at.z))
                  : at;
        any = true;
    }
    return any;
}

std::span<const scene::mat4_t> skin_palette(std::uint32_t mesh, const scene::tweened_t& tweened) {
    skinned_t* found = skinned_of(scene::scene().entities.slots.handle(mesh));
    if (found == nullptr) {
        return {};
    }
    const skin_t& skin = skins().kept[found->skin];
    if (found->version != skin.version) {
        bind(*found);
    }
    const scene::entities_t& entities = scene::scene().entities;
    // From the world to the mesh's space; one scaled to nothing has none, and is drawn as nothing.
    scene::mat4_t to_mesh;
    if (!scene::inverse_affine(tweened.pose(mesh).matrix, to_mesh)) {
        to_mesh = scene::mat4_t(scene::vec3_t{}, scene::vec3_t{}, scene::vec3_t{}, scene::vec3_t{});
    }
    found->palette.assign(skin.joints.size(), scene::mat4_t{});
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const auto joint = entities.slots.find(found->joints[i]);
        if (joint) {
            found->palette[i] = to_mesh * tweened.pose(*joint).matrix * skin.inverse_binds[i];
        }
    }
    return found->palette;
}

} // namespace fbe::anim

using fbe::fail;
using fbe::anim::skin_slot;
using fbe::anim::skins;
using fbe::scene::mat4_t;
using fbe::scene::none;

extern "C" {

FBE_API uint64_t fbe_skin_create(void) {
    try {
        auto& made = skins();
        const std::uint32_t slot = made.slots.make();
        try {
            if (slot == made.kept.size()) {
                made.kept.emplace_back();
            } else {
                made.kept[slot] = {};
            }
        } catch (...) {
            made.slots.free(slot);
            throw;
        }
        made.kept[slot].references = 1;
        return made.slots.handle(slot);
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return 0;
    }
}

FBE_API bool fbe_skin_model_invalid(void) {
    return skins().model_invalid;
}

FBE_API uint64_t fbe_skin_from_model(uint64_t load, int32_t index) {
    skins().model_invalid = false;
    const fbe::assets::load_t* found = fbe::assets::loads().get(load);
    if (found == nullptr || !found->done || found->error ||
        found->kind != fbe::assets::load_kind_t::MODEL) {
        fail("the load isn't a model load done and loaded");
        return 0;
    }
    const fbe::assets::model_t& model = found->model;
    if (index < 0 || std::cmp_greater_equal(index, model.skins.size())) {
        fail(std::format("the model has {} skins; there's no skin {}", model.skins.size(), index));
        return 0;
    }
    const fbe::assets::skin_t& from = model.skins[static_cast<std::size_t>(index)];
    // What's in the file: refused, and said to be why.
    const auto invalid = [](const std::string& why) {
        skins().model_invalid = true;
        fail(why);
        return std::uint64_t{ 0 };
    };
    if (from.inverse_binds.size() != from.joints.size()) {
        return invalid("the skin has an inverse bind for each joint");
    }
    try {
        fbe::anim::skin_t made;
        for (std::size_t i = 0; i < from.joints.size(); ++i) {
            if (from.joints[i] >= model.nodes.size()) {
                return invalid(std::format("the skin's joint {} is no node of the model's", i));
            }
            const std::string& named = model.nodes[from.joints[i]].name;
            if (named.empty()) {
                return invalid(std::format("the skin's joint {} is a node with no name", i));
            }
            if (std::ranges::find(made.joints, named) != made.joints.end()) {
                return invalid(std::format("the skin has two joints named \"{}\"", named));
            }
            if (!std::ranges::all_of(from.inverse_binds[i],
                                     [](float value) { return std::isfinite(value); })) {
                return invalid(
                    std::format("the skin's joint {}'s inverse bind matrix is numbers", i));
            }
            made.joints.push_back(named);
            mat4_t matrix;
            std::ranges::copy(from.inverse_binds[i], matrix.m.begin());
            made.inverse_binds.push_back(matrix);
        }
        const std::uint64_t handle = fbe_skin_create();
        fbe::anim::skin_t& kept = skins().kept[static_cast<std::uint32_t>(handle)];
        kept.joints = std::move(made.joints);
        kept.inverse_binds = std::move(made.inverse_binds);
        return handle;
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return 0;
    }
}

FBE_API bool fbe_skin_retain(uint64_t skin) {
    const std::uint32_t slot = skin_slot(skin);
    if (slot == none) {
        return false;
    }
    ++skins().kept[slot].references;
    return true;
}

FBE_API bool fbe_skin_release(uint64_t skin) {
    const std::uint32_t slot = skin_slot(skin);
    if (slot == none) {
        return false;
    }
    fbe::anim::release_skin(slot);
    return true;
}

FBE_API bool fbe_skin_alive(uint64_t skin) {
    return skins().slots.find(skin).has_value();
}

FBE_API bool fbe_skin_add_joint(uint64_t skin, const char* name, const float* inverse_bind) {
    const std::uint32_t slot = skin_slot(skin);
    if (slot == none) {
        return false;
    }
    fbe::anim::skin_t& found = skins().kept[slot];
    if (name == nullptr || *name == '\0') {
        return fail("a joint has a name");
    }
    const std::string_view named = name;
    if (std::ranges::contains(found.joints, named)) {
        return fail(std::format("the skin has a joint \"{}\" already", named));
    }
    if (inverse_bind == nullptr) {
        return fail("a joint has an inverse bind matrix");
    }
    const std::span<const float> given(inverse_bind, 16);
    if (!std::ranges::all_of(given, [](float value) { return std::isfinite(value); })) {
        return fail("a joint's inverse bind matrix is numbers");
    }
    mat4_t matrix;
    std::ranges::copy(given, matrix.m.begin());
    try {
        found.inverse_binds.push_back(matrix);
        try {
            found.joints.emplace_back(named);
        } catch (...) {
            found.inverse_binds.pop_back();
            throw;
        }
        ++found.version;
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API int32_t fbe_skin_joint_count(uint64_t skin) {
    const std::uint32_t slot = skin_slot(skin);
    return slot != none ? static_cast<int32_t>(skins().kept[slot].joints.size()) : 0;
}

FBE_API const char* fbe_skin_joint_name(uint64_t skin, int32_t joint) {
    const std::uint32_t slot = skin_slot(skin);
    if (slot == none) {
        return "";
    }
    const std::vector<std::string>& joints = skins().kept[slot].joints;
    if (joint < 0 || static_cast<std::size_t>(joint) >= joints.size()) {
        fail(std::format("the skin has {} joints; there's no joint {}", joints.size(), joint));
        return "";
    }
    return joints[static_cast<std::size_t>(joint)].c_str();
}

FBE_API bool fbe_skin_attach(uint64_t mesh, uint64_t skin, uint64_t root) {
    if (fbe::anim::mesh_slot(mesh) == none) {
        return false;
    }
    std::uint32_t slot = none;
    if (skin != 0) {
        slot = skin_slot(skin);
        if (slot == none || fbe::scene::entity_slot(root) == none) {
            return false;
        }
    }
    fbe::anim::skinned_t* found = fbe::anim::skinned_of(mesh);
    if (slot == none) {
        if (found != nullptr) {
            fbe::anim::release_skin(found->skin);
            std::erase_if(skins().skinned,
                          [&](const fbe::anim::skinned_t& each) { return each.mesh == mesh; });
        }
        return true;
    }
    try {
        // All that can run out of memory first, so running out leaves the mesh as it was.
        fbe::anim::skinned_t made;
        made.mesh = mesh;
        made.skin = slot;
        made.root = root;
        fbe::anim::bind(made);
        if (found == nullptr) {
            skins().skinned.push_back(std::move(made));
            ++skins().kept[slot].references;
        } else {
            // Held first, as it can be the one let go of.
            ++skins().kept[slot].references;
            fbe::anim::release_skin(found->skin);
            *found = std::move(made);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API uint64_t fbe_skin_of(uint64_t mesh) {
    if (fbe::anim::mesh_slot(mesh) == none) {
        return 0;
    }
    const fbe::anim::skinned_t* found = fbe::anim::skinned_of(mesh);
    return found != nullptr ? skins().slots.handle(found->skin) : 0;
}

FBE_API int32_t fbe_skin_palette(uint64_t mesh, float* matrices, int32_t capacity) {
    const std::uint32_t slot = fbe::anim::mesh_slot(mesh);
    if (slot == none) {
        return -1;
    }
    const fbe::anim::skinned_t* found = fbe::anim::skinned_of(mesh);
    if (found == nullptr) {
        fail("the mesh isn't skinned");
        return -1;
    }
    const std::size_t count = skins().kept[found->skin].joints.size();
    if (std::cmp_less(capacity, count) || (matrices == nullptr && count > 0)) {
        fail(std::format("the mesh's palette is {} joints", count));
        return -1;
    }
    try {
        const std::span<const mat4_t> palette =
            fbe::anim::skin_palette(slot, fbe::scene::tweened_t(1.0F));
        const std::span<float> out(matrices, matrices != nullptr ? count * 16 : 0);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            std::ranges::copy(palette[i].m, out.subspan(i * 16, 16).begin());
        }
        return static_cast<int32_t>(count);
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return -1;
    }
}

} // extern "C"
