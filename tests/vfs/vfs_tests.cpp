// res://: paths, folders and paks, mounted over each other, and read from many threads while the
// mounts change. The paks are made here, by a small zip writer that can make what a pak mustn't
// be, too.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace vfs = fbe::vfs;

vfs::path_t path(std::string_view text) {
    vfs::result_t<vfs::path_t> parsed = vfs::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::string text_of(const vfs::bytes_t& bytes) {
    return { bytes.begin(), bytes.end() };
}

// A folder of its own under the OS's temporary folder, gone with it.
struct temp_folder_t {
    temp_folder_t() {
        std::random_device random;
        path = fs::temp_directory_path() / ("fbengine-vfs-" + std::to_string(random()));
        fs::create_directories(path);
    }
    // Only remove_all's allocations can throw, which end the tests.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~temp_folder_t() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    temp_folder_t(const temp_folder_t&) = delete;
    temp_folder_t& operator=(const temp_folder_t&) = delete;
    temp_folder_t(temp_folder_t&&) = delete;
    temp_folder_t& operator=(temp_folder_t&&) = delete;

    void write(std::string_view name, std::string_view text) const {
        const fs::path file = path / vfs::disk_path(name);
        fs::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::binary);
        stream << text;
    }

    fs::path path;
};

struct zip_entry_t {
    std::string name;
    std::string data;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    // Unix's file modes in the top half, when made_by says Unix.
    std::uint32_t attributes = 0;
    std::uint16_t made_by = 20;
};

class zip_t {
public:
    void u16(std::uint64_t value) {
        add(value, 2);
    }
    void u32(std::uint64_t value) {
        add(value, 4);
    }
    void u64(std::uint64_t value) {
        add(value, 8);
    }
    void text(std::string_view text) {
        bytes.insert(bytes.end(), text.begin(), text.end());
    }

    vfs::bytes_t bytes;

private:
    void add(std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        }
    }
};

// A zip of `entries`, with every size and offset in Zip64's records when `zip64` says so.
vfs::bytes_t zip(const std::vector<zip_entry_t>& entries, bool zip64 = false,
                 std::string_view comment = "") {
    zip_t out;
    std::vector<std::uint64_t> offsets;
    for (const zip_entry_t& entry : entries) {
        offsets.push_back(out.bytes.size());
        out.u32(0x0403'4B50);
        out.u16(zip64 ? 45 : 20);
        out.u16(entry.flags);
        out.u16(entry.method);
        out.u32(0);
        out.u32(0);
        out.u32(zip64 ? 0xFFFF'FFFF : entry.data.size());
        out.u32(zip64 ? 0xFFFF'FFFF : entry.data.size());
        out.u16(entry.name.size());
        out.u16(zip64 ? 20 : 0);
        out.text(entry.name);
        if (zip64) {
            out.u16(1);
            out.u16(16);
            out.u64(entry.data.size());
            out.u64(entry.data.size());
        }
        out.text(entry.data);
    }
    const std::uint64_t directory = out.bytes.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const zip_entry_t& entry = entries[i];
        out.u32(0x0201'4B50);
        out.u16(entry.made_by);
        out.u16(zip64 ? 45 : 20);
        out.u16(entry.flags);
        out.u16(entry.method);
        out.u32(0);
        out.u32(0);
        out.u32(zip64 ? 0xFFFF'FFFF : entry.data.size());
        out.u32(zip64 ? 0xFFFF'FFFF : entry.data.size());
        out.u16(entry.name.size());
        out.u16(zip64 ? 28 : 0);
        out.u16(0);
        out.u16(0);
        out.u16(0);
        out.u32(entry.attributes);
        out.u32(zip64 ? 0xFFFF'FFFF : offsets[i]);
        out.text(entry.name);
        if (zip64) {
            out.u16(1);
            out.u16(24);
            out.u64(entry.data.size());
            out.u64(entry.data.size());
            out.u64(offsets[i]);
        }
    }
    const std::uint64_t directory_size = out.bytes.size() - directory;
    if (zip64) {
        const std::uint64_t record = out.bytes.size();
        out.u32(0x0606'4B50);
        out.u64(44);
        out.u16(45);
        out.u16(45);
        out.u32(0);
        out.u32(0);
        out.u64(entries.size());
        out.u64(entries.size());
        out.u64(directory_size);
        out.u64(directory);
        out.u32(0x0706'4B50);
        out.u32(0);
        out.u64(record);
        out.u32(1);
    }
    out.u32(0x0605'4B50);
    out.u16(0);
    out.u16(0);
    out.u16(zip64 ? 0xFFFF : entries.size());
    out.u16(zip64 ? 0xFFFF : entries.size());
    out.u32(zip64 ? 0xFFFF'FFFF : directory_size);
    out.u32(zip64 ? 0xFFFF'FFFF : directory);
    out.u16(comment.size());
    out.text(comment);
    return out.bytes;
}

std::shared_ptr<vfs::source_t> pak_of(const std::vector<zip_entry_t>& entries, bool zip64 = false) {
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(zip(entries, zip64));
    REQUIRE(source.has_value());
    return *source;
}

vfs::failure_t pak_failure(const vfs::bytes_t& bytes) {
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(bytes);
    REQUIRE_FALSE(source.has_value());
    return source.error().failure;
}

std::string read(const vfs::vfs_t& files, std::string_view at) {
    vfs::result_t<vfs::bytes_t> bytes = files.read(path(at));
    REQUIRE(bytes.has_value());
    return text_of(*bytes);
}

vfs::failure_t read_failure(const vfs::vfs_t& files, std::string_view at) {
    vfs::result_t<vfs::bytes_t> bytes = files.read(path(at));
    REQUIRE_FALSE(bytes.has_value());
    return bytes.error().failure;
}

vfs::kind_t kind(const vfs::vfs_t& files, std::string_view at) {
    vfs::result_t<vfs::kind_t> kind = files.kind(path(at));
    REQUIRE(kind.has_value());
    return *kind;
}

// The listing as "name" for files and "name/" for folders.
std::vector<std::string> list(const vfs::vfs_t& files, std::string_view at) {
    vfs::result_t<std::vector<vfs::entry_t>> entries = files.list(path(at));
    REQUIRE(entries.has_value());
    std::vector<std::string> names;
    for (const vfs::entry_t& entry : *entries) {
        names.push_back(entry.kind == vfs::kind_t::FOLDER ? entry.name + "/" : entry.name);
    }
    return names;
}

} // namespace

TEST_CASE("a res:// path takes either slash and any case", "[vfs]") {
    const vfs::path_t parsed = path(R"(RES://GFX\map\\Door01.png)");
    REQUIRE(parsed.text() == "res://GFX/map/Door01.png");
    REQUIRE(parsed.key() == "res://gfx/map/door01.png");
    REQUIRE(path("res://a/./b/../c/").key() == "res://a/c");
    REQUIRE(path("res://").key() == "res://");
    REQUIRE(path("res://a/..").names.empty());
    // Only ASCII folds.
    REQUIRE(path("res://ÄB").key() == "res://Äb");
}

TEST_CASE("what isn't a res:// path is an invalid path", "[vfs]") {
    for (const std::string_view text : {
             "textures/a.png",
             "C:/game/a.png",
             "res:/a",
             "",
             "res://..",
             "res://a/../../b",
             "res://a:b",
             "res://a*",
             "res://what?",
             "res://a\"b",
             "res://<a>",
             "res://a|b",
             "res://tab\there",
             "res://a\x7F",
         }) {
        const vfs::result_t<vfs::path_t> parsed = vfs::parse(text);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error().failure == vfs::failure_t::INVALID_PATH);
    }
}

TEST_CASE("a path resolves from the folder of the file it's in", "[vfs]") {
    const vfs::path_t model = path("res://Models/door.gltf");
    REQUIRE(vfs::resolve(model, "door.bin")->text() == "res://Models/door.bin");
    REQUIRE(vfs::resolve(model, "../textures\\door.png")->text() == "res://textures/door.png");
    REQUIRE(vfs::resolve(model, "res://other/a.png")->text() == "res://other/a.png");
    REQUIRE(vfs::resolve(path("res://top.gltf"), "a.bin")->text() == "res://a.bin");
    REQUIRE(vfs::resolve(model, "../../a.bin").error().failure == vfs::failure_t::INVALID_PATH);
}

TEST_CASE("a folder on the disk reads as it's asked, in any case", "[vfs]") {
    const temp_folder_t folder;
    folder.write("GFX/Map/door01.png", "door");
    folder.write("readme.txt", "hello");
    folder.write("ünïcödé/naïve.txt", "unicode");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));

    REQUIRE(read(files, "res://GFX/Map/door01.png") == "door");
    REQUIRE(read(files, R"(res://gfx\map\DOOR01.PNG)") == "door");
    REQUIRE(read(files, "res://ünïcödé/naïve.txt") == "unicode");
    REQUIRE(kind(files, "res://gfx") == vfs::kind_t::FOLDER);
    REQUIRE(kind(files, "res://Readme.TXT") == vfs::kind_t::FILE);
    REQUIRE(kind(files, "res://missing") == vfs::kind_t::NONE);
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "GFX/", "readme.txt", "ünïcödé/" });
    REQUIRE(list(files, "res://gfx/MAP") == std::vector<std::string>{ "door01.png" });
    REQUIRE(read_failure(files, "res://gfx") == vfs::failure_t::IS_A_FOLDER);
    REQUIRE(read_failure(files, "res://gfx/nothing.png") == vfs::failure_t::NOT_FOUND);
    REQUIRE(files.list(path("res://readme.txt")).error().failure == vfs::failure_t::NOT_A_FOLDER);

    // It sees what changes.
    folder.write("GFX/Map/door01.png", "new door");
    folder.write("late.txt", "late");
    REQUIRE(read(files, "res://gfx/map/door01.png") == "new door");
    REQUIRE(read(files, "res://late.txt") == "late");
}

#ifndef _WIN32
TEST_CASE("a folder's names that differ only in case give the first by its bytes", "[vfs]") {
    const temp_folder_t folder;
    folder.write("b.txt", "lower");
    folder.write("B.txt", "upper");
    folder.write("odd:name.txt", "no path names it");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // As written first, then the first by bytes.
    REQUIRE(read(files, "res://b.txt") == "lower");
    REQUIRE(read(files, "res://B.txt") == "upper");
    REQUIRE(read(files, "res://B.TXT") == "upper");
    // A name no res:// path can have isn't listed, and of two that fold the same, the first by
    // its bytes is.
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "B.txt" });
}
#endif

TEST_CASE("a folder that isn't there is an empty mount", "[vfs]") {
    const temp_folder_t folder;
    vfs::vfs_t files;
    files.mount(path("res://data"), vfs::folder(folder.path / "not yet"));
    REQUIRE(kind(files, "res://data") == vfs::kind_t::FOLDER);
    REQUIRE(list(files, "res://data").empty());
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "data/" });
    REQUIRE(read_failure(files, "res://data/a.txt") == vfs::failure_t::NOT_FOUND);
    folder.write("not yet/a.txt", "now");
    REQUIRE(read(files, "res://data/a.txt") == "now");
}

TEST_CASE("later mounts come first, and folders list every mount's", "[vfs]") {
    const temp_folder_t base;
    const temp_folder_t mod;
    base.write("textures/wall.png", "base wall");
    base.write("textures/floor.png", "base floor");
    base.write("sounds/step.ogg", "step");
    mod.write("textures/Wall.png", "mod wall");
    mod.write("textures/new.png", "mod new");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(base.path));
    files.mount(path("res://"), vfs::folder(mod.path));
    files.mount(path("res://music/menu"), pak_of({ { .name = "theme.ogg", .data = "theme" } }));

    REQUIRE(read(files, "res://textures/wall.png") == "mod wall");
    REQUIRE(read(files, "res://textures/floor.png") == "base floor");
    // The last mount's name for one two have.
    REQUIRE(list(files, "res://textures") ==
            std::vector<std::string>{ "floor.png", "new.png", "Wall.png" });
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "music/", "sounds/", "textures/" });
    REQUIRE(kind(files, "res://music") == vfs::kind_t::FOLDER);
    REQUIRE(list(files, "res://music") == std::vector<std::string>{ "menu/" });
    REQUIRE(read(files, "res://Music/Menu/Theme.ogg") == "theme");
    REQUIRE(read_failure(files, "res://music/menu") == vfs::failure_t::IS_A_FOLDER);

    // Unmounting takes out the last at that place.
    REQUIRE(files.unmount(path("res://")));
    REQUIRE(read(files, "res://textures/wall.png") == "base wall");
    REQUIRE(read_failure(files, "res://textures/new.png") == vfs::failure_t::NOT_FOUND);
    REQUIRE(files.unmount(path("RES://Music/Menu")));
    REQUIRE_FALSE(files.unmount(path("res://music/menu")));
    REQUIRE(kind(files, "res://music") == vfs::kind_t::NONE);
    REQUIRE(files.list(path("res://music")).error().failure == vfs::failure_t::NOT_FOUND);
}

TEST_CASE("a later mount's file stands in for an earlier one's folder", "[vfs]") {
    vfs::vfs_t files;
    files.mount(path("res://"), pak_of({ { .name = "thing/inside.txt", .data = "inside" } }));
    files.mount(path("res://"), pak_of({ { .name = "thing", .data = "a file" } }));
    REQUIRE(kind(files, "res://thing") == vfs::kind_t::FILE);
    REQUIRE(read(files, "res://thing") == "a file");
    REQUIRE(files.list(path("res://thing")).error().failure == vfs::failure_t::NOT_A_FOLDER);
    // What's under the folder is still the earlier mount's.
    REQUIRE(read(files, "res://thing/inside.txt") == "inside");
}

TEST_CASE("a pak reads its stored files through its index", "[vfs]") {
    for (const bool zip64 : { false, true }) {
        const std::shared_ptr<vfs::source_t> pak = pak_of(
            {
                { .name = "GFX/", .data = "" },
                { .name = "GFX/Map/door01.png", .data = "door" },
                { .name = "readme.txt", .data = "hello" },
                { .name = R"(sounds\step.ogg)", .data = "step" },
                { .name = "empty.txt", .data = "" },
            },
            zip64);
        vfs::vfs_t files;
        files.mount(path("res://"), pak);
        REQUIRE(read(files, "res://gfx/map/DOOR01.png") == "door");
        REQUIRE(read(files, "res://readme.txt") == "hello");
        REQUIRE(read(files, "res://sounds/step.ogg") == "step");
        REQUIRE(read(files, "res://empty.txt").empty());
        REQUIRE(kind(files, "res://gfx/map") == vfs::kind_t::FOLDER);
        REQUIRE(list(files, "res://") ==
                std::vector<std::string>{ "empty.txt", "GFX/", "readme.txt", "sounds/" });
        REQUIRE(list(files, "res://GFX") == std::vector<std::string>{ "Map/" });
        REQUIRE(read_failure(files, "res://gfx") == vfs::failure_t::IS_A_FOLDER);
        REQUIRE(read_failure(files, "res://nothing") == vfs::failure_t::NOT_FOUND);
    }
}

TEST_CASE("a pak on the disk, and one with a comment, read as one in memory", "[vfs]") {
    const temp_folder_t folder;
    const vfs::bytes_t bytes =
        zip({ { .name = "a.txt", .data = "on the disk" } }, false, "a comment, PK\x05\x06 too");
    {
        std::ofstream stream(folder.path / "game.pak", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    vfs::result_t<std::shared_ptr<vfs::source_t>> pak = vfs::pak(folder.path / "game.pak");
    REQUIRE(pak.has_value());
    vfs::vfs_t files;
    files.mount(path("res://"), *pak);
    REQUIRE(read(files, "res://a.txt") == "on the disk");

    vfs::result_t<std::shared_ptr<vfs::source_t>> missing = vfs::pak(folder.path / "none.pak");
    REQUIRE(missing.error().failure == vfs::failure_t::NOT_FOUND);
}

TEST_CASE("a pak leaves out what no path reaches, and keeps a name's last entry", "[vfs]") {
    const std::shared_ptr<vfs::source_t> pak = pak_of({
        { .name = "../outside.txt", .data = "outside" },
        { .name = "a/../../up.txt", .data = "up" },
        { .name = "odd:name.txt", .data = "odd" },
        {
            .name = "link",
            .data = "target.txt",
            .attributes = 0xA1FFU << 16U,
            .made_by = (3U << 8U) | 20U,
        },
        { .name = "same.txt", .data = "first" },
        { .name = "SAME.txt", .data = "last" },
        { .name = "/", .data = "" },
    });
    vfs::vfs_t files;
    files.mount(path("res://"), pak);
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "SAME.txt" });
    REQUIRE(read(files, "res://same.txt") == "last");
    REQUIRE(read_failure(files, "res://link") == vfs::failure_t::NOT_FOUND);
}

TEST_CASE("a zip that isn't a pak is invalid data", "[vfs]") {
    SECTION("compressed") {
        REQUIRE(pak_failure(zip({ { .name = "a.txt", .data = "x", .method = 8 } })) ==
                vfs::failure_t::INVALID_DATA);
    }
    SECTION("encrypted") {
        REQUIRE(pak_failure(zip({ { .name = "a.txt", .data = "x", .flags = 1 } })) ==
                vfs::failure_t::INVALID_DATA);
    }
    SECTION("a place that's a file and a folder too") {
        REQUIRE(pak_failure(
                    zip({ { .name = "a", .data = "x" }, { .name = "a/b.txt", .data = "y" } })) ==
                vfs::failure_t::INVALID_DATA);
        REQUIRE(pak_failure(zip({ { .name = "a/b.txt", .data = "y" },
                                  { .name = "A", .data = "x" } })) == vfs::failure_t::INVALID_DATA);
        REQUIRE(pak_failure(zip({ { .name = "a/", .data = "" }, { .name = "a", .data = "x" } })) ==
                vfs::failure_t::INVALID_DATA);
    }
    SECTION("not a zip, or cut short") {
        REQUIRE(pak_failure({}) == vfs::failure_t::INVALID_DATA);
        REQUIRE(pak_failure(vfs::bytes_t(100, 7)) == vfs::failure_t::INVALID_DATA);
        vfs::bytes_t whole = zip({ { .name = "a.txt", .data = "some text" } });
        for (const std::size_t cut : { std::size_t{ 1 }, std::size_t{ 30 }, whole.size() - 1 }) {
            const vfs::bytes_t start(whole.begin(), whole.end() - static_cast<std::ptrdiff_t>(cut));
            REQUIRE(pak_failure(start) == vfs::failure_t::INVALID_DATA);
        }
    }
    SECTION("a file past its end") {
        vfs::bytes_t bytes = zip({ { .name = "a.txt", .data = "some text" } });
        // The central directory's offset of the local header, made to point past the end.
        const std::size_t directory = 30 + 5 + 9;
        bytes[directory + 42] = 0xFF;
        bytes[directory + 43] = 0x7F;
        vfs::result_t<std::shared_ptr<vfs::source_t>> pak = vfs::pak(bytes);
        REQUIRE(pak.has_value());
        vfs::vfs_t files;
        files.mount(path("res://"), *pak);
        REQUIRE(read_failure(files, "res://a.txt") == vfs::failure_t::INVALID_DATA);
    }
}

TEST_CASE("reads on many threads see whole mounts while the mounts change", "[vfs][stress]") {
    vfs::vfs_t files;
    const std::shared_ptr<vfs::source_t> first = pak_of({ { .name = "a.txt", .data = "first" } });
    const std::shared_ptr<vfs::source_t> second =
        pak_of({ { .name = "a.txt", .data = "second" }, { .name = "b.txt", .data = "only" } });
    const temp_folder_t folder;
    folder.write("a.txt", "folder");
    files.mount(path("res://"), first);

    std::atomic<bool> stop = false;
    std::atomic<int> wrong = 0;
    std::atomic<int> reads = 0;
    std::vector<std::thread> readers;
    readers.reserve(8);
    // Catch2's checks are this thread's, so the readers count what they find for it.
    const vfs::path_t a = path("res://A.txt");
    const vfs::path_t b = path("res://b.txt");
    const vfs::path_t root = path("res://");
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            while (!stop) {
                const vfs::result_t<vfs::bytes_t> bytes = files.read(a);
                const std::string text = bytes ? text_of(*bytes) : "";
                if (text != "first" && text != "second" && text != "folder") {
                    ++wrong;
                }
                const vfs::result_t<vfs::bytes_t> other = files.read(b);
                if (other && text_of(*other) != "only") {
                    ++wrong;
                }
                if (!files.list(root)) {
                    ++wrong;
                }
                ++reads;
            }
        });
    }
    for (int i = 0; i < 2'000; ++i) {
        files.mount(path("res://"), i % 2 == 0 ? second : vfs::folder(folder.path));
        files.unmount(path("res://"));
    }
    while (reads < 1'000) {
        std::this_thread::yield();
    }
    stop = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE(wrong == 0);
    REQUIRE(read(files, "res://a.txt") == "first");
}
