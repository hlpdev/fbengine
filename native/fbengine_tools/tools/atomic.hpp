#pragma once

// A file written whole or not at all, as std's File.WriteBytesAtomic is: to a hidden file of its
// own beside it, then onto the disk and renamed over it.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace fbe::tools {

class atomic_file_t {
public:
    atomic_file_t() = default;
    atomic_file_t(const atomic_file_t&) = delete;
    atomic_file_t& operator=(const atomic_file_t&) = delete;
    atomic_file_t(atomic_file_t&&) = delete;
    atomic_file_t& operator=(atomic_file_t&&) = delete;
    /// Takes out what was written of one that isn't committed.
    ~atomic_file_t();

    /// Starts the file at `path`, a UTF-8 path on the disk, making its folder: the one a link
    /// there leads to, so the link stays, beside it under a name no other writer has. It gives why
    /// it can't, as for a folder there.
    std::expected<void, std::string> open(std::string_view path);

    /// Writes `bytes` at `offset`; false if it can't.
    [[nodiscard]] bool write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes) const;

    /// Puts it onto the disk and renames it over the file, which keeps the mode it had. On
    /// Windows, a file another handle holds open, sharing delete, as a mounted pak is, is
    /// replaced, and goes on reading as it was. It gives why it can't, leaving what was there.
    std::expected<void, std::string> commit();

private:
    void close();
    void discard();

    std::string m_shown;
    std::filesystem::path m_file;
    std::filesystem::path m_temporary;
#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_handle = -1;
#endif
};

} // namespace fbe::tools
