// Khronos's skinned and morphing sample models with their bytes changed, cut short, their numbers
// made huge or not numbers, and their nodes made cyclic, each loaded and all of it made: what's
// made of it either is made or is refused, saying why, and a pose a clip gives stays in numbers.
// Nothing crashes or hangs, and the ASan build says nothing reads out of bounds.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// A generator from `seed`, the same for each run.
std::mt19937 seeded(std::uint32_t seed) {
    return std::mt19937(seed);
}

using bytes_t = std::vector<std::uint8_t>;

bytes_t read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// A folder of its own, mounted at res://fuzz-model, that each changed file's written into.
class folder_t {
public:
    // One of its own, as ctest runs the cases at once, each a process.
    folder_t()
        : m_folder(std::filesystem::temp_directory_path() /
                   ("fbengine-model-fuzz-" + std::to_string(std::random_device()()))) {
        std::filesystem::create_directories(m_folder);
        REQUIRE(fbe_vfs_mount_folder("res://fuzz-model", m_folder.string().c_str()));
    }

    folder_t(const folder_t&) = delete;
    folder_t& operator=(const folder_t&) = delete;
    folder_t(folder_t&&) = delete;
    folder_t& operator=(folder_t&&) = delete;

    // Only remove_all's allocations can throw, which end the tests.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~folder_t() {
        fbe_vfs_unmount("res://fuzz-model");
        std::error_code ignored;
        std::filesystem::remove_all(m_folder, ignored);
    }

    void write(std::string_view name, const bytes_t& bytes) const {
        std::ofstream file(m_folder / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::filesystem::path m_folder;
};

bool finite(const fbe_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Whether every entity under `entity`, and it, is placed, turned, and scaled by numbers.
bool posed_in_numbers(std::uint64_t entity) {
    fbe_vec3_t at{};
    fbe_vec3_t scale{};
    fbe_quat_t turned{};
    fbe_entity_position(entity, FBE_SPACE_LOCAL, &at);
    fbe_entity_scale(entity, FBE_SPACE_LOCAL, &scale);
    fbe_entity_rotation(entity, FBE_SPACE_LOCAL, &turned);
    if (!finite(at) || !finite(scale) || !std::isfinite(turned.x) || !std::isfinite(turned.y) ||
        !std::isfinite(turned.z) || !std::isfinite(turned.w)) {
        return false;
    }
    const std::int32_t count = fbe_entity_child_count(entity);
    for (std::int32_t i = 0; i < count; ++i) {
        if (!posed_in_numbers(fbe_entity_child(entity, i))) {
            return false;
        }
    }
    return true;
}

/// Loads `name` from the folder and makes all there is of it: its materials, geometries, skins,
/// and clips, each scene spawned, and each clip played over its length and past it. What's made
/// is made or refused with a reason; what a clip poses is in numbers. True if it's loaded and each
/// of its scenes spawned.
bool exercise(std::string_view name) {
    const std::string path = "res://fuzz-model/" + std::string(name);
    const std::uint64_t load = fbe_assets_load(FBE_ASSETS_MODEL, path.c_str(), false, false);
    REQUIRE(load != 0);
    if (fbe_assets_failed(load)) {
        CHECK(!std::string_view(fbe_assets_error(load)).empty());
        fbe_assets_free(load);
        return false;
    }
    fbe_model_info_t info{};
    REQUIRE(fbe_assets_model(load, &info));
    std::vector<std::uint64_t> materials;
    for (std::int32_t i = 0; i < info.materials; ++i) {
        const std::uint64_t made = fbe_model_material(load, i);
        CHECK((made != 0 || !std::string_view(fbe_error()).empty()));
        materials.push_back(made);
    }
    std::vector<std::uint64_t> geometries;
    for (std::int32_t i = 0; i < info.meshes; ++i) {
        const std::uint64_t made = fbe_model_geometry(load, i, materials.data(),
                                                      static_cast<std::int32_t>(materials.size()));
        CHECK((made != 0 || !std::string_view(fbe_error()).empty()));
        geometries.push_back(made);
    }
    std::vector<std::uint64_t> skins;
    for (std::int32_t i = 0; i < info.skins; ++i) {
        const std::uint64_t made = fbe_skin_from_model(load, i);
        CHECK((made != 0 || !std::string_view(fbe_error()).empty()));
        skins.push_back(made);
    }
    std::vector<std::uint64_t> clips;
    for (std::int32_t i = 0; i < info.clips; ++i) {
        const std::uint64_t made = fbe_clip_from_model(load, i);
        CHECK((made != 0 || !std::string_view(fbe_error()).empty()));
        if (made != 0) {
            clips.push_back(made);
        }
    }
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t root = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    bool whole = true;
    for (std::int32_t scene = -1; scene < info.scenes; ++scene) {
        const bool spawned = fbe_model_spawn(load, scene, root, geometries.data(),
                                             static_cast<std::int32_t>(geometries.size()),
                                             skins.data(), static_cast<std::int32_t>(skins.size()));
        CHECK((spawned || !std::string_view(fbe_error()).empty()));
        whole = whole && spawned;
    }
    const std::uint64_t player = fbe_player_create(root);
    for (const std::uint64_t clip : clips) {
        if (!fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F)) {
            continue;
        }
        const float length = fbe_clip_length(clip);
        for (const float at : { 0.0F, 0.37F, length * 0.5F, length, (length * 3.0F) + 0.1F }) {
            fbe_player_set_time(player, 0, at);
            INFO(name << " at " << at);
            CHECK(posed_in_numbers(root));
        }
    }
    fbe_player_free(player);
    fbe_world_free(world);
    for (const std::uint64_t clip : clips) {
        fbe_clip_release(clip);
    }
    for (const std::uint64_t skin : skins) {
        if (skin != 0) {
            fbe_skin_release(skin);
        }
    }
    for (const std::uint64_t geometry : geometries) {
        if (geometry != 0) {
            fbe_geometry_release(geometry);
        }
    }
    for (const std::uint64_t material : materials) {
        if (material != 0) {
            fbe_material_release(material);
        }
    }
    fbe_assets_free(load);
    return whole;
}

/// A binary glTF's JSON and its binary chunk, apart.
struct glb_t {
    std::string json;
    bytes_t bin;
};

std::uint32_t word_at(const bytes_t& bytes, std::size_t at) {
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + at, sizeof word);
    return word;
}

glb_t split(const bytes_t& glb) {
    const std::uint32_t json_length = word_at(glb, 12);
    glb_t parts;
    parts.json.assign(reinterpret_cast<const char*>(glb.data() + 20), json_length);
    const std::size_t bin_at = 20 + std::size_t{ json_length };
    if (bin_at + 8 <= glb.size()) {
        const std::uint32_t bin_length = word_at(glb, bin_at);
        parts.bin.assign(glb.begin() + static_cast<std::ptrdiff_t>(bin_at + 8),
                         glb.begin() + static_cast<std::ptrdiff_t>(bin_at + 8 + bin_length));
    }
    return parts;
}

void put_word(bytes_t& into, std::uint32_t word) {
    const auto bits = std::bit_cast<std::array<std::uint8_t, 4>>(word);
    into.insert(into.end(), bits.begin(), bits.end());
}

/// A binary glTF of `parts`, its JSON padded with spaces and its chunks' lengths what they hold.
bytes_t joined(glb_t parts) {
    while (parts.json.size() % 4 != 0) {
        parts.json.push_back(' ');
    }
    while (parts.bin.size() % 4 != 0) {
        parts.bin.push_back(0);
    }
    bytes_t glb;
    put_word(glb, 0x46546C67U);
    put_word(glb, 2);
    const std::size_t total =
        12 + 8 + parts.json.size() + (parts.bin.empty() ? 0 : 8 + parts.bin.size());
    put_word(glb, static_cast<std::uint32_t>(total));
    put_word(glb, static_cast<std::uint32_t>(parts.json.size()));
    put_word(glb, 0x4E4F534AU);
    glb.insert(glb.end(), parts.json.begin(), parts.json.end());
    if (!parts.bin.empty()) {
        put_word(glb, static_cast<std::uint32_t>(parts.bin.size()));
        put_word(glb, 0x004E4942U);
        glb.insert(glb.end(), parts.bin.begin(), parts.bin.end());
    }
    return glb;
}

/// `json` with one of its numbers, picked by `random`, made `with`.
std::string renumbered(std::string json, std::mt19937& random, std::string_view with) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 1; i < json.size(); ++i) {
        const bool digit = std::isdigit(static_cast<unsigned char>(json[i])) != 0;
        const bool after =
            json[i - 1] == ':' || json[i - 1] == '[' || json[i - 1] == ',' || json[i - 1] == ' ';
        if (digit && after) {
            starts.push_back(i);
        }
    }
    if (starts.empty()) {
        return json;
    }
    const std::size_t at = starts[random() % starts.size()];
    std::size_t end = at;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) != 0 || json[end] == '.' ||
            json[end] == 'e' || json[end] == 'E' || json[end] == '-' || json[end] == '+')) {
        ++end;
    }
    json.replace(at, end - at, with);
    return json;
}

/// Where in `json` past `at` the next character that isn't a space is, or its size.
std::size_t past_spaces(const std::string& json, std::size_t at) {
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])) != 0) {
        ++at;
    }
    return at;
}

/// `json` with `inserted` put just inside its `which`th node, the same if it has none such. The
/// nodes are the array of objects named "nodes", not a scene's of indices.
std::string in_nodes(std::string json, std::string_view inserted, int which) {
    std::size_t at = std::string::npos;
    for (std::size_t key = json.find("\"nodes\""); key != std::string::npos;
         key = json.find("\"nodes\"", key + 1)) {
        const std::size_t colon = past_spaces(json, key + 7);
        const std::size_t open =
            colon < json.size() && json[colon] == ':' ? past_spaces(json, colon + 1) : json.size();
        const std::size_t first =
            open < json.size() && json[open] == '[' ? past_spaces(json, open + 1) : json.size();
        if (first < json.size() && json[first] == '{') {
            at = first;
            break;
        }
    }
    for (int i = 0; i < which && at != std::string::npos; ++i) {
        // The next node: past this one's closing brace and a comma, or none at the array's end.
        int depth = 0;
        std::size_t walk = at;
        for (; walk < json.size(); ++walk) {
            if (json[walk] == '{') {
                ++depth;
            } else if (json[walk] == '}') {
                --depth;
            }
            if (depth == 0) {
                break;
            }
        }
        const std::size_t comma = past_spaces(json, walk + 1);
        const std::size_t next =
            comma < json.size() && json[comma] == ',' ? past_spaces(json, comma + 1) : json.size();
        at = next < json.size() && json[next] == '{' ? next : std::string::npos;
    }
    if (at != std::string::npos) {
        json.insert(at + 1, inserted);
    }
    return json;
}

const std::array<std::string_view, 10> numbers{
    "-1",   "0",     "4294967295", "2147483648", "-2147483649", "18446744073709551615",
    "1e30", "1e400", "-0",         "3",
};

/// The seeds: each sample model's file, and whether it's a binary glTF.
struct seed_t {
    std::string_view folder;
    std::string_view file;
    bool binary;
};

const std::array<seed_t, 6> seeds{
    seed_t{ .folder = "RiggedSimple", .file = "RiggedSimple.glb", .binary = true },
    seed_t{ .folder = "AnimatedMorphCube", .file = "AnimatedMorphCube.glb", .binary = true },
    seed_t{ .folder = "MorphPrimitivesTest", .file = "MorphPrimitivesTest.glb", .binary = true },
    seed_t{ .folder = "Fox", .file = "Fox.glb", .binary = true },
    seed_t{ .folder = "SimpleSkin", .file = "SimpleSkin.gltf", .binary = false },
    seed_t{ .folder = "SimpleMorph", .file = "SimpleMorph.gltf", .binary = false },
};

bytes_t seed_bytes(const seed_t& seed) {
    return read_file(std::filesystem::path(FBENGINE_MODELS_DIR) / seed.folder / seed.file);
}

/// What's written for `seed` with its JSON `json`, and, binary, its chunk `bin`.
bytes_t as_file(const seed_t& seed, const std::string& json, const bytes_t& bin) {
    if (seed.binary) {
        return joined({ .json = json, .bin = bin });
    }
    return { json.begin(), json.end() };
}

} // namespace

TEST_CASE("a whole-number accessor's bounds past what it holds are refused as the file's",
          "[stability]") {
    const folder_t folder;
    for (const std::string_view bound : { "1e30", "-1e30", "-9.3e18", "1e400" }) {
        const std::string json = std::string(R"({"asset": {"version": "2.0"},
                "buffers": [{"byteLength": 4, "uri": "data:application/octet-stream;base64,AAABAA=="}],
                "bufferViews": [{"buffer": 0, "byteLength": 4}],
                "accessors": [{"bufferView": 0, "componentType": 5123, "count": 2, "type": "SCALAR",
                               "min": [)") +
                                 std::string(bound) + R"(], "max": [1]}]})";
        folder.write("bounds.gltf", bytes_t(json.begin(), json.end()));
        const std::uint64_t load =
            fbe_assets_load(FBE_ASSETS_MODEL, "res://fuzz-model/bounds.gltf", false, false);
        REQUIRE(load != 0);
        INFO(bound << ": " << fbe_assets_error(load));
        CHECK(fbe_assets_failed(load));
        fbe_assets_free(load);
    }
    // And one in its range is read.
    const std::string json = R"({"asset": {"version": "2.0"},
        "buffers": [{"byteLength": 4, "uri": "data:application/octet-stream;base64,AAABAA=="}],
        "bufferViews": [{"buffer": 0, "byteLength": 4}],
        "accessors": [{"bufferView": 0, "componentType": 5123, "count": 2, "type": "SCALAR",
                       "min": [0.0], "max": [1]}]})";
    folder.write("bounds.gltf", bytes_t(json.begin(), json.end()));
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://fuzz-model/bounds.gltf", false, false);
    REQUIRE(load != 0);
    INFO(fbe_assets_error(load));
    CHECK_FALSE(fbe_assets_failed(load));
    fbe_assets_free(load);
}

TEST_CASE("a sample model cut short anywhere is refused or made, never read past", "[stability]") {
    const folder_t folder;
    for (const seed_t& seed : seeds) {
        const bytes_t whole = seed_bytes(seed);
        folder.write(seed.file, whole);
        exercise(seed.file);
        for (int cut = 0; cut < 48; ++cut) {
            const std::size_t at = whole.size() * static_cast<std::size_t>(cut) / 48;
            folder.write(seed.file,
                         bytes_t(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(at)));
            INFO(seed.file << " cut at " << at);
            exercise(seed.file);
        }
    }
}

TEST_CASE("a sample model with its bytes changed is refused or made, never read past",
          "[stability]") {
    const folder_t folder;
    std::mt19937 random = seeded(7);
    for (const seed_t& seed : seeds) {
        const bytes_t whole = seed_bytes(seed);
        // Half the changes in the JSON, which is most of what's read, a binary glTF's first chunk.
        const std::size_t json_at = seed.binary ? 20 : 0;
        const std::size_t json_length = seed.binary ? word_at(whole, 12) : whole.size();
        for (int round = 0; round < 120; ++round) {
            bytes_t changed = whole;
            const int changes = 1 + static_cast<int>(random() % 8);
            for (int i = 0; i < changes; ++i) {
                const std::size_t at = random() % 2 == 0 ? json_at + (random() % json_length)
                                                         : random() % changed.size();
                changed[at] = static_cast<std::uint8_t>(random());
            }
            folder.write(seed.file, changed);
            INFO(seed.file << " round " << round);
            exercise(seed.file);
        }
    }
}

TEST_CASE("a sample model's numbers made huge, negative, or past a float are refused or made",
          "[stability]") {
    const folder_t folder;
    std::mt19937 random = seeded(11);
    for (const seed_t& seed : seeds) {
        const bytes_t whole = seed_bytes(seed);
        const glb_t parts =
            seed.binary ? split(whole) : glb_t{ .json = std::string(whole.begin(), whole.end()) };
        for (int round = 0; round < 150; ++round) {
            std::string json = parts.json;
            const int changes = 1 + static_cast<int>(random() % 3);
            for (int i = 0; i < changes; ++i) {
                json = renumbered(json, random, numbers[random() % numbers.size()]);
            }
            folder.write(seed.file, as_file(seed, json, parts.bin));
            INFO(seed.file << " round " << round << ": " << json.substr(0, 400));
            exercise(seed.file);
        }
    }
}

TEST_CASE("a sample model whose buffer holds what isn't a number poses in numbers or is refused",
          "[stability]") {
    const folder_t folder;
    std::mt19937 random = seeded(13);
    constexpr std::array<std::uint32_t, 4> not_numbers{
        0x7FC00000U,
        0x7F800000U,
        0xFF800000U,
        0x7F7FFFFFU,
    };
    for (const seed_t& seed : seeds) {
        if (!seed.binary) {
            continue;
        }
        const glb_t parts = split(seed_bytes(seed));
        for (int round = 0; round < 80; ++round) {
            bytes_t bin = parts.bin;
            for (int i = 0; i < 16 && bin.size() >= 4; ++i) {
                const std::size_t at = (random() % (bin.size() / 4)) * 4;
                const std::uint32_t word = not_numbers[random() % not_numbers.size()];
                std::memcpy(bin.data() + at, &word, sizeof word);
            }
            folder.write(seed.file, as_file(seed, parts.json, bin));
            INFO(seed.file << " round " << round);
            exercise(seed.file);
        }
    }
}

TEST_CASE("a sample model whose nodes are their own children, or two nodes', is refused",
          "[stability]") {
    const folder_t folder;
    int changed = 0;
    for (const seed_t& seed : seeds) {
        const bytes_t whole = seed_bytes(seed);
        const glb_t parts =
            seed.binary ? split(whole) : glb_t{ .json = std::string(whole.begin(), whole.end()) };
        for (const auto& [inserted, which] : {
                 std::pair{ "\"children\":[0],", 0 },
                 std::pair{ "\"children\":[1],", 1 },
                 std::pair{ "\"children\":[0],", 1 },
                 std::pair{ "\"children\":[1],", 2 },
                 std::pair{ "\"children\":[99999],", 0 },
                 std::pair{ "\"children\":[-1],", 0 },
             }) {
            const std::string json = in_nodes(parts.json, inserted, which);
            if (json == parts.json) {
                continue; // It has no such node.
            }
            ++changed;
            folder.write(seed.file, as_file(seed, json, parts.bin));
            INFO(seed.file << " with " << inserted << " in node " << which);
            CHECK_FALSE(exercise(seed.file));
        }
    }
    CHECK(changed > 20);
}
