#include "stability.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace fbe::tests {

namespace {

/// What a decoder checks a value against, more often than a random byte is.
constexpr std::array<std::uint8_t, 8> edges{ 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x40 };

/// An Ogg page's header, 27 bytes, and its segment table, up to 255 more.
constexpr std::size_t ogg_header = 27 + 255;
/// An MPEG start code and the header after it, as long as a pack's or a sequence's.
constexpr std::size_t mpeg_header = 16;

/// Where each header of `bytes` starts, with how far it can go.
std::vector<std::pair<std::size_t, std::size_t>> headers_of(std::span<const std::uint8_t> bytes) {
    std::vector<std::pair<std::size_t, std::size_t>> found;
    constexpr std::string_view ogg = "OggS";
    for (std::size_t at = 0; at + 4 <= bytes.size(); ++at) {
        if (std::equal(ogg.begin(), ogg.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at))) {
            found.emplace_back(at, ogg_header);
        } else if (bytes[at] == 0 && bytes[at + 1] == 0 && bytes[at + 2] == 1) {
            found.emplace_back(at, mpeg_header);
        }
    }
    return found;
}

/// Where a change goes in `bytes`, changed from `seed`'s, aimed at a header half the time.
std::size_t where(std::span<const std::uint8_t> bytes, const seed_t& seed, random_t& random) {
    const std::size_t first = std::min<std::size_t>(bytes.size(), 512);
    switch (random.below(4)) {
    case 0:
        return random.below(first);
    case 1: {
        if (seed.headers.empty()) {
            return random.below(first);
        }
        const auto [start, length] = seed.headers[random.below(seed.headers.size())];
        return std::min(bytes.size() - 1, start + random.below(length));
    }
    default:
        return random.below(bytes.size());
    }
}

} // namespace

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

seed_t::seed_t(std::vector<std::uint8_t> file)
    : bytes(std::move(file)), headers(headers_of(bytes)) {}

std::vector<std::uint8_t> changed(const seed_t& seed, random_t& random) {
    std::vector<std::uint8_t> bytes = seed.bytes;
    const std::size_t changes = 1 + random.below(4);
    for (std::size_t c = 0; c < changes && !bytes.empty(); ++c) {
        const std::size_t at = where(bytes, seed, random);
        const std::size_t length = std::min<std::size_t>(1 + random.below(32), bytes.size() - at);
        const auto from = bytes.begin() + static_cast<std::ptrdiff_t>(at);
        switch (random.below(6)) {
        case 0:
            bytes[at] = static_cast<std::uint8_t>(random.next());
            break;
        case 1:
            bytes[at] = edges.at(random.below(edges.size()));
            break;
        case 2: {
            std::vector<std::uint8_t> put(length);
            for (std::uint8_t& byte : put) {
                byte = static_cast<std::uint8_t>(random.next());
            }
            bytes.insert(from, put.begin(), put.end());
            break;
        }
        case 3:
            bytes.erase(from, from + static_cast<std::ptrdiff_t>(length));
            break;
        case 4: {
            const std::vector<std::uint8_t> again(from, from + static_cast<std::ptrdiff_t>(length));
            bytes.insert(from + static_cast<std::ptrdiff_t>(length), again.begin(), again.end());
            break;
        }
        default:
            std::fill(from, from + static_cast<std::ptrdiff_t>(length), std::uint8_t{ 0 });
            break;
        }
    }
    return bytes;
}

std::uint64_t malloc_bytes() {
#ifdef _WIN32
    HEAP_SUMMARY summary{};
    summary.cb = sizeof(summary);
    return HeapSummary(GetProcessHeap(), 0, &summary) != 0 ? summary.cbAllocated : 0;
#elifdef __APPLE__
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#else
    // What's in use in every arena, and what's mapped for an allocation of its own.
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#endif
}

} // namespace fbe::tests
