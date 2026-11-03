// The engine's own font: a Latin subset of Noto Sans, in the DLL, so a game with none of its own
// looks the same on every machine, with no file to ship (tools/make_default_font.py makes it).

#include "core/engine.hpp"
#include "fbengine.h"
#include "text/font.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <vector>

namespace {

constexpr auto default_font = std::to_array<std::uint8_t>({
#include "text/default_font.inc"
});

/// Its face, kept for good once a font's asked for it, so a font made for one call, each frame,
/// doesn't read it again, or rasterize its glyphs again under a new id. FreeType's library outlives
/// it, as it's never closed.
std::shared_ptr<fbe::text::face_t> default_face;

} // namespace

extern "C" {

FBE_API uint64_t fbe_text_embedded_font(float size, int32_t style) {
    if (size <= 0.0F || !std::isfinite(size)) {
        fbe::fail(std::format("a font's size is over 0, not {}", size));
        return 0;
    }
    if (default_face == nullptr) {
        default_face = fbe::text::open_face(
            std::vector<std::uint8_t>(default_font.begin(), default_font.end()), 0,
            "the engine's font");
        if (default_face == nullptr) {
            return 0;
        }
    }
    return fbe::text::add_font(
        fbe::text::styled(default_face, size, static_cast<std::uint32_t>(style)));
}

} // extern "C"
