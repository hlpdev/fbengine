// Skins' C API: joints by name with their inverse binds, shared by the meshes skinned by them, each
// mesh's joints bound to entities under its root, and the palette it's drawn with.

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
/// version they were bound at, and the entity of each joint, 0 for none.
struct skinned_t {
    std::uint64_t mesh = 0;
    std::uint32_t skin = scene::none;
    std::uint64_t root = 0;
    std::uint64_t version = 0;
    std::vector<std::uint64_t> joints;
};

struct skins_t {
    handle_slots_t slots{ SKIN_KIND };
    std::vector<skin_t> kept;
    std::vector<skinned_t> skinned;
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

/// Lets go of the skins of meshes freed since, so they don't keep them.
void prune() {
    std::vector<skinned_t>& skinned = skins().skinned;
    const auto gone = std::ranges::remove_if(skinned, [](const skinned_t& each) {
        if (scene::scene().entities.slots.find(each.mesh)) {
            return false;
        }
        release_skin(each.skin);
        return true;
    });
    skinned.erase(gone.begin(), gone.end());
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

} // namespace fbe::anim

using fbe::fail;
using fbe::anim::skin_slot;
using fbe::anim::skins;
using fbe::scene::mat4_t;
using fbe::scene::none;

extern "C" {

FBE_API uint64_t fbe_skin_create(void) {
    fbe::anim::prune();
    auto& made = skins();
    const std::uint32_t slot = made.slots.make();
    if (slot == made.kept.size()) {
        made.kept.emplace_back();
    } else {
        made.kept[slot] = {};
    }
    made.kept[slot].references = 1;
    return made.slots.handle(slot);
}

FBE_API uint64_t fbe_skin_from_model(uint64_t load, int32_t index) {
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
    if (from.inverse_binds.size() != from.joints.size()) {
        fail("the skin has an inverse bind for each joint");
        return 0;
    }
    try {
        fbe::anim::skin_t made;
        for (std::size_t i = 0; i < from.joints.size(); ++i) {
            if (from.joints[i] >= model.nodes.size()) {
                fail(std::format("the skin's joint {} is no node of the model's", i));
                return 0;
            }
            const std::string& named = model.nodes[from.joints[i]].name;
            if (named.empty()) {
                fail(std::format("the skin's joint {} is a node with no name", i));
                return 0;
            }
            if (std::ranges::find(made.joints, named) != made.joints.end()) {
                fail(std::format("the skin has two joints named \"{}\"", named));
                return 0;
            }
            if (!std::ranges::all_of(from.inverse_binds[i],
                                     [](float value) { return std::isfinite(value); })) {
                fail(std::format("the skin's joint {}'s inverse bind matrix is numbers", i));
                return 0;
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
    const std::string named = name != nullptr ? name : "";
    if (named.empty()) {
        return fail("a joint has a name");
    }
    if (std::ranges::find(found.joints, named) != found.joints.end()) {
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
    found.joints.push_back(named);
    found.inverse_binds.push_back(matrix);
    ++found.version;
    return true;
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
    fbe::anim::prune();
    std::uint32_t slot = none;
    if (skin != 0) {
        slot = skin_slot(skin);
        if (slot == none || fbe::scene::entity_slot(root) == none) {
            return false;
        }
        ++skins().kept[slot].references;
    }
    fbe::anim::skinned_t* found = fbe::anim::skinned_of(mesh);
    if (found != nullptr) {
        fbe::anim::release_skin(found->skin);
        if (slot == none) {
            std::erase_if(skins().skinned,
                          [&](const fbe::anim::skinned_t& each) { return each.mesh == mesh; });
            return true;
        }
    } else if (slot == none) {
        return true;
    } else {
        found = &skins().skinned.emplace_back();
        found->mesh = mesh;
    }
    found->skin = slot;
    found->root = root;
    fbe::anim::bind(*found);
    return true;
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
    fbe::anim::skinned_t* found = fbe::anim::skinned_of(mesh);
    if (found == nullptr) {
        fail("the mesh isn't skinned");
        return -1;
    }
    const fbe::anim::skin_t& skin = skins().kept[found->skin];
    if (found->version != skin.version) {
        fbe::anim::bind(*found);
    }
    const std::size_t count = skin.joints.size();
    if (std::cmp_less(capacity, count) || (matrices == nullptr && count > 0)) {
        fail(std::format("the mesh's palette is {} joints", count));
        return -1;
    }
    fbe::scene::entities_t& entities = fbe::scene::scene().entities;
    fbe::scene::update_world(slot);
    // From the world to the mesh's space; one scaled to nothing has none, and is drawn as nothing.
    mat4_t to_mesh;
    if (!fbe::scene::inverse_affine(entities.world_matrix[slot], to_mesh)) {
        to_mesh = mat4_t(fbe::scene::vec3_t{}, fbe::scene::vec3_t{}, fbe::scene::vec3_t{},
                         fbe::scene::vec3_t{});
    }
    const std::span<float> out(matrices, matrices != nullptr ? count * 16 : 0);
    for (std::size_t i = 0; i < count; ++i) {
        const auto joint = entities.slots.find(found->joints[i]);
        mat4_t matrix;
        if (joint) {
            fbe::scene::update_world(*joint);
            matrix = to_mesh * entities.world_matrix[*joint] * skin.inverse_binds[i];
        }
        std::ranges::copy(matrix.m, out.subspan(i * 16, 16).begin());
    }
    return static_cast<int32_t>(count);
}

} // extern "C"
