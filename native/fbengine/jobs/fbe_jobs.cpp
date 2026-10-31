// The engine's jobs, started the first time something needs them, and the tickets they hand to the
// FB side, which keeps them until the FB side forgets them.

#include "core/engine.hpp"
#include "fbengine.h"
#include "jobs/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

namespace {

std::atomic<bool> started = false;
std::atomic<std::uint64_t> last_ticket = 0;
// The tickets handed over that the FB side hasn't forgotten, and how many hand-overs it's asked
// for; the main thread's.
std::vector<std::uint64_t> handed;
std::uint64_t hand_overs = 0;

} // namespace

namespace fbe {

int engine_workers() {
#ifdef __linux__
    // The cores this process may run on, which a container or taskset can make fewer.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(CPU_COUNT(&set) - 1, 1);
    }
#endif
    return std::max(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1);
}

jobs_t& jobs() {
    static auto* const engine = new jobs_t(engine_workers());
    started = true;
    return *engine;
}

void hand_over_jobs() {
    if (started) {
        jobs().hand_over();
    }
}

std::uint64_t new_ticket() {
    return ++last_ticket;
}

void hand_to_fb(std::uint64_t ticket) {
    handed.push_back(ticket);
}

} // namespace fbe

extern "C" {

FBE_API int32_t fbe_jobs_workers(void) {
    return fbe::engine_workers();
}

FBE_API void fbe_jobs_hand_over(void) {
    ++hand_overs;
    fbe::hand_over_jobs();
}

FBE_API uint64_t fbe_jobs_hand_overs(void) {
    return hand_overs;
}

FBE_API int32_t fbe_jobs_finished_count(void) {
    return static_cast<int32_t>(handed.size());
}

FBE_API uint64_t fbe_jobs_finished_at(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= handed.size()) {
        return 0;
    }
    return handed[static_cast<std::size_t>(index)];
}

FBE_API void fbe_jobs_forget_finished(void) {
    handed.clear();
}

FBE_API uint64_t fbe_jobs_simulate(int32_t microseconds) {
    if (microseconds < 0) {
        fbe::fail(std::format("a job takes 0 microseconds or more, not {}", microseconds));
        return 0;
    }
    const std::uint64_t ticket = fbe::new_ticket();
    fbe::jobs_t& jobs = fbe::jobs();
    jobs.run([&jobs, ticket, microseconds] {
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
        jobs.to_main([ticket] { fbe::hand_to_fb(ticket); });
    });
    return ticket;
}

} // extern "C"
