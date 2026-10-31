// The cooker's C API: each call reads its file, cooks it in memory, and writes what it made.

#include "cook/cook.hpp"
#include "fbengine_tools.h"
#include "tools/tools.hpp"

#include <format>
#include <new>

namespace {

namespace tools = fbe::tools;

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
    } catch (const std::bad_alloc&) {
        return tools::fail(std::format("there isn't the memory to cook {}", from));
    }
}

} // extern "C"
