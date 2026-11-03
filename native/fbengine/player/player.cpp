// Players' C API: clips played on an entity and what's under it in layers, each over the ones
// before, with modes, speeds, crossfades, and masks; their nodes bound to entities by name, posed
// at a time, moved on by a world's update, and the events they reach kept for the FB side.

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
#include <iterator>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fbe::anim {

namespace {

enum class play_mode_t : std::uint8_t { ONCE, LOOP, PING_PONG };

/// How many times round a loop one step reaches events in: past this, the loops between are
/// skipped.
constexpr int most_rounds = 64;

/// An entity a player's clips name: each of its properties where it was when a clip began moving
/// it, which a clip fading out fades back to, and whether a clip moved it in the last pose. Its
/// `weights` are those of the first targets of its mesh that a clip moved in the last pose, as a
/// channel moves targets from the first.
struct bone_t {
    std::uint64_t entity = 0;
    scene::vec3_t position;
    scene::quat_t rotation;
    scene::vec3_t scale{ 1.0F, 1.0F, 1.0F };
    std::vector<float> weights;
    std::array<bool, 4> moving{};
};

/// What one of a clip's channels moves: its bone, or none, and the key it sampled last.
struct bound_t {
    std::uint32_t bone = scene::none;
    std::size_t cursor = 0;
};

/// A clip a layer plays, which it holds: the clip's version its channels were bound at, where it
/// is in it and whether it's on its way back, how fast, how it goes on at its end, whether it's
/// moved on since it started, so an event at its start is reached once, and its weight, going to
/// `target` at `rate` a second.
struct track_t {
    std::uint32_t clip = scene::none;
    std::uint64_t version = 0;
    std::vector<bound_t> bound;
    float time = 0.0F;
    bool backward = false;
    float speed = 1.0F;
    play_mode_t mode = play_mode_t::ONCE;
    bool started = false;
    float weight = 1.0F;
    float target = 1.0F;
    float rate = 0.0F;
};

/// A layer: how much of it's over the layers before it, the entities whose trees it's kept to,
/// none for all, and its tracks, the one played last at the back.
struct layer_t {
    float weight = 1.0F;
    std::vector<std::uint64_t> mask;
    std::vector<track_t> tracks;
};

/// An event a player reached.
struct reached_t {
    std::string name;
    std::string clip;
    std::int32_t layer = 0;
    float time = 0.0F;
};

/// A player: its entity, 0 for a free slot, its layers, the bones its clips move, the entities left
/// to the game, the events they reached since the FB side took them, and whether an advance found
/// its entity freed.
struct player_t {
    std::uint64_t entity = 0;
    std::array<layer_t, FBE_PLAYER_LAYERS> layers;
    std::vector<bone_t> bones;
    std::vector<std::uint64_t> released;
    std::vector<reached_t> events;
    bool orphaned = false;
};

/// What a layer makes of one bone's property: its tracks' values summed by their weights.
struct sum_t {
    std::array<float, 4> value{};
    float weight = 0.0F;
};

/// What a pose works in, kept from one to the next so a frame's poses allocate little: each bone's
/// entity's slot, or none, its transform and morph weights so far, whether a clip's moved each
/// property and how many targets, and what the layer being worked out makes of it: of a target, the
/// sum and total weight of the tracks that reach it.
struct scratch_t {
    std::vector<std::uint32_t> slots;
    std::vector<scene::vec3_t> positions;
    std::vector<scene::quat_t> rotations;
    std::vector<scene::vec3_t> scales;
    std::vector<std::vector<float>> weights;
    std::vector<std::array<bool, 4>> moved;
    std::vector<std::size_t> widths;
    std::vector<std::array<sum_t, 3>> sums;
    std::vector<std::vector<float>> weight_sums;
    std::vector<std::vector<float>> weight_totals;
    std::vector<float> sampled;
    std::vector<bool> allowed;
};

struct players_t {
    handle_slots_t slots{ PLAYER_KIND };
    std::vector<player_t> kept;
    /// The slots of the players with events kept.
    std::vector<std::uint32_t> evented;
    /// The slots of the players an advance found their entities freed, until the FB side takes
    /// them.
    std::vector<std::uint32_t> orphans;
    scratch_t scratch;
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

/// The live player and its layer, as live_player, or null, with fbe_error saying why.
layer_t* live_layer(std::uint64_t handle, std::int32_t layer, player_t*& player,
                    std::uint32_t& root) {
    player = live_player(handle, root);
    if (player == nullptr) {
        return nullptr;
    }
    if (layer < 0 || layer >= FBE_PLAYER_LAYERS) {
        fail(std::format("a player's layers are 0 to {}, not {}", FBE_PLAYER_LAYERS - 1, layer));
        return nullptr;
    }
    return &player->layers[static_cast<std::size_t>(layer)];
}

/// The live slot of a player a handle is for, without a check on its entity, or none.
std::uint32_t player_slot(std::uint64_t handle) {
    return scene::live_slot(players().slots, handle, "player");
}

bool valid_mode(std::int32_t mode) {
    if (mode < FBE_PLAYER_ONCE || mode > FBE_PLAYER_PING_PONG) {
        return fail(std::format("{} isn't a mode a clip plays in", mode));
    }
    return true;
}

bool valid_speed(float speed) {
    if (!std::isfinite(speed)) {
        return fail(std::format("a clip plays at a speed that's a number, not {}", speed));
    }
    return true;
}

bool valid_seconds(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return fail(std::format("a fade takes 0 seconds or more, not {}", seconds));
    }
    return true;
}

float length_of(const track_t& track) {
    return kept_clip(track.clip).clip.length;
}

/// A layer's tracks let go of.
void clear(layer_t& layer) {
    for (const track_t& track : layer.tracks) {
        release_clip(track.clip);
    }
    layer.tracks.clear();
}

/// A player's every layer let go of, and its events.
void stop_all(player_t& player) {
    for (layer_t& layer : player.layers) {
        clear(layer);
    }
    player.events.clear();
}

/// Once tracks have gone, forgets that a clip's moving what none of those left moves, so what's
/// done with it now is where the next clip to move it begins.
void note_still_moved(player_t& player) {
    std::vector<std::array<bool, 4>>& moved = players().scratch.moved;
    std::vector<std::size_t>& widths = players().scratch.widths;
    moved.assign(player.bones.size(), {});
    widths.assign(player.bones.size(), 0);
    for (const layer_t& layer : player.layers) {
        for (const track_t& track : layer.tracks) {
            const std::vector<channel_t>& channels = kept_clip(track.clip).clip.channels;
            for (std::size_t c = 0; c < track.bound.size(); ++c) {
                const std::uint32_t bone = track.bound[c].bone;
                if (bone < moved.size()) {
                    moved[bone][static_cast<std::size_t>(channels[c].property)] = true;
                    if (channels[c].property == property_t::WEIGHTS) {
                        widths[bone] = std::max<std::size_t>(widths[bone], channels[c].width);
                    }
                }
            }
        }
    }
    for (std::size_t i = 0; i < player.bones.size(); ++i) {
        for (std::size_t p = 0; p < moved[i].size(); ++p) {
            player.bones[i].moving[p] = player.bones[i].moving[p] && moved[i][p];
        }
        player.bones[i].weights.resize(std::min(player.bones[i].weights.size(), widths[i]));
    }
}

/// The entity named `name`: the player's own, in slot `root`, or the first under it with the name,
/// depth first; "" is the player's own, whatever its name.
std::uint64_t named(const player_t& player, std::uint32_t root, const std::string& name) {
    if (name.empty() || scene::scene().entities.name[root] == name) {
        return player.entity;
    }
    return fbe_entity_find(player.entity, name.c_str());
}

/// The bone of an entity, made if it hasn't one yet.
std::uint32_t bone_of(player_t& player, std::uint64_t entity) {
    const auto found = std::ranges::find_if(
        player.bones, [&](const bone_t& bone) { return bone.entity == entity; });
    if (found != player.bones.end()) {
        return static_cast<std::uint32_t>(found - player.bones.begin());
    }
    player.bones.push_back({ .entity = entity });
    return static_cast<std::uint32_t>(player.bones.size() - 1);
}

/// Binds each of a track's clip's channels to the bone of the entity its node names, under the
/// player's entity, in slot `root`.
void bind_channels(player_t& player, track_t& track, std::uint32_t root) {
    const kept_clip_t& kept = kept_clip(track.clip);
    std::vector<std::uint32_t> nodes;
    nodes.reserve(kept.clip.nodes.size());
    for (const std::string& name : kept.clip.nodes) {
        const std::uint64_t entity = named(player, root, name);
        nodes.push_back(entity != 0 ? bone_of(player, entity) : scene::none);
    }
    track.bound.assign(kept.clip.channels.size(), {});
    for (std::size_t i = 0; i < kept.clip.channels.size(); ++i) {
        track.bound[i].bone = nodes[kept.clip.channels[i].node];
    }
    track.version = kept.version;
}

/// Binds a track as bind_channels does, first forgetting the bones of entities freed since, and
/// binding the other tracks again to what's left of them.
void bind(player_t& player, track_t& track, std::uint32_t root) {
    const scene::entities_t& entities = scene::scene().entities;
    const std::size_t erased = std::erase_if(player.bones, [&](const bone_t& bone) {
        return !entities.slots.find(bone.entity).has_value();
    });
    if (erased > 0) {
        // Each is out of date until it's bound again, should running out of memory stop that here.
        for (layer_t& layer : player.layers) {
            for (track_t& other : layer.tracks) {
                other.version = UINT64_MAX;
                other.bound.clear();
            }
        }
        for (layer_t& layer : player.layers) {
            for (track_t& other : layer.tracks) {
                bind_channels(player, other, root);
            }
        }
    }
    bind_channels(player, track, root);
}

/// A new track of a clip, held, at its start, or its end going backward, bound.
track_t track_of(player_t& player, std::uint32_t root, std::uint32_t clip, play_mode_t mode,
                 float speed) {
    track_t made;
    made.clip = clip;
    made.mode = mode;
    made.speed = speed;
    made.time = speed < 0.0F ? kept_clip(clip).clip.length : 0.0F;
    bind(player, made, root);
    retain_clip(clip);
    return made;
}

/// `time` as a track's mode has it, within its clip: held at the ends once, round for a loop,
/// and there and back for ping-pong, which says whether it's on the way back.
float placed(const track_t& track, float time, bool& backward) {
    const float length = length_of(track);
    backward = false;
    if (length <= 0.0F) {
        return 0.0F;
    }
    switch (track.mode) {
    case play_mode_t::ONCE:
        return std::clamp(time, 0.0F, length);
    case play_mode_t::LOOP: {
        const float round = std::fmod(time, length);
        return round < 0.0F ? round + length : round;
    }
    case play_mode_t::PING_PONG: {
        float there = std::fmod(time, 2.0F * length);
        if (there < 0.0F) {
            there += 2.0F * length;
        }
        backward = there > length;
        return backward ? (2.0F * length) - there : there;
    }
    }
    return time;
}

/// Where a track is reaching events from: its player's slot, and its layer.
struct reacher_t {
    std::uint32_t player = 0;
    std::int32_t layer = 0;
};

/// The events of `clip` a track passes going from `from` to `to`, forward or back, in the order
/// it reaches them: after `from`, or at it too with `at_from`, up to `to`.
void reach(const clip_t& clip, float from, float to, bool forward, bool at_from, reacher_t by) {
    std::vector<reached_t>& events = players().kept[by.player].events;
    const auto add = [&](const event_t& event) {
        if (events.empty()) {
            players().evented.push_back(by.player);
        }
        events.push_back(
            { .name = event.name, .clip = clip.name, .layer = by.layer, .time = event.time });
    };
    if (forward) {
        for (const event_t& event : clip.events) {
            if ((event.time > from || (at_from && event.time == from)) && event.time <= to) {
                add(event);
            }
        }
    } else {
        for (const event_t& event : std::views::reverse(clip.events)) {
            if ((event.time < from || (at_from && event.time == from)) && event.time >= to) {
                add(event);
            }
        }
    }
}

/// Moves a track on by `seconds`: its weight toward where it's going, and its time as its speed
/// and mode say, keeping the events it reaches.
void move_on(track_t& track, float seconds, reacher_t by) {
    if (track.weight != track.target && seconds > 0.0F) {
        const float step = track.rate * seconds;
        track.weight = track.weight < track.target ? std::min(track.weight + step, track.target)
                                                   : std::max(track.weight - step, track.target);
    }
    const clip_t& clip = kept_clip(track.clip).clip;
    const float length = clip.length;
    const bool at_start = !track.started;
    track.started = true;
    if (length <= 0.0F) {
        track.time = 0.0F;
        if (at_start) {
            reach(clip, 0.0F, 0.0F, true, true, by);
        }
        return;
    }
    float left = std::abs(seconds * track.speed);
    bool forward = (track.speed >= 0.0F) != track.backward;
    bool at_from = at_start;
    float at = track.time;
    // Past many rounds, the ones between are skipped: their events once, not thousands of times.
    const float round = track.mode == play_mode_t::PING_PONG ? 2.0F * length : length;
    if (!std::isfinite(left)) {
        left = round;
    } else if (track.mode != play_mode_t::ONCE && left > round * static_cast<float>(most_rounds)) {
        left = std::fmod(left, round) + round;
    }
    for (int rounds = 0; rounds <= 2 * most_rounds; ++rounds) {
        const float room = forward ? length - at : at;
        if (left <= room || track.mode == play_mode_t::ONCE) {
            const float to = forward ? at + std::min(left, room) : at - std::min(left, room);
            if (to != at || at_from) {
                reach(clip, at, to, forward, at_from, by);
            }
            at = to;
            break;
        }
        reach(clip, at, forward ? length : 0.0F, forward, at_from, by);
        left -= room;
        if (track.mode == play_mode_t::LOOP) {
            at = forward ? 0.0F : length;
            at_from = true;
        } else {
            at = forward ? length : 0.0F;
            forward = !forward;
            at_from = false;
        }
    }
    track.time = at;
    track.backward = forward != (track.speed >= 0.0F);
}

/// Whether the entity in `slot` is one a layer moves: all for no mask, else those its mask names
/// and those under them.
bool masked_in(const layer_t& layer, std::uint32_t slot) {
    if (layer.mask.empty()) {
        return true;
    }
    const scene::entities_t& entities = scene::scene().entities;
    for (std::uint32_t at = slot; at != scene::none; at = entities.parent[at]) {
        if (std::ranges::contains(layer.mask, entities.slots.handle(at))) {
            return true;
        }
    }
    return false;
}

scene::vec3_t lerp(scene::vec3_t a, scene::vec3_t b, float t) {
    return { scene::mix(a.x, b.x, t), scene::mix(a.y, b.y, t), scene::mix(a.z, b.z, t) };
}

/// A rotation part of the way to another, the short way round, in a line and made length 1.
scene::quat_t nlerp(scene::quat_t a, scene::quat_t b, float t) {
    const float dot = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    const float to = dot < 0.0F ? -t : t;
    return scene::normalize({ (a.x * (1.0F - t)) + (b.x * to), (a.y * (1.0F - t)) + (b.y * to),
                              (a.z * (1.0F - t)) + (b.z * to), (a.w * (1.0F - t)) + (b.w * to) });
}

/// A clip's about to move a bone's property that no clip moved in the last pose: it starts from
/// where the entity is now.
void begin_moving(bone_t& bone, std::size_t bone_index, std::size_t property) {
    scene::entities_t& entities = scene::scene().entities;
    scratch_t& scratch = players().scratch;
    const std::uint32_t slot = scratch.slots[bone_index];
    switch (static_cast<property_t>(property)) {
    case property_t::TRANSLATION:
        bone.position = entities.position[slot];
        scratch.positions[bone_index] = bone.position;
        break;
    case property_t::ROTATION:
        bone.rotation = entities.rotation[slot];
        scratch.rotations[bone_index] = bone.rotation;
        break;
    case property_t::SCALE:
        bone.scale = entities.scale[slot];
        scratch.scales[bone_index] = bone.scale;
        break;
    case property_t::WEIGHTS:
        break;
    }
}

/// A clip's about to move a bone's mesh's first `width` targets: any that no clip moved in the last
/// pose starts from where it is now. reserve_weights left room for them.
void begin_weights(bone_t& bone, std::size_t bone_index, std::size_t width) {
    const scene::scene_t& all = scene::scene();
    scratch_t& scratch = players().scratch;
    const std::uint32_t slot = scratch.slots[bone_index];
    const std::vector<float>& now = all.meshes[all.entities.component[slot]].weights;
    for (std::size_t k = bone.weights.size(); k < width; ++k) {
        const float rest = k < now.size() ? now[k] : 0.0F;
        bone.weights.push_back(rest);
        scratch.weights[bone_index].push_back(rest);
    }
    scratch.widths[bone_index] = std::max(scratch.widths[bone_index], width);
}

/// Whether an entity has morph weights to move.
bool has_weights(std::uint32_t slot) {
    return scene::scene().entities.kind[slot] == FBE_ENTITY_MESH;
}

/// Makes room for the morph weights a player's clips could move, so that a pose, past this, can't
/// run out of memory with some of its entities posed. Bones' slots are in the scratch.
void reserve_weights(player_t& player) {
    scene::scene_t& all = scene::scene();
    scratch_t& scratch = players().scratch;
    std::size_t most = 0;
    for (const layer_t& layer : player.layers) {
        for (const track_t& track : layer.tracks) {
            const std::vector<channel_t>& channels = kept_clip(track.clip).clip.channels;
            for (std::size_t c = 0; c < channels.size(); ++c) {
                const std::uint32_t bone = track.bound[c].bone;
                if (channels[c].property != property_t::WEIGHTS || bone == scene::none) {
                    continue;
                }
                const std::size_t width = channels[c].width;
                most = std::max(most, width);
                player.bones[bone].weights.reserve(width);
                scratch.weights[bone].reserve(width);
                scratch.weight_sums[bone].reserve(width);
                scratch.weight_totals[bone].reserve(width);
                const std::uint32_t slot = scratch.slots[bone];
                if (slot != scene::none && has_weights(slot)) {
                    all.meshes[all.entities.component[slot]].weights.reserve(width);
                }
            }
        }
    }
    scratch.sampled.reserve(most);
}

/// Sums what a layer's tracks make of each bone's properties into the scratch's sums, marking what
/// they move, whatever their weights: at none, they take it back to where it began.
void sum_tracks(player_t& player, layer_t& layer) {
    scratch_t& scratch = players().scratch;
    std::array<float, 4> value{};
    for (track_t& track : layer.tracks) {
        const clip_t& clip = kept_clip(track.clip).clip;
        const float weight = layer.weight > 0.0F ? track.weight : 0.0F;
        for (std::size_t c = 0; c < clip.channels.size(); ++c) {
            const channel_t& channel = clip.channels[c];
            const std::uint32_t bone = track.bound[c].bone;
            if (bone == scene::none || !scratch.allowed[bone] ||
                (channel.property == property_t::WEIGHTS && !has_weights(scratch.slots[bone]))) {
                continue;
            }
            const auto property = static_cast<std::size_t>(channel.property);
            if (!scratch.moved[bone][property]) {
                scratch.moved[bone][property] = true;
                if (!player.bones[bone].moving[property]) {
                    begin_moving(player.bones[bone], bone, property);
                }
            }
            if (channel.property == property_t::WEIGHTS) {
                begin_weights(player.bones[bone], bone, channel.width);
            }
            if (weight <= 0.0F) {
                continue;
            }
            if (channel.property == property_t::WEIGHTS) {
                scratch.sampled.resize(channel.width);
                sample(channel, track.time, track.bound[c].cursor, scratch.sampled);
                std::vector<float>& sum = scratch.weight_sums[bone];
                std::vector<float>& totals = scratch.weight_totals[bone];
                sum.resize(std::max<std::size_t>(sum.size(), channel.width), 0.0F);
                totals.resize(sum.size(), 0.0F);
                for (std::size_t k = 0; k < channel.width; ++k) {
                    sum[k] += scratch.sampled[k] * weight;
                    totals[k] += weight;
                }
                continue;
            }
            sum_t& sum = scratch.sums[bone][property];
            sample(channel, track.time, track.bound[c].cursor, value);
            float w = weight;
            // Each rotation the same way round as those before it, so they don't cancel.
            if (channel.property == property_t::ROTATION && sum.weight > 0.0F &&
                (sum.value[0] * value[0]) + (sum.value[1] * value[1]) + (sum.value[2] * value[2]) +
                        (sum.value[3] * value[3]) <
                    0.0F) {
                w = -w;
            }
            for (std::size_t k = 0; k < channel.width; ++k) {
                sum.value[k] += value[k] * w;
            }
            sum.weight += weight;
        }
    }
}

/// Lays a layer's sums over what the layers before it made.
void lay_over(float weight, std::size_t count) {
    scratch_t& scratch = players().scratch;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& [moving, turning, growing] = scratch.sums[i];
        if (moving.weight > 0.0F) {
            const float t = weight * std::min(moving.weight, 1.0F);
            const scene::vec3_t mean{ moving.value[0] / moving.weight,
                                      moving.value[1] / moving.weight,
                                      moving.value[2] / moving.weight };
            scratch.positions[i] = lerp(scratch.positions[i], mean, t);
        }
        if (turning.weight > 0.0F) {
            const float t = weight * std::min(turning.weight, 1.0F);
            const scene::quat_t mean = scene::normalize(
                { turning.value[0], turning.value[1], turning.value[2], turning.value[3] });
            scratch.rotations[i] = nlerp(scratch.rotations[i], mean, t);
        }
        if (growing.weight > 0.0F) {
            const float t = weight * std::min(growing.weight, 1.0F);
            const scene::vec3_t mean{ growing.value[0] / growing.weight,
                                      growing.value[1] / growing.weight,
                                      growing.value[2] / growing.weight };
            scratch.scales[i] = lerp(scratch.scales[i], mean, t);
        }
        // Each target by the tracks that reach it, so one fading out of a wider clip keeps going
        // to where it began however the narrower clip's weight goes.
        const std::vector<float>& sum = scratch.weight_sums[i];
        const std::vector<float>& totals = scratch.weight_totals[i];
        std::vector<float>& weights = scratch.weights[i];
        for (std::size_t k = 0; k < sum.size(); ++k) {
            const float t = weight * std::min(totals[k], 1.0F);
            weights[k] = scene::mix(weights[k], sum[k] / totals[k], t);
        }
    }
}

/// Poses the entities a player's clips move: each property from where it was when a clip began
/// moving it, each layer over the ones before; what no clip moves now is left as it is. Its entity
/// is in slot `root`, and tracks whose clips changed are bound again first.
void pose(player_t& player, std::uint32_t root) {
    for (layer_t& layer : player.layers) {
        for (track_t& track : layer.tracks) {
            if (track.version != kept_clip(track.clip).version) {
                bind(player, track, root);
            }
        }
    }
    scene::entities_t& entities = scene::scene().entities;
    scratch_t& scratch = players().scratch;
    const std::size_t count = player.bones.size();
    scratch.slots.resize(count);
    scratch.positions.resize(count);
    scratch.rotations.resize(count);
    scratch.scales.resize(count);
    scratch.weights.resize(count);
    scratch.weight_sums.resize(count);
    scratch.weight_totals.resize(count);
    scratch.moved.assign(count, {});
    scratch.widths.assign(count, 0);
    scratch.sums.resize(count);
    scratch.allowed.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bone_t& bone = player.bones[i];
        scratch.slots[i] = entities.slots.find(bone.entity).value_or(scene::none);
        scratch.positions[i] = bone.position;
        scratch.rotations[i] = bone.rotation;
        scratch.scales[i] = bone.scale;
        scratch.weights[i] = bone.weights;
    }
    reserve_weights(player);
    for (layer_t& layer : player.layers) {
        if (layer.tracks.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) {
            scratch.sums[i] = {};
            scratch.weight_sums[i].clear();
            scratch.weight_totals[i].clear();
            scratch.allowed[i] = scratch.slots[i] != scene::none &&
                                 masked_in(layer, scratch.slots[i]) &&
                                 !std::ranges::contains(player.released, player.bones[i].entity);
        }
        sum_tracks(player, layer);
        lay_over(layer.weight, count);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::array<bool, 4>& moved = scratch.moved[i];
        player.bones[i].moving = moved;
        const std::size_t width = scratch.widths[i];
        player.bones[i].weights.resize(width);
        const std::uint32_t slot = scratch.slots[i];
        // Only the targets a clip moved: the game's are its own.
        if (slot != scene::none && width > 0) {
            std::vector<float>& weights = scene::scene().meshes[entities.component[slot]].weights;
            weights.resize(std::max(weights.size(), width), 0.0F);
            std::copy_n(scratch.weights[i].begin(), width, weights.begin());
        }
        if (slot == scene::none || (!moved[0] && !moved[1] && !moved[2])) {
            continue;
        }
        if (moved[0]) {
            entities.position[slot] = scratch.positions[i];
        }
        if (moved[1]) {
            entities.rotation[slot] = scratch.rotations[i];
        }
        if (moved[2]) {
            entities.scale[slot] = scratch.scales[i];
        }
        scene::mark_moved(slot);
    }
}

/// Lets go of the tracks of a layer that have faded out; gives whether any have.
bool drop_faded(layer_t& layer) {
    return std::erase_if(layer.tracks, [](const track_t& track) {
               if (track.target > 0.0F || track.weight > 0.0F) {
                   return false;
               }
               release_clip(track.clip);
               return true;
           }) > 0;
}

/// Whether a track's clip has somewhere still to go: not one played once and at its end, which
/// stays there until it's stopped.
bool still_playing(const track_t& track) {
    if (track.mode != play_mode_t::ONCE) {
        return true;
    }
    return track.speed >= 0.0F ? track.time < length_of(track) : track.time > 0.0F;
}

/// Where in a layer's tracks the last of `clip` is, or their end.
std::vector<track_t>::iterator track_of_clip(layer_t& layer, std::uint32_t clip) {
    const auto found =
        std::ranges::find_if(std::views::reverse(layer.tracks),
                             [&](const track_t& track) { return track.clip == clip; });
    return found != std::views::reverse(layer.tracks).end() ? std::prev(found.base())
                                                            : layer.tracks.end();
}

/// Whether `clip` is what a layer's playing: the track played last, not stopping.
bool is_playing(const layer_t& layer, std::uint32_t clip) {
    return !layer.tracks.empty() && layer.tracks.back().clip == clip &&
           layer.tracks.back().target > 0.0F;
}

/// A track going on in `mode` at `speed`, forward unless ping-pong has it on its way back.
void set_going(track_t& track, play_mode_t mode, float speed) {
    if (mode != play_mode_t::PING_PONG) {
        track.backward = false;
    }
    track.mode = mode;
    track.speed = speed;
}

/// The track a layer played last, which it plays or fades in, or null for none, or one stopping.
track_t* newest(layer_t& layer) {
    if (layer.tracks.empty() || layer.tracks.back().target <= 0.0F) {
        return nullptr;
    }
    return &layer.tracks.back();
}

/// A player's event, or null, with fbe_error saying why there's none.
const reached_t* event_of(std::uint64_t player, std::int32_t event) {
    const std::uint32_t slot = player_slot(player);
    if (slot == scene::none) {
        return nullptr;
    }
    const std::vector<reached_t>& events = players().kept[slot].events;
    if (event < 0 || std::cmp_greater_equal(event, events.size())) {
        fail(std::format("the player has {} events; there's no event {}", events.size(), event));
        return nullptr;
    }
    return &events[static_cast<std::size_t>(event)];
}

} // namespace

} // namespace fbe::anim

using fbe::fail;
using fbe::anim::event_of;
using fbe::anim::layer_t;
using fbe::anim::play_mode_t;
using fbe::anim::player_t;
using fbe::anim::players;
using fbe::anim::track_t;
using fbe::scene::none;

extern "C" {

FBE_API uint64_t fbe_player_create(uint64_t entity) {
    if (fbe::scene::entity_slot(entity) == none) {
        return 0;
    }
    try {
        auto& made = players();
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
        made.kept[slot].entity = entity;
        return made.slots.handle(slot);
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return 0;
    }
}

FBE_API bool fbe_player_free(uint64_t player) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    if (slot == none) {
        return false;
    }
    fbe::anim::stop_all(players().kept[slot]);
    players().kept[slot] = {};
    std::erase(players().evented, slot);
    std::erase(players().orphans, slot);
    players().slots.free(slot);
    return true;
}

FBE_API bool fbe_player_alive(uint64_t player) {
    return players().slots.find(player).has_value();
}

FBE_API uint64_t fbe_player_entity(uint64_t player) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    return slot != none ? players().kept[slot].entity : 0;
}

FBE_API bool fbe_player_play(uint64_t player, int32_t layer, uint64_t clip, int32_t mode,
                             float speed) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr || !fbe::anim::valid_mode(mode) || !fbe::anim::valid_speed(speed)) {
        return false;
    }
    const std::uint32_t slot = fbe::anim::clip_slot(clip);
    if (slot == none) {
        return false;
    }
    try {
        const auto playing = static_cast<play_mode_t>(mode);
        if (fbe::anim::is_playing(*on, slot)) {
            fbe::anim::set_going(on->tracks.back(), playing, speed);
            return true;
        }
        on->tracks.reserve(on->tracks.size() + 1);
        track_t made = fbe::anim::track_of(*found, root, slot, playing, speed);
        fbe::anim::clear(*on);
        on->tracks.push_back(std::move(made));
        fbe::anim::pose(*found, root);
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API bool fbe_player_crossfade(uint64_t player, int32_t layer, uint64_t clip, float seconds,
                                  int32_t mode, float speed) {
    if (!fbe::anim::valid_seconds(seconds)) {
        return false;
    }
    if (seconds == 0.0F) {
        return fbe_player_play(player, layer, clip, mode, speed);
    }
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr || !fbe::anim::valid_mode(mode) || !fbe::anim::valid_speed(speed)) {
        return false;
    }
    const std::uint32_t slot = fbe::anim::clip_slot(clip);
    if (slot == none) {
        return false;
    }
    try {
        const auto playing = static_cast<play_mode_t>(mode);
        if (fbe::anim::is_playing(*on, slot)) {
            fbe::anim::set_going(on->tracks.back(), playing, speed);
            return true;
        }
        on->tracks.reserve(on->tracks.size() + 1);
        const auto again = fbe::anim::track_of_clip(*on, slot);
        track_t coming;
        if (again != on->tracks.end()) {
            coming = std::move(*again);
            on->tracks.erase(again);
            fbe::anim::set_going(coming, playing, speed);
        } else {
            coming = fbe::anim::track_of(*found, root, slot, playing, speed);
            coming.weight = 0.0F;
        }
        coming.target = 1.0F;
        coming.rate = (1.0F - coming.weight) / seconds;
        for (track_t& going : on->tracks) {
            going.target = 0.0F;
            going.rate = going.weight / seconds;
        }
        on->tracks.push_back(std::move(coming));
        fbe::anim::pose(*found, root);
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API bool fbe_player_stop(uint64_t player, int32_t layer, float seconds) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr || !fbe::anim::valid_seconds(seconds)) {
        return false;
    }
    if (seconds == 0.0F) {
        fbe::anim::clear(*on);
        try {
            fbe::anim::note_still_moved(*found);
            return true;
        } catch (const std::bad_alloc&) {
            return fail("out of memory");
        }
    }
    for (track_t& going : on->tracks) {
        going.target = 0.0F;
        going.rate = going.weight / seconds;
    }
    return true;
}

FBE_API uint64_t fbe_player_clip(uint64_t player, int32_t layer) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    const track_t* track = on != nullptr ? fbe::anim::newest(*on) : nullptr;
    return track != nullptr ? fbe::anim::clip_handle(track->clip) : 0;
}

FBE_API bool fbe_player_playing(uint64_t player, int32_t layer) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    const track_t* track = on != nullptr ? fbe::anim::newest(*on) : nullptr;
    return track != nullptr && fbe::anim::still_playing(*track);
}

FBE_API bool fbe_player_set_time(uint64_t player, int32_t layer, float time) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr) {
        return false;
    }
    if (!std::isfinite(time)) {
        return fail(std::format("a clip's time is a number of seconds, not {}", time));
    }
    track_t* track = fbe::anim::newest(*on);
    if (track == nullptr) {
        return true;
    }
    const float time_was = track->time;
    const bool backward_was = track->backward;
    const bool started_was = track->started;
    track->time = fbe::anim::placed(*track, time, track->backward);
    track->started = true;
    try {
        fbe::anim::pose(*found, root);
        return true;
    } catch (const std::bad_alloc&) {
        track->time = time_was;
        track->backward = backward_was;
        track->started = started_was;
        return fail("out of memory");
    }
}

FBE_API float fbe_player_time(uint64_t player, int32_t layer) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    const track_t* track = on != nullptr ? fbe::anim::newest(*on) : nullptr;
    return track != nullptr ? track->time : 0.0F;
}

FBE_API bool fbe_player_set_speed(uint64_t player, int32_t layer, float speed) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr || !fbe::anim::valid_speed(speed)) {
        return false;
    }
    track_t* track = fbe::anim::newest(*on);
    if (track != nullptr) {
        track->speed = speed;
    }
    return true;
}

FBE_API float fbe_player_speed(uint64_t player, int32_t layer) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    const track_t* track = on != nullptr ? fbe::anim::newest(*on) : nullptr;
    return track != nullptr ? track->speed : 0.0F;
}

FBE_API bool fbe_player_set_weight(uint64_t player, int32_t layer, float weight) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr) {
        return false;
    }
    if (std::isnan(weight) || weight < 0.0F || weight > 1.0F) {
        return fail(std::format("a layer's weight is from 0 to 1, not {}", weight));
    }
    on->weight = weight;
    return true;
}

FBE_API float fbe_player_weight(uint64_t player, int32_t layer) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    const layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    return on != nullptr ? on->weight : 0.0F;
}

FBE_API bool fbe_player_set_mask(uint64_t player, int32_t layer, const uint64_t* entities,
                                 int32_t count) {
    player_t* found = nullptr;
    std::uint32_t root = none;
    layer_t* on = fbe::anim::live_layer(player, layer, found, root);
    if (on == nullptr) {
        return false;
    }
    const std::span<const uint64_t> given(
        entities, entities != nullptr && count > 0 ? static_cast<std::size_t>(count) : 0);
    for (const uint64_t entity : given) {
        if (fbe::scene::entity_slot(entity) == none) {
            return false;
        }
    }
    try {
        on->mask.assign(given.begin(), given.end());
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API bool fbe_player_release(uint64_t player, uint64_t bone) {
    std::uint32_t root = none;
    player_t* found = fbe::anim::live_player(player, root);
    if (found == nullptr || fbe::scene::entity_slot(bone) == none) {
        return false;
    }
    if (std::ranges::contains(found->released, bone)) {
        return true;
    }
    try {
        // What was released and has been freed since isn't kept.
        const fbe::scene::entities_t& entities = fbe::scene::scene().entities;
        std::erase_if(found->released, [&](std::uint64_t released) {
            return !entities.slots.find(released).has_value();
        });
        found->released.push_back(bone);
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API bool fbe_player_reclaim(uint64_t player, uint64_t bone) {
    std::uint32_t root = none;
    player_t* found = fbe::anim::live_player(player, root);
    if (found == nullptr || fbe::scene::entity_slot(bone) == none) {
        return false;
    }
    std::erase(found->released, bone);
    return true;
}

FBE_API bool fbe_player_released(uint64_t player, uint64_t bone) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    return slot != none && fbe::scene::scene().entities.slots.find(bone).has_value() &&
           std::ranges::contains(players().kept[slot].released, bone);
}

FBE_API bool fbe_player_advance(uint64_t world, float seconds) {
    const std::uint32_t in = fbe::scene::world_slot(world);
    if (in == none) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return fail(std::format("a world is moved on by 0 seconds or more, not {}", seconds));
    }
    try {
        const fbe::scene::entities_t& entities = fbe::scene::scene().entities;
        auto& all = players();
        for (std::uint32_t slot = 0; slot < all.kept.size(); ++slot) {
            player_t& player = all.kept[slot];
            if (player.entity == 0) {
                continue;
            }
            const auto root = entities.slots.find(player.entity);
            // One whose entity was freed lets go of what it played, once.
            if (!root) {
                if (!player.orphaned) {
                    player.orphaned = true;
                    fbe::anim::stop_all(player);
                    std::erase(all.evented, slot);
                    all.orphans.push_back(slot);
                }
                continue;
            }
            if (entities.world[*root] != in || !fbe::scene::showing(*root)) {
                continue;
            }
            bool playing = false;
            for (std::size_t index = 0; index < player.layers.size(); ++index) {
                layer_t& layer = player.layers[index];
                for (track_t& track : layer.tracks) {
                    fbe::anim::move_on(
                        track, seconds,
                        { .player = slot, .layer = static_cast<std::int32_t>(index) });
                }
                playing = playing || !layer.tracks.empty();
            }
            if (playing) {
                fbe::anim::pose(player, *root);
            }
            bool dropped = false;
            for (layer_t& layer : player.layers) {
                dropped = fbe::anim::drop_faded(layer) || dropped;
            }
            if (dropped) {
                fbe::anim::note_still_moved(player);
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API int32_t fbe_player_evented(uint64_t* handles, int32_t capacity) {
    const auto& all = players();
    const std::size_t room =
        handles != nullptr && capacity > 0 ? static_cast<std::size_t>(capacity) : 0;
    const std::span<uint64_t> out(handles, std::min(room, all.evented.size()));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = all.slots.handle(all.evented[i]);
    }
    return static_cast<int32_t>(all.evented.size());
}

FBE_API int32_t fbe_player_orphans(uint64_t* handles, int32_t capacity) {
    auto& all = players();
    const std::size_t room =
        handles != nullptr && capacity > 0 ? static_cast<std::size_t>(capacity) : 0;
    const std::span<uint64_t> out(handles, std::min(room, all.orphans.size()));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = all.slots.handle(all.orphans[i]);
    }
    all.orphans.erase(all.orphans.begin(),
                      all.orphans.begin() + static_cast<std::ptrdiff_t>(out.size()));
    return static_cast<int32_t>(out.size());
}

FBE_API int64_t fbe_player_count(void) {
    return static_cast<int64_t>(std::ranges::count_if(
        players().kept, [](const player_t& player) { return player.entity != 0; }));
}

FBE_API int32_t fbe_player_bone_count(uint64_t player) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    return slot != none ? static_cast<int32_t>(players().kept[slot].bones.size()) : 0;
}

FBE_API int32_t fbe_player_event_count(uint64_t player) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    return slot != none ? static_cast<int32_t>(players().kept[slot].events.size()) : 0;
}

FBE_API const char* fbe_player_event_name(uint64_t player, int32_t event) {
    const fbe::anim::reached_t* found = event_of(player, event);
    return found != nullptr ? found->name.c_str() : "";
}

FBE_API const char* fbe_player_event_clip(uint64_t player, int32_t event) {
    const fbe::anim::reached_t* found = event_of(player, event);
    return found != nullptr ? found->clip.c_str() : "";
}

FBE_API int32_t fbe_player_event_layer(uint64_t player, int32_t event) {
    const fbe::anim::reached_t* found = event_of(player, event);
    return found != nullptr ? found->layer : -1;
}

FBE_API float fbe_player_event_time(uint64_t player, int32_t event) {
    const fbe::anim::reached_t* found = event_of(player, event);
    return found != nullptr ? found->time : 0.0F;
}

FBE_API bool fbe_player_clear_events(uint64_t player) {
    const std::uint32_t slot = fbe::anim::player_slot(player);
    if (slot == none) {
        return false;
    }
    players().kept[slot].events.clear();
    std::erase(players().evented, slot);
    return true;
}

} // extern "C"
