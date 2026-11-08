// tests/own_new.cpp, the out-of-memory tests' operator new and delete: each form allocates through
// it, is counted, fails when it's told to, and is freed by its own matching delete, which counts
// the free; a sanitizer's build checks that no allocation's freed by another family's.

#include "own_new.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace {

constexpr std::size_t size = 40;
constexpr auto alignment = std::align_val_t{ 64 };
constexpr std::size_t plain = alignof(std::max_align_t);
constexpr std::size_t aligned = 64;

/// A form of operator new, whether it gives null rather than throwing, the delete that frees what
/// it gives, and the alignment of what it gives.
struct form_t {
    const char* name;
    void* (*allocate)();
    bool nothrow;
    void (*free)(void*);
    std::size_t align;
};

// NOLINTBEGIN(cppcoreguidelines-owning-memory)
constexpr std::array<form_t, 12> forms{
    {
        {
            .name = "new, delete",
            .allocate = [] { return ::operator new(size); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete(p); },
            .align = plain,
        },
        {
            .name = "new, sized delete",
            .allocate = [] { return ::operator new(size); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete(p, size); },
            .align = plain,
        },
        {
            .name = "new[], delete[]",
            .allocate = [] { return ::operator new[](size); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete[](p); },
            .align = plain,
        },
        {
            .name = "new[], sized delete[]",
            .allocate = [] { return ::operator new[](size); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete[](p, size); },
            .align = plain,
        },
        {
            .name = "nothrow new, nothrow delete",
            .allocate = [] { return ::operator new(size, std::nothrow); },
            .nothrow = true,
            .free = [](void* p) { ::operator delete(p, std::nothrow); },
            .align = plain,
        },
        {
            .name = "nothrow new[], nothrow delete[]",
            .allocate = [] { return ::operator new[](size, std::nothrow); },
            .nothrow = true,
            .free = [](void* p) { ::operator delete[](p, std::nothrow); },
            .align = plain,
        },
        {
            .name = "aligned new, aligned delete",
            .allocate = [] { return ::operator new(size, alignment); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete(p, alignment); },
            .align = aligned,
        },
        {
            .name = "aligned new, sized aligned delete",
            .allocate = [] { return ::operator new(size, alignment); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete(p, size, alignment); },
            .align = aligned,
        },
        {
            .name = "aligned new[], aligned delete[]",
            .allocate = [] { return ::operator new[](size, alignment); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete[](p, alignment); },
            .align = aligned,
        },
        {
            .name = "aligned new[], sized aligned delete[]",
            .allocate = [] { return ::operator new[](size, alignment); },
            .nothrow = false,
            .free = [](void* p) { ::operator delete[](p, size, alignment); },
            .align = aligned,
        },
        {
            .name = "aligned nothrow new, aligned nothrow delete",
            .allocate = [] { return ::operator new(size, alignment, std::nothrow); },
            .nothrow = true,
            .free = [](void* p) { ::operator delete(p, alignment, std::nothrow); },
            .align = aligned,
        },
        {
            .name = "aligned nothrow new[], aligned nothrow delete[]",
            .allocate = [] { return ::operator new[](size, alignment, std::nothrow); },
            .nothrow = true,
            .free = [](void* p) { ::operator delete[](p, alignment, std::nothrow); },
            .align = aligned,
        },
    },
};
// NOLINTEND(cppcoreguidelines-owning-memory)

/// Whether `form`'s allocation failed as its form does: null from a nothrow one, bad_alloc from
/// the rest.
bool failed_as_it_does(const form_t& form) {
    try {
        const void* made = form.allocate();
        return form.nothrow && made == nullptr;
    } catch (const std::bad_alloc&) {
        return !form.nothrow;
    }
}

} // namespace

TEST_CASE("every form of operator new and delete is the out-of-memory tests' own, as a set") {
    using namespace fbe::tests;
    for (const form_t& form : forms) {
        INFO(form.name);
        // Told to fail the next allocation, it does.
        failing = allocated;
        failed = false;
        CHECK(failed_as_it_does(form));
        CHECK(failed);
        // Counted, aligned, and freed by its own delete.
        failing = 1'000'000;
        allocated = 0;
        void* const made = form.allocate();
        failing = -1;
        REQUIRE(made != nullptr);
        CHECK(allocated == 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an address's alignment.
        CHECK(reinterpret_cast<std::uintptr_t>(made) % form.align == 0);
        const long before = freed;
        form.free(made);
        CHECK(freed == before + 1);
    }
    // And through new and delete expressions, which pick the sized and aligned forms themselves.
    struct alignas(64) wide_t {
        std::array<std::byte, 64> bytes;
    };
    const long before = freed;
    delete new wide_t{};
    delete[] new wide_t[3]{};
    delete new int{ 4 };
    CHECK(freed == before + 3);
}

// What a soak checks after each round: a new left each round is still live after it, and what's
// freed isn't.
TEST_CASE("a new left each round is counted live after it, and what's freed isn't") {
    using fbe::tests::live;
    std::vector<int*> left;
    left.reserve(5);
    const long before = live;
    for (int round = 0; round < 5; ++round) {
        const std::vector<int> freed_by_its_end(100, round);
        left.push_back(
            new int{ round }); // NOLINT(cppcoreguidelines-owning-memory): left on purpose.
    }
    CHECK(live == before + 5);
    for (const int* leaked : left) {
        delete leaked; // NOLINT(cppcoreguidelines-owning-memory)
    }
    CHECK(live == before);
}
