// A program another thread ends with a plain exit() while its moves are on the job system's
// workers, as a panic on a Spawn's thread does: on Linux and macOS exit() ends a library's statics
// with the workers still reading them, so what they read has to outlive it. Each round's a process
// of its own, at another moment in the moves, and each has to end with its exit's code.

#include "fbengine.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

[[noreturn]] void round_of(int delay) {
    const std::uint64_t world = fbe_world_create();
    fbe_world_set_collision(world, 1, 2, FBE_RESPONSE_SLIDE);
    const std::uint64_t terrain = fbe_entity_create(world, 0, FBE_ENTITY_TERRAIN);
    fbe_terrain_set_size(terrain, 256);
    fbe_entity_set_collision_group(terrain, 2);
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
    std::thread ender([delay] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        std::exit(101);
    });
    ender.detach();
    fbe_world_collide(world);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::_Exit(7);
}

} // namespace

int main() {
    for (int round = 0; round < 60; ++round) {
        const pid_t child = fork();
        if (child == 0) {
            round_of(1 + (round * 2));
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 101) {
            std::puts("a round ended some other way");
            return 1;
        }
    }
    std::puts("ended");
    return 0;
}
