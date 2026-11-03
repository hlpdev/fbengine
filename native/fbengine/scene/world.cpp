// Worlds and the tree of entities in each: making, freeing, naming, parenting, finding, hiding,
// and copying them.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/scene.hpp"
#include "skin/skin.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

/// The kinds fbe_entity_create takes.
bool valid_kind(std::int32_t kind) {
    return kind >= FBE_ENTITY_PIVOT && kind <= FBE_ENTITY_TERRAIN;
}

/// Where an entity sits: its parent's children, or its world's roots.
std::vector<std::uint32_t>& siblings_of(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    const std::uint32_t parent = entities.parent[entity];
    if (parent != none) {
        return entities.children[parent];
    }
    return scene().worlds[entities.world[entity]].roots;
}

/// Takes an entity out of its parent's children or its world's roots.
void detach(std::uint32_t entity) {
    std::erase(siblings_of(entity), entity);
    scene().entities.parent[entity] = none;
}

/// Puts an entity last among `parent`'s children, or `world`'s roots when it's none.
void attach(std::uint32_t entity, std::uint32_t world, std::uint32_t parent) {
    entities_t& entities = scene().entities;
    entities.parent[entity] = parent;
    if (parent != none) {
        entities.children[parent].push_back(entity);
    } else {
        scene().worlds[world].roots.push_back(entity);
    }
}

/// A new entity, with nothing set, not yet in the tree.
std::uint32_t make_entity(std::uint32_t world, std::int32_t kind) {
    entities_t& entities = scene().entities;
    const std::uint32_t entity = entities.slots.make();
    if (entity == entities.kind.size()) {
        entities.kind.push_back(kind);
        entities.world.push_back(world);
        entities.parent.push_back(none);
        entities.children.emplace_back();
        entities.name.emplace_back();
        entities.hidden.push_back(false);
        entities.position.emplace_back();
        entities.rotation.emplace_back();
        entities.scale.emplace_back();
        entities.world_matrix.emplace_back();
        entities.world_rotation.emplace_back();
        entities.world_scale.emplace_back();
        entities.moved.push_back(true);
        entities.component.push_back(none);
        entities.blocks_sight.push_back(false);
        entities.colliders.emplace_back();
        entities.pickable.push_back(none);
        entities.captured_position.emplace_back();
        entities.captured_rotation.emplace_back();
        entities.captured_scale.emplace_back();
        entities.captured.push_back(false);
        entities.fade_near.push_back(0.0F);
        entities.fade_far.push_back(0.0F);
        entities.made.push_back(0);
    } else {
        entities.kind[entity] = kind;
        entities.world[entity] = world;
        entities.parent[entity] = none;
        entities.children[entity].clear();
        entities.name[entity].clear();
        entities.hidden[entity] = false;
        entities.blocks_sight[entity] = false;
        entities.colliders[entity] = {};
        entities.captured[entity] = false;
        entities.fade_near[entity] = 0.0F;
        entities.fade_far[entity] = 0.0F;
    }
    entities.made[entity] = ++entities.mades;
    reset_transform(entity);
    add_component(entity);
    ++scene().worlds[world].count;
    return entity;
}

/// An entity and what's under it, depth first, each parent before its children, with each one's
/// parent's index in the list, by a stack of their own, so no tree is too deep.
std::vector<std::pair<std::uint32_t, std::size_t>> list_tree(std::uint32_t entity) {
    const entities_t& entities = scene().entities;
    std::vector<std::pair<std::uint32_t, std::size_t>> tree;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack{ { entity, 0 } };
    while (!stack.empty()) {
        const auto [at, parent] = stack.back();
        stack.pop_back();
        const std::size_t index = tree.size();
        tree.emplace_back(at, parent);
        const std::vector<std::uint32_t>& children = entities.children[at];
        for (const std::uint32_t child : std::views::reverse(children)) {
            stack.emplace_back(child, index);
        }
    }
    return tree;
}

/// Frees what list_tree listed, which is already out of the tree but for its first's own children.
void free_listed(const std::vector<std::pair<std::uint32_t, std::size_t>>& tree) {
    entities_t& entities = scene().entities;
    for (const auto& [at, parent] : tree) {
        entities.children[at].clear();
        entities.name[at].clear();
        entities.name[at].shrink_to_fit();
        entities.colliders[at] = {};
        pickable_changed(at);
        remove_component(at);
        --scene().worlds[entities.world[at]].count;
        scene().freed.push_back(entities.slots.handle(at));
        entities.slots.free(at);
    }
}

/// Frees an entity and what's under it, as free_listed does.
void free_tree(std::uint32_t entity) {
    free_listed(list_tree(entity));
}

/// Sets the world of an entity and what's under it, moving their counts.
void move_tree(std::uint32_t entity, std::uint32_t world) {
    entities_t& entities = scene().entities;
    for (const auto& [at, parent] : list_tree(entity)) {
        --scene().worlds[entities.world[at]].count;
        ++scene().worlds[world].count;
        entities.world[at] = world;
        pickable_changed(at);
    }
}

/// Gives a captured entity, moved under a new parent, the captured local transform that puts it
/// where it was at the capture, `was`, under its new parent's capture.
void recapture(std::uint32_t entity, const mat4_t& was) {
    entities_t& entities = scene().entities;
    const std::uint32_t parent = entities.parent[entity];
    mat4_t into;
    if (!inverse_affine(parent != none ? tweened_matrix(parent, 0.0F) : mat4_t(), into)) {
        return;
    }
    const mat4_t local = into * was;
    vec3_t x = local.axis(0);
    vec3_t y = local.axis(1);
    vec3_t z = local.axis(2);
    vec3_t scale(length(x), length(y), length(z));
    if (scale.x <= 0.0F || scale.y <= 0.0F || scale.z <= 0.0F) {
        return;
    }
    x = x * (1.0F / scale.x);
    y = y * (1.0F / scale.y);
    z = z * (1.0F / scale.z);
    // A mirror goes in the scale, so what's left is a turn.
    if (dot(cross(x, y), z) < 0.0F) {
        x = -x;
        scale.x = -scale.x;
    }
    entities.captured_position[entity] = local.axis(3);
    entities.captured_rotation[entity] = from_axes(x, y, z);
    entities.captured_scale[entity] = scale;
}

/// Whether `entity` is `ancestor` or under it.
bool under(std::uint32_t entity, std::uint32_t ancestor) {
    const entities_t& entities = scene().entities;
    for (std::uint32_t at = entity; at != none; at = entities.parent[at]) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

/// The first of `among` or what's under them, depth first, each parent before its children, with
/// the name.
std::uint32_t find_in(const std::vector<std::uint32_t>& among, const char* name) {
    if (name == nullptr) {
        return none;
    }
    const entities_t& entities = scene().entities;
    std::vector<std::uint32_t> stack(among.rbegin(), among.rend());
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        if (entities.name[at] == name) {
            return at;
        }
        const std::vector<std::uint32_t>& children = entities.children[at];
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    return none;
}

/// Writes up to `capacity` of `slots` as handles and gives how many there are.
std::int32_t write_handles(const slots_t& slots, const std::vector<std::uint32_t>& from,
                           std::uint64_t* to, std::int32_t capacity) {
    const std::size_t count =
        std::min(from.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        to[i] = slots.handle(from[i]);
    }
    return static_cast<std::int32_t>(from.size());
}

/// A new world, empty, in a new slot or a freed one, which keeps nothing of the world it held.
std::uint32_t make_world() {
    scene_t& s = scene();
    const std::uint32_t world = s.world_slots.make();
    if (world == s.worlds.size()) {
        s.worlds.emplace_back();
    } else {
        s.worlds[world] = {};
    }
    return world;
}

/// The main world's slot, made the first time it's asked for.
std::uint32_t main_world() {
    scene_t& s = scene();
    if (s.main_world == none) {
        s.main_world = make_world();
    }
    return s.main_world;
}

/// Where a new entity or copy goes: under `parent`, in its world, or at the top of `world`; a
/// nonzero `world` must be live and, with a parent, the parent's. False with fbe_error saying why.
bool place(std::uint64_t world, std::uint64_t parent, std::uint32_t& world_index,
           std::uint32_t& parent_slot) {
    parent_slot = none;
    world_index = none;
    if (world != 0) {
        world_index = world_slot(world);
        if (world_index == none) {
            return false;
        }
    }
    if (parent != 0) {
        parent_slot = entity_slot(parent);
        if (parent_slot == none) {
            return false;
        }
        const std::uint32_t parents = scene().entities.world[parent_slot];
        if (world_index != none && world_index != parents) {
            return fail("the parent is in another World");
        }
        world_index = parents;
    }
    return world_index != none || fail("no World or parent was given");
}

} // namespace

scene_t& scene() {
    // Never freed, as the job system isn't: on Linux and macOS another thread's exit() ends statics
    // with moves still reading it on the workers.
    static scene_t& the_scene = *new scene_t();
    return the_scene;
}

bool showing(std::uint32_t entity) {
    const entities_t& entities = scene().entities;
    for (std::uint32_t at = entity; at != none; at = entities.parent[at]) {
        if (entities.hidden[at]) {
            return false;
        }
    }
    return true;
}

std::uint32_t live_slot(const slots_t& slots, std::uint64_t handle, const char* what) {
    const std::optional<std::uint32_t> slot = slots.find(handle);
    if (slot) {
        return *slot;
    }
    if (handle == 0) {
        fail(std::format("no {} was given", what));
    } else if (kind_of(handle) != slots.kind()) {
        const bool vowel = std::string_view("aeiou").contains(what[0]);
        fail(std::format("that handle isn't {} {}'s", vowel ? "an" : "a", what));
    } else {
        fail(std::format("the {} was freed", what));
    }
    return none;
}

std::uint32_t entity_slot(std::uint64_t handle) {
    return live_slot(scene().entities.slots, handle, "entity");
}

std::uint32_t world_slot(std::uint64_t handle) {
    return live_slot(scene().world_slots, handle, "World");
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::world_slot;

extern "C" {

FBE_API uint64_t fbe_world_create(void) {
    return scene().world_slots.handle(fbe::scene::make_world());
}

FBE_API uint64_t fbe_world_main(void) {
    return scene().world_slots.handle(fbe::scene::main_world());
}

FBE_API bool fbe_world_free(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    if (slot == scene().main_world) {
        return fbe::fail("the main World stays; Clear empties it");
    }
    fbe_world_clear(world);
    scene().world_slots.free(slot);
    return true;
}

FBE_API bool fbe_world_clear(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    const std::vector<std::uint32_t> roots = std::move(scene().worlds[slot].roots);
    scene().worlds[slot].roots.clear();
    for (const std::uint32_t root : roots) {
        fbe::scene::free_tree(root);
    }
    return true;
}

FBE_API bool fbe_world_alive(uint64_t world) {
    return scene().world_slots.find(world).has_value();
}

FBE_API int32_t fbe_world_entity_count(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    return slot != none ? scene().worlds[slot].count : -1;
}

FBE_API int32_t fbe_world_roots(uint64_t world, uint64_t* roots, int32_t capacity) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return -1;
    }
    return fbe::scene::write_handles(scene().entities.slots, scene().worlds[slot].roots, roots,
                                     capacity);
}

FBE_API uint64_t fbe_world_find(uint64_t world, const char* name) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return 0;
    }
    const std::uint32_t found = fbe::scene::find_in(scene().worlds[slot].roots, name);
    return found != none ? scene().entities.slots.handle(found) : 0;
}

FBE_API uint64_t fbe_entity_create(uint64_t world, uint64_t parent, int32_t kind) {
    if (!fbe::scene::valid_kind(kind)) {
        fbe::fail("that isn't a kind of entity");
        return 0;
    }
    std::uint32_t parent_slot = none;
    std::uint32_t world_index = none;
    if (!fbe::scene::place(world, parent, world_index, parent_slot)) {
        return 0;
    }
    const std::uint32_t entity = fbe::scene::make_entity(world_index, kind);
    fbe::scene::attach(entity, world_index, parent_slot);
    return scene().entities.slots.handle(entity);
}

FBE_API bool fbe_entity_free(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    try {
        // All it allocates comes first, so running out of memory frees nothing.
        const auto tree = fbe::scene::list_tree(slot);
        std::vector<std::uint64_t>& freed = scene().freed;
        if (freed.capacity() - freed.size() < tree.size()) {
            freed.reserve(std::max(freed.capacity() * 2, freed.size() + tree.size()));
        }
        fbe::scene::detach(slot);
        fbe::scene::free_listed(tree);
        return true;
    } catch (const std::bad_alloc&) {
        return fbe::fail("out of memory");
    }
}

FBE_API bool fbe_entity_alive(uint64_t entity) {
    return scene().entities.slots.find(entity).has_value();
}

FBE_API int32_t fbe_entity_freed(uint64_t* entities, int32_t capacity) {
    std::vector<std::uint64_t>& freed = scene().freed;
    const std::size_t count =
        std::min(freed.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(freed.begin(), count, entities);
    freed.erase(freed.begin(), freed.begin() + static_cast<std::ptrdiff_t>(count));
    return static_cast<std::int32_t>(count);
}

FBE_API int32_t fbe_entity_kind(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? scene().entities.kind[slot] : -1;
}

FBE_API uint64_t fbe_entity_world(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? scene().world_slots.handle(scene().entities.world[slot]) : 0;
}

FBE_API bool fbe_entity_set_name(uint64_t entity, const char* name) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    scene().entities.name[slot] = name != nullptr ? name : "";
    return true;
}

FBE_API const char* fbe_entity_name(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? scene().entities.name[slot].c_str() : nullptr;
}

FBE_API bool fbe_entity_set_parent(uint64_t entity, uint64_t parent, bool keep_world) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    fbe::scene::entities_t& entities = scene().entities;
    std::uint32_t parent_slot = none;
    std::uint32_t world = entities.world[slot];
    if (parent != 0) {
        parent_slot = entity_slot(parent);
        if (parent_slot == none) {
            return false;
        }
        if (fbe::scene::under(parent_slot, slot)) {
            return fbe::fail(parent_slot == slot ? "an entity can't be its own parent"
                                                 : "an entity can't go under what's under it");
        }
        world = entities.world[parent_slot];
    }
    if (entities.parent[slot] == parent_slot && entities.world[slot] == world) {
        return true;
    }
    fbe::scene::update_world(slot);
    const fbe::scene::vec3_t position = entities.world_matrix[slot].axis(3);
    const fbe::scene::quat_t rotation = entities.world_rotation[slot];
    const fbe::scene::vec3_t scale = entities.world_scale[slot];
    // Where it was at its World's capture, which drawing still tweens from.
    const bool captured = entities.captured[slot];
    const fbe::scene::mat4_t was =
        captured ? fbe::scene::tweened_matrix(slot, 0.0F) : fbe::scene::mat4_t();
    fbe::scene::detach(slot);
    if (world != entities.world[slot]) {
        fbe::scene::move_tree(slot, world);
    }
    fbe::scene::attach(slot, world, parent_slot);
    fbe::scene::mark_moved(slot);
    // A jump to where a new parent takes it isn't a move to sweep.
    if (!keep_world) {
        entities.colliders[slot].has_previous = false;
    }
    if (keep_world) {
        fbe::scene::set_world_rotation(slot, rotation);
        fbe::scene::set_world_scale(slot, scale);
        fbe::scene::set_world_position(slot, position);
    }
    if (captured) {
        fbe::scene::recapture(slot, was);
    }
    return true;
}

FBE_API bool fbe_entity_set_sibling_index(uint64_t entity, int32_t index) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    std::vector<std::uint32_t>& siblings = fbe::scene::siblings_of(slot);
    if (index < 0 || static_cast<std::size_t>(index) >= siblings.size()) {
        return fbe::fail(std::format("a place among {} siblings is 0 to {}, not {}",
                                     siblings.size(), siblings.size() - 1, index));
    }
    std::erase(siblings, slot);
    siblings.insert(siblings.begin() + index, slot);
    return true;
}

FBE_API uint64_t fbe_entity_parent(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return 0;
    }
    const std::uint32_t parent = scene().entities.parent[slot];
    return parent != none ? scene().entities.slots.handle(parent) : 0;
}

FBE_API int32_t fbe_entity_child_count(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? static_cast<std::int32_t>(scene().entities.children[slot].size()) : -1;
}

FBE_API uint64_t fbe_entity_child(uint64_t entity, int32_t index) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return 0;
    }
    const std::vector<std::uint32_t>& children = scene().entities.children[slot];
    if (index < 0 || static_cast<std::size_t>(index) >= children.size()) {
        fbe::fail("there's no child there");
        return 0;
    }
    return scene().entities.slots.handle(children[static_cast<std::size_t>(index)]);
}

FBE_API int32_t fbe_entity_children(uint64_t entity, uint64_t* children, int32_t capacity) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return -1;
    }
    return fbe::scene::write_handles(scene().entities.slots, scene().entities.children[slot],
                                     children, capacity);
}

FBE_API uint64_t fbe_entity_find(uint64_t entity, const char* name) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return 0;
    }
    const std::uint32_t found = fbe::scene::find_in(scene().entities.children[slot], name);
    return found != none ? scene().entities.slots.handle(found) : 0;
}

FBE_API bool fbe_entity_set_hidden(uint64_t entity, bool hidden) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    scene().entities.hidden[slot] = hidden;
    return true;
}

FBE_API bool fbe_entity_hidden(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none && scene().entities.hidden[slot];
}

FBE_API bool fbe_entity_showing(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none && fbe::scene::showing(slot);
}

FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return 0;
    }
    fbe::scene::entities_t& entities = scene().entities;
    std::uint32_t parent_slot = none;
    std::uint32_t world_index = none;
    if (world == 0 && parent == 0) {
        world_index = entities.world[slot];
    } else if (!fbe::scene::place(world, parent, world_index, parent_slot)) {
        return 0;
    }

    // The whole tree is listed first, so a copy put under its own original isn't copied again.
    const std::vector<std::pair<std::uint32_t, std::size_t>> tree = fbe::scene::list_tree(slot);
    std::vector<std::uint32_t> copies(tree.size(), none);
    for (std::size_t i = 0; i < tree.size(); ++i) {
        const std::uint32_t original = tree[i].first;
        const std::uint32_t copy = fbe::scene::make_entity(world_index, entities.kind[original]);
        entities.name[copy] = entities.name[original];
        entities.hidden[copy] = entities.hidden[original];
        entities.position[copy] = entities.position[original];
        entities.rotation[copy] = entities.rotation[original];
        entities.scale[copy] = entities.scale[original];
        entities.blocks_sight[copy] = entities.blocks_sight[original];
        entities.fade_near[copy] = entities.fade_near[original];
        entities.fade_far[copy] = entities.fade_far[original];
        // A copy collides as its original does, from where it is, with nothing touched yet.
        entities.colliders[copy] = entities.colliders[original];
        entities.colliders[copy].has_previous = false;
        entities.colliders[copy].contacts.clear();
        fbe::scene::pickable_changed(copy);
        fbe::scene::copy_component(original, copy);
        fbe::scene::attach(copy, world_index, i == 0 ? parent_slot : copies[tree[i].second]);
        copies[i] = copy;
    }
    fbe::scene::renumber_cameras(tree, copies);
    // Made anew in the order their originals were, so what goes by that goes as it did for them.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> made;
    made.reserve(copies.size());
    for (std::size_t i = 0; i < copies.size(); ++i) {
        made.emplace_back(entities.made[tree[i].first], copies[i]);
    }
    std::ranges::sort(made);
    for (const auto& [when, copy] : made) {
        entities.made[copy] = ++entities.mades;
    }
    // Its meshes skinned as their originals, once the whole tree is there to find their roots in.
    fbe::anim::copy_skins(tree, copies);
    return entities.slots.handle(copies[0]);
}

} // extern "C"
