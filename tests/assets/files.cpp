// The assets tests' files. A 16-bit PNG is made by hand, from stb_image_write's zlib, since
// stb_image_write writes only 8-bit ones.

#include "files.hpp"

#include "vfs/disk.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stb_image_write.h>
#include <string>

// stb_image_write's zlib, which stb_write.c has, and its header doesn't say.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len,
                                             int quality);

namespace test {

namespace {

// NOLINTNEXTLINE(misc-const-correctness): stb_image_write's callbacks take a void*.
void collect(void* context, void* data, int size) {
    // stb_image_write's callback takes a void*, which it only reads.
    auto* out = static_cast<bytes_t*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

void big_endian(bytes_t& to, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        to.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB8'8320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void chunk(bytes_t& png, std::string_view type, const bytes_t& data) {
    big_endian(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), data.begin(), data.end());
    big_endian(png, crc32(&png[start], png.size() - start));
}

} // namespace

bytes_t png(int width, int height, int channels, const std::vector<std::uint8_t>& texels) {
    bytes_t out;
    stbi_write_png_to_func(collect, &out, width, height, channels, texels.data(), width * channels);
    return out;
}

bytes_t png16(int width, int height, const std::vector<std::uint16_t>& texels) {
    bytes_t rows;
    for (int y = 0; y < height; ++y) {
        rows.push_back(0);
        for (int i = 0; i < width * 4; ++i) {
            const std::uint16_t value =
                texels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4) +
                       static_cast<std::size_t>(i)];
            rows.push_back(static_cast<std::uint8_t>(value >> 8U));
            rows.push_back(static_cast<std::uint8_t>(value));
        }
    }
    int size = 0;
    unsigned char* compressed =
        stbi_zlib_compress(rows.data(), static_cast<int>(rows.size()), &size, 8);
    const bytes_t zlib(compressed, compressed + size);
    std::free(compressed); // NOLINT(cppcoreguidelines-no-malloc): stb's memory is malloc's.

    bytes_t out = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    bytes_t header;
    big_endian(header, static_cast<std::uint32_t>(width));
    big_endian(header, static_cast<std::uint32_t>(height));
    // 16 bits, RGBA, deflate, adaptive filtering, not interlaced.
    header.insert(header.end(), { 16, 6, 0, 0, 0 });
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", zlib);
    chunk(out, "IEND", {});
    return out;
}

bytes_t jpeg(int width, int height, std::array<std::uint8_t, 3> color) {
    std::vector<std::uint8_t> texels;
    for (int i = 0; i < width * height; ++i) {
        texels.insert(texels.end(), color.begin(), color.end());
    }
    bytes_t out;
    stbi_write_jpg_to_func(collect, &out, width, height, 3, texels.data(), 100);
    return out;
}

std::string base64(const bytes_t& bytes) {
    constexpr std::string_view digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t group =
            (std::uint32_t{ bytes[i] } << 16U) |
            (i + 1 < bytes.size() ? std::uint32_t{ bytes[i + 1] } << 8U : 0U) |
            (i + 2 < bytes.size() ? std::uint32_t{ bytes[i + 2] } : 0U);
        text += digits[(group >> 18U) & 63U];
        text += digits[(group >> 12U) & 63U];
        text += i + 1 < bytes.size() ? digits[(group >> 6U) & 63U] : '=';
        text += i + 2 < bytes.size() ? digits[group & 63U] : '=';
    }
    return text;
}

bytes_t glb(std::string_view json, const bytes_t& buffer) {
    std::string padded(json);
    padded.append((4 - (padded.size() % 4)) % 4, ' ');
    bytes_t bin = buffer;
    bin.resize(bin.size() + ((4 - (bin.size() % 4)) % 4), 0);
    bytes_t out;
    const auto little = [&out](std::uint32_t value) { append<std::uint32_t>(out, { value }); };
    out.insert(out.end(), { 'g', 'l', 'T', 'F' });
    little(2);
    little(static_cast<std::uint32_t>(12 + 8 + padded.size() + (bin.empty() ? 0 : 8 + bin.size())));
    little(static_cast<std::uint32_t>(padded.size()));
    little(0x4E4F'534AU);
    out.insert(out.end(), padded.begin(), padded.end());
    if (!bin.empty()) {
        little(static_cast<std::uint32_t>(bin.size()));
        little(0x004E'4942U);
        out.insert(out.end(), bin.begin(), bin.end());
    }
    return out;
}

temp_folder_t::temp_folder_t() {
    std::random_device random;
    path = std::filesystem::temp_directory_path() / ("fbengine-assets-" + std::to_string(random()));
    std::filesystem::create_directories(path);
}

// Only remove_all's allocations can throw, which end the tests.
// NOLINTNEXTLINE(bugprone-exception-escape)
temp_folder_t::~temp_folder_t() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

void temp_folder_t::write(std::string_view name, const bytes_t& bytes) const {
    const std::filesystem::path file = path / fbe::vfs::disk_path(name);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void temp_folder_t::write(std::string_view name, std::string_view text) const {
    write(name, bytes_t(text.begin(), text.end()));
}

} // namespace test
