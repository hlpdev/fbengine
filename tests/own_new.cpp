// The out-of-memory tests' allocation functions, every form of them, so none of the platform's
// frees what one of these allocates, or the other way round: an allocation that isn't aligned is
// malloc's and its every delete free's, and an aligned one is aligned_alloc's (_aligned_malloc's
// on Windows) and its every delete the matching free. The default nothrow and array forms would
// call this file's plain new, as libstdc++'s DLL on Windows calls its own, so they're here too.

#include "own_new.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

/// Counts an allocation of this thread's, throwing for the one it's told to fail.
void count() {
    using namespace fbe::tests;
    if (failing >= 0 && allocated++ == failing) {
        failing = -1;
        failed = true;
        throw std::bad_alloc();
    }
}

// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
void* plain(std::size_t size) {
    count();
    void* made = std::malloc(size != 0 ? size : 1);
    if (made == nullptr) {
        throw std::bad_alloc();
    }
    ++fbe::tests::live;
    return made;
}

void* aligned(std::size_t size, std::align_val_t alignment) {
    count();
    const auto align = static_cast<std::size_t>(alignment);
    // aligned_alloc's size is a whole number of its alignment.
    const std::size_t whole = ((size != 0 ? size : 1) + align - 1) / align * align;
#ifdef _WIN32
    void* made = _aligned_malloc(whole, align);
#else
    void* made = std::aligned_alloc(align, whole);
#endif
    if (made == nullptr) {
        throw std::bad_alloc();
    }
    ++fbe::tests::live;
    return made;
}

void release(void* pointer) noexcept {
    if (pointer != nullptr) {
        ++fbe::tests::freed;
        --fbe::tests::live;
    }
    std::free(pointer);
}

void release_aligned(void* pointer) noexcept {
    if (pointer != nullptr) {
        ++fbe::tests::freed;
        --fbe::tests::live;
    }
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

} // namespace

// Each parameter's name differs from the one the platform's headers give it, a reserved name.
// NOLINTBEGIN(misc-new-delete-overloads, readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    return plain(size);
}

void* operator new[](std::size_t size) {
    return plain(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return plain(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return plain(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return aligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return aligned(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return aligned(size, alignment);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*nothrow*/) noexcept {
    try {
        return aligned(size, alignment);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept {
    release(pointer);
}

void operator delete[](void* pointer) noexcept {
    release(pointer);
}

void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    release(pointer);
}

void operator delete[](void* pointer, std::size_t /*size*/) noexcept {
    release(pointer);
}

void operator delete(void* pointer, const std::nothrow_t& /*nothrow*/) noexcept {
    release(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t& /*nothrow*/) noexcept {
    release(pointer);
}

void operator delete(void* pointer, std::align_val_t /*alignment*/) noexcept {
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t /*alignment*/) noexcept {
    release_aligned(pointer);
}

void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::size_t /*size*/,
                       std::align_val_t /*alignment*/) noexcept {
    release_aligned(pointer);
}

void operator delete(void* pointer, std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*nothrow*/) noexcept {
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*nothrow*/) noexcept {
    release_aligned(pointer);
}
// NOLINTEND(misc-new-delete-overloads, readability-inconsistent-declaration-parameter-name)
