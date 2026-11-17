#pragma once

// An encrypted pak's format, which vfs/pak.cpp reads and the cooker's pak writer writes. It isn't
// a zip: its header, then each file sealed, then its index sealed, the names and where each file
// is. To seal is XChaCha20-Poly1305's, with a file's name, or the header's, as what it
// authenticates, so a file read is the one written there, with the name it was written with. The
// game's key makes three keys of its own, by keyed BLAKE2b: one that seals, one that makes each
// nonce, and the id that tells a pak of another key from a broken one. A nonce is of the bytes it
// seals, so a file that doesn't change seals the same in every build, and a store's patch of the
// pak is only what changed. The key is in the game, so this stops someone opening the pak as a
// zip, not someone who takes the key from the executable.
//
// Its layout, little-endian:
//   header: "FBEPAK", 0, 1; the key's id, 16 bytes; the index's offset and size, 8 bytes each
//   each file: its nonce, 24 bytes; its bytes, encrypted; its tag, 16 bytes
//   the index, last, sealed as a file is: a count, 8 bytes, and for each file, its name's size,
//   4 bytes, its name, its offset in the pak, 8 bytes, and its size, 8 bytes

#include <array>
#include <cstddef>
#include <cstdint>
#include <monocypher.h>
#include <span>
#include <string_view>

namespace fbe::vfs::sealed {

constexpr std::size_t key_size = 32;
constexpr std::size_t id_size = 16;
constexpr std::size_t nonce_size = 24;
constexpr std::size_t tag_size = 16;
// What sealing adds to bytes.
constexpr std::size_t overhead = nonce_size + tag_size;
constexpr std::array<std::uint8_t, 8> magic = { 'F', 'B', 'E', 'P', 'A', 'K', 0, 1 };
constexpr std::size_t header_size = magic.size() + id_size + 8 + 8;
// The header's start, its magic and the key's id, which the index authenticates.
constexpr std::size_t header_start = magic.size() + id_size;
// An index entry's fixed part: its name's size, its offset, and its size.
constexpr std::size_t entry_size = 4 + 8 + 8;

using key_t = std::array<std::uint8_t, key_size>;
using nonce_t = std::array<std::uint8_t, nonce_size>;
using tag_t = std::array<std::uint8_t, tag_size>;
using id_t = std::array<std::uint8_t, id_size>;

/// The keys a game's key makes.
struct keys_t {
    key_t seal{};
    key_t nonce{};
    id_t id{};
};

keys_t derived(const key_t& key);

/// What a file's or the index's nonce is of, given a part at a time: a file's name, then its
/// bytes, or the index's bytes.
class nonce_maker_t {
public:
    /// One for the file named `name`.
    nonce_maker_t(const keys_t& keys, std::string_view name);
    /// One for the index.
    explicit nonce_maker_t(const keys_t& keys);
    nonce_maker_t(const nonce_maker_t&) = delete;
    nonce_maker_t& operator=(const nonce_maker_t&) = delete;
    nonce_maker_t(nonce_maker_t&&) = delete;
    nonce_maker_t& operator=(nonce_maker_t&&) = delete;
    ~nonce_maker_t();

    void add(std::span<const std::uint8_t> bytes);
    nonce_t made();

private:
    crypto_blake2b_ctx m_hash{};
};

/// Seals bytes given a part at a time, in place, each part but the last a multiple of 64 bytes,
/// as XChaCha20-Poly1305 does them all at once, with `authenticated` as what it authenticates.
class sealer_t {
public:
    sealer_t(const keys_t& keys, const nonce_t& nonce, std::span<const std::uint8_t> authenticated);
    sealer_t(const sealer_t&) = delete;
    sealer_t& operator=(const sealer_t&) = delete;
    sealer_t(sealer_t&&) = delete;
    sealer_t& operator=(sealer_t&&) = delete;
    ~sealer_t();

    void add(std::span<std::uint8_t> bytes);
    tag_t tag();

private:
    key_t m_key{};
    nonce_t m_nonce{};
    std::uint64_t m_counter = 1;
    std::uint64_t m_authenticated = 0;
    std::uint64_t m_size = 0;
    crypto_poly1305_ctx m_mac{};
};

/// Opens `bytes`, sealed with `nonce` and `tag`, in place; false, leaving them as they were, when
/// they aren't what was sealed with these keys and `authenticated`.
bool opened(const keys_t& keys, const nonce_t& nonce, const tag_t& tag,
            std::span<const std::uint8_t> authenticated, std::span<std::uint8_t> bytes);

} // namespace fbe::vfs::sealed
