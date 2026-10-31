// The cooker's paks, written a file at a time: what the engine's res:// reads back, with Zip64's
// fields where a count needs them or the test asks for them everywhere, and nothing put in place
// of what was there by one that isn't finished.

#include "../assets/files.hpp"
#include "cook/pak.hpp"
#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace tools = fbe::tools;
namespace vfs = fbe::vfs;
using test::bytes_t;

vfs::path_t path(std::string_view text) {
    return *vfs::parse(text);
}

std::string utf8(const std::filesystem::path& file) {
    return vfs::utf8(file);
}

// The writer of a pak at `to`, which the test fails without.
std::unique_ptr<tools::pak_writer_t> opened(const std::filesystem::path& to, bool zip64 = false) {
    std::expected<std::unique_ptr<tools::pak_writer_t>, tools::error_t> writer =
        tools::pak_writer_t::open(utf8(to), zip64);
    if (!writer) {
        FAIL(writer.error().message);
    }
    return std::move(*writer);
}

void added(tools::pak_writer_t& writer, std::string_view name, const std::filesystem::path& from) {
    if (std::expected<void, tools::error_t> done = writer.add(name, utf8(from)); !done) {
        FAIL(done.error().message);
    }
}

void finished(tools::pak_writer_t& writer) {
    if (std::expected<void, tools::error_t> done = writer.finish(); !done) {
        FAIL(done.error().message);
    }
}

// The pak at `file`, which the test fails without.
std::shared_ptr<vfs::source_t> pak(const std::filesystem::path& file) {
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(file);
    if (!source) {
        FAIL(source.error().message);
    }
    return *source;
}

bytes_t read(const vfs::vfs_t& files, std::string_view at) {
    vfs::result_t<bytes_t> bytes = files.read(path(at));
    if (!bytes) {
        FAIL(bytes.error().message);
    }
    return *bytes;
}

std::string text_of(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), {} };
}

} // namespace

TEST_CASE("a pak written a file at a time is what res:// reads", "[tools]") {
    // Bigger than the writer's chunk, so it's written in parts.
    bytes_t big(((std::size_t{ 1 } << 20U) * 3) + 5);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>((i * 7) + (i >> 12U));
    }
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha");
    folder.write("big.bin", big);
    folder.write("empty.txt", "");
    for (const bool zip64 : { false, true }) {
        const std::filesystem::path to = folder.path / "out" / "game.pak";
        const std::unique_ptr<tools::pak_writer_t> writer = opened(to, zip64);
        added(*writer, "a.txt", folder.path / "a.txt");
        added(*writer, "Sub/Big.bin", folder.path / "big.bin");
        added(*writer, "sub/empty.txt", folder.path / "empty.txt");
        finished(*writer);
        // Its folder made, and nothing beside it.
        REQUIRE(std::distance(std::filesystem::directory_iterator(to.parent_path()),
                              std::filesystem::directory_iterator()) == 1);
        vfs::vfs_t files;
        files.mount(path("res://"), pak(to));
        REQUIRE(read(files, "res://a.txt") == test::bytes("alpha"));
        REQUIRE(read(files, "res://sub/big.bin") == big);
        REQUIRE(read(files, "res://sub/empty.txt").empty());
        vfs::result_t<std::vector<vfs::entry_t>> listed = files.list(path("res://sub"));
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 2);
    }
}

TEST_CASE("a pak of more files than a zip's count holds has Zip64's end records", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("one.txt", "one");
    const std::filesystem::path to = folder.path / "many.pak";
    const std::unique_ptr<tools::pak_writer_t> writer = opened(to);
    constexpr std::size_t count = 0x10000 + 1;
    for (std::size_t i = 0; i < count; ++i) {
        added(*writer, std::format("files/{}.txt", i), folder.path / "one.txt");
    }
    finished(*writer);
    const std::string bytes = text_of(to);
    REQUIRE(bytes.contains(std::string_view("PK\x06\x06", 4)));
    REQUIRE(bytes.contains(std::string_view("PK\x06\x07", 4)));
    vfs::vfs_t files;
    files.mount(path("res://"), pak(to));
    REQUIRE(read(files, "res://files/0.txt") == test::bytes("one"));
    REQUIRE(read(files, std::format("res://files/{}.txt", count - 1)) == test::bytes("one"));
    vfs::result_t<std::vector<vfs::entry_t>> listed = files.list(path("res://files"));
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == count);
}

TEST_CASE("a pak that isn't finished leaves what was there", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("game.pak", "what was there");
    folder.write("a.txt", "alpha");
    const std::filesystem::path to = folder.path / "game.pak";
    {
        const std::unique_ptr<tools::pak_writer_t> writer = opened(to);
        added(*writer, "a.txt", folder.path / "a.txt");
        // What a pak can't name, and what isn't there, aren't added, and the pak goes on.
        for (const std::string_view name : {
                 std::string_view(""),
                 std::string_view("a\\b.txt"),
                 std::string_view("folder/"),
                 std::string_view("nul\0.txt", 8),
             }) {
            std::expected<void, tools::error_t> refused =
                writer->add(name, utf8(folder.path / "a.txt"));
            REQUIRE_FALSE(refused.has_value());
            REQUIRE(refused.error().failure == vfs::failure_t::INVALID_DATA);
        }
        std::expected<void, tools::error_t> missing =
            writer->add("b.txt", utf8(folder.path / "b.txt"));
        REQUIRE_FALSE(missing.has_value());
        REQUIRE(missing.error().failure == vfs::failure_t::NOT_FOUND);
        REQUIRE(text_of(to) == "what was there");
    }
    // Let go of without finishing, what it wrote is gone, and what was there is.
    REQUIRE(text_of(to) == "what was there");
    REQUIRE(std::distance(std::filesystem::directory_iterator(folder.path),
                          std::filesystem::directory_iterator()) == 2);
    // Finished, it's put in its place.
    const std::unique_ptr<tools::pak_writer_t> writer = opened(to);
    added(*writer, "a.txt", folder.path / "a.txt");
    finished(*writer);
    REQUIRE_FALSE(writer->finish().has_value());
    vfs::vfs_t files;
    files.mount(path("res://"), pak(to));
    REQUIRE(read(files, "res://a.txt") == test::bytes("alpha"));
}
