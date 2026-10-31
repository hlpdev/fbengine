// Worlds and the tree of entities in each: making, freeing, naming, parenting, finding, hiding,
// and copying them.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

/// The kinds fbe_entity_create takes.
bool valid_kind(std::int32_t kind) {
    return kind == FBE_ENTITY_PIVOT || kind == FBE_ENTITY_CAMERA;
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
    } else {
        entities.kind[entity] = kind;
        entities.world[entity] = world;
        entities.parent[entity] = none;
        entities.children[entity].clear();
        entities.name[entity].clear();
        entities.hidden[entity] = false;
    }
    reset_transform(entity);
    add_component(entity);
    ++scene().worlds[world].count;
    return entity;
}

/// Frees an entity and what's under it, which are already out of the tree but for the entity's
/// own children.
void free_tree(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    for (const std::uint32_t child : entities.children[entity]) {
        free_tree(child);
    }
    entities.children[entity].clear();
    entities.name[entity].clear();
    entities.name[entity].shrink_to_fit();
    remove_component(entity);
    --scene().worlds[entities.world[entity]].count;
    scene().freed.push_back(entities.slots.handle(entity));
    entities.slots.free(entity);
}

/// Sets the world of an entity and what's under it, moving their counts.
void move_tree(std::uint32_t entity, std::uint32_t world) {
    entities_t& entities = scene().entities;
    --scene().worlds[entities.world[entity]].count;
    ++scene().worlds[world].count;
    entities.world[entity] = world;
    for (const std::uint32_t child : entities.children[entity]) {
        move_tree(child, world);
    }
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

/// The first of `entities` or what's under them, depth first, with the name.
std::uint32_t find_in(const std::vector<std::uint32_t>& among, const char* name) {
    if (name == nullptr) {
        return none;
    }
    const entities_t& entities = scene().entities;
    for (const std::uint32_t entity : among) {
        if (entities.name[entity] == name) {
            return entity;
        }
        const std::uint32_t found = find_in(entities.children[entity], name);
        if (found != none) {
            return found;
        }
    }
    return none;
}

/// An entity and what's under it in order, each with its parent's index in the list.
void list_tree(std::uint32_t entity, std::size_t parent,
               std::vector<std::pair<std::uint32_t, std::size_t>>& tree) {
    const std::size_t index = tree.size();
    tree.emplace_back(entity, parent);
    for (const std::uint32_t child : scene().entities.children[entity]) {
        list_tree(child, index, tree);
    }
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

/// A world's slot, made the first time for the main one.
std::uint32_t main_world() {
    scene_t& s = scene();
    if (s.main_world == none) {
        s.main_world = s.world_slots.make();
        if (s.main_world == s.worlds.size()) {
            s.worlds.emplace_back();
        }
    }
    return s.main_world;
}

} // namespace

std::uint32_t slots_t::make() {
    if (!m_free.empty()) {
        const std::uint32_t slot = m_free.back();
        m_free.pop_back();
        m_live[slot] = true;
        return slot;
    }
    m_generations.push_back(1);
    m_live.push_back(true);
    return static_cast<std::uint32_t>(m_generations.size() - 1);
}

void slots_t::free(std::uint32_t slot) {
    m_live[slot] = false;
    // A slot whose generation would wrap is never used again, so no old handle comes back.
    if (++m_generations[slot] != 0) {
        m_free.push_back(slot);
    }
}

std::optional<std::uint32_t> slots_t::find(std::uint64_t handle) const {
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32U);
    if (slot >= m_generations.size() || !m_live[slot] || m_generations[slot] != generation) {
        return std::nullopt;
    }
    return slot;
}

std::uint64_t slots_t::handle(std::uint32_t slot) const {
    return (static_cast<std::uint64_t>(m_generations[slot]) << 32U) | slot;
}

std::uint32_t slots_t::size() const {
    return static_cast<std::uint32_t>(m_generations.size());
}

scene_t& scene() {
    static scene_t the_scene;
    return the_scene;
}

std::uint32_t entity_slot(std::uint64_t handle) {
    const std::optional<std::uint32_t> slot = scene().entities.slots.find(handle);
    if (!slot) {
        fail(handle == 0 ? "no entity was given" : "the entity was freed");
        return none;
    }
    return *slot;
}

std::uint32_t world_slot(std::uint64_t handle) {
    const std::optional<std::uint32_t> slot = scene().world_slots.find(handle);
    if (!slot) {
        fail(handle == 0 ? "no World was given" : "the World was freed");
        return none;
    }
    return *slot;
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::world_slot;

extern "C" {

FBE_API uint64_t fbe_world_create(void) {
    fbe::scene::scene_t& s = scene();
    const std::uint32_t world = s.world_slots.make();
    if (world == s.worlds.size()) {
        s.worlds.emplace_back();
    } else {
        s.worlds[world] = {};
    }
    return s.world_slots.handle(world);
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
    if (parent != 0) {
        parent_slot = entity_slot(parent);
        if (parent_slot == none) {
            return 0;
        }
        world_index = scene().entities.world[parent_slot];
    } else {
        world_index = world_slot(world);
        if (world_index == none) {
            return 0;
        }
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
    fbe::scene::detach(slot);
    fbe::scene::free_tree(slot);
    return true;
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
    fbe::scene::detach(slot);
    if (world != entities.world[slot]) {
        fbe::scene::move_tree(slot, world);
    }
    fbe::scene::attach(slot, world, parent_slot);
    fbe::scene::mark_moved(slot);
    if (keep_world) {
        fbe::scene::set_world_rotation(slot, rotation);
        fbe::scene::set_world_scale(slot, scale);
        fbe::scene::set_world_position(slot, position);
    }
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
    if (slot == none) {
        return false;
    }
    const fbe::scene::entities_t& entities = scene().entities;
    for (std::uint32_t at = slot; at != none; at = entities.parent[at]) {
        if (entities.hidden[at]) {
            return false;
        }
    }
    return true;
}

FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return 0;
    }
    fbe::scene::entities_t& entities = scene().entities;
    std::uint32_t parent_slot = none;
    std::uint32_t world_index = entities.world[slot];
    if (parent != 0) {
        parent_slot = entity_slot(parent);
        if (parent_slot == none) {
            return 0;
        }
        world_index = entities.world[parent_slot];
    } else if (world != 0) {
        world_index = world_slot(world);
        if (world_index == none) {
            return 0;
        }
    }

    // The whole tree is listed first, so a copy put under its own original isn't copied again.
    std::vector<std::pair<std::uint32_t, std::size_t>> tree;
    fbe::scene::list_tree(slot, 0, tree);
    std::vector<std::uint32_t> copies(tree.size(), none);
    for (std::size_t i = 0; i < tree.size(); ++i) {
        const std::uint32_t original = tree[i].first;
        const std::uint32_t copy = fbe::scene::make_entity(world_index, entities.kind[original]);
        entities.name[copy] = entities.name[original];
        entities.hidden[copy] = entities.hidden[original];
        entities.position[copy] = entities.position[original];
        entities.rotation[copy] = entities.rotation[original];
        entities.scale[copy] = entities.scale[original];
        fbe::scene::copy_component(original, copy);
        fbe::scene::attach(copy, world_index, i == 0 ? parent_slot : copies[tree[i].second]);
        copies[i] = copy;
    }
    return entities.slots.handle(copies[0]);
}

} // extern "C"
