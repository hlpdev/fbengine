// Players' C API: a clip played on an entity and what's under it, its nodes bound to entities by
// name, posed at a time, and moved on by a world's update.

#include "clip/clip.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace fbe::anim {

namespace {

/// What one of a clip's channels moves: the entity its node's name found, 0 for none, and the key
/// it sampled last.
struct bound_t {
    std::uint64_t entity = 0;
    std::size_t cursor = 0;
};

/// A player: its entity, 0 for a free slot, the clip it plays, which it holds, the clip's version
/// its channels were bound at, and where it is in it.
struct player_t {
    std::uint64_t entity = 0;
    std::uint32_t clip = scene::none;
    std::uint64_t version = 0;
    float time = 0.0F;
    std::vector<bound_t> bound;
};

struct players_t {
    handle_slots_t slots{ PLAYER_KIND };
    std::vector<player_t> kept;
};

players_t& players() {
    static players_t made;
    return made;
}

/// The live player a handle is for, with its entity's slot, or null, with fbe_error saying it's
/// freed or its entity is.
player_t* live_player(std::uint64_t handle, std::uint32_t& root) {
    const std::uint32_t slot = scene::live_slot(players().slots, handle, "player");
    if (slot == scene::none) {
        return nullptr;
    }
    player_t& found = players().kept[slot];
    const auto entity = scene::scene().entities.slots.find(found.entity);
    if (!entity) {
        fail("the player's entity was freed");
        return nullptr;
    }
    root = *entity;
    return &found;
}

/// Lets go of the clip a player plays.
void stop(player_t& player) {
    if (player.clip != scene::none) {
        release_clip(player.clip);
    }
    player.clip = scene::none;
    player.bound.clear();
    player.time = 0.0F;
}

/// The entity named `name`: the player's own, in slot `root`, or the first under it with the name,
/// depth first; none has no name.
std::uint64_t named(const player_t& player, std::uint32_t root, const std::string& name) {
    if (name.empty()) {
        return 0;
    }
    if (scene::scene().entities.name[root] == name) {
        return player.entity;
    }
    return fbe_entity_find(player.entity, name.c_str());
}

/// Binds each of the clip's channels to the entity its node names, under the player's entity, in
/// slot `root`.
void bind(player_t& player, std::uint32_t root) {
    const kept_clip_t& kept = kept_clip(player.clip);
    std::vector<std::uint64_t> nodes;
    nodes.reserve(kept.clip.nodes.size());
    for (const std::string& name : kept.clip.nodes) {
        nodes.push_back(named(player, root, name));
    }
    player.bound.assign(kept.clip.channels.size(), {});
    for (std::size_t i = 0; i < kept.clip.channels.size(); ++i) {
        player.bound[i].entity = nodes[kept.clip.channels[i].node];
    }
    player.version = kept.version;
}

/// Poses the entities the player moves as its clip has them at its time, binding them first if
/// the clip changed since; its entity is in slot `root`. Morph weights wait on the morph targets
/// meshes will have.
void pose(player_t& player, std::uint32_t root) {
    if (player.clip == scene::none) {
        return;
    }
    const kept_clip_t& kept = kept_clip(player.clip);
    if (player.version != kept.version) {
        bind(player, root);
    }
    scene::entities_t& entities = scene::scene().entities;
    std::array<float, 4> value{};
    for (std::size_t i = 0; i < kept.clip.channels.size(); ++i) {
        const channel_t& channel = kept.clip.channels[i];
        bound_t& to = player.bound[i];
        const auto slot = entities.slots.find(to.entity);
        if (channel.property == property_t::WEIGHTS || !slot) {
            continue;
        }
        sample(channel, player.time, to.cursor, value);
        switch (channel.property) {
        case property_t::TRANSLATION:
            entities.position[*slot] = { value[0], value[1], value[2] };
            break;
        case property_t::ROTATION:
            entities.rotation[*slot] = { value[0], value[1], value[2], value[3] };
            break;
        case property_t::SCALE:
            entities.scale[*slot] = { value[0], value[1], value[2] };
            break;
        case property_t::WEIGHTS:
            break;
        }
        scene::mark_moved(*slot);
    }
}

} // namespace

} // namespace fbe::anim

using fbe::fail;
using fbe::anim::player_t;
using fbe::anim::players;
using fbe::scene::none;

extern "C" {

FBE_API uint64_t fbe_player_create(uint64_t entity) {
    if (fbe::scene::entity_slot(entity) == none) {
        return 0;
    }
    auto& made = players();
    const std::uint32_t slot = made.slots.make();
    if (slot == made.kept.size()) {
        made.kept.emplace_back();
    } else {
        made.kept[slot] = {};
    }
    made.kept[slot].entity = entity;
    return made.slots.handle(slot);
}

FBE_API bool fbe_player_free(uint64_t player) {
    const std::uint32_t slot = fbe::scene::live_slot(players().slots, player, "player");
    if (slot == none) {
        return false;
    }
    fbe::anim::stop(players().kept[slot]);
    players().kept[slot] = {};
    players().slots.free(slot);
    return true;
}

FBE_API bool fbe_player_alive(uint64_t player) {
    return players().slots.find(player).has_value();
}

FBE_API uint64_t fbe_player_entity(uint64_t player) {
    const std::uint32_t slot = fbe::scene::live_slot(players().slots, player, "player");
    return slot != none ? players().kept[slot].entity : 0;
}

FBE_API bool fbe_player_play(uint64_t player, uint64_t clip) {
    std::uint32_t root = none;
    player_t* found = fbe::anim::live_player(player, root);
    if (found == nullptr) {
        return false;
    }
    if (clip == 0) {
        fbe::anim::stop(*found);
        return true;
    }
    const std::uint32_t slot = fbe::anim::clip_slot(clip);
    if (slot == none) {
        return false;
    }
    fbe::anim::retain_clip(slot);
    fbe::anim::stop(*found);
    found->clip = slot;
    fbe::anim::bind(*found, root);
    fbe::anim::pose(*found, root);
    return true;
}

FBE_API uint64_t fbe_player_clip(uint64_t player) {
    const std::uint32_t slot = fbe::scene::live_slot(players().slots, player, "player");
    if (slot == none || players().kept[slot].clip == none) {
        return 0;
    }
    return fbe::anim::clip_handle(players().kept[slot].clip);
}

FBE_API bool fbe_player_set_time(uint64_t player, float time) {
    std::uint32_t root = none;
    player_t* found = fbe::anim::live_player(player, root);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(time)) {
        return fail(std::format("a player's time is a number of seconds, not {}", time));
    }
    found->time = time;
    fbe::anim::pose(*found, root);
    return true;
}

FBE_API float fbe_player_time(uint64_t player) {
    const std::uint32_t slot = fbe::scene::live_slot(players().slots, player, "player");
    return slot != none ? players().kept[slot].time : 0.0F;
}

FBE_API bool fbe_player_advance(uint64_t world, float seconds) {
    const std::uint32_t in = fbe::scene::world_slot(world);
    if (in == none) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return fail(std::format("a world is moved on by 0 seconds or more, not {}", seconds));
    }
    const fbe::scene::entities_t& entities = fbe::scene::scene().entities;
    for (player_t& player : players().kept) {
        if (player.entity == 0 || player.clip == none) {
            continue;
        }
        const auto slot = entities.slots.find(player.entity);
        // One whose entity was freed lets go of its clip.
        if (!slot) {
            fbe::anim::stop(player);
            continue;
        }
        if (entities.world[*slot] != in) {
            continue;
        }
        const float length = fbe::anim::kept_clip(player.clip).clip.length;
        player.time = std::min(player.time + seconds, std::max(length, player.time));
        fbe::anim::pose(player, *slot);
    }
    return true;
}

} // extern "C"
