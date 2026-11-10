// Shared textures: the textures made of images that are files of their own, by what they're of and
// how they're made.

#include "assets/shared.hpp"

#include <format>
#include <map>
#include <mutex>
#include <unordered_map>

namespace fbe::assets {

namespace {

struct shared_t {
    std::mutex mutex;
    /// Each texture by its file, whether it's decoded as a color, and how it's made, in that
    /// order, so one file's are together.
    std::map<std::string, std::uint64_t, std::less<>> textures;
    std::unordered_map<std::uint64_t, std::string> keys;
};

shared_t& shared() {
    static shared_t all;
    return all;
}

std::string decoded(std::string_view file, bool decoded_srgb) {
    return std::format("{}{}\n", file, decoded_srgb ? "color" : "linear");
}

} // namespace

std::string shared_file(std::string_view path, std::uint64_t hash) {
    return std::format("{}\n{:016x}\n", path, hash);
}

std::uint64_t shared_hash(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ULL ^ size;
    for (std::size_t at = 0; at < size; ++at) {
        hash = (hash ^ bytes[at]) * 0x100000001b3ULL;
    }
    return hash;
}

bool shared_known(std::string_view file, bool decoded_srgb) {
    const std::string prefix = decoded(file, decoded_srgb);
    shared_t& all = shared();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.textures.lower_bound(prefix);
    return found != all.textures.end() && found->first.starts_with(prefix);
}

std::uint64_t shared_find(std::string_view file, bool decoded_srgb, std::string_view made) {
    const std::string key = decoded(file, decoded_srgb).append(made);
    shared_t& all = shared();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.textures.find(key);
    return found != all.textures.end() ? found->second : 0;
}

void shared_add(std::string_view file, bool decoded_srgb, std::string_view made,
                std::uint64_t texture) {
    std::string key = decoded(file, decoded_srgb).append(made);
    shared_t& all = shared();
    const std::scoped_lock lock(all.mutex);
    if (const auto was = all.textures.find(key); was != all.textures.end()) {
        all.keys.erase(was->second);
    }
    all.textures[key] = texture;
    all.keys[texture] = std::move(key);
}

void shared_forget(std::uint64_t texture) {
    shared_t& all = shared();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.keys.find(texture);
    if (found == all.keys.end()) {
        return;
    }
    // Another may have its place by now.
    if (const auto held = all.textures.find(found->second);
        held != all.textures.end() && held->second == texture) {
        all.textures.erase(held);
    }
    all.keys.erase(found);
}

std::size_t shared_count() {
    shared_t& all = shared();
    const std::scoped_lock lock(all.mutex);
    return all.textures.size();
}

} // namespace fbe::assets
