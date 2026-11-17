// The cooker's encrypted paks: what the engine's res:// reads back with the game's key and
// nothing without it, the same bytes for the same files, every file and the index checked as it's
// read, and an index that's sealed but says what isn't there refused, never read past.

#include "../assets/files.hpp"
#include "cook/pak.hpp"
#include "vfs/disk.hpp"
#include "vfs/sealed.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <monocypher.h>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace tools = fbe::tools;
namespace vfs = fbe::vfs;
namespace sealed = fbe::vfs::sealed;
using test::bytes_t;

constexpr std::size_t chunk = std::size_t{ 1 } << 20U;

constexpr sealed::key_t key_of(std::uint8_t first) {
    sealed::key_t key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(first + i);
    }
    return key;
}

constexpr sealed::key_t game_key = key_of(1);

vfs::path_t path(std::string_view text) {
    return *vfs::parse(text);
}

std::string utf8(const std::filesystem::path& file) {
    return vfs::utf8(file);
}

std::unique_ptr<tools::pak_writer_t> opened(const std::filesystem::path& to,
                                            const sealed::key_t& key = game_key) {
    std::expected<std::unique_ptr<tools::pak_writer_t>, tools::error_t> writer =
        tools::pak_writer_t::open(utf8(to), key);
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

std::shared_ptr<vfs::source_t> pak(const std::filesystem::path& file,
                                   const sealed::key_t& key = game_key) {
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(file, key);
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

bytes_t bytes_of(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), {} };
}

bool contains(const bytes_t& bytes, std::string_view text) {
    const std::string_view all(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return all.contains(text);
}

// The error of mounting `bytes`, a pak, with `key`, which the test fails without.
vfs::error_t refused(const bytes_t& bytes, const std::optional<sealed::key_t>& key = game_key) {
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(bytes, key);
    if (source) {
        FAIL("the pak mounted");
    }
    return source.error();
}

void put(bytes_t& bytes, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

// `plain` sealed as a file named `name` is, or the index for no name: its nonce, its bytes, and
// its tag.
bytes_t seal(const sealed::keys_t& keys, std::span<const std::uint8_t> authenticated,
             const bytes_t& plain, std::optional<std::string_view> name) {
    std::optional<sealed::nonce_maker_t> maker;
    if (name) {
        maker.emplace(keys, *name);
    } else {
        maker.emplace(keys);
    }
    maker->add(plain);
    const sealed::nonce_t nonce = maker->made();
    bytes_t bytes(nonce.begin(), nonce.end());
    bytes_t sealed_bytes = plain;
    sealed::sealer_t sealer(keys, nonce, authenticated);
    sealer.add(sealed_bytes);
    bytes.insert(bytes.end(), sealed_bytes.begin(), sealed_bytes.end());
    const sealed::tag_t tag = sealer.tag();
    bytes.insert(bytes.end(), tag.begin(), tag.end());
    return bytes;
}

// An encrypted pak of `files`, sealed bytes laid one after another, and `index`, the index's
// bytes as they're sealed, whatever they say: a pak whose index the cooker wouldn't write.
bytes_t forged(const bytes_t& files, const bytes_t& index) {
    const sealed::keys_t keys = sealed::derived(game_key);
    bytes_t header(sealed::magic.begin(), sealed::magic.end());
    header.insert(header.end(), keys.id.begin(), keys.id.end());
    const bytes_t sealed_index = seal(keys, header, index, std::nullopt);
    bytes_t pak = header;
    put(pak, sealed::header_size + files.size(), 8);
    put(pak, sealed_index.size(), 8);
    pak.insert(pak.end(), files.begin(), files.end());
    pak.insert(pak.end(), sealed_index.begin(), sealed_index.end());
    return pak;
}

// Reads everything the pak at res:// has, as a game would, and how many of its files it read.
std::size_t walk(const vfs::view_t& view, const std::string& at, int depth) {
    vfs::result_t<std::vector<vfs::entry_t>> listed = view.list(path(at));
    if (!listed || depth > 8) {
        return 0;
    }
    std::size_t count = 0;
    for (const vfs::entry_t& entry : *listed) {
        const std::string inside = std::format("{}/{}", at == "res://" ? "res:/" : at, entry.name);
        if (entry.kind == vfs::kind_t::FOLDER) {
            count += walk(view, inside, depth + 1);
        } else {
            if (view.read(path(inside)).has_value()) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("an encrypted pak is what res:// reads with its key, and shows nothing without it",
          "[tools]") {
    // Bigger than the writer's chunk, so it's sealed in parts.
    bytes_t big((chunk * 3) + 5);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>((i * 7) + (i >> 12U));
    }
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha, the game's own text");
    folder.write("big.bin", big);
    folder.write("empty.txt", "");
    const std::filesystem::path to = folder.path / "out" / "game.pak";
    const std::unique_ptr<tools::pak_writer_t> writer = opened(to);
    added(*writer, "a.txt", folder.path / "a.txt");
    added(*writer, "Sub/Big.bin", folder.path / "big.bin");
    added(*writer, "sub/empty.txt", folder.path / "empty.txt");
    finished(*writer);
    REQUIRE(std::distance(std::filesystem::directory_iterator(to.parent_path()),
                          std::filesystem::directory_iterator()) == 1);

    vfs::vfs_t files;
    files.mount(path("res://"), pak(to));
    REQUIRE(read(files, "res://a.txt") == test::bytes("alpha, the game's own text"));
    REQUIRE(read(files, "res://sub/big.bin") == big);
    REQUIRE(read(files, "res://sub/empty.txt").empty());
    vfs::result_t<std::vector<vfs::entry_t>> listed = files.list(path("res://sub"));
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    REQUIRE(files.kind(path("res://SUB")) == vfs::kind_t::FOLDER);

    // Neither a file's bytes nor a name, and nothing of a zip's.
    const bytes_t written = bytes_of(to);
    REQUIRE(std::equal(sealed::magic.begin(), sealed::magic.end(), written.begin()));
    REQUIRE_FALSE(contains(written, "alpha"));
    REQUIRE_FALSE(contains(written, "Big.bin"));
    REQUIRE_FALSE(contains(written, "empty.txt"));
    REQUIRE_FALSE(contains(written, std::string_view("PK", 4)));
    REQUIRE_FALSE(contains(written, std::string_view("PK", 4)));
    REQUIRE(written.size() == sealed::header_size + (3 * sealed::overhead) + 26 + big.size() +
                                  sealed::overhead + 8 + (3 * sealed::entry_size) + 5 + 11 + 13);
}

TEST_CASE("an encrypted pak seals a file the same in every build, wherever it is", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "first");
    folder.write("b.bin", std::string(5000, 'b'));
    const auto cooked = [&](const std::filesystem::path& to) {
        const std::unique_ptr<tools::pak_writer_t> writer = opened(to);
        added(*writer, "a.txt", folder.path / "a.txt");
        added(*writer, "b.bin", folder.path / "b.bin");
        finished(*writer);
        return bytes_of(to);
    };
    const bytes_t one = cooked(folder.path / "one.pak");
    const bytes_t two = cooked(folder.path / "two.pak");
    REQUIRE(one == two);

    // a.txt grows, so b.bin moves, and its sealed bytes are as they were.
    folder.write("a.txt", "first, and then more");
    const bytes_t three = cooked(folder.path / "three.pak");
    const std::size_t b_size = sealed::overhead + 5000;
    const auto b_in = [&](const bytes_t& pak, std::size_t a_size) {
        const std::size_t at = sealed::header_size + sealed::overhead + a_size;
        return bytes_t(pak.begin() + static_cast<std::ptrdiff_t>(at),
                       pak.begin() + static_cast<std::ptrdiff_t>(at + b_size));
    };
    REQUIRE(b_in(one, 5) == b_in(three, 20));
    const auto a_in = [&](const bytes_t& pak, std::size_t a_size) {
        return bytes_t(pak.begin() + static_cast<std::ptrdiff_t>(sealed::header_size),
                       pak.begin() + static_cast<std::ptrdiff_t>(sealed::header_size +
                                                                 sealed::overhead + a_size));
    };
    REQUIRE(a_in(one, 5) != a_in(three, 20));

    // The same bytes under another name seal another way.
    const std::unique_ptr<tools::pak_writer_t> renamed = opened(folder.path / "renamed.pak");
    added(*renamed, "c.txt", folder.path / "b.bin");
    finished(*renamed);
    const bytes_t four = bytes_of(folder.path / "renamed.pak");
    REQUIRE(bytes_t(four.begin() + static_cast<std::ptrdiff_t>(sealed::header_size),
                    four.begin() + static_cast<std::ptrdiff_t>(sealed::header_size + b_size)) !=
            b_in(one, 5));
}

TEST_CASE("an encrypted pak opens only with its own key, and a zip with none", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha");
    const std::unique_ptr<tools::pak_writer_t> writer = opened(folder.path / "game.pak");
    added(*writer, "a.txt", folder.path / "a.txt");
    finished(*writer);
    const bytes_t written = bytes_of(folder.path / "game.pak");

    const vfs::error_t other_key = refused(written, key_of(2));
    REQUIRE(other_key.failure == vfs::failure_t::INVALID_DATA);
    REQUIRE(other_key.message.contains("encrypted with another game's key"));
    const vfs::error_t no_key = refused(written, std::nullopt);
    REQUIRE(no_key.failure == vfs::failure_t::INVALID_DATA);
    REQUIRE(no_key.message.contains("it's encrypted, and opens only with its game's key"));

    std::expected<std::unique_ptr<tools::pak_writer_t>, tools::error_t> zip =
        tools::pak_writer_t::open(utf8(folder.path / "zip.pak"));
    REQUIRE(zip.has_value());
    added(**zip, "a.txt", folder.path / "a.txt");
    finished(**zip);
    const vfs::error_t plain = refused(bytes_of(folder.path / "zip.pak"));
    REQUIRE(plain.failure == vfs::failure_t::INVALID_DATA);
    REQUIRE(plain.message.contains("it isn't encrypted, as its game's pak is"));
    // A mod's pak, a zip, mounts without a key, as it did.
    REQUIRE(vfs::pak(bytes_of(folder.path / "zip.pak")).has_value());
}

TEST_CASE("an encrypted pak's file that isn't what was written fails as it's read", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha");
    folder.write("b.txt", "bravo");
    const std::unique_ptr<tools::pak_writer_t> writer = opened(folder.path / "game.pak");
    added(*writer, "a.txt", folder.path / "a.txt");
    added(*writer, "b.txt", folder.path / "b.txt");
    finished(*writer);
    const bytes_t written = bytes_of(folder.path / "game.pak");

    // Each byte of a's sealed bytes, its nonce, its text, and its tag, changed.
    for (std::size_t at = sealed::header_size; at < sealed::header_size + sealed::overhead + 5;
         ++at) {
        INFO(at);
        bytes_t changed = written;
        changed[at] ^= 0x01U;
        vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(changed, game_key);
        REQUIRE(source.has_value());
        vfs::vfs_t files;
        files.mount(path("res://"), *source);
        vfs::result_t<bytes_t> a = files.read(path("res://a.txt"));
        REQUIRE_FALSE(a.has_value());
        REQUIRE(a.error().failure == vfs::failure_t::INVALID_DATA);
        REQUIRE(a.error().message.contains("a.txt isn't what was written"));
        REQUIRE(read(files, "res://b.txt") == test::bytes("bravo"));
    }

    // a's and b's sealed bytes swapped, which are each other's names', not their own.
    const std::size_t size = sealed::overhead + 5;
    bytes_t swapped = written;
    std::swap_ranges(swapped.begin() + static_cast<std::ptrdiff_t>(sealed::header_size),
                     swapped.begin() + static_cast<std::ptrdiff_t>(sealed::header_size + size),
                     swapped.begin() + static_cast<std::ptrdiff_t>(sealed::header_size + size));
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(swapped, game_key);
    REQUIRE(source.has_value());
    vfs::vfs_t files;
    files.mount(path("res://"), *source);
    REQUIRE_FALSE(files.read(path("res://a.txt")).has_value());
    REQUIRE_FALSE(files.read(path("res://b.txt")).has_value());

    // Each byte of the index, and of the header, changed.
    for (std::size_t at = 0; at < written.size(); ++at) {
        if (at >= sealed::header_size && at < sealed::header_size + (2 * size)) {
            continue;
        }
        INFO(at);
        bytes_t changed = written;
        changed[at] ^= 0x80U;
        REQUIRE(refused(changed).failure == vfs::failure_t::INVALID_DATA);
    }
    // Cut short anywhere.
    for (std::size_t size_cut = 0; size_cut < written.size(); ++size_cut) {
        INFO(size_cut);
        const bytes_t cut(written.begin(), written.begin() + static_cast<std::ptrdiff_t>(size_cut));
        REQUIRE(refused(cut).failure == vfs::failure_t::INVALID_DATA);
    }
}

TEST_CASE("an encrypted pak seals as XChaCha20-Poly1305 does all at once", "[tools]") {
    const sealed::keys_t keys = sealed::derived(game_key);
    REQUIRE(keys.seal != game_key);
    REQUIRE(keys.nonce != keys.seal);
    constexpr std::array<std::size_t, 6> sizes = { 0, 1, 63, 64, 200, 4096 + 9 };
    for (const std::size_t size : sizes) {
        INFO(size);
        bytes_t plain(size);
        for (std::size_t i = 0; i < size; ++i) {
            plain[i] = static_cast<std::uint8_t>(i * 13);
        }
        const bytes_t authenticated = test::bytes("models/door.glb");
        sealed::nonce_maker_t maker(keys, "models/door.glb");
        maker.add(plain);
        const sealed::nonce_t nonce = maker.made();

        bytes_t whole(size);
        sealed::tag_t whole_tag{};
        crypto_aead_lock(whole.data(), whole_tag.data(), keys.seal.data(), nonce.data(),
                         authenticated.data(), authenticated.size(), plain.data(), plain.size());

        // In parts of 64 and 128 bytes, and what's left.
        bytes_t parts = plain;
        sealed::sealer_t sealer(keys, nonce, authenticated);
        std::size_t at = 0;
        for (const std::size_t part : { std::size_t{ 64 }, std::size_t{ 128 } }) {
            const std::size_t taken = std::min(part, size - at);
            sealer.add(std::span(parts).subspan(at, taken));
            at += taken;
        }
        sealer.add(std::span(parts).subspan(at));
        REQUIRE(parts == whole);
        const sealed::tag_t tag = sealer.tag();
        REQUIRE(tag == whole_tag);

        REQUIRE(sealed::opened(keys, nonce, tag, authenticated, parts));
        REQUIRE(parts == plain);
    }
}

TEST_CASE("an encrypted pak's names are what the engine's reader takes, as a zip's are",
          "[tools]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha");
    const std::unique_ptr<tools::pak_writer_t> writer = opened(folder.path / "game.pak");
    for (const std::string_view name : { "../a.txt", "a/./b.txt", "con.txt", "a\\b.txt", "" }) {
        INFO(name);
        std::expected<void, tools::error_t> done = writer->add(name, utf8(folder.path / "a.txt"));
        REQUIRE_FALSE(done.has_value());
        REQUIRE(done.error().failure == vfs::failure_t::INVALID_DATA);
    }
    added(*writer, "A.txt", folder.path / "a.txt");
    REQUIRE_FALSE(writer->add("a.TXT", utf8(folder.path / "a.txt")).has_value());
    finished(*writer);
    vfs::vfs_t files;
    files.mount(path("res://"), pak(folder.path / "game.pak"));
    REQUIRE(read(files, "res://a.txt") == test::bytes("alpha"));
}

TEST_CASE("an encrypted pak's index that says what isn't there is refused", "[tools]") {
    const sealed::keys_t keys = sealed::derived(game_key);
    const bytes_t file = seal(keys, test::bytes("a.txt"), test::bytes("alpha"), "a.txt");
    // An index of one entry, named `name`, at `offset`, of `size`, with `count` said.
    const auto index = [](std::uint64_t count, std::string_view name, std::uint64_t name_size,
                          std::uint64_t offset, std::uint64_t size) {
        bytes_t bytes;
        put(bytes, count, 8);
        put(bytes, name_size, 4);
        bytes.insert(bytes.end(), name.begin(), name.end());
        put(bytes, offset, 8);
        put(bytes, size, 8);
        return bytes;
    };
    const std::uint64_t at = sealed::header_size;

    // What the cooker writes, forged as it's written, mounts.
    {
        vfs::result_t<std::shared_ptr<vfs::source_t>> source =
            vfs::pak(forged(file, index(1, "a.txt", 5, at, 5)), game_key);
        REQUIRE(source.has_value());
        vfs::vfs_t files;
        files.mount(path("res://"), *source);
        REQUIRE(read(files, "res://a.txt") == test::bytes("alpha"));
    }
    const std::vector<std::pair<bytes_t, std::string_view>> cases = {
        { index(2, "a.txt", 5, at, 5), "cut short" },
        { index(UINT64_MAX, "a.txt", 5, at, 5), "cut short" },
        { index(1, "a.txt", 6, at, 5), "cut short" },
        { index(1, "a.txt", UINT32_MAX, at, 5), "cut short" },
        { index(1, "a.txt", 5, at, 6), "past its end" },
        { index(1, "a.txt", 5, at, UINT64_MAX), "past its end" },
        { index(1, "a.txt", 5, at + 1, 5), "past its end" },
        { index(1, "a.txt", 5, 0, 5), "past its end" },
        { index(1, "a.txt", 5, UINT64_MAX, 5), "past its end" },
        { index(1, "../a.t", 6, at, 5), "isn't a path in res://" },
        { index(1, "", 0, at, 5), "isn't a path in res://" },
        { index(1, "a:txt", 5, at, 5), "isn't a path in res://" },
    };
    for (const auto& [bytes, why] : cases) {
        INFO(why);
        const vfs::error_t error = refused(forged(file, bytes));
        REQUIRE(error.failure == vfs::failure_t::INVALID_DATA);
        REQUIRE(error.message.contains(why));
    }
    // A file and a folder of one name, in either order.
    bytes_t both;
    put(both, 2, 8);
    for (const std::string_view name : { "a", "a/b" }) {
        put(both, name.size(), 4);
        both.insert(both.end(), name.begin(), name.end());
        put(both, at, 8);
        put(both, 5, 8);
    }
    REQUIRE(refused(forged(file, both)).message.contains("a file and a folder too"));
    // An index too short to count its files.
    REQUIRE(refused(forged(file, bytes_t(4))).message.contains("isn't where it says"));
}

TEST_CASE("an encrypted pak's read with no memory for its bytes fails as OTHER", "[tools]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "alpha");
    const std::unique_ptr<tools::pak_writer_t> writer = opened(folder.path / "game.pak");
    added(*writer, "a.txt", folder.path / "a.txt");
    finished(*writer);
    vfs::vfs_t files;
    files.mount(path("res://"), pak(folder.path / "game.pak"));
    vfs::inject(vfs::fault_t::OUT_OF_MEMORY);
    vfs::result_t<bytes_t> bytes = files.read(path("res://a.txt"));
    REQUIRE_FALSE(bytes.has_value());
    REQUIRE(bytes.error().failure == vfs::failure_t::OTHER);
    REQUIRE(read(files, "res://a.txt") == test::bytes("alpha"));
    REQUIRE_FALSE(files.read(path("res://a.txt"), 4).has_value());
}

// A broken encrypted pak isn't one, or is one whose index is what was sealed: none that crashes,
// hangs, or reads past its end, opened, listed, and read.
TEST_CASE("broken encrypted paks fail, never crash or hang", "[tools][fuzz][stability]") {
    const test::temp_folder_t folder;
    folder.write("a.txt", "first");
    folder.write("b.glb", std::string(300, 'b'));
    folder.write("c.png", "cc");
    const std::unique_ptr<tools::pak_writer_t> writer = opened(folder.path / "game.pak");
    added(*writer, "a.txt", folder.path / "a.txt");
    added(*writer, "models/b.glb", folder.path / "b.glb");
    added(*writer, "models/deep/c.png", folder.path / "c.png");
    finished(*writer);
    const bytes_t seed = bytes_of(folder.path / "game.pak");
    {
        vfs::vfs_t files;
        files.mount(path("res://"), pak(folder.path / "game.pak"));
        REQUIRE(walk(files.view(), "res://", 0) == 3);
    }
    std::mt19937 random(20261116); // NOLINT(bugprone-random-generator-seed)
    for (int i = 0; i < 4000; ++i) {
        bytes_t bytes = seed;
        switch (random() % 3) {
        case 0:
            bytes.resize(random() % bytes.size());
            break;
        case 1:
            for (std::uint32_t flips = 1 + (random() % 4); flips-- > 0;) {
                bytes[random() % bytes.size()] = static_cast<std::uint8_t>(random());
            }
            break;
        default:
            // The header's offset or size of the index, anything at all.
            for (std::size_t at = sealed::header_start; at < sealed::header_size; ++at) {
                if (random() % 2 == 0) {
                    bytes[at] = static_cast<std::uint8_t>(random());
                }
            }
            break;
        }
        const auto started = std::chrono::steady_clock::now();
        vfs::result_t<std::shared_ptr<vfs::source_t>> source = vfs::pak(bytes, game_key);
        if (source.has_value()) {
            vfs::vfs_t files;
            files.mount(path("res://"), *source);
            (void)walk(files.view(), "res://", 0);
        }
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }
}
