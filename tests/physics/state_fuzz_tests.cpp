// fbengine_physics' states, fuzzed: a state whose header was made again for what it holds, as one
// a game is sent over a network might be, passes its hash, so what it holds is all that's left to
// trip on. Each changed is refused, the world left as it was, or restores to a world that steps
// on soundly, with finite numbers; none crashes or hangs.

#include "fbengine_physics.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// A state's header: its magic, format, Jolt's version, fbengine's, and the length and hash of what
// follows, which `resealed` makes again.
constexpr std::size_t length_at = 28;
constexpr std::size_t hash_at = 36;
constexpr std::size_t header_size = 44;

fbe_body_vec3_t at(float x, float y, float z) {
    return { .x = x, .y = y, .z = z };
}

fbe_joint_vec3_t joint_at(float x, float y, float z) {
    return { .x = x, .y = y, .z = z };
}

// `state` with its header's length and hash made again for what it holds now.
std::vector<std::uint8_t> resealed(std::vector<std::uint8_t> state) {
    const std::uint64_t length = state.size() - header_size;
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (std::size_t i = header_size; i < state.size(); ++i) {
        hash = (hash ^ state[i]) * 0x100000001B3ULL;
    }
    std::memcpy(state.data() + length_at, &length, sizeof(length));
    std::memcpy(state.data() + hash_at, &hash, sizeof(hash));
    return state;
}

bool finite(const fbe_body_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A world of everything a state holds: a floor, which doesn't move, boxes on it, touching, a
// kinematic body, each kind of joint, and a character standing on the floor, stepped a while.
struct scene_t {
    std::uint64_t world = fbe_physics_new(64);
    std::vector<std::uint64_t> bodies;
    std::uint64_t floor = 0;
    std::uint64_t character = 0;

    scene_t() {
        const fbe_shape_vec3_t floor_size{ .x = 20.0F, .y = 1.0F, .z = 20.0F };
        const fbe_shape_vec3_t box_size{ .x = 1.0F, .y = 1.0F, .z = 1.0F };
        const fbe_body_vec3_t floor_at = at(0.0F, -0.5F, 0.0F);
        floor =
            fbe_body_add(world, fbe_shape_box(&floor_size), FBE_BODY_STATIC, &floor_at, &unturned);
        const std::uint64_t box = fbe_shape_box(&box_size);
        const auto add = [&](float x, float y, float z, int32_t motion) {
            const fbe_body_vec3_t where = at(x, y, z);
            const std::uint64_t body = fbe_body_add(world, box, motion, &where, &unturned);
            bodies.push_back(body);
            return body;
        };
        const std::uint64_t a = add(-3.0F, 0.5F, 0.0F, FBE_BODY_DYNAMIC);
        const std::uint64_t b = add(-3.0F, 1.5F, 0.0F, FBE_BODY_DYNAMIC);
        fbe_joint_fixed(a, b);
        const std::uint64_t c = add(-1.0F, 0.5F, 0.0F, FBE_BODY_DYNAMIC);
        const fbe_joint_vec3_t ball = joint_at(-1.0F, 2.0F, 0.0F);
        fbe_joint_point(c, add(-1.0F, 1.5F, 0.0F, FBE_BODY_DYNAMIC), &ball);
        const std::uint64_t door = add(1.0F, 2.0F, 0.0F, FBE_BODY_DYNAMIC);
        const fbe_joint_vec3_t hinge = joint_at(1.0F, 3.0F, 0.0F);
        const fbe_joint_vec3_t across = joint_at(0.0F, 0.0F, 1.0F);
        const std::uint64_t hinged = fbe_joint_hinge(door, 0, &hinge, &across);
        fbe_joint_set_motor(hinged, FBE_JOINT_MOTOR_SPEED, 1.0F, 10.0F);
        const std::uint64_t drawer = add(3.0F, 0.5F, 0.0F, FBE_BODY_DYNAMIC);
        const fbe_joint_vec3_t along = joint_at(1.0F, 0.0F, 0.0F);
        const std::uint64_t slid = fbe_joint_slider(drawer, 0, &along);
        fbe_joint_set_limits(slid, -1.0F, 1.0F);
        const std::uint64_t hung = add(5.0F, 3.0F, 0.0F, FBE_BODY_DYNAMIC);
        const fbe_joint_vec3_t top = joint_at(5.0F, 5.0F, 0.0F);
        const fbe_joint_vec3_t under = joint_at(5.0F, 3.5F, 0.0F);
        fbe_joint_distance(hung, 0, &under, &top);
        add(0.0F, 0.5F, 4.0F, FBE_BODY_KINEMATIC);
        const fbe_character_vec3_t standing{ .x = 0.0F, .y = 1.0F, .z = -3.0F };
        character = fbe_character_new(world, fbe_shape_capsule(1.8F, 0.3F), &standing);
        for (int i = 0; i < 30; ++i) {
            const fbe_character_vec3_t walking{ .x = 0.5F, .y = -1.0F, .z = 0.0F };
            fbe_character_move(character, &walking, 1.0F / 60);
            fbe_physics_step(world, 1.0F / 60, 1);
        }
    }

    ~scene_t() {
        fbe_physics_free(world);
    }

    scene_t(const scene_t&) = delete;
    scene_t& operator=(const scene_t&) = delete;
    scene_t(scene_t&&) = delete;
    scene_t& operator=(scene_t&&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> save() const {
        std::vector<std::uint8_t> state(static_cast<std::size_t>(fbe_physics_save(world, 0.25)));
        fbe_physics_saved(world, state.data(), static_cast<std::int64_t>(state.size()));
        return state;
    }

    // Whether everything in the world is somewhere, turned, and going at finite numbers.
    [[nodiscard]] bool sound() const {
        for (const std::uint64_t body : bodies) {
            fbe_body_vec3_t where{};
            fbe_body_vec3_t going{};
            fbe_body_vec3_t turning{};
            fbe_body_quat_t turned{};
            fbe_body_position(body, &where);
            fbe_body_velocity(body, &going);
            fbe_body_angular_velocity(body, &turning);
            fbe_body_rotation(body, &turned);
            if (!finite(where) || !finite(going) || !finite(turning) ||
                !std::isfinite(turned.x + turned.y + turned.z + turned.w)) {
                return false;
            }
        }
        fbe_character_vec3_t standing{};
        fbe_character_position(character, &standing);
        return std::isfinite(standing.x + standing.y + standing.z);
    }

    // Restores `state`, and checks it was refused, the world left as `before`, or that the world
    // restored steps on soundly; then puts `before` back. True if it was refused.
    bool tried(const std::vector<std::uint8_t>& state, const std::vector<std::uint8_t>& before) {
        double carried = 0.0;
        const bool restored = fbe_physics_restore(
            world, state.data(), static_cast<std::int64_t>(state.size()), &carried);
        if (!restored) {
            const std::string said = fbe_physics_error();
            CHECK(
                (said == "that isn't a world's state" ||
                 said == "the state is of other bodies, joints, or characters than the world has"));
            CHECK(save() == before);
        } else {
            CHECK(std::isfinite(carried));
            for (int i = 0; i < 10; ++i) {
                const fbe_character_vec3_t walking{ .x = 0.5F, .y = -1.0F, .z = 0.0F };
                fbe_character_move(character, &walking, 1.0F / 60);
                fbe_physics_step(world, 1.0F / 60, 1);
            }
            CHECK(sound());
            REQUIRE(fbe_physics_restore(world, before.data(),
                                        static_cast<std::int64_t>(before.size()), &carried));
        }
        return !restored;
    }
};

// Where the first body's record is in a state of `scene`: after the header, its handles, and
// Jolt's flags, last step's time, gravity, and count of bodies.
std::size_t first_body(const scene_t& scene) {
    const std::size_t bodies = scene.bodies.size() + 2;
    const std::size_t joints = 5;
    const std::size_t characters = 1;
    return header_size + (8 * 3) + (8 * (bodies + joints + characters)) + 1 + 4 + 12 + 4;
}

// Where `character`'s record is in `state`: its handle, the last eight bytes that are.
std::size_t character_at(const std::vector<std::uint8_t>& state, std::uint64_t character) {
    std::array<std::uint8_t, 8> handle{};
    std::memcpy(handle.data(), &character, sizeof(character));
    for (std::size_t i = state.size() - handle.size(); i > header_size; --i) {
        if (std::memcmp(state.data() + i, handle.data(), handle.size()) == 0) {
            return i;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("a state resealed, with a byte of it changed anywhere, restores soundly or is refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    REQUIRE(scene.sound());
    int refused = 0;
    int tried = 0;
    for (std::size_t i = header_size; i < before.size(); ++i) {
        for (const std::uint8_t value :
             std::array<std::uint8_t, 5>{ 0x00, 0x01, 0x7F, 0x80, 0xFF }) {
            if (before[i] == value) {
                continue;
            }
            std::vector<std::uint8_t> changed = before;
            changed[i] = value;
            refused += scene.tried(resealed(changed), before) ? 1 : 0;
            ++tried;
        }
    }
    // Most of a state is numbers, any of which a world could be at, but its counts, handles, and
    // flags aren't.
    CHECK(refused > tried / 20);
    CHECK(refused < tried);
}

TEST_CASE("a state resealed, with numbers of it not finite, is refused", "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    for (std::size_t i = header_size; i + 4 <= before.size(); ++i) {
        for (const float value :
             { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
            std::vector<std::uint8_t> changed = before;
            std::memcpy(changed.data() + i, &value, sizeof(value));
            scene.tried(resealed(changed), before);
        }
    }
}

TEST_CASE("a state resealed, cut short, made longer, or of random bytes, is refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    for (std::size_t length = header_size; length < before.size(); ++length) {
        const std::vector<std::uint8_t> cut(before.begin(),
                                            before.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK(scene.tried(resealed(cut), before));
    }
    for (const std::size_t more : { 1U, 4U, 100U, 100000U }) {
        std::vector<std::uint8_t> longer = before;
        longer.resize(before.size() + more, 0xAB);
        CHECK(scene.tried(resealed(longer), before));
    }
    std::mt19937 random(1);
    for (int i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> noise = before;
        for (std::size_t j = header_size; j < noise.size(); ++j) {
            noise[j] = static_cast<std::uint8_t>(random());
        }
        CHECK(scene.tried(resealed(noise), before));
    }
}

TEST_CASE("a state resealed, with a few bytes of it changed at random, restores soundly or is "
          "refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    std::mt19937 random(2);
    const auto held = before.size() - header_size;
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::uint8_t> changed = before;
        const std::uint32_t changes = 1 + (random() % 8);
        for (std::uint32_t j = 0; j < changes; ++j) {
            changed[header_size + (random() % held)] = static_cast<std::uint8_t>(random());
        }
        scene.tried(resealed(changed), before);
    }
}

// Jolt puts a body back as awake if its state says so, and a body that doesn't move has nowhere
// to keep that: it was a crash.
TEST_CASE("a state resealed, saying a body that doesn't move is awake, is refused", "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    std::vector<std::uint8_t> changed = before;
    // The floor's id, then whether it's awake.
    const std::size_t awake = first_body(scene) + 4;
    REQUIRE(changed[awake] == 0);
    changed[awake] = 1;
    CHECK(scene.tried(resealed(changed), before));
}

TEST_CASE("a state resealed, with a body somewhere not finite, or turned by a quat not of "
          "length 1, is refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    // The floor's position, after its id and whether it's awake, then its rotation.
    const std::size_t position = first_body(scene) + 5;
    std::vector<std::uint8_t> changed = before;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(changed.data() + position, &nan, sizeof(nan));
    CHECK(scene.tried(resealed(changed), before));
    changed = before;
    const float doubled = 2.0F;
    std::memcpy(changed.data() + position + 12 + 12, &doubled, sizeof(doubled));
    CHECK(scene.tried(resealed(changed), before));
}

// Jolt makes room for as many contacts as a character's state says it had, which could be all the
// memory there is.
TEST_CASE("a state resealed, saying a character had billions of contacts, is refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    const std::size_t record = character_at(before, scene.character);
    REQUIRE(record > 0);
    // Its handle, its ground's state, body, part, position, normal, and velocity, then its
    // position, rotation, velocity, last move's time, and whether it hit too much.
    const std::size_t contacts = record + 8 + 4 + 4 + 4 + 12 + 12 + 12 + 12 + 16 + 12 + 4 + 1;
    std::uint32_t had = 0;
    std::memcpy(&had, before.data() + contacts, sizeof(had));
    REQUIRE(had > 0);
    REQUIRE(had < 64);
    std::vector<std::uint8_t> changed = before;
    const std::uint32_t billions = 4000000000U;
    std::memcpy(changed.data() + contacts, &billions, sizeof(billions));
    CHECK(scene.tried(resealed(changed), before));
}

namespace {

// Where the contacts Jolt keeps are in a state of `scene`, after its bodies, and where they end: a
// body that doesn't move is its id, whether it's awake, where it is, and how it's turned, and one
// that does is that and its velocities, forces, the spheres it's tested still by, how long it's
// been still, and whether it sleeps.
std::pair<std::size_t, std::size_t> contacts_in(const scene_t& scene,
                                                const std::vector<std::uint8_t>& state) {
    const std::size_t moving = scene.bodies.size() + 1;
    std::size_t at = first_body(scene) + 33 + (moving * 134);
    const std::size_t start = at;
    const auto take = [&](std::size_t size) {
        std::uint32_t value = 0;
        std::memcpy(&value, state.data() + at, size);
        at += size;
        return value;
    };
    const std::uint32_t pairs = take(4);
    for (std::uint32_t i = 0; i < pairs; ++i) {
        at += 8 + 12 + 12;
        const std::uint32_t manifolds = take(4);
        for (std::uint32_t j = 0; j < manifolds; ++j) {
            at += 16;
            const std::uint32_t points = take(2);
            at += 12 + 8 + 4 + (points * (12 + 12 + 4));
        }
    }
    at += 16 * take(4);
    return { start, at };
}

} // namespace

// Jolt keeps room for so many pairs of bodies touching, and would run out partway through a state
// of more, with some of it put back.
TEST_CASE("a state resealed, of more bodies touching than Jolt has room for, is refused",
          "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    const auto [start, end] = contacts_in(scene, before);
    std::uint32_t had = 0;
    std::memcpy(&had, before.data() + start, sizeof(had));
    REQUIRE(had > 0);
    REQUIRE(had < 64);
    // Its first body's id, and the world's first constraint's index, 0, after.
    REQUIRE(end + 8 <= before.size());
    std::uint32_t joints = 0;
    std::memcpy(&joints, before.data() + end, sizeof(joints));
    REQUIRE(joints == 5);
    // A state of `pairs` pairs, each in order, of no manifolds, in place of what it had.
    const auto crowded = [&](std::uint32_t pairs) {
        std::vector<std::uint8_t> made(before.begin(),
                                       before.begin() + static_cast<std::ptrdiff_t>(start));
        const auto put = [&made](std::uint32_t value) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            made.insert(made.end(), bytes, bytes + sizeof(value));
        };
        put(pairs);
        for (std::uint32_t i = 0; i < pairs; ++i) {
            put(0);
            put(i + 1);
            made.insert(made.end(), 24, 0);
            put(0);
        }
        put(0);
        made.insert(made.end(), before.begin() + static_cast<std::ptrdiff_t>(end), before.end());
        return resealed(made);
    };
    // The world has room for 1024.
    CHECK_FALSE(scene.tried(crowded(1024), before));
    CHECK(scene.tried(crowded(1025), before));
    CHECK(scene.tried(crowded(100000), before));
}

namespace {

// A manifold's key: its first body and part, and its second's.
struct key_t {
    std::uint32_t a;
    std::uint32_t a_part;
    std::uint32_t b;
    std::uint32_t b_part;
};

// A pair of bodies touching, by their ids, and its manifolds' parts, each of no points.
struct pair_t {
    std::uint32_t a;
    std::uint32_t b;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> parts;
};

// `before`, a state of `scene`, with the contacts Jolt keeps made of `pairs`, and of `continuous`,
// the manifolds of continuous collisions, resealed.
std::vector<std::uint8_t> with_contacts(const scene_t& scene,
                                        const std::vector<std::uint8_t>& before,
                                        const std::vector<pair_t>& pairs,
                                        const std::vector<key_t>& continuous) {
    const auto [start, end] = contacts_in(scene, before);
    std::vector<std::uint8_t> made(before.begin(),
                                   before.begin() + static_cast<std::ptrdiff_t>(start));
    const auto put = [&made](const auto& value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        made.insert(made.end(), bytes, bytes + sizeof(value));
    };
    put(static_cast<std::uint32_t>(pairs.size()));
    for (const pair_t& pair : pairs) {
        put(pair.a);
        put(pair.b);
        // How far they moved and turned since.
        made.insert(made.end(), 24, 0);
        put(static_cast<std::uint32_t>(pair.parts.size()));
        for (const auto& [a_part, b_part] : pair.parts) {
            put(key_t{ .a = pair.a, .a_part = a_part, .b = pair.b, .b_part = b_part });
            // No points, a normal straight up, and no friction.
            put(std::uint16_t{ 0 });
            put(std::array<float, 3>{ 0.0F, 1.0F, 0.0F });
            made.insert(made.end(), 12, 0);
        }
    }
    put(static_cast<std::uint32_t>(continuous.size()));
    for (const key_t& key : continuous) {
        put(key);
    }
    made.insert(made.end(), before.begin() + static_cast<std::ptrdiff_t>(end), before.end());
    return resealed(made);
}

} // namespace

// Jolt keeps its pairs and manifolds in one hash map, which it asserts has no key twice; it saves
// them in order, by their keys, and a state in another order, or of a key twice, is refused.
TEST_CASE("a state resealed, of contacts out of order or twice, is refused", "[stability]") {
    scene_t scene;
    const std::vector<std::uint8_t> before = scene.save();
    const pair_t first{ .a = 0, .b = 1, .parts = { { 0, 0 }, { 0, 1 } } };
    const pair_t second{ .a = 0, .b = 2, .parts = { { 0, 0 } } };
    // In order, each once, it restores.
    CHECK_FALSE(
        scene.tried(with_contacts(scene, before, { first, second }, { { 0, 0, 3, 0 } }), before));
    // Pairs out of order, or twice.
    CHECK(scene.tried(with_contacts(scene, before, { second, first }, {}), before));
    CHECK(scene.tried(with_contacts(scene, before, { first, first }, {}), before));
    // A pair's manifolds out of order, or twice, or of other bodies than the pair's.
    const pair_t backwards{ .a = 0, .b = 1, .parts = { { 0, 1 }, { 0, 0 } } };
    const pair_t again{ .a = 0, .b = 1, .parts = { { 0, 1 }, { 0, 1 } } };
    CHECK(scene.tried(with_contacts(scene, before, { backwards }, {}), before));
    CHECK(scene.tried(with_contacts(scene, before, { again }, {}), before));
    std::vector<std::uint8_t> other = with_contacts(scene, before, { second }, {});
    // The manifold's first body, after the count, the pair, its movement, and its count.
    const auto [start, end] = contacts_in(scene, before);
    const std::size_t manifold = start + 4 + 8 + 24 + 4;
    other[manifold] = 5;
    CHECK(scene.tried(resealed(other), before));
    // Continuous collisions' manifolds out of order, twice, or with a pair's manifold's key.
    CHECK(
        scene.tried(with_contacts(scene, before, {}, { { 0, 0, 4, 0 }, { 0, 0, 3, 0 } }), before));
    CHECK(
        scene.tried(with_contacts(scene, before, {}, { { 0, 0, 3, 0 }, { 0, 0, 3, 0 } }), before));
    CHECK(scene.tried(with_contacts(scene, before, { first }, { { 0, 0, 1, 1 } }), before));
}
