// Particles: an entity that spawns them, each written once, as it's born, to where the GPU keeps
// them, whose vertex shader then has its motion in closed form: where it is follows from its birth,
// its velocity, gravity, and drag. So nothing's simulated on the CPU, or anywhere, frame by frame,
// and a million cost the CPU only those born.

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
#include <numbers>
#include <ranges>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

constexpr std::int32_t most_particles = 1048576;
/// The most a step counts for: past a life's length it changes nothing but the clock's place.
constexpr float most_step = 1000000.0F;

float next_random(particles_t& found) {
    // xorshift32, so a seed's run is the same on every machine.
    std::uint32_t state = found.random;
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    found.random = state;
    return static_cast<float>(state >> 8U) / 16777216.0F;
}

float between(particles_t& found, float low, float high) {
    return low + ((high - low) * next_random(found));
}

/// One born at `place`, `ago` seconds before the clock, turned as `turn`.
void spawn(particles_t& found, vec3_t place, quat_t turn, float ago) {
    const auto cosine = std::cos(found.spread);
    const float height = between(found, cosine, 1.0F);
    const float around = next_random(found) * 2.0F * std::numbers::pi_v<float>;
    const float out = std::sqrt(std::fmax(1.0F - (height * height), 0.0F));
    // Around its forward axis, -Z, as far off it as its spread.
    const vec3_t way = turn * vec3_t(out * std::cos(around), out * std::sin(around), -height);
    const float speed = between(found, found.speed_min, found.speed_max);
    const vec3_t velocity = way * speed;
    const float life = between(found, found.life_min, found.life_max);
    // Dead already, at the end of a long step, as if it never was.
    if (ago >= life) {
        return;
    }
    float birth = found.clock - ago;
    if (birth < 0.0F) {
        birth += particles_wrap;
    }
    const particle_t made{
        .position = { place.x, place.y, place.z },
        .birth = birth,
        .velocity = { velocity.x, velocity.y, velocity.z },
        .life = life,
        .roll = between(found, -found.roll, found.roll),
        .spin = between(found, -found.spin, found.spin),
    };
    const auto slot = static_cast<std::uint32_t>(found.next);
    found.pending.emplace_back(slot, made);
    found.next = (found.next + 1) % static_cast<std::size_t>(found.capacity);
    found.born = std::min(found.born + 1, found.capacity);
    found.deaths[slot] = found.total + life - ago;
    found.latest_death = std::fmax(found.latest_death, found.deaths[slot]);
    // Where it can reach by its speed, for the box what's alive is in.
    const float reach = speed * life;
    const vec3_t low(place.x - reach, place.y - reach, place.z - reach);
    const vec3_t high(place.x + reach, place.y + reach, place.z + reach);
    bounds_slice_t& slice = found.slices[found.slice];
    slice.low = slice.any ? vec3_t(std::fmin(slice.low.x, low.x), std::fmin(slice.low.y, low.y),
                                   std::fmin(slice.low.z, low.z))
                          : low;
    slice.high = slice.any
                     ? vec3_t(std::fmax(slice.high.x, high.x), std::fmax(slice.high.y, high.y),
                              std::fmax(slice.high.z, high.z))
                     : high;
    slice.any = true;
}

/// What waits for the GPU, never drawn, stays at twice what it holds: what a slot's last had is all
/// that counts of it.
void trim_pending(particles_t& found) {
    const auto most = static_cast<std::size_t>(found.capacity);
    if (found.pending.size() <= most * 2) {
        return;
    }
    std::vector<bool> kept(most);
    std::vector<std::pair<std::uint32_t, particle_t>> latest;
    latest.reserve(most);
    for (const auto& one : std::ranges::reverse_view(found.pending)) {
        if (!kept[one.first]) {
            kept[one.first] = true;
            latest.push_back(one);
        }
    }
    std::ranges::reverse(latest);
    found.pending = std::move(latest);
}

/// Clears what's dead, so its birth coming round again doesn't bring it back.
void sweep(particles_t& found) {
    for (std::size_t slot = 0; slot < found.deaths.size(); ++slot) {
        if (found.deaths[slot] != 0.0 && found.deaths[slot] <= found.total) {
            found.pending.emplace_back(static_cast<std::uint32_t>(slot), particle_t{});
            found.deaths[slot] = 0.0;
        }
    }
}

/// Forgets what it has, its slots' deaths made ready first, as what can run out of memory.
void make_room(particles_t& found, std::int32_t capacity) {
    std::vector<double> deaths(static_cast<std::size_t>(capacity), 0.0);
    found.deaths = std::move(deaths);
    found.capacity = capacity;
    found.pending.clear();
    found.next = 0;
    found.born = 0;
    ++found.generation;
}

/// Its slots' deaths, which a new emitter only has once it's first wanted.
void ready_slots(particles_t& found) {
    if (found.deaths.size() != static_cast<std::size_t>(found.capacity)) {
        make_room(found, found.capacity);
    }
}

void step_one(particles_t& found, float seconds) {
    const scene_t& s = scene();
    update_world(found.entity);
    const vec3_t now = s.entities.world_matrix[found.entity].axis(3);
    const quat_t turn = s.entities.world_rotation[found.entity];
    const vec3_t from = found.moved ? found.last_place : now;
    found.moved = true;
    found.last_place = now;
    found.clock = std::fmod(found.clock + seconds, particles_wrap);
    found.total += static_cast<double>(seconds);
    const auto sweeps = static_cast<std::uint64_t>(found.total / (particles_wrap / 2.0F));
    if (sweeps != found.sweeps) {
        found.sweeps = sweeps;
        sweep(found);
    }
    // A slice of where they're born, each a part of the longest life, so a slice goes when all it
    // has are dead: one more than that, since the last it had was born as it ended.
    // Once all that had a longer life than it has now are dead, its slices go by the life it has.
    if (found.longest > found.life_max && found.total >= found.long_until) {
        found.longest = found.life_max;
    }
    found.slice_age += seconds;
    const float slice_length = found.longest / static_cast<float>(found.slices.size() - 1);
    const float passed = std::floor(found.slice_age / slice_length);
    const auto turns =
        static_cast<std::size_t>(std::fmin(passed, static_cast<float>(found.slices.size())));
    for (std::size_t at = 0; at < turns; ++at) {
        found.slice = (found.slice + 1) % found.slices.size();
        found.slices[found.slice].any = false;
    }
    found.slice_age -= passed * slice_length;
    if (!showing(found.entity)) {
        return;
    }
    // Only what's born at the end of the step, a life long at most, can live to its end.
    const float window = std::fmin(seconds, found.life_max);
    const auto most = static_cast<float>(found.capacity);
    found.owed = std::fmin(found.owed + (found.rate * window), most + 1.0F);
    const auto count = static_cast<std::int32_t>(std::fmin(found.owed, most));
    found.owed -= std::floor(found.owed);
    const float start = seconds > 0.0F ? 1.0F - (window / seconds) : 0.0F;
    const vec3_t across = now - from;
    for (std::int32_t i = 0; i < count; ++i) {
        // Evenly along it, and from where the entity went: no clumps where it was each step.
        const float part = (static_cast<float>(i) + 0.5F) / static_cast<float>(count);
        const float along_step = start + ((1.0F - start) * part);
        spawn(found, from + across * along_step, turn, window * (1.0F - part));
    }
    trim_pending(found);
}

} // namespace

particles_t* particles_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_PARTICLES) {
        fail("the entity isn't a particle emitter");
        return nullptr;
    }
    return &scene().particles[scene().entities.component[entity]];
}

void add_particles(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.particles.size());
    particles_t made;
    made.entity = entity;
    // Each its own, from when it was made.
    made.random = static_cast<std::uint32_t>(s.entities.made[entity] * 2654435761ULL) | 1U;
    s.particles.push_back(std::move(made));
}

void remove_particles(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    release_material(s.particles[index].material);
    if (index != s.particles.size() - 1) {
        s.particles[index] = std::move(s.particles.back());
        s.entities.component[s.particles[index].entity] = index;
    }
    s.particles.pop_back();
}

void copy_particles(std::uint32_t from, std::uint32_t to) {
    scene_t& s = scene();
    const particles_t& original = s.particles[s.entities.component[from]];
    particles_t& copy = s.particles[s.entities.component[to]];
    const std::uint32_t random = copy.random;
    copy = original;
    copy.entity = to;
    copy.random = random;
    copy.moved = false;
    copy.clock = 0.0F;
    copy.captured_clock = 0.0F;
    copy.total = 0.0;
    copy.sweeps = 0;
    copy.long_until = 0.0;
    copy.latest_death = 0.0;
    copy.longest = copy.life_max;
    copy.owed = 0.0F;
    copy.slices = {};
    copy.slice = 0;
    copy.slice_age = 0.0F;
    copy.deaths.clear();
    copy.pending.clear();
    copy.next = 0;
    copy.born = 0;
    ++copy.generation;
    retain_material(copy.material);
}

void capture_particles(std::uint32_t entity) {
    scene_t& s = scene();
    particles_t& found = s.particles[s.entities.component[entity]];
    found.captured_clock = found.clock;
}

float particles_clock(const particles_t& found, float tween) {
    // What's elapsed since the capture, across the clock's wrap if it's been past it.
    float since = found.clock - found.captured_clock;
    if (since < 0.0F) {
        since += particles_wrap;
    }
    float clock = found.captured_clock + (since * tween);
    if (clock >= particles_wrap) {
        clock -= particles_wrap;
    }
    return clock;
}

bool particles_bounds(const particles_t& found, vec3_t& min, vec3_t& max) {
    bool any = false;
    for (const bounds_slice_t& slice : found.slices) {
        if (!slice.any) {
            continue;
        }
        min = any ? vec3_t(std::fmin(min.x, slice.low.x), std::fmin(min.y, slice.low.y),
                           std::fmin(min.z, slice.low.z))
                  : slice.low;
        max = any ? vec3_t(std::fmax(max.x, slice.high.x), std::fmax(max.y, slice.high.y),
                           std::fmax(max.z, slice.high.z))
                  : slice.high;
        any = true;
    }
    if (!any) {
        return false;
    }
    // What gravity can add to that, drag or none, and a turned quad's corner.
    const float falling = 0.5F * length(found.gravity) * found.longest * found.longest;
    const float reach =
        falling + (std::fmax(found.size_start, found.size_end) * std::numbers::sqrt2_v<float>);
    const vec3_t grown(reach, reach, reach);
    min = min - grown;
    max = max + grown;
    return true;
}

} // namespace fbe::scene

using fbe::scene::particles_t;
using fbe::scene::scene;

extern "C" {

FBE_API bool fbe_world_step_particles(uint64_t world, float seconds) try {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return fbe::fail(std::format("a step takes 0 seconds or more, not {}", seconds));
    }
    for (particles_t& found : scene().particles) {
        if (scene().entities.world[found.entity] == slot) {
            fbe::scene::ready_slots(found);
            fbe::scene::step_one(found, std::fmin(seconds, fbe::scene::most_step));
        }
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
}

FBE_API bool fbe_particles_set_material(uint64_t particles, uint64_t material) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    std::uint32_t slot = fbe::scene::none;
    if (material != 0) {
        slot = fbe::scene::material_slot(material);
        if (slot == fbe::scene::none) {
            return false;
        }
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_particles_material(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr && found->material != fbe::scene::none
               ? scene().material_slots.handle(found->material)
               : 0;
}

FBE_API bool fbe_particles_set_capacity(uint64_t particles, int32_t capacity) try {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (capacity < 1 || capacity > fbe::scene::most_particles) {
        return fbe::fail(std::format("an emitter holds 1 to {} particles, not {}",
                                     fbe::scene::most_particles, capacity));
    }
    fbe::scene::make_room(*found, capacity);
    return true;
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
}

FBE_API int32_t fbe_particles_capacity(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->capacity : -1;
}

FBE_API bool fbe_particles_set_rate(uint64_t particles, float per_second) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(per_second) || per_second < 0.0F) {
        return fbe::fail(
            std::format("particles are born at 0 a second or more, not {}", per_second));
    }
    found->rate = per_second;
    return true;
}

FBE_API float fbe_particles_rate(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->rate : -1.0F;
}

FBE_API bool fbe_particles_set_life(uint64_t particles, float least, float most) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    const float longest = fbe::scene::particles_wrap / 2.0F;
    if (!std::isfinite(least) || !std::isfinite(most) || least <= 0.0F || most < least ||
        most > longest) {
        return fbe::fail(std::format(
            "a particle lives above 0 seconds, the least to the most, up to {}, not {} to {}",
            longest, least, most));
    }
    found->life_min = least;
    found->life_max = most;
    if (most >= found->longest) {
        found->longest = most;
    } else {
        found->long_until = found->latest_death;
    }
    return true;
}

FBE_API bool fbe_particles_set_motion(uint64_t particles, float least_speed, float most_speed,
                                      float spread, float spin) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(least_speed) || !std::isfinite(most_speed) || !std::isfinite(spread) ||
        !std::isfinite(spin) || least_speed < 0.0F || most_speed < least_speed || spread < 0.0F ||
        spread > std::numbers::pi_v<float> || spin < 0.0F) {
        return fbe::fail(std::format("a particle's speeds are 0 or more, least to most, its spread "
                                     "0 to Pi from its axis, and "
                                     "its spin 0 or more, not {} to {}, {}, {}",
                                     least_speed, most_speed, spread, spin));
    }
    found->speed_min = least_speed;
    found->speed_max = most_speed;
    found->spread = spread;
    found->spin = spin;
    return true;
}

FBE_API bool fbe_particles_set_forces(uint64_t particles, float x, float y, float z, float drag) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(drag) ||
        drag < 0.0F) {
        return fbe::fail(
            std::format("a particle's gravity is finite and its drag 0 or more, not {}", drag));
    }
    found->gravity = fbe::scene::vec3_t(x, y, z);
    found->drag = drag;
    return true;
}

FBE_API bool fbe_particles_set_sizes(uint64_t particles, float start, float end) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0F || end < 0.0F) {
        return fbe::fail(
            std::format("a particle's half-size is 0 or more, not {} to {}", start, end));
    }
    found->size_start = start;
    found->size_end = end;
    return true;
}

FBE_API bool fbe_particles_set_colors(uint64_t particles, const fbe_color_t* start,
                                      const fbe_color_t* end) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::valid_color(start->r, start->g, start->b, start->a, "a particle's") ||
        !fbe::scene::valid_color(end->r, end->g, end->b, end->a, "a particle's")) {
        return false;
    }
    found->color_start = { start->r, start->g, start->b, start->a };
    found->color_end = { end->r, end->g, end->b, end->a };
    return true;
}

FBE_API float fbe_particles_life_least(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->life_min : -1.0F;
}

FBE_API float fbe_particles_life_most(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->life_max : -1.0F;
}

FBE_API float fbe_particles_speed_least(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->speed_min : -1.0F;
}

FBE_API float fbe_particles_speed_most(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->speed_max : -1.0F;
}

FBE_API float fbe_particles_spread(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->spread : -1.0F;
}

FBE_API float fbe_particles_spin(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->spin : -1.0F;
}

FBE_API float fbe_particles_gravity_x(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->gravity.x : -1.0F;
}

FBE_API float fbe_particles_gravity_y(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->gravity.y : -1.0F;
}

FBE_API float fbe_particles_gravity_z(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->gravity.z : -1.0F;
}

FBE_API float fbe_particles_drag(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->drag : -1.0F;
}

FBE_API float fbe_particles_size_start(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->size_start : -1.0F;
}

FBE_API float fbe_particles_size_end(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->size_end : -1.0F;
}

FBE_API float fbe_particles_roll(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    return found != nullptr ? found->roll : -1.0F;
}

FBE_API bool fbe_particles_color(uint64_t particles, bool end, fbe_color_t* color) {
    const particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    const std::array<float, 4>& kept = end ? found->color_end : found->color_start;
    *color = { .r = kept[0], .g = kept[1], .b = kept[2], .a = kept[3] };
    return true;
}

FBE_API bool fbe_particles_set_roll(uint64_t particles, float most) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(most) || most < 0.0F || most > std::numbers::pi_v<float>) {
        return fbe::fail(std::format("a particle's roll is up to 0 to Pi each way, not {}", most));
    }
    found->roll = most;
    return true;
}

FBE_API bool fbe_particles_set_seed(uint64_t particles, uint32_t seed) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    found->random = seed != 0 ? seed : 1U;
    return true;
}

FBE_API bool fbe_particles_burst(uint64_t particles, int32_t count) try {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (count < 0) {
        return fbe::fail(std::format("a burst is 0 particles or more, not {}", count));
    }
    const std::uint32_t entity = found->entity;
    fbe::scene::update_world(entity);
    const fbe::scene::vec3_t place = scene().entities.world_matrix[entity].axis(3);
    const fbe::scene::quat_t turn = scene().entities.world_rotation[entity];
    fbe::scene::ready_slots(*found);
    // Born when what's drawn between steps starts from, so it shows all the way from there, even in
    // a pause that never gets to the clock: if that's within its least life, or it'd be dead.
    float since = found->clock - found->captured_clock;
    if (since < 0.0F) {
        since += fbe::scene::particles_wrap;
    }
    const float ago = scene().entities.captured[entity] && since < found->life_min ? since : 0.0F;
    for (std::int32_t i = 0; i < std::min(count, found->capacity); ++i) {
        fbe::scene::spawn(*found, place, turn, ago);
    }
    fbe::scene::trim_pending(*found);
    return true;
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
}

FBE_API int32_t fbe_particles_alive(uint64_t particles) {
    const particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return -1;
    }
    const auto count = std::ranges::count_if(
        found->deaths, [found](double death) { return death > found->total; });
    return static_cast<std::int32_t>(count);
}

} // extern "C"
