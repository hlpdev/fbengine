#pragma once

// Files the assets tests make: images, base64 for glTF's data URIs, binary glTFs, and a folder of
// their own to put them in.

#include "vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace test {

using fbe::vfs::bytes_t;

/// `text`'s bytes, NULs too, as a "..."sv literal keeps them.
inline bytes_t bytes(std::string_view text) {
    return { text.begin(), text.end() };
}

/// A PNG of `channels` 8-bit channels, 1 to 4.
bytes_t png(int width, int height, int channels, const std::vector<std::uint8_t>& texels);
/// A 16-bit RGBA PNG.
bytes_t png16(int width, int height, const std::vector<std::uint16_t>& texels);
/// A JPEG of one color, at its best quality.
bytes_t jpeg(int width, int height, std::array<std::uint8_t, 3> color);

std::string base64(const bytes_t& bytes);

/// A binary glTF of `json` and its buffer.
bytes_t glb(std::string_view json, const bytes_t& buffer);

/// Floats' and integers' bytes, for glTF buffers.
template <typename T>
void append(bytes_t& to, std::initializer_list<T> values) {
    for (const T value : values) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        to.insert(to.end(), bytes, bytes + sizeof(T));
    }
}

/// A folder of its own under the OS's temporary folder, gone with it.
struct temp_folder_t {
    temp_folder_t();
    ~temp_folder_t();
    temp_folder_t(const temp_folder_t&) = delete;
    temp_folder_t& operator=(const temp_folder_t&) = delete;
    temp_folder_t(temp_folder_t&&) = delete;
    temp_folder_t& operator=(temp_folder_t&&) = delete;

    void write(std::string_view name, const bytes_t& bytes) const;
    void write(std::string_view name, std::string_view text) const;

    std::filesystem::path path;
};

} // namespace test
