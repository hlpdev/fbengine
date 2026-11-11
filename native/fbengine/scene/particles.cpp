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
/// The most steps an age counts as, and the biggest a grown half-size is, so both are finite
/// whatever a step's length and growth are: the scene shader has the same.
constexpr float most_steps = 1e9F;
constexpr float most_grown = 1e9F;
constexpr float most_term = 1e30F;

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

/// How many steps an age is: whole ones, held, or part of one too; none with no step.
float steps_at(const particles_t& found, float age) {
    if (found.step <= 0.0F) {
        return 0.0F;
    }
    const float steps = found.held ? std::floor(age / found.step) : age / found.step;
    return std::fmin(steps, most_steps);
}

/// A half-size after `steps` steps, each adding to it and then multiplying it: never below 0, and
/// never past most_grown. What's added is times the sum of the multiple's powers below `steps`,
/// which is worked out from the power's logarithm, so a multiple near 1 loses nothing to rounding.
float grown(const particles_t& found, float size, float steps) {
    if (steps <= 0.0F) {
        return size;
    }
    const float times = found.grow_multiplied;
    float power = 1.0F;
    float sum = steps;
    if (times != 1.0F) {
        // Near 1 its logarithm's from its series, as the shader has it.
        const float past = times - 1.0F;
        const float logarithm = std::fabs(past) < 0.01F
                                    ? past * (1.0F - (past * (0.5F - (past / 3.0F))))
                                    : std::log(times);
        const float grows = std::clamp(steps * logarithm, -100.0F, 41.0F);
        power = std::exp(grows);
        const float less = std::fabs(grows) < 0.01F
                               ? grows * (1.0F + (grows * (0.5F + (grows / 6.0F))))
                               : power - 1.0F;
        sum = less / past;
    }
    const float all = std::fmin(times * sum, most_term);
    const float added = std::clamp(found.grow_added * all, -most_term, most_term);
    return std::clamp(std::fmin(size * power, most_term) + added, 0.0F, most_grown);
}

/// When one of this life is gone: its life, or sooner if its alpha's faded to nothing by then,
/// which, held, is at the first whole step after its birth that has none left.
float ends_at(const particles_t& found, float life) {
    if (found.step <= 0.0F || found.fade <= 0.0F) {
        return life;
    }
    const float falls =
        (found.fade / found.step) - ((found.color_end[3] - found.color_start[3]) / life);
    if (falls <= 0.0F) {
        return life;
    }
    float gone = found.color_start[3] / falls;
    if (found.held) {
        gone = std::fmax(std::ceil((gone / found.step) - 1e-4F), 1.0F) * found.step;
    }
    return std::fmin(life, gone);
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
    vec3_t velocity = way * speed;
    const float life = between(found, found.life_min, found.life_max);
    // What's new takes its numbers only where it's set, so a seed's run of what was there before
    // is what it was.
    if (found.start_box.x != 0.0F || found.start_box.y != 0.0F || found.start_box.z != 0.0F) {
        const float x = between(found, -found.start_box.x, found.start_box.x);
        const float y = between(found, -found.start_box.y, found.start_box.y);
        const float z = between(found, -found.start_box.z, found.start_box.z);
        place = place + turn * vec3_t(x, y, z);
    }
    const vec3_t least = found.velocity_low;
    const vec3_t most = found.velocity_high;
    if (least.x != most.x || least.y != most.y || least.z != most.z) {
        const float x = between(found, least.x, most.x);
        const float y = between(found, least.y, most.y);
        const float z = between(found, least.z, most.z);
        velocity = velocity + vec3_t(x, y, z);
    } else {
        velocity = velocity + least;
    }
    const float scale = found.scale_min != found.scale_max
                            ? between(found, found.scale_min, found.scale_max)
                            : found.scale_min;
    const float ends = ends_at(found, life);
    // Dead already, at the end of a long step, as if it never was.
    if (ago >= ends) {
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
        .scale = scale,
        .ends = ends,
    };
    const auto slot = static_cast<std::uint32_t>(found.next);
    found.pending.emplace_back(slot, made);
    if (!found.kept.empty()) {
        found.kept[slot] = made;
    }
    found.next = (found.next + 1) % static_cast<std::size_t>(found.capacity);
    found.born = std::min(found.born + 1, found.capacity);
    found.deaths[slot] = found.total + ends - ago;
    found.latest_death = std::fmax(found.latest_death, found.deaths[slot]);
    // Where it can reach by its speed, for the box what's alive is in.
    const float reach = length(velocity) * life;
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
            if (!found.kept.empty()) {
                found.kept[slot] = {};
            }
        }
    }
}

/// Forgets what it has, its slots' deaths made ready first, as what can run out of memory.
void make_room(particles_t& found, std::int32_t capacity) {
    std::vector<double> deaths(static_cast<std::size_t>(capacity), 0.0);
    found.deaths = std::move(deaths);
    if (!found.kept.empty()) {
        std::vector<particle_t> kept(static_cast<std::size_t>(capacity));
        found.kept = std::move(kept);
    }
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

/// The bursts a step of `seconds` has, each born where the emitter was then, on its way from
/// `from` by `across`: those at so many seconds since they were set off, 0 and each `burst_every`
/// after while they last, from the step's start up to its end, which is the next step's.
void step_bursts(particles_t& found, float seconds, vec3_t from, vec3_t across, quat_t turn) {
    if (found.burst_count <= 0 || found.burst_since < 0.0) {
        return;
    }
    const auto every = static_cast<double>(found.burst_every);
    const auto again = static_cast<double>(found.burst_again);
    const auto lasts = static_cast<double>(found.burst_lasts);
    const auto wrap = [&](double since) { return again > 0.0 ? std::fmod(since, again) : since; };
    // The bursts due in `left` seconds from `since`, `next` the next of them: each is given how
    // long before the end it comes.
    const auto walk = [&](double left, double& since, std::int64_t& next, const auto& burst) {
        while (left > 0.0) {
            const double at = static_cast<double>(next) * every;
            const bool due = (lasts <= 0.0 || at < lasts) && (again <= 0.0 || at < again);
            // How long till the next thing: a burst, or their being set off again.
            const double until = (due ? at : again) - since;
            if ((!due && again <= 0.0) || until >= left) {
                since += left;
                break;
            }
            left -= std::fmax(until, 0.0);
            if (!due) {
                since = 0.0;
                next = 0;
                continue;
            }
            since = at;
            ++next;
            burst(left);
        }
    };
    // What's born a longest life before the step's end is dead by it: that much of a long step
    // goes by with nothing to show for it.
    auto left = static_cast<double>(seconds);
    const auto window = static_cast<double>(found.life_max);
    if (left > window) {
        found.burst_since = wrap(found.burst_since + (left - window));
        found.burst_next = static_cast<std::int64_t>(std::ceil(found.burst_since / every));
        left = window;
    }
    // Of the rest, a burst is passed over only if those due after it fill the emitter with what's
    // sure to be alive at the step's end, each of them younger than the shortest life there is,
    // so it would be lost to them anyway: they're counted first.
    const std::int32_t each = std::min(found.burst_count, found.capacity);
    const std::int64_t needed = (static_cast<std::int64_t>(found.capacity) + each - 1) / each;
    const double sure = 0.999 * static_cast<double>(std::fmin(ends_at(found, found.life_min),
                                                              ends_at(found, found.life_max)));
    std::int64_t all = 0;
    std::int64_t alive = 0;
    double since = found.burst_since;
    std::int64_t next = found.burst_next;
    walk(left, since, next, [&](double ago) {
        ++all;
        if (ago < sure) {
            ++alive;
        }
    });
    std::int64_t passed = alive >= needed ? all - needed : 0;
    walk(left, found.burst_since, found.burst_next, [&](double ago) {
        if (passed > 0) {
            --passed;
            return;
        }
        const float part = seconds > 0.0F ? 1.0F - static_cast<float>(ago / seconds) : 1.0F;
        for (std::int32_t i = 0; i < each; ++i) {
            spawn(found, from + across * part, turn, static_cast<float>(ago));
        }
    });
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
    step_bursts(found, seconds, from, across, turn);
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
    copy.kept.clear();
    copy.burst_since = std::fmin(copy.burst_since, 0.0);
    copy.burst_next = 0;
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

bool particle_state(const particles_t& found, const particle_t& particle, float now,
                    particle_state_t& state) {
    float age = now - particle.birth;
    if (age < 0.0F) {
        age += particles_wrap;
    }
    if (age >= particle.ends) {
        return false;
    }
    const vec3_t born(particle.position[0], particle.position[1], particle.position[2]);
    const vec3_t going(particle.velocity[0], particle.velocity[1], particle.velocity[2]);
    // As the scene shader's particleVertex has it.
    const float dragged = found.drag * age;
    float left = age * (1.0F - (dragged * (0.5F - (dragged / 6.0F))));
    float fell = age * age * (0.5F - (dragged * ((1.0F / 6.0F) - (dragged / 24.0F))));
    if (dragged >= 0.01F) {
        left = (1.0F - std::exp(-dragged)) / found.drag;
        fell = (age - left) / found.drag;
    }
    const float part = age / particle.life;
    const float steps = steps_at(found, age);
    const float size =
        (found.size_start + ((found.size_end - found.size_start) * part)) * particle.scale;
    const float alpha = found.color_start[3] + ((found.color_end[3] - found.color_start[3]) * part);
    state = {
        .place = born + going * left + found.gravity * fell,
        .half_width = grown(found, size * found.shape[0], steps),
        .half_height = grown(found, size * found.shape[1], steps),
        .alpha = std::fmax(alpha - (found.fade * steps), 0.0F),
        .age = age,
    };
    return true;
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
    // Its biggest half-size: at its birth, or, grown, at the longest life's end.
    const float widest = std::fmax(found.size_start, found.size_end) * found.scale_max *
                         std::fmax(found.shape[0], found.shape[1]);
    const float biggest = std::fmax(widest, grown(found, widest, steps_at(found, found.longest)));
    const float reach = falling + (biggest * std::numbers::sqrt2_v<float>);
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

FBE_API bool fbe_particles_set_start_box(uint64_t particles, float x, float y, float z) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x < 0.0F || y < 0.0F ||
        z < 0.0F) {
        return fbe::fail(
            std::format("a particle's start box is 0 or more each way, not {}, {}, {}", x, y, z));
    }
    found->start_box = fbe::scene::vec3_t(x, y, z);
    return true;
}

FBE_API bool fbe_particles_start_box(uint64_t particles, fbe_vec3_t* box) {
    const particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    *box = { .x = found->start_box.x, .y = found->start_box.y, .z = found->start_box.z };
    return true;
}

FBE_API bool fbe_particles_set_velocity_box(uint64_t particles, const fbe_vec3_t* least,
                                            const fbe_vec3_t* most) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    const std::array numbers{ least->x, least->y, least->z, most->x, most->y, most->z };
    if (!std::ranges::all_of(numbers, [](float each) { return std::isfinite(each); }) ||
        most->x < least->x || most->y < least->y || most->z < least->z) {
        return fbe::fail("a particle's added velocity is finite, least to most on each axis");
    }
    found->velocity_low = fbe::scene::vec3_t(least->x, least->y, least->z);
    found->velocity_high = fbe::scene::vec3_t(most->x, most->y, most->z);
    return true;
}

FBE_API bool fbe_particles_velocity_box(uint64_t particles, fbe_vec3_t* least, fbe_vec3_t* most) {
    const particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    *least = { .x = found->velocity_low.x, .y = found->velocity_low.y, .z = found->velocity_low.z };
    *most = {
        .x = found->velocity_high.x,
        .y = found->velocity_high.y,
        .z = found->velocity_high.z,
    };
    return true;
}

FBE_API bool fbe_particles_set_size_range(uint64_t particles, float least, float most) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(least) || !std::isfinite(most) || least < 0.0F || most < least) {
        return fbe::fail(
            std::format("what a particle's size is times is 0 or more, least to most, not {} to {}",
                        least, most));
    }
    found->scale_min = least;
    found->scale_max = most;
    return true;
}

FBE_API bool fbe_particles_set_shape(uint64_t particles, float width, float height) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0.0F || height < 0.0F) {
        return fbe::fail(
            std::format("a particle's width and height are 0 or more times its size, not {} and {}",
                        width, height));
    }
    found->shape = { width, height };
    return true;
}

FBE_API bool fbe_particles_set_steps(uint64_t particles, float seconds, bool held) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return fbe::fail(std::format("a particle's step is 0 seconds or more, not {}", seconds));
    }
    found->step = seconds;
    found->held = held;
    return true;
}

FBE_API bool fbe_particles_set_growth(uint64_t particles, float added, float multiplied) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(added) || !std::isfinite(multiplied) || multiplied <= 0.0F) {
        return fbe::fail(std::format(
            "what's added to a particle's size a step is finite, and what it's times above 0, not "
            "{} and {}",
            added, multiplied));
    }
    found->grow_added = added;
    found->grow_multiplied = multiplied;
    return true;
}

FBE_API bool fbe_particles_set_fade(uint64_t particles, float a_step) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(a_step) || a_step < 0.0F) {
        return fbe::fail(
            std::format("what's taken off a particle's alpha a step is 0 or more, not {}", a_step));
    }
    found->fade = a_step;
    return true;
}

FBE_API bool fbe_particles_set_roll_along(uint64_t particles, bool along) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    found->roll_along = along;
    return true;
}

FBE_API bool fbe_particles_settings(uint64_t particles, fbe_particles_settings_t* settings) {
    const particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    *settings = {
        .size_least = found->scale_min,
        .size_most = found->scale_max,
        .width = found->shape[0],
        .height = found->shape[1],
        .step = found->step,
        .added = found->grow_added,
        .multiplied = found->grow_multiplied,
        .fade = found->fade,
        .burst_every = found->burst_every,
        .burst_lasts = found->burst_lasts,
        .burst_again = found->burst_again,
        .burst_count = found->burst_count,
        .held = found->held,
        .roll_along = found->roll_along,
        .bursting = found->burst_count > 0 && found->burst_since >= 0.0 &&
                    (found->burst_lasts <= 0.0F || found->burst_again > 0.0F ||
                     found->burst_since < static_cast<double>(found->burst_lasts)),
    };
    return true;
}

FBE_API bool fbe_particles_set_bursts(uint64_t particles, int32_t count, float every, float lasts,
                                      float again) {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return false;
    }
    if (count < 0 || !std::isfinite(every) || !std::isfinite(lasts) || !std::isfinite(again) ||
        every < fbe::scene::least_burst || lasts < 0.0F ||
        (again != 0.0F && again < fbe::scene::least_burst)) {
        return fbe::fail(std::format(
            "bursts are of 0 particles or more, every 0.001 seconds or more, lasting 0 or more, "
            "and set off again after 0, for never, or 0.001 or more, not {}, {}, {}, {}",
            count, every, lasts, again));
    }
    found->burst_count = count;
    found->burst_every = every;
    found->burst_lasts = lasts;
    found->burst_again = again;
    found->burst_since = count > 0 ? 0.0 : -1.0;
    found->burst_next = 0;
    return true;
}

FBE_API int32_t fbe_particles_read(uint64_t particles, fbe_particle_t* read, int32_t capacity) try {
    particles_t* found = fbe::scene::particles_of(particles);
    if (found == nullptr) {
        return -1;
    }
    if (found->kept.empty()) {
        // From now on: what was born before isn't known here.
        found->kept.resize(static_cast<std::size_t>(found->capacity));
        return 0;
    }
    std::int32_t count = 0;
    for (const fbe::scene::particle_t& particle : found->kept) {
        fbe::scene::particle_state_t state;
        if (!fbe::scene::particle_state(*found, particle, found->clock, state)) {
            continue;
        }
        if (count < capacity && read != nullptr) {
            read[count] = {
                .position = { .x = state.place.x, .y = state.place.y, .z = state.place.z },
                .half_width = state.half_width,
                .half_height = state.half_height,
                .alpha = state.alpha,
                .age = state.age,
            };
        }
        ++count;
    }
    return count;
} catch (const std::bad_alloc&) {
    fbe::fail("out of memory");
    return -1;
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
