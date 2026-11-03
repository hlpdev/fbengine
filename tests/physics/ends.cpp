// A program that makes a world, on Jolt's most threads, steps it wrongly, and ends at once, as a
// game that panics as it starts does, while Jolt's threads would be starting; ends.cmake runs it
// again and again.

#include "fbengine_physics.h"

#include <cstdint>
#include <cstdlib>

int main() {
    fbe_physics_set_threads(FBE_PHYSICS_MAX_THREADS);
    const std::uint64_t world = fbe_physics_new(16);
    // As a panic's program exits, with 101.
    if (!fbe_physics_step(world, 1e30F, 1)) {
        std::exit(101);
    }
    return 0;
}
