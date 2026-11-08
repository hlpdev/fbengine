#ifndef FBENGINE_TESTS_STABILITY_HPP
#define FBENGINE_TESTS_STABILITY_HPP

// What the stability tests share, the fuzzes, races, and soaks a Catch2 tag [stability] labels for
// .github/workflows/sanitizers.yml: a file's bytes, random numbers from a seed, a file changed at
// random, what the process has allocated, and whether a sanitizer's built in.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

namespace fbe::tests {

/// The bytes of the file at `path`, none if it can't be read.
std::vector<std::uint8_t> read(const std::filesystem::path& path);

/// xorshift64, from a seed, so a run's the same each time.
class random_t {
public:
    explicit random_t(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return m_state;
    }

    /// From 0 to `count`, not counting it.
    std::size_t below(std::size_t count) {
        return count == 0 ? 0 : static_cast<std::size_t>(next() % count);
    }

    /// From 0 to 1.
    float unit() {
        return static_cast<float>(next() % 10001) / 10000.0F;
    }

private:
    std::uint64_t m_state;
};

/// A file fuzzing changes, and where its headers are: each Ogg page's (from its "OggS") and MPEG
/// start code's (from its 00 00 01), with how far each can go.
struct seed_t {
    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<std::size_t, std::size_t>> headers;

    explicit seed_t(std::vector<std::uint8_t> file);
};

/// `seed`'s bytes, with one to four changes of `random`'s: a byte set, a byte set to a value a
/// decoder checks against, bytes put in, taken out, repeated after themselves, or zeroed. Half are
/// aimed at a header: a quarter in the first 512 bytes, and a quarter in one of its headers, or
/// the first 512 bytes when it has none.
std::vector<std::uint8_t> changed(const seed_t& seed, random_t& random);

/// The bytes the process's malloc holds now: glibc's mallinfo2, macOS's malloc zones, and the
/// Windows process heap's, which UCRT's malloc allocates from.
std::uint64_t allocated();

/// Whether a sanitizer's allocator is built in, which keeps what's freed a while and counts it
/// its own way, so allocated() says nothing of a leak: LeakSanitizer says that there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitized = true;
#elifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitized = true;
#else
inline constexpr bool sanitized = false;
#endif
#else
inline constexpr bool sanitized = false;
#endif

/// A thread that runs `work` with a flag that says when to stop, stopped and joined when it goes,
/// so a REQUIRE that fails on the thread that made it reports, never terminating with it joinable.
/// std::jthread would be this, but Apple's libc++ has it only as an experimental library.
class stopping_thread_t {
public:
    template <typename Work>
    explicit stopping_thread_t(Work work) : m_thread([this, work] { work(m_stop); }) {}

    stopping_thread_t(const stopping_thread_t&) = delete;
    stopping_thread_t& operator=(const stopping_thread_t&) = delete;
    stopping_thread_t(stopping_thread_t&&) = delete;
    stopping_thread_t& operator=(stopping_thread_t&&) = delete;

    ~stopping_thread_t() {
        stop();
    }

    /// Tells the work to stop, and waits until it has.
    void stop() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    // Made before the thread that reads it.
    std::atomic<bool> m_stop = false;
    std::thread m_thread;
};

} // namespace fbe::tests

#endif
