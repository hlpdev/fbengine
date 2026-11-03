// The cooker's C API: each call reads its file, cooks it in memory, and writes what it made.

#include "cook/cook.hpp"
#include "cook/pak.hpp"
#include "fbengine_tools.h"
#include "tools/tools.hpp"
#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

namespace tools = fbe::tools;
namespace vfs = fbe::vfs;

// `from`'s res:// path, with `root` as res://, or why it has none.
std::expected<vfs::path_t, std::string> res_path(const char* from, const char* root) {
    const std::filesystem::path relative =
        tools::disk_path(from).lexically_normal().lexically_relative(
            tools::disk_path(root).lexically_normal());
    std::string text = vfs::utf8(relative);
    std::ranges::replace(text, '\\', '/');
    if (relative.empty() || text.starts_with("..")) {
        return std::unexpected(std::format("{} isn't in {}", from, root));
    }
    vfs::result_t<vfs::path_t> path = vfs::parse("res://" + text);
    if (!path) {
        return std::unexpected(path.error().message);
    }
    return *path;
}

// The paks being written, by handle.
struct paks_t {
    std::mutex mutex;
    std::map<std::uint64_t, std::unique_ptr<tools::pak_writer_t>> open;
    std::uint64_t last = 0;
};

paks_t& paks() {
    static paks_t made;
    return made;
}

// The pak of `handle`, or null.
tools::pak_writer_t* pak_of(std::uint64_t handle) {
    paks_t& all = paks();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.open.find(handle);
    return found == all.open.end() ? nullptr : found->second.get();
}

bool no_pak(std::uint64_t handle) {
    return tools::fail(std::format("there's no pak {} being written", handle));
}

} // namespace

extern "C" {

FBE_TOOLS_API bool fbe_cook_image(const char* from, const char* to,
                                  const fbe_cook_image_t* options) {
    try {
        std::expected<tools::bytes_t, std::string> source = tools::read_file(from);
        if (!source) {
            return tools::fail(source.error());
        }
        std::expected<tools::bytes_t, tools::error_t> cooked = tools::cook_image(*source, *options);
        if (!cooked) {
            return tools::fail({
                .failure = cooked.error().failure,
                .message = std::format("couldn't cook {}: {}", from, cooked.error().message),
            });
        }
        if (std::expected<void, std::string> written = tools::write_file(to, *cooked); !written) {
            return tools::fail(written.error());
        }
        return true;
    } catch (const std::exception&) {
        return tools::fail(std::format("there isn't the memory to cook {}", from));
    }
}

FBE_TOOLS_API bool fbe_cook_model(const char* from, const char* to, const char* root,
                                  const fbe_cook_model_t* options) {
    try {
        const std::expected<vfs::path_t, std::string> path = res_path(from, root);
        if (!path) {
            return tools::fail(path.error());
        }
        vfs::vfs_t files;
        files.mount(*vfs::parse("res://"), vfs::folder(tools::disk_path(root)));
        const vfs::view_t view = files.view();
        vfs::result_t<vfs::bytes_t> source = view.read(*path);
        if (!source) {
            return tools::fail(tools::other(source.error().message));
        }
        std::expected<tools::bytes_t, tools::error_t> cooked =
            tools::cook_model(*source, *path, view, *options);
        if (!cooked) {
            // What it names that can't be read, as a buffer that isn't there, is OTHER's.
            return tools::fail({
                .failure = cooked.error().failure == vfs::failure_t::INVALID_DATA
                               ? vfs::failure_t::INVALID_DATA
                               : vfs::failure_t::OTHER,
                .message = std::format("couldn't cook {}: {}", from, cooked.error().message),
            });
        }
        if (std::expected<void, std::string> written = tools::write_file(to, *cooked); !written) {
            return tools::fail(written.error());
        }
        return true;
    } catch (const std::exception&) {
        return tools::fail(std::format("there isn't the memory to cook {}", from));
    }
}

FBE_TOOLS_API uint64_t fbe_cook_pak_open(const char* to) {
    try {
        std::expected<std::unique_ptr<tools::pak_writer_t>, tools::error_t> made =
            tools::pak_writer_t::open(to);
        if (!made) {
            tools::fail(made.error());
            return 0;
        }
        paks_t& all = paks();
        const std::scoped_lock lock(all.mutex);
        all.open.emplace(++all.last, std::move(*made));
        return all.last;
    } catch (const std::exception&) {
        tools::fail(std::format("there isn't the memory to write {}", to));
        return 0;
    }
}

FBE_TOOLS_API bool fbe_cook_pak_add(uint64_t pak, const char* name, const char* from) {
    tools::pak_writer_t* const writer = pak_of(pak);
    if (writer == nullptr) {
        return no_pak(pak);
    }
    try {
        if (std::expected<void, tools::error_t> added = writer->add(name, from); !added) {
            return tools::fail(added.error());
        }
        return true;
    } catch (const std::exception&) {
        return tools::fail(std::format("there isn't the memory to add {}", from));
    }
}

FBE_TOOLS_API bool fbe_cook_pak_finish(uint64_t pak) {
    tools::pak_writer_t* const writer = pak_of(pak);
    if (writer == nullptr) {
        return no_pak(pak);
    }
    bool finished = false;
    try {
        if (std::expected<void, tools::error_t> written = writer->finish(); !written) {
            tools::fail(written.error());
        } else {
            finished = true;
        }
    } catch (const std::exception&) {
        tools::fail("there isn't the memory to finish a pak");
    }
    fbe_cook_pak_free(pak);
    return finished;
}

FBE_TOOLS_API void fbe_cook_pak_free(uint64_t pak) {
    std::unique_ptr<tools::pak_writer_t> freed;
    paks_t& all = paks();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.open.find(pak);
    if (found != all.open.end()) {
        freed = std::move(found->second);
        all.open.erase(found);
    }
}

} // extern "C"
