// The engine's jobs, started the first time something needs them, and the tickets each flip hands
// to the FB side.

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

namespace {

std::atomic<bool> started = false;
std::atomic<std::uint64_t> last_ticket = 0;
// The tickets the last flip handed over; the main thread's.
std::vector<std::uint64_t> handed;

} // namespace

namespace fbe {

int engine_workers() {
    const unsigned cores = std::thread::hardware_concurrency();
    return std::max(static_cast<int>(cores) - 1, 1);
}

jobs_t& jobs() {
    static auto* const engine = new jobs_t(engine_workers());
    started = true;
    return *engine;
}

void hand_over_jobs() {
    handed.clear();
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

FBE_API int32_t fbe_jobs_finished_count(void) {
    return static_cast<int32_t>(handed.size());
}

FBE_API uint64_t fbe_jobs_finished_at(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= handed.size()) {
        return 0;
    }
    return handed[static_cast<std::size_t>(index)];
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
