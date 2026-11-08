#ifndef FBENGINE_TESTS_OWN_NEW_HPP
#define FBENGINE_TESTS_OWN_NEW_HPP

// The out-of-memory tests' operator new and delete, tests/own_new.cpp, built into each of them:
// every form, plain, sized, aligned, aligned and sized, nothrow, and array, replaced as a set, so
// what one allocates its own delete frees. Each allocation on a thread counts, and the one it's
// told to fails, and every thread's that's still allocated is counted, for a soak.

#include <atomic>

namespace fbe::tests {

/// Which of this thread's allocations fails, -1 for none, how many there have been, whether one
/// failed, and how many frees there have been.
inline thread_local long failing = -1;
inline thread_local long allocated = 0;
inline thread_local bool failed = false;
inline thread_local long freed = 0;

/// How many allocations, on every thread, haven't been freed: the same after each of a soak's
/// rounds, once they've warmed, unless one leaks.
inline std::atomic<long> live = 0;

} // namespace fbe::tests

#endif
