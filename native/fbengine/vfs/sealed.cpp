// An encrypted pak's sealing, as vfs/sealed.hpp says it: Monocypher's XChaCha20 and Poly1305,
// a part at a time, as its crypto_aead_lock does them all at once, and its keyed BLAKE2b.

#include "vfs/sealed.hpp"

namespace fbe::vfs::sealed {

namespace {

// What each key a game's key makes is made of, and what starts what a nonce is of.
constexpr std::string_view seal_label = "fbengine pak seal";
constexpr std::string_view nonce_label = "fbengine pak nonce";
constexpr std::string_view id_label = "fbengine pak id";
constexpr std::uint8_t file_kind = 'f';
constexpr std::uint8_t index_kind = 'i';
constexpr std::size_t block = 64;
constexpr std::array<std::uint8_t, 16> zeros{};

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

template <std::size_t Size>
std::array<std::uint8_t, Size> keyed(const key_t& key, std::string_view label) {
    std::array<std::uint8_t, Size> made{};
    crypto_blake2b_keyed(made.data(), made.size(), key.data(), key.size(), bytes_of(label).data(),
                         label.size());
    return made;
}

// The zeros after `size` bytes that take them to a multiple of 16.
std::span<const std::uint8_t> padding(std::uint64_t size) {
    return std::span(zeros).first(static_cast<std::size_t>((16 - (size % 16)) % 16));
}

std::array<std::uint8_t, 8> little(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    return bytes;
}

} // namespace

keys_t derived(const key_t& key) {
    return {
        .seal = keyed<key_size>(key, seal_label),
        .nonce = keyed<key_size>(key, nonce_label),
        .id = keyed<id_size>(key, id_label),
    };
}

nonce_maker_t::nonce_maker_t(const keys_t& keys, std::string_view name) {
    crypto_blake2b_keyed_init(&m_hash, nonce_size, keys.nonce.data(), keys.nonce.size());
    const std::array<std::uint8_t, 1> kind = { file_kind };
    add(kind);
    const std::array<std::uint8_t, 8> size = little(name.size());
    add(std::span(size).first(4));
    add(bytes_of(name));
}

nonce_maker_t::nonce_maker_t(const keys_t& keys) {
    crypto_blake2b_keyed_init(&m_hash, nonce_size, keys.nonce.data(), keys.nonce.size());
    const std::array<std::uint8_t, 1> kind = { index_kind };
    add(kind);
}

nonce_maker_t::~nonce_maker_t() {
    crypto_wipe(&m_hash, sizeof(m_hash));
}

void nonce_maker_t::add(std::span<const std::uint8_t> bytes) {
    crypto_blake2b_update(&m_hash, bytes.data(), bytes.size());
}

nonce_t nonce_maker_t::made() {
    nonce_t nonce{};
    crypto_blake2b_final(&m_hash, nonce.data());
    return nonce;
}

sealer_t::sealer_t(const keys_t& keys, const nonce_t& nonce,
                   std::span<const std::uint8_t> authenticated)
    : m_key(keys.seal), m_nonce(nonce), m_authenticated(authenticated.size()) {
    // Poly1305's key is the first half of the stream's first block, as crypto_aead_lock's is.
    std::array<std::uint8_t, block> first{};
    crypto_chacha20_x(first.data(), nullptr, first.size(), m_key.data(), m_nonce.data(), 0);
    crypto_poly1305_init(&m_mac, first.data());
    crypto_wipe(first.data(), first.size());
    crypto_poly1305_update(&m_mac, authenticated.data(), authenticated.size());
    const std::span<const std::uint8_t> pad = padding(authenticated.size());
    crypto_poly1305_update(&m_mac, pad.data(), pad.size());
}

sealer_t::~sealer_t() {
    crypto_wipe(m_key.data(), m_key.size());
    crypto_wipe(&m_mac, sizeof(m_mac));
}

void sealer_t::add(std::span<std::uint8_t> bytes) {
    m_counter = crypto_chacha20_x(bytes.data(), bytes.data(), bytes.size(), m_key.data(),
                                  m_nonce.data(), m_counter);
    crypto_poly1305_update(&m_mac, bytes.data(), bytes.size());
    m_size += bytes.size();
}

tag_t sealer_t::tag() {
    const std::span<const std::uint8_t> pad = padding(m_size);
    crypto_poly1305_update(&m_mac, pad.data(), pad.size());
    const std::array<std::uint8_t, 8> authenticated = little(m_authenticated);
    const std::array<std::uint8_t, 8> size = little(m_size);
    crypto_poly1305_update(&m_mac, authenticated.data(), authenticated.size());
    crypto_poly1305_update(&m_mac, size.data(), size.size());
    tag_t tag{};
    crypto_poly1305_final(&m_mac, tag.data());
    return tag;
}

bool opened(const keys_t& keys, const nonce_t& nonce, const tag_t& tag,
            std::span<const std::uint8_t> authenticated, std::span<std::uint8_t> bytes) {
    return crypto_aead_unlock(bytes.data(), tag.data(), keys.seal.data(), nonce.data(),
                              authenticated.data(), authenticated.size(), bytes.data(),
                              bytes.size()) == 0;
}

} // namespace fbe::vfs::sealed
