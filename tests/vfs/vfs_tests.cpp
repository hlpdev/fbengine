// res://: paths, folders and paks, mounted over each other, and read from many threads while the
// mounts change. The paks are made here, by a small zip writer that can make what a pak mustn't
// be, too.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
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

namespace {

// Whether the disk keeps two names that differ only in case apart, which macOS's and Windows' don't
// by default.
bool case_sensitive(const temp_folder_t& folder) {
    folder.write("case.txt", "lower");
    folder.write("CASE.txt", "upper");
    std::size_t count = 0;
    for ([[maybe_unused]] const fs::directory_entry& entry : fs::directory_iterator(folder.path)) {
        ++count;
    }
    std::error_code error;
    fs::remove(folder.path / "case.txt", error);
    fs::remove(folder.path / "CASE.txt", error);
    return count == 2;
}

} // namespace

TEST_CASE("a folder's names that differ only in case give the first by its bytes", "[vfs]") {
    const temp_folder_t folder;
    if (!case_sensitive(folder)) {
        SKIP("this disk doesn't keep names that differ only in case apart");
    }
    folder.write("b.txt", "lower");
    folder.write("B.txt", "upper");
    folder.write("odd:name.txt", "no path names it");
    folder.write("Data/x.txt", "x");
    folder.write("data/y.txt", "y");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // However it's written, as a pak of these files would have it.
    REQUIRE(read(files, "res://b.txt") == "upper");
    REQUIRE(read(files, "res://B.txt") == "upper");
    REQUIRE(read(files, "res://B.TXT") == "upper");
    // Folders whose names fold the same are one.
    for (const std::string_view at :
         { "res://data/y.txt", "res://DATA/y.txt", "res://Data/y.txt" }) {
        REQUIRE(read(files, at) == "y");
    }
    REQUIRE(read(files, "res://data/X.txt") == "x");
    REQUIRE(list(files, "res://DATA") == std::vector<std::string>{ "x.txt", "y.txt" });
    // A name no res:// path can have isn't listed, and of two that fold the same, the first by
    // its bytes is.
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "B.txt", "Data/" });
}

TEST_CASE("a folder finds no name a pak wouldn't", "[vfs]") {
    const temp_folder_t folder;
    folder.write("readme.txt", "readme");
    folder.write("textures_highres/wall.png", "wall");
    folder.write("été.txt", "ete");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // Windows' short 8.3 name for a folder, and case past ASCII.
    REQUIRE(read_failure(files, "res://TEXTUR~1/wall.png") == vfs::failure_t::NOT_FOUND);
    REQUIRE(read_failure(files, "res://ÉTÉ.txt") == vfs::failure_t::NOT_FOUND);
    REQUIRE(read(files, "res://été.TXT") == "ete");
    // A dot or a space at a name's end, which Windows takes off, and Windows' devices, aren't
    // names a path can have.
    for (const std::string_view text : {
             "res://readme.txt.",
             "res://readme.txt ",
             "res://readme.txt. .",
             "res://nul.txt",
             "res://missing/nul",
             "res://textures_highres/NUL",
             "res://aux",
             "res://com1",
             "res://PRN",
             "res://lpt1.png",
             "res://con",
             "res://Nul .tar.gz",
             "res://COM0",
             // COM and LPT with a superscript one, two, or three, and the console's own.
             "res://COM\xC2\xB9",
             "res://LPT\xC2\xB2.txt",
             "res://com\xC2\xB3",
             "res://CONIN$",
             "res://conout$.log",
         }) {
        const vfs::result_t<vfs::path_t> parsed = vfs::parse(text);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error().failure == vfs::failure_t::INVALID_PATH);
    }
    // Names that only start like a device's are names.
    for (const std::string_view text : {
             "res://console.txt",
             "res://com10",
             "res://nul_x.txt",
             "res://lpt",
             "res://.hidden",
             "res://com\xE2\x81\xB4",
             "res://conin",
             "res://conout$x",
         }) {
        REQUIRE(vfs::parse(text).has_value());
    }
}

TEST_CASE("well-formed text's size is counted before it's made", "[vfs]") {
    // U+FFFD for each byte that doesn't start one, 3 bytes each, and what's fine as it is.
    REQUIRE(vfs::well_formed_size("\xFF\xFF") == 6);
    REQUIRE(vfs::well_formed("\xFF\xFF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    REQUIRE(vfs::well_formed_size("a\xC3\xA9") == 3);
    REQUIRE(vfs::well_formed("a\xC3\xA9") == "a\xC3\xA9");
    REQUIRE(vfs::well_formed_size("\xF0\x9F\x98\x80") == 4);
}

TEST_CASE("a folder hides names starting with a dot, as a cooked pak has none", "[vfs]") {
    const temp_folder_t folder;
    folder.write("seen.txt", "seen");
    folder.write(".DS_Store", "the OS's");
    folder.write(".git/HEAD", "a repository's");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    REQUIRE(read_failure(files, "res://.DS_Store") == vfs::failure_t::NOT_FOUND);
    REQUIRE(read_failure(files, "res://.git/HEAD") == vfs::failure_t::NOT_FOUND);
    const vfs::result_t<std::vector<vfs::entry_t>> listed = files.list(path("res://"));
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE(listed->front().name == "seen.txt");
}

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
    // A comment with an end record's signature in it, far enough from the end to be looked at.
    const std::string comment =
        std::string("a comment, ") + std::string("PK\x05\x06", 4) + std::string(18, '\0') + " too";
    const vfs::bytes_t bytes = zip({ { .name = "a.txt", .data = "on the disk" } }, false, comment);
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

    // And one whose comment is a whole end record, ending the file as the real one would.
    const vfs::bytes_t fake = zip({ { .name = "a.txt", .data = "in memory" } }, false,
                                  std::string("PK\x05\x06", 4) + std::string(18, '\0'));
    vfs::vfs_t from_memory;
    from_memory.mount(path("res://"), *vfs::pak(fake));
    REQUIRE(read(from_memory, "res://a.txt") == "in memory");
}

TEST_CASE("a mounted pak on the disk can be replaced and removed, and still reads", "[vfs]") {
    const temp_folder_t folder;
    const auto write = [](const fs::path& file, const vfs::bytes_t& bytes) {
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    };
    write(folder.path / "game.pak", zip({ { .name = "a.txt", .data = "first" } }));
    vfs::vfs_t files;
    files.mount(path("res://"), *vfs::pak(folder.path / "game.pak"));
    write(folder.path / "new.pak", zip({ { .name = "a.txt", .data = "second" } }));
    // As an updater removes a pak and puts another in its place: the mounted one still reads.
    // (Windows' plain rename over a file that's open can't, whatever the file was opened to share;
    // one with POSIX semantics can.)
    std::error_code error;
    fs::remove(folder.path / "game.pak", error);
    REQUIRE_FALSE(error);
    REQUIRE(read(files, "res://a.txt") == "first");
    fs::rename(folder.path / "new.pak", folder.path / "game.pak", error);
    REQUIRE_FALSE(error);
    REQUIRE(read(files, "res://a.txt") == "first");
    files.mount(path("res://"), *vfs::pak(folder.path / "game.pak"));
    REQUIRE(read(files, "res://a.txt") == "second");
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
        REQUIRE(pak_failure(bytes) == vfs::failure_t::INVALID_DATA);
        // And a size past it.
        bytes = zip({ { .name = "a.txt", .data = "some text" } });
        bytes[directory + 23] = 0x7F;
        bytes[directory + 27] = 0x7F;
        REQUIRE(pak_failure(bytes) == vfs::failure_t::INVALID_DATA);
    }
    SECTION("a Zip64 end record that isn't there, in a pak too small for one") {
        // A Zip64 locator and an end record, 42 bytes, its locator pointing anywhere.
        const temp_folder_t folder;
        for (const std::uint64_t record : { 0ULL, 1ULL, 22ULL, 0x7FFF'FFFF'FFFF'FFFFULL }) {
            zip_t out;
            out.u32(0x0706'4B50);
            out.u32(0);
            out.u64(record);
            out.u32(1);
            out.u32(0x0605'4B50);
            out.u16(0);
            out.u16(0);
            out.u16(0xFFFF);
            out.u16(0xFFFF);
            out.u32(0xFFFF'FFFF);
            out.u32(0xFFFF'FFFF);
            out.u16(0);
            REQUIRE(pak_failure(out.bytes) == vfs::failure_t::INVALID_DATA);
            folder.write("small.pak", std::string(out.bytes.begin(), out.bytes.end()));
            REQUIRE(vfs::pak(folder.path / "small.pak").error().failure ==
                    vfs::failure_t::INVALID_DATA);
        }
    }
    SECTION("a central directory too big or too small for its entries") {
        // A megabyte that its Zip64 records say is one entry's central directory.
        zip_t out;
        out.bytes.resize(std::size_t{ 1 } << 20U, 0);
        const std::uint64_t record = out.bytes.size();
        out.u32(0x0606'4B50);
        out.u64(44);
        out.u16(45);
        out.u16(45);
        out.u32(0);
        out.u32(0);
        out.u64(1);
        out.u64(1);
        out.u64(record);
        out.u64(0);
        out.u32(0x0706'4B50);
        out.u32(0);
        out.u64(record);
        out.u32(1);
        out.u32(0x0605'4B50);
        out.u16(0);
        out.u16(0);
        out.u16(0xFFFF);
        out.u16(0xFFFF);
        out.u32(0xFFFF'FFFF);
        out.u32(0xFFFF'FFFF);
        out.u16(0);
        REQUIRE(pak_failure(out.bytes) == vfs::failure_t::INVALID_DATA);
        // And a billion entries in one entry's bytes.
        vfs::bytes_t many = zip({ { .name = "a.txt", .data = "x" } }, true);
        const std::size_t record_at = many.size() - 22 - 20 - 56;
        many[record_at + 24 + 3] = 0x40;
        many[record_at + 32 + 3] = 0x40;
        REQUIRE(pak_failure(many) == vfs::failure_t::INVALID_DATA);
    }
    SECTION("a name longer or deeper than a path can be") {
        std::string deep = "a";
        while (deep.size() + 2 <= 65'535) {
            deep += "/a";
        }
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(pak_failure(zip({ { .name = deep, .data = "x" } })) ==
                vfs::failure_t::INVALID_DATA);
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        std::string names = "a";
        for (int i = 1; i < 256; ++i) {
            names += "/a";
        }
        REQUIRE(pak_failure(zip({ { .name = names, .data = "x" } })) ==
                vfs::failure_t::INVALID_DATA);
        REQUIRE(pak_failure(zip({ { .name = std::string(4097, 'a'), .data = "x" } })) ==
                vfs::failure_t::INVALID_DATA);
        // 255 names, and 4096 bytes, are a path.
        names.resize(names.size() - 2);
        REQUIRE(vfs::pak(zip({ { .name = names, .data = "x" } })).has_value());
        REQUIRE(vfs::pak(zip({ { .name = std::string(4096, 'a'), .data = "x" } })).has_value());
    }
}

TEST_CASE("a pak's index takes time and memory as its names do", "[vfs]") {
    // A thousand entries 200 folders deep each, in all about 400 KB of names.
    std::vector<zip_entry_t> entries;
    for (int e = 0; e < 1'000; ++e) {
        std::string name = std::to_string(e);
        for (int i = 0; i < 200; ++i) {
            name += "/f";
        }
        entries.push_back({ .name = name + "/file.txt", .data = "x" });
    }
    const auto started = std::chrono::steady_clock::now();
    const std::shared_ptr<vfs::source_t> pak = pak_of(entries);
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    vfs::vfs_t files;
    files.mount(path("res://"), pak);
    std::string last = "res://999";
    for (int i = 0; i < 200; ++i) {
        last += "/F";
    }
    REQUIRE(read(files, last + "/file.txt") == "x");
}

TEST_CASE("a newer mount at or under a place makes it a folder, as kind and read say", "[vfs]") {
    vfs::vfs_t files;
    files.mount(path("res://"), pak_of({ { .name = "data", .data = "a file" } }));
    files.mount(path("res://data"), pak_of({ { .name = "inside.txt", .data = "inside" } }));
    REQUIRE(kind(files, "res://data") == vfs::kind_t::FOLDER);
    REQUIRE(read_failure(files, "res://data") == vfs::failure_t::IS_A_FOLDER);
    REQUIRE(list(files, "res://data") == std::vector<std::string>{ "inside.txt" });

    // And a newer mount's folder over an older one's file.
    vfs::vfs_t over;
    over.mount(path("res://"), pak_of({ { .name = "thing", .data = "a file" } }));
    over.mount(path("res://"), pak_of({ { .name = "thing/in.txt", .data = "in" } }));
    REQUIRE(kind(over, "res://thing") == vfs::kind_t::FOLDER);
    REQUIRE(read_failure(over, "res://thing") == vfs::failure_t::IS_A_FOLDER);
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
    // The mounts change until the readers have read a while meanwhile, however slow they are.
    const int before = reads;
    for (int i = 0; i < 2'000 || reads - before < 2'000; ++i) {
        files.mount(path("res://"), i % 2 == 0 ? second : vfs::folder(folder.path));
        files.unmount(path("res://"));
    }
    stop = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE(wrong == 0);
    REQUIRE(read(files, "res://a.txt") == "first");
}

TEST_CASE("res:// is a folder with nothing mounted, as kind, list, and read say", "[vfs]") {
    const vfs::vfs_t files;
    REQUIRE(kind(files, "res://") == vfs::kind_t::FOLDER);
    REQUIRE(list(files, "res://").empty());
    REQUIRE(read_failure(files, "res://") == vfs::failure_t::IS_A_FOLDER);
}

namespace {

// Waits until what was just written to a folder has settled, so the folder's listing is kept: a
// change as old as a disk's coarsest times, 2 s.
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
}

} // namespace

TEST_CASE("a folder's listing is kept until the folder changes", "[vfs]") {
    const temp_folder_t folder;
    folder.write("a.txt", "a");
    folder.write("b.txt", "b");
    folder.write("sub/c.txt", "c");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // Long enough after its last change that it's kept as it's listed, on a disk of times in
    // whole seconds or two too.
    settle();
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "a.txt", "b.txt", "sub/" });
    REQUIRE(read(files, "res://sub/c.txt") == "c");

    // What's added, taken out, or made a folder is seen at once.
    folder.write("new.txt", "new");
    REQUIRE(read(files, "res://new.txt") == "new");
    fs::remove(folder.path / "a.txt");
    REQUIRE(read_failure(files, "res://a.txt") == vfs::failure_t::NOT_FOUND);
    fs::remove(folder.path / "b.txt");
    folder.write("b.txt/inside.txt", "inside");
    REQUIRE(kind(files, "res://b.txt") == vfs::kind_t::FOLDER);
    REQUIRE(read(files, "res://b.txt/inside.txt") == "inside");
    fs::rename(folder.path / "sub", folder.path / "moved");
    REQUIRE(read_failure(files, "res://sub/c.txt") == vfs::failure_t::NOT_FOUND);
    REQUIRE(read(files, "res://moved/c.txt") == "c");
    // And a file's new bytes, which aren't the listing's.
    settle();
    REQUIRE(read(files, "res://new.txt") == "new");
    folder.write("new.txt", "newer");
    REQUIRE(read(files, "res://new.txt") == "newer");
    // A folder taken out is gone, and one put back is seen.
    fs::remove_all(folder.path / "moved");
    REQUIRE(kind(files, "res://moved") == vfs::kind_t::NONE);
    folder.write("moved/d.txt", "d");
    REQUIRE(read(files, "res://moved/d.txt") == "d");
}

TEST_CASE("reading each file of a big folder takes a stat of each folder on its path", "[vfs]") {
    constexpr int count = 2000;
    const temp_folder_t folder;
    for (int i = 0; i < count; ++i) {
        folder.write(std::format("GFX/map/tex{}.png", i), "x");
    }
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // Its folders' listings are kept once they've settled.
    settle();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        REQUIRE(read(files, std::format("res://gfx/MAP/tex{}.png", i)) == "x");
        REQUIRE(read_failure(files, std::format("res://gfx/map/missing{}.png", i)) ==
                vfs::failure_t::NOT_FOUND);
    }
    // Listing the folder at each read takes minutes.
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
}

TEST_CASE("a read with no memory for a file's bytes fails as OTHER", "[vfs]") {
    const temp_folder_t folder;
    folder.write("a.txt", "a");
    vfs::vfs_t files;
    files.mount(path("res://disk"), vfs::folder(folder.path));
    files.mount(path("res://pak"), pak_of({ { .name = "b.txt", .data = "b" } }));
    for (const std::string_view at : { "res://disk/a.txt", "res://pak/b.txt" }) {
        vfs::inject(vfs::fault_t::OUT_OF_MEMORY);
        const vfs::result_t<vfs::bytes_t> bytes = files.read(path(at));
        REQUIRE_FALSE(bytes.has_value());
        REQUIRE(bytes.error().failure == vfs::failure_t::OTHER);
        REQUIRE(bytes.error().message.contains("memory"));
        // Once.
        REQUIRE(files.read(path(at)).has_value());
    }
}

TEST_CASE("a view reads the mounts there were when it was taken", "[vfs]") {
    vfs::vfs_t files;
    files.mount(path("res://"), pak_of({ { .name = "a.txt", .data = "first" } }));
    const vfs::view_t view = files.view();
    files.mount(path("res://"), pak_of({ { .name = "a.txt", .data = "second" } }));
    REQUIRE(read(files, "res://a.txt") == "second");
    REQUIRE(files.unmount(path("res://")));
    REQUIRE(files.unmount(path("res://")));
    REQUIRE(read_failure(files, "res://a.txt") == vfs::failure_t::NOT_FOUND);
    const vfs::result_t<vfs::bytes_t> kept = view.read(path("res://a.txt"));
    REQUIRE(kept.has_value());
    REQUIRE(text_of(*kept) == "first");
    REQUIRE(view.kind(path("res://a.txt")) == vfs::kind_t::FILE);
}

TEST_CASE("a linked folder's listing follows what's in its target", "[vfs]") {
    const temp_folder_t folder;
    folder.write("shared/a.txt", "a");
    fs::create_directories(folder.path / "mounted");
    const fs::path link = folder.path / "mounted" / "linked";
#ifdef _WIN32
    // A junction, which Windows makes without the privilege a link needs, as a game's checkout
    // of shared textures would be.
    const std::string command = std::format(R"(cmd /c mklink /J "{}" "{}" >nul)", vfs::utf8(link),
                                            vfs::utf8(folder.path / "shared"));
    // The test's own command, of its own folder's paths.
    REQUIRE(std::system(command.c_str()) == 0); // NOLINT(bugprone-command-processor)
#else
    fs::create_directory_symlink(folder.path / "shared", link);
#endif
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path / "mounted"));
    files.mount(path("res://direct"), vfs::folder(link));
    settle();
    REQUIRE(read(files, "res://linked/a.txt") == "a");
    REQUIRE(read(files, "res://direct/a.txt") == "a");
    // Kept and settled, then a change in the target, which is the link's own folder's.
    folder.write("shared/b.txt", "b");
    REQUIRE(read(files, "res://linked/b.txt") == "b");
    REQUIRE(read(files, "res://direct/b.txt") == "b");
    settle();
    fs::remove(folder.path / "shared" / "a.txt");
    REQUIRE(kind(files, "res://linked/a.txt") == vfs::kind_t::NONE);
    REQUIRE(list(files, "res://direct") == std::vector<std::string>{ "b.txt" });
#ifdef _WIN32
    // A junction is taken out as a folder, leaving its target.
    fs::remove(link);
#endif
}

namespace {

// Puts a folder's last-write time back to `time`, which std::filesystem can't on Windows, where a
// folder opens only with backup semantics.
void put_back(const fs::path& folder, fs::file_time_type time) {
#ifdef _WIN32
    const fs::file_time_type now = fs::last_write_time(folder);
    // FILETIMEs are 100 ns ticks, as file_time_type's are on Windows.
    const auto ticks = [](fs::file_time_type at) {
        return static_cast<std::uint64_t>(at.time_since_epoch().count());
    };
    HANDLE handle = CreateFileW(folder.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    FILETIME written{};
    REQUIRE(GetFileTime(handle, nullptr, nullptr, &written) != 0);
    const std::uint64_t back =
        ((static_cast<std::uint64_t>(written.dwHighDateTime) << 32U) | written.dwLowDateTime) -
        (ticks(now) - ticks(time));
    written.dwLowDateTime = static_cast<DWORD>(back);
    written.dwHighDateTime = static_cast<DWORD>(back >> 32U);
    REQUIRE(SetFileTime(handle, nullptr, nullptr, &written) != 0);
    CloseHandle(handle);
#else
    fs::last_write_time(folder, time);
#endif
}

} // namespace

TEST_CASE("a change on a disk of coarse times is seen", "[vfs]") {
    // A folder listed in the same second as it changed, then changed again, its time put back, as
    // a disk of whole seconds or two leaves it.
    const temp_folder_t folder;
    folder.write("a.txt", "a");
    const fs::file_time_type written = fs::last_write_time(folder.path);
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "a.txt" });
    folder.write("b.txt", "b");
    put_back(folder.path, written);
    REQUIRE(read(files, "res://b.txt") == "b");
    REQUIRE(list(files, "res://") == std::vector<std::string>{ "a.txt", "b.txt" });
}

namespace {

// `seed` broken `count` ways by `random`, as the loaders' fuzz tests break theirs: cut short, a few
// bytes flipped, a 4-byte number made 0, 1, huge, or negative, a piece repeated or taken out.
std::vector<vfs::bytes_t> broken(const vfs::bytes_t& seed, std::mt19937& random, int count) {
    std::vector<vfs::bytes_t> out;
    for (std::size_t length = 0; length < seed.size(); length += 3) {
        out.emplace_back(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(length));
    }
    const auto at = [&random](std::size_t size) {
        return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    };
    constexpr std::array<std::uint32_t, 6> numbers{ 0,           1,           0x7fffffffU,
                                                    0xffffffffU, 0x80000000U, 0xffffU };
    for (int made = 0; made < count; ++made) {
        vfs::bytes_t bytes = seed;
        switch (random() % 5) {
        case 0:
            bytes.resize(at(bytes.size()));
            break;
        case 1:
            for (std::uint32_t flips = 1 + (random() % 4); flips > 0; --flips) {
                bytes[at(bytes.size())] ^= static_cast<std::uint8_t>(1U << (random() % 8));
            }
            break;
        case 2: {
            const std::size_t from = at(bytes.size() - 1);
            const std::uint32_t number = numbers.at(random() % numbers.size());
            for (std::size_t byte = 0; byte < 4 && from + byte < bytes.size(); ++byte) {
                bytes[from + byte] = static_cast<std::uint8_t>(number >> (8U * byte));
            }
            break;
        }
        case 3: {
            const std::size_t from = at(bytes.size());
            const std::size_t length =
                std::min<std::size_t>(1 + (random() % 48), bytes.size() - from);
            const vfs::bytes_t piece(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(from), piece.begin(),
                         piece.end());
            break;
        }
        default: {
            const std::size_t from = at(bytes.size());
            const std::size_t length =
                std::min<std::size_t>(1 + (random() % 48), bytes.size() - from);
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                        bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            break;
        }
        }
        out.push_back(std::move(bytes));
    }
    return out;
}

// Everything under `folder` in `view`, listed and read, as a game walking a mounted pak would;
// how many files it read.
int walk(const vfs::view_t& view, const std::string& folder, int depth) {
    if (depth > 16) {
        return 0;
    }
    const vfs::result_t<std::vector<vfs::entry_t>> listed = view.list(path(folder));
    if (!listed.has_value()) {
        return 0;
    }
    int read = 0;
    for (const vfs::entry_t& entry : *listed) {
        const std::string at = folder + (folder.ends_with('/') ? "" : "/") + entry.name;
        const vfs::result_t<vfs::path_t> parsed = vfs::parse(at);
        if (!parsed.has_value()) {
            continue;
        }
        if (entry.kind == vfs::kind_t::FOLDER) {
            read += walk(view, at, depth + 1);
        } else {
            if (view.read(*parsed, 1U << 20U).has_value()) {
                ++read;
            }
        }
    }
    return read;
}

} // namespace

// A broken pak isn't one, or is one whose index is what it has: none that crashes, hangs, or
// reads past its end, opened, listed, and read.
TEST_CASE("broken paks fail, never crash or hang", "[vfs][fuzz][stability]") {
    std::mt19937 random(20261109); // NOLINT(bugprone-random-generator-seed)
    for (const bool zip64 : { false, true }) {
        INFO(zip64);
        const std::vector<zip_entry_t> entries = {
            { .name = "a.txt", .data = "first" },
            { .name = "models/b.glb", .data = std::string(300, 'b') },
            { .name = "models/deep/c.png", .data = "cc" },
            { .name = "d/", .data = "" },
            { .name = "e.bin", .data = std::string(40, '\0') },
        };
        const vfs::bytes_t seed = zip(entries, zip64);
        {
            vfs::result_t<std::shared_ptr<vfs::source_t>> whole = vfs::pak(seed);
            REQUIRE(whole.has_value());
            vfs::vfs_t mounted;
            mounted.mount(path("res://"), *whole);
            REQUIRE(walk(mounted.view(), "res://", 0) == 4);
        }
        for (const vfs::bytes_t& bytes : broken(seed, random, 4000)) {
            const auto started = std::chrono::steady_clock::now();
            vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(bytes);
            if (source.has_value()) {
                vfs::vfs_t files;
                files.mount(path("res://"), *source);
                (void)walk(files.view(), "res://", 0);
            }
            REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        }
    }
}
