#ifndef FBENGINE_TESTS_PHYSICS_LIVE_HPP
#define FBENGINE_TESTS_PHYSICS_LIVE_HPP

// What fbengine_physics_internal_tests counts, tests/physics/leak_tests.cpp's: every allocation of
// its C++, by the test's own operator new, and of Jolt's, by its allocator's hooks, on every
// thread, so what one of Jolt's threads allocates and the caller frees balances.

#include <cstdint>

namespace fbe::tests {

/// Counts Jolt's allocations from now on as well, passing each on to what Jolt had; once Jolt's
/// started, before any world's counted.
void count_jolt();

/// How many allocations are live now.
std::int64_t live_allocations();

} // namespace fbe::tests

#endif
