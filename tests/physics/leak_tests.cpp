// What fbengine_physics leaves allocated, counted: every allocation of its C++ and of Jolt's, by
// the test's own operator new and Jolt's allocator hooks, which pass each on as it was. A world of
// bodies, characters, and joints made, stepped, and freed, again and again, leaves the count as it
// was, to the allocation.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <Jolt/Core/Memory.h>
#include <new>
#include <vector>

namespace {

std::atomic<std::int64_t> live{ 0 };

JPH::AllocateFunction jolt_allocate = nullptr;
JPH::ReallocateFunction jolt_reallocate = nullptr;
JPH::FreeFunction jolt_free = nullptr;
JPH::AlignedAllocateFunction jolt_aligned_allocate = nullptr;
JPH::AlignedFreeFunction jolt_aligned_free = nullptr;

void* counted_allocate(std::size_t size) {
    void* made = jolt_allocate(size);
    if (made != nullptr) {
        ++live;
    }
    return made;
}

void* counted_reallocate(void* block, std::size_t old_size, std::size_t new_size) {
    void* made = jolt_reallocate(block, old_size, new_size);
    if (block == nullptr && made != nullptr) {
        ++live;
    }
    return made;
}

void counted_free(void* block) {
    if (block != nullptr) {
        --live;
    }
    jolt_free(block);
}

void* counted_aligned_allocate(std::size_t size, std::size_t alignment) {
    void* made = jolt_aligned_allocate(size, alignment);
    if (made != nullptr) {
        ++live;
    }
    return made;
}

void counted_aligned_free(void* block) {
    if (block != nullptr) {
        --live;
    }
    jolt_aligned_free(block);
}

// Counts Jolt's allocations from now, passing each on to what Jolt had.
void count_jolt() {
    if (jolt_allocate != nullptr) {
        return;
    }
    jolt_allocate = JPH::Allocate;
    jolt_reallocate = JPH::Reallocate;
    jolt_free = JPH::Free;
    jolt_aligned_allocate = JPH::AlignedAllocate;
    jolt_aligned_free = JPH::AlignedFree;
    JPH::Allocate = counted_allocate;
    JPH::Reallocate = counted_reallocate;
    JPH::Free = counted_free;
    JPH::AlignedAllocate = counted_aligned_allocate;
    JPH::AlignedFree = counted_aligned_free;
}

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// A world of 300 bodies of their own shapes, joints between them, and characters, stepped, saved,
// restored, and freed a piece at a time or with the world; true if each was made.
bool round(bool piece_by_piece) {
    const std::uint64_t world = fbe_physics_new(400);
    const fbe_shape_vec3_t floor_size{ .x = 100.0F, .y = 1.0F, .z = 100.0F };
    const fbe_body_vec3_t floor_at{ .x = 0.0F, .y = -0.5F, .z = 0.0F };
    std::vector<std::uint64_t> shapes{ fbe_shape_box(&floor_size) };
    std::vector<std::uint64_t> bodies{ fbe_body_add(world, shapes[0], FBE_BODY_STATIC, &floor_at,
                                                    &unturned) };
    for (int i = 0; i < 300; ++i) {
        const fbe_shape_vec3_t box{ .x = 0.5F, .y = 0.5F, .z = 0.5F };
        shapes.push_back(i % 2 == 0 ? fbe_shape_box(&box) : fbe_shape_sphere(0.4F));
        const fbe_body_vec3_t where{ .x = static_cast<float>(i % 15) - 7.0F,
                                     .y = 0.5F + static_cast<float>(i / 150),
                                     .z = static_cast<float>((i / 15) % 10) - 5.0F };
        bodies.push_back(fbe_body_add(world, shapes.back(), FBE_BODY_DYNAMIC, &where, &unturned));
    }
    std::vector<std::uint64_t> joints;
    for (std::size_t i = 1; i + 1 < bodies.size(); i += 6) {
        const fbe_joint_vec3_t point{ .x = 0.0F, .y = 1.0F, .z = 0.0F };
        joints.push_back(i % 4 == 1 ? fbe_joint_fixed(bodies[i], bodies[i + 1])
                                    : fbe_joint_point(bodies[i], bodies[i + 1], &point));
    }
    shapes.push_back(fbe_shape_capsule(1.8F, 0.3F));
    std::vector<std::uint64_t> characters;
    for (int i = 0; i < 20; ++i) {
        const fbe_character_vec3_t where{ .x = static_cast<float>(i) - 10.0F,
                                          .y = 1.0F,
                                          .z = 20.0F };
        characters.push_back(fbe_character_new(world, shapes.back(), &where));
    }
    bool made = world != 0;
    for (const std::vector<std::uint64_t>* all : { &shapes, &bodies, &joints, &characters }) {
        for (const std::uint64_t handle : *all) {
            made = made && handle != 0;
        }
    }
    const auto step = [&] {
        for (const std::uint64_t character : characters) {
            const fbe_character_vec3_t walking{ .x = 0.0F, .y = -1.0F, .z = -1.0F };
            fbe_character_move(character, &walking, 1.0F / 60);
        }
        fbe_physics_step(world, 1.0F / 60, 1);
    };
    for (int i = 0; i < 10; ++i) {
        step();
    }
    std::vector<std::uint8_t> state(static_cast<std::size_t>(fbe_physics_save(world, 0.0)));
    fbe_physics_saved(world, state.data(), static_cast<std::int64_t>(state.size()));
    step();
    double carried = 0.0;
    made = made && fbe_physics_restore(world, state.data(), static_cast<std::int64_t>(state.size()),
                                       &carried);
    step();
    if (piece_by_piece) {
        for (const std::uint64_t joint : joints) {
            fbe_joint_free(joint);
        }
        for (const std::uint64_t character : characters) {
            fbe_character_free(character);
        }
        for (const std::uint64_t body : bodies) {
            fbe_body_free(body);
        }
        step();
    }
    fbe_physics_free(world);
    for (const std::uint64_t shape : shapes) {
        fbe_shape_free(shape);
    }
    return made;
}

} // namespace

// The test's own, counting every allocation of the program's C++ as it goes.
void* operator new(std::size_t size) {
    void* made = std::malloc(size == 0 ? 1 : size);
    if (made == nullptr) {
        throw std::bad_alloc();
    }
    ++live;
    return made;
}

void operator delete(void* block) noexcept {
    if (block != nullptr) {
        --live;
    }
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    operator delete(block);
}

TEST_CASE("worlds of bodies, characters, and joints made and freed leave no allocation behind",
          "[stability]") {
    // Jolt, its threads, and its types start with the first world.
    REQUIRE(fbe_physics_free(fbe_physics_new(1)));
    count_jolt();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(round(i % 2 == 0));
    }
    bool made = true;
    std::vector<std::int64_t> after;
    after.reserve(20);
    const std::int64_t before = live;
    for (int i = 0; i < 20; ++i) {
        made = round(i % 2 == 0) && made;
        after.push_back(live - before);
    }
    REQUIRE(made);
    for (std::size_t i = 0; i < after.size(); ++i) {
        INFO("after round " << i);
        CHECK(after[i] == 0);
    }
}
