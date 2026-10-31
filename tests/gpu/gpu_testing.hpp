#pragma once

// What the GPU's C++ tests share: calls required to work, environment variables, and stopping the
// GPU at a test's end, which has to leave nothing behind.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fbengine.h>
#include <gpu/vulkan.hpp>

namespace fbe::tests {

/// Requires a call that can fail to have worked, saying fbe_error's why if it didn't.
inline void require(bool worked) {
    INFO(fbe_error());
    REQUIRE(worked);
}

/// Sets an environment variable, as the loader and SDL read them, before they start.
inline void set_variable(const char* name, const char* value) {
#ifdef _WIN32
    REQUIRE(_putenv_s(name, value) == 0);
#else
    REQUIRE(setenv(name, value, 1) == 0);
#endif
}

/// Stops the GPU, which has to free everything it made: the validation layers find no error
/// (destroying a device with something of it left is one), and VMA has nothing left.
inline void stop_gpu() {
    fbe_gpu_stop();
    CHECK(!fbe_gpu_started());
    CHECK(fbe_gpu_validation_errors() == 0);
    CHECK(fbe::gpu::allocations_left() == 0);
}

} // namespace fbe::tests
