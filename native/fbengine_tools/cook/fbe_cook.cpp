// The cooker's C API: each call reads its file, cooks it in memory, and writes what it made.

#include "cook/cook.hpp"
#include "fbengine_tools.h"
#include "tools/tools.hpp"
#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
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

} // namespace

extern "C" {

FBE_TOOLS_API bool fbe_cook_image(const char* from, const char* to,
                                  const fbe_cook_image_t* options) {
    try {
        std::expected<tools::bytes_t, std::string> source = tools::read_file(from);
        if (!source) {
            return tools::fail(source.error());
        }
        std::expected<tools::bytes_t, std::string> cooked = tools::cook_image(*source, *options);
        if (!cooked) {
            return tools::fail(std::format("couldn't cook {}: {}", from, cooked.error()));
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
            return tools::fail(source.error().message);
        }
        std::expected<tools::bytes_t, std::string> cooked =
            tools::cook_model(*source, *path, view, *options);
        if (!cooked) {
            return tools::fail(std::format("couldn't cook {}: {}", from, cooked.error()));
        }
        if (std::expected<void, std::string> written = tools::write_file(to, *cooked); !written) {
            return tools::fail(written.error());
        }
        return true;
    } catch (const std::exception&) {
        return tools::fail(std::format("there isn't the memory to cook {}", from));
    }
}

} // extern "C"
