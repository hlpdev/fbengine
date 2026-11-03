// A program whose moves went to the job system's workers, then, after they've finished, ends at
// once, its workers waiting for more, as a panic ends one: it has to end, whatever they've kept.
// ctest gives it a timeout, and looks for what it prints.

#include "fbengine.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
    const std::uint64_t world = fbe_world_create();
    fbe_world_set_collision(world, 1, 2, FBE_RESPONSE_SLIDE);
    const std::uint64_t terrain = fbe_entity_create(world, 0, FBE_ENTITY_TERRAIN);
    fbe_terrain_set_size(terrain, 256);
    fbe_entity_set_collision_group(terrain, 2);
    // Long sweeps over a big terrain, many of them, which are worth the workers.
    std::array<std::uint64_t, 400> movers{};
    for (std::uint64_t& mover : movers) {
        mover = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
        fbe_entity_set_collision_group(mover, 1);
        fbe_entity_set_collision_radius(mover, 0.5F, 0.5F);
        fbe_entity_set_position(mover, 1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD);
    }
    fbe_world_collide(world);
    for (const std::uint64_t mover : movers) {
        fbe_entity_set_position(mover, 255.0F, 1.0F, 255.0F, FBE_SPACE_WORLD);
    }
    fbe_world_collide(world);
    std::puts("moved");
    std::fflush(stdout);
    std::exit(101);
}
