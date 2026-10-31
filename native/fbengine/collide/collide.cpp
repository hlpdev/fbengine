// Masks: an image's solid pixels, a bit each, in rows of 64-bit words, and whether placed masks
// meet, as the GPU draws them: their bounds first, then row by row, by whole words where neither
// is turned or scaled, and a pixel of theirs at a time over the span of the row each covers where
// one is; or, turned and scaled up, a pixel of theirs at a time against the other's.

#include "core/engine.hpp"
#include "fbengine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace {

/// Handles' kind for masks (architecture.md's handles).
constexpr std::uint8_t kind = 8;

/// The most pixels a mask is across or down.
constexpr std::int32_t largest = 65536;

/// An image's solid pixels: rows of 64-bit words, the first pixel bit 0 of a row's first word,
/// each row a word longer than its pixels need, so a run read past its end reads clear.
struct mask_t {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::size_t words = 1;
    std::vector<std::uint64_t> bits;
    std::int64_t solid = 0;

    /// The word `column` of row `y` is in, a row and a column in it.
    [[nodiscard]] const std::uint64_t* row(std::int32_t y) const {
        return bits.data() + (static_cast<std::size_t>(y) * words);
    }

    /// Whether the pixel at `x`, `y` is solid; false outside it.
    [[nodiscard]] bool test(std::int64_t x, std::int64_t y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return false;
        }
        const auto column = static_cast<std::uint64_t>(x);
        const std::uint64_t word = row(static_cast<std::int32_t>(y))[column >> 6U];
        return ((word >> (column & 63U)) & 1U) != 0;
    }

    /// `count` (1 to 64) of row `y`'s pixels from column `from`, the first bit 0, clear outside
    /// the mask.
    [[nodiscard]] std::uint64_t run(std::int64_t y, std::int64_t from, std::int64_t count) const {
        if (y < 0 || y >= height || from >= width || from + count <= 0) {
            return 0;
        }
        std::uint64_t value = 0;
        if (from >= 0) {
            const auto start = static_cast<std::uint64_t>(from);
            const std::uint64_t* words_of = row(static_cast<std::int32_t>(y));
            const std::uint64_t word = start >> 6U;
            const std::uint64_t shift = start & 63U;
            value = words_of[word] >> shift;
            if (shift != 0) {
                value |= words_of[word + 1] << (64U - shift);
            }
        } else {
            // What's before the row is clear, the rest the row's start.
            value = run(y, 0, count + from) << static_cast<std::uint64_t>(-from);
        }
        if (count < 64) {
            value &= (std::uint64_t{ 1 } << static_cast<std::uint64_t>(count)) - 1U;
        }
        return value;
    }
};

/// Masks by slot, null where a slot's free, each slot's generation, and the free slots.
std::vector<std::unique_ptr<mask_t>> masks;
std::vector<std::uint32_t> generations;
std::vector<std::uint32_t> free_slots;
constexpr std::uint32_t last_generation = (1U << 24U) - 1U;

std::uint64_t handle_of(std::uint32_t slot) {
    return (std::uint64_t{ kind } << 56U) | (std::uint64_t{ generations[slot] } << 32U) | slot;
}

/// The mask a handle is for, or null with fbe_error's why.
mask_t* mask_of(std::uint64_t handle) {
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32U) & 0xFFFFFFU;
    const auto tag = static_cast<std::uint8_t>(handle >> 56U);
    if (tag != kind || slot >= masks.size() || masks[slot] == nullptr ||
        generations[slot] != generation) {
        fbe::fail(handle == 0 ? "no mask was given" : "the mask was freed");
        return nullptr;
    }
    return masks[slot].get();
}

/// The farthest from the origin a screen pixel's row or column is taken, the canvas's reach.
constexpr double farthest = 16777216.0;

/// The farthest a corner's snapped to the GPU's 256ths of a pixel, where they're still whole in a
/// double; past it, a mask's placed as its corners are, unsnapped.
constexpr double snapped_reach = 2147483648.0;

/// The least a turned mask's pixels are across on the screen, and the most rows it covers, for it
/// to be gone through a pixel of its at a time rather than a row of the screen's.
constexpr double cell_size = 32.0;
constexpr std::int64_t many_rows = 64;

/// How a placed mask covers the screen.
enum class shape_t : std::uint8_t {
    /// Not at all: it has no pixels, or it's scaled to nothing.
    NONE,
    /// Neither turned nor scaled: a screen pixel is its pixel moved by (dx, dy).
    PLAIN,
    /// By one map from a screen pixel's middle into its pixels: where its snapped corners are a
    /// rectangle on the screen's axes, turned by quarters, mirrored, or scaled, exactly; or where
    /// they're too far to snap.
    MAPPED,
    /// Turned otherwise, as the GPU draws its quad: two triangles, each its own map.
    SLANTED,
};

/// One of the two triangles, of corners 0 1 2 and 0 2 3, that the GPU draws a sprite's quad as, in
/// 256ths of a pixel: its edges, each from a corner along a side, a middle inside where each's
/// value, along x (its y - from y) - along y (its x - from x), is positive, or 0 on a left or top
/// edge; each value the weight, out of their sum, twice its area, of the corner across from it,
/// and whether that corner's at the mask's right and at its bottom; and how far u and v go a
/// screen pixel across and down.
struct triangle_t {
    bool drawn = false;
    std::array<double, 3> from_x{};
    std::array<double, 3> from_y{};
    std::array<double, 3> along_x{};
    std::array<double, 3> along_y{};
    std::array<bool, 3> top_left{};
    std::array<bool, 3> right{};
    std::array<bool, 3> bottom{};
    double total = 0.0;
    double across_u = 0.0;
    double across_v = 0.0;
    double down_u = 0.0;
    double down_v = 0.0;
};

/// A mask placed: how it covers the screen, the screen pixels' rows and columns it may cover,
/// [left, right) by [top, bottom), and how a screen pixel's middle maps into it, by one arithmetic
/// every path takes, so they agree with each other, and with the GPU's: a sprite's corners snapped
/// to 256ths of a pixel, its two triangles, each pixel whose middle's inside one, or exactly on a
/// left or top edge of it, and its pixel there, NEAREST.
struct placed_t {
    const mask_t* mask = nullptr;
    shape_t shape = shape_t::NONE;
    /// How many screen pixels across its pixels are, the narrower way.
    double texel = 1.0;
    std::int64_t dx = 0;
    std::int64_t dy = 0;
    /// MAPPED: u = ((x - anchor x) u_x + (y - anchor y) u_y) / u_over + u_at, and v the same way,
    /// (x, y) a pixel's middle in 256ths of a pixel.
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double u_x = 0.0;
    double u_y = 0.0;
    double u_over = 1.0;
    double u_at = 0.0;
    double v_x = 0.0;
    double v_y = 0.0;
    double v_over = 1.0;
    double v_at = 0.0;
    /// How far u and v go a pixel across and a pixel down, to find where they reach whole numbers.
    double across_u = 0.0;
    double across_v = 0.0;
    double down_u = 0.0;
    double down_v = 0.0;
    /// Whether a middle exactly on the edges u = 0, u = width, v = 0, and v = height is inside.
    bool u_low = true;
    bool u_high = false;
    bool v_low = true;
    bool v_high = false;
    std::array<triangle_t, 2> triangles{};
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

/// `value` kept to the canvas's reach, as a screen pixel's row or column.
std::int64_t pixel_of(double value) {
    return static_cast<std::int64_t>(std::clamp(value, -farthest, farthest));
}

/// A screen pixel's middle, of its column or row, in 256ths of a pixel.
double middle(std::int64_t at) {
    return (static_cast<double>(at) * 256.0) + 128.0;
}

/// Whether every field of `place` is finite, or false with fbe_error's why.
bool finite(const fbe_collide_place_t* place) {
    if (place == nullptr) {
        return fbe::fail("no place was given");
    }
    const bool all = std::isfinite(place->x) && std::isfinite(place->y) &&
                     std::isfinite(place->handle_x) && std::isfinite(place->handle_y) &&
                     std::isfinite(place->rotation) && std::isfinite(place->scale_x) &&
                     std::isfinite(place->scale_y);
    return all ? true : fbe::fail("a place's position, handle, rotation, and scale are finite");
}

/// The corners of `mask` placed as `place`, in 256ths of a pixel, x then y of each, as the canvas
/// draws a sprite's: from its handle, scaled, turned clockwise, and moved, by the canvas's own
/// arithmetic, to the float, then snapped to the 256th, as the GPU's rasterizer takes them; true
/// if they were. Where one's past `snapped_reach`, none is, and they're as they are.
bool corners(const mask_t& mask, const fbe_collide_place_t& place, std::array<double, 8>& found) {
    // Unturned, its cosine and sine are 1 and 0 exactly, without asking.
    const auto turn = static_cast<double>(place.rotation);
    const double cosine = turn == 0.0 ? 1.0 : std::cos(turn);
    const double sine = turn == 0.0 ? 0.0 : std::sin(turn);
    const auto across = static_cast<float>(mask.width);
    const auto down = static_cast<float>(mask.height);
    const std::array<std::array<float, 2>, 4> part{
        {
            { 0.0F, 0.0F },
            { across, 0.0F },
            { across, down },
            { 0.0F, down },
        },
    };
    std::array<double, 8> exact{};
    bool near = true;
    for (std::size_t at = 0; at < part.size(); ++at) {
        // The handle's taken off as a float, as the canvas does.
        const double from_x = (part[at][0] - place.handle_x) * static_cast<double>(place.scale_x);
        const double from_y = (part[at][1] - place.handle_y) * static_cast<double>(place.scale_y);
        exact[at * 2] = static_cast<double>(place.x) + (from_x * cosine) - (from_y * sine);
        exact[(at * 2) + 1] = static_cast<double>(place.y) + (from_x * sine) + (from_y * cosine);
        near = near && std::abs(exact[at * 2]) < snapped_reach &&
               std::abs(exact[(at * 2) + 1]) < snapped_reach;
    }
    for (std::size_t at = 0; at < exact.size(); ++at) {
        found[at] = near ? std::round(static_cast<double>(static_cast<float>(exact[at])) * 256.0)
                         : exact[at] * 256.0;
    }
    return near;
}

/// Whether an edge whose inward normal on the screen is (`across`, `down`) is a left or a top one,
/// whose middles are inside.
bool top_left(double across, double down) {
    return across > 0.0 || (across == 0.0 && down > 0.0);
}

/// The GPU's triangle of `corners`' `first`, `second`, and `third`, for a mask `width` by
/// `height`.
triangle_t triangle_of(const std::array<double, 8>& corners, std::size_t first, std::size_t second,
                       std::size_t third, double width, double height) {
    // Corners 1 and 2 are at its right, 2 and 3 at its bottom.
    constexpr std::array<bool, 4> at_right{ false, true, true, false };
    constexpr std::array<bool, 4> at_bottom{ false, false, true, true };
    const auto x = [&](std::size_t corner) { return corners[corner * 2]; };
    const auto y = [&](std::size_t corner) { return corners[(corner * 2) + 1]; };
    triangle_t made;
    std::array<std::size_t, 3> each{ first, second, third };
    const double area = ((x(second) - x(first)) * (y(third) - y(first))) -
                        ((y(second) - y(first)) * (x(third) - x(first)));
    if (area == 0.0 || !std::isfinite(area)) {
        return made;
    }
    // Wound so its area's positive, as a rasterizer that culls neither way takes it.
    if (area < 0.0) {
        std::swap(each[1], each[2]);
    }
    made.drawn = true;
    made.total = std::abs(area);
    double across_right = 0.0;
    double down_right = 0.0;
    double across_bottom = 0.0;
    double down_bottom = 0.0;
    for (std::size_t edge = 0; edge < 3; ++edge) {
        const std::size_t from = each[(edge + 1) % 3];
        const std::size_t to = each[(edge + 2) % 3];
        made.from_x[edge] = x(from);
        made.from_y[edge] = y(from);
        made.along_x[edge] = x(to) - x(from);
        made.along_y[edge] = y(to) - y(from);
        // Its inward normal is (-along y, along x): a left edge's points right, a top edge's down.
        made.top_left[edge] = top_left(-made.along_y[edge], made.along_x[edge]);
        made.right[edge] = at_right[each[edge]];
        made.bottom[edge] = at_bottom[each[edge]];
        if (made.right[edge]) {
            across_right -= 256.0 * made.along_y[edge];
            down_right += 256.0 * made.along_x[edge];
        }
        if (made.bottom[edge]) {
            across_bottom -= 256.0 * made.along_y[edge];
            down_bottom += 256.0 * made.along_x[edge];
        }
    }
    made.across_u = across_right * width / made.total;
    made.down_u = down_right * width / made.total;
    made.across_v = across_bottom * height / made.total;
    made.down_v = down_bottom * height / made.total;
    return made;
}

/// Edge `edge` of `triangle`'s value at (`x`, `y`), in 256ths of a pixel.
double value(const triangle_t& triangle, std::size_t edge, double x, double y) {
    return (triangle.along_x[edge] * (y - triangle.from_y[edge])) -
           (triangle.along_y[edge] * (x - triangle.from_x[edge]));
}

/// Where (`x`, `y`), in 256ths of a pixel, is in a mask's pixels by `triangle`'s map, `u` across
/// and `v` down, as its corners' weights there give it; and whether it's inside the triangle.
bool inside(const triangle_t& triangle, const mask_t& mask, double x, double y, double& u,
            double& v) {
    bool in = true;
    double right = 0.0;
    double bottom = 0.0;
    for (std::size_t edge = 0; edge < 3; ++edge) {
        const double at = value(triangle, edge, x, y);
        in = in && (at > 0.0 || (at == 0.0 && triangle.top_left[edge]));
        right += triangle.right[edge] ? at : 0.0;
        bottom += triangle.bottom[edge] ? at : 0.0;
    }
    u = (right * static_cast<double>(mask.width)) / triangle.total;
    v = (bottom * static_cast<double>(mask.height)) / triangle.total;
    return in;
}

/// Whether a mask `count` pixels across or down, drawn `spanned` 256ths from `corner`, covers the
/// screen's pixels as it would drawn `count` whole pixels from there, a pixel each: a float's
/// corners can snap a 256th or so more or less apart than its size, but a pixel's middle lands
/// in the same pixel of it either way unless it's that close to its corner's 256th, mod 256.
bool nearly_plain(double corner, double spanned, double count) {
    const double off = spanned - (256.0 * count);
    if (off == 0.0) {
        return true;
    }
    if (spanned <= 0.0) {
        return false;
    }
    // Where the middles are from the corner, mod 256: each pixel's that far into one of its.
    const double into = std::fmod(std::fmod(128.0 - corner, 256.0) + 256.0, 256.0);
    return std::abs(off) < into && into < 256.0 - std::abs(off);
}

/// `mask` placed as `place`.
placed_t placed(const mask_t& mask, const fbe_collide_place_t& place) {
    placed_t made;
    made.mask = &mask;
    if (mask.width == 0 || mask.height == 0 || place.scale_x == 0.0F || place.scale_y == 0.0F) {
        return made;
    }
    std::array<double, 8> at{};
    const bool snapped = corners(mask, place, at);
    const auto width = static_cast<double>(mask.width);
    const auto height = static_cast<double>(mask.height);
    made.texel = std::min(std::abs(static_cast<double>(place.scale_x)),
                          std::abs(static_cast<double>(place.scale_y)));
    // The pixels whose middles are inside its corners' bounds, a pixel more each way for what's
    // rounded, kept to the canvas's reach.
    double least_x = at[0];
    double most_x = at[0];
    double least_y = at[1];
    double most_y = at[1];
    for (std::size_t corner = 1; corner < 4; ++corner) {
        least_x = std::min(least_x, at[corner * 2]);
        most_x = std::max(most_x, at[corner * 2]);
        least_y = std::min(least_y, at[(corner * 2) + 1]);
        most_y = std::max(most_y, at[(corner * 2) + 1]);
    }
    made.left = pixel_of(std::ceil((least_x - 128.0) / 256.0) - 1.0);
    made.right = pixel_of(std::floor((most_x - 128.0) / 256.0) + 2.0);
    made.top = pixel_of(std::ceil((least_y - 128.0) / 256.0) - 1.0);
    made.bottom = pixel_of(std::floor((most_y - 128.0) / 256.0) + 2.0);
    // On the screen's axes, from corner 0 to 1 across and to 3 down, or, turned a quarter, down and
    // across.
    const bool along = at[1] == at[3] && at[2] == at[4] && at[5] == at[7] && at[6] == at[0];
    const bool standing = at[0] == at[2] && at[3] == at[5] && at[4] == at[6] && at[7] == at[1];
    if (snapped && !along && !standing) {
        made.triangles = {
            triangle_of(at, 0, 1, 2, width, height),
            triangle_of(at, 0, 2, 3, width, height),
        };
        if (made.triangles[0].drawn || made.triangles[1].drawn) {
            made.shape = shape_t::SLANTED;
        }
        return made;
    }
    if (snapped) {
        const double u_size = along ? at[2] - at[0] : at[3] - at[1];
        const double v_size = along ? at[7] - at[1] : at[6] - at[0];
        if (u_size == 0.0 || v_size == 0.0) {
            return made;
        }
        if (along && nearly_plain(at[0], u_size, width) && nearly_plain(at[1], v_size, height)) {
            // A screen pixel's middle lands in the mask's pixel it's drawn from, floored.
            made.shape = shape_t::PLAIN;
            made.dx = pixel_of(-std::floor((128.0 - at[0]) / 256.0));
            made.dy = pixel_of(-std::floor((128.0 - at[1]) / 256.0));
            made.left = std::max(made.dx, pixel_of(-farthest));
            made.right = std::min(made.dx + mask.width, pixel_of(farthest));
            made.top = std::max(made.dy, pixel_of(-farthest));
            made.bottom = std::min(made.dy + mask.height, pixel_of(farthest));
            return made;
        }
        // Exactly: a whole number of 256ths from corner 0, times its size, over a whole number.
        made.shape = shape_t::MAPPED;
        made.anchor_x = at[0];
        made.anchor_y = at[1];
        made.u_x = along ? width : 0.0;
        made.u_y = along ? 0.0 : width;
        made.u_over = u_size;
        made.v_x = along ? 0.0 : height;
        made.v_y = along ? height : 0.0;
        made.v_over = v_size;
    } else {
        // Too far to snap: from its position, turned back, scaled back, from its handle.
        const auto turn = static_cast<double>(place.rotation);
        made.shape = shape_t::MAPPED;
        made.anchor_x = static_cast<double>(place.x) * 256.0;
        made.anchor_y = static_cast<double>(place.y) * 256.0;
        made.u_x = std::cos(turn);
        made.u_y = std::sin(turn);
        made.u_over = static_cast<double>(place.scale_x) * 256.0;
        made.u_at = place.handle_x;
        made.v_x = -std::sin(turn);
        made.v_y = std::cos(turn);
        made.v_over = static_cast<double>(place.scale_y) * 256.0;
        made.v_at = place.handle_y;
    }
    made.across_u = 256.0 * made.u_x / made.u_over;
    made.down_u = 256.0 * made.u_y / made.u_over;
    made.across_v = 256.0 * made.v_x / made.v_over;
    made.down_v = 256.0 * made.v_y / made.v_over;
    made.u_low = top_left(made.across_u, made.down_u);
    made.u_high = top_left(-made.across_u, -made.down_u);
    made.v_low = top_left(made.across_v, made.down_v);
    made.v_high = top_left(-made.across_v, -made.down_v);
    return made;
}

/// Where the middle of the screen pixel `x`, `y` is in a MAPPED mask's pixels.
void mapped(const placed_t& place, std::int64_t x, std::int64_t y, double& u, double& v) {
    const double from_x = middle(x) - place.anchor_x;
    const double from_y = middle(y) - place.anchor_y;
    u = (((from_x * place.u_x) + (from_y * place.u_y)) / place.u_over) + place.u_at;
    v = (((from_x * place.v_x) + (from_y * place.v_y)) / place.v_over) + place.v_at;
}

/// The column or row `value` is in of `size`, or -1 outside, its edges by `low` and `high`.
std::int64_t index_of(double value, std::int32_t size, bool low, bool high) {
    const auto whole = static_cast<double>(size);
    const bool inside =
        (value > 0.0 || (value == 0.0 && low)) && (value < whole || (value == whole && high));
    if (!inside) {
        return -1;
    }
    return value == whole ? size - 1 : static_cast<std::int64_t>(value);
}

/// `value`, a column or row of `size` a triangle's map gives, clamped to it, as the GPU's sampler
/// does.
std::int64_t clamped(double value, std::int32_t size) {
    return static_cast<std::int64_t>(std::clamp(value, 0.0, static_cast<double>(size - 1)));
}

/// Whether the screen pixel at `x`, `y` is one of a placed mask's solid ones.
bool solid_at(const placed_t& place, std::int64_t x, std::int64_t y) {
    switch (place.shape) {
    case shape_t::NONE:
        return false;
    case shape_t::PLAIN:
        return place.mask->test(x - place.dx, y - place.dy);
    case shape_t::MAPPED: {
        double u = 0.0;
        double v = 0.0;
        mapped(place, x, y, u, v);
        const std::int64_t column = index_of(u, place.mask->width, place.u_low, place.u_high);
        const std::int64_t row = index_of(v, place.mask->height, place.v_low, place.v_high);
        return column >= 0 && row >= 0 && place.mask->test(column, row);
    }
    case shape_t::SLANTED:
        // The first triangle that has it, as the GPU's two draw in turn.
        for (const triangle_t& triangle : place.triangles) {
            double u = 0.0;
            double v = 0.0;
            if (triangle.drawn && inside(triangle, *place.mask, middle(x), middle(y), u, v)) {
                return place.mask->test(clamped(u, place.mask->width),
                                        clamped(v, place.mask->height));
            }
        }
        return false;
    }
    return false;
}

/// The next pixel past `at` where `value`, going `step` a pixel and `ahead` at the next pixel, may
/// reach a whole number, a pixel early so rounding never passes one, or `to` if it doesn't before;
/// on one already, the next pixel, unless it stays there, as a value too big for its step does,
/// when it's where it'd reach the next one.
std::int64_t reaching(double value, double step, double ahead, std::int64_t at, std::int64_t to) {
    if (step == 0.0 || !std::isfinite(value)) {
        return to;
    }
    const double whole = std::floor(value);
    double target = step > 0.0 ? whole + 1.0 : whole;
    if (value == whole) {
        if (ahead != value) {
            return at + 1;
        }
        target = step > 0.0 ? whole + 1.0 : whole - 1.0;
    }
    const double early = std::ceil(static_cast<double>(at) + ((target - value) / step)) - 1.0;
    if (!(early < static_cast<double>(to))) {
        return to;
    }
    return std::max(at + 1, static_cast<std::int64_t>(early));
}

/// The next pixel past `at` where `value`, going `step` a pixel, may change its sign, a pixel early
/// so rounding never passes it, or `to` if it doesn't before; on 0, the next pixel.
std::int64_t crossing(double value, double step, std::int64_t at, std::int64_t to) {
    if (value == 0.0) {
        return at + 1;
    }
    if (step == 0.0 || (value > 0.0) == (step > 0.0)) {
        return to;
    }
    const double early = std::ceil(static_cast<double>(at) - (value / step)) - 1.0;
    if (!(early < static_cast<double>(to))) {
        return to;
    }
    return std::max(at + 1, static_cast<std::int64_t>(early));
}

/// Where along a row `start` + x `step` is from 0 to `size`, narrowing [from, to), a pixel past
/// each end for what's rounded; false where it never is. A `step` of 0 narrows nothing.
bool within(double start, double step, double size, std::int64_t& from, std::int64_t& to) {
    if (step == 0.0 || !std::isfinite(step)) {
        return true;
    }
    double low = -start / step;
    double high = (size - start) / step;
    if (step < 0.0) {
        std::swap(low, high);
    }
    from = std::max(from, pixel_of(std::floor(low)) - 1);
    to = std::min(to, pixel_of(std::ceil(high)) + 1);
    return from < to;
}

/// The columns of row `y` a placed mask may have solid, narrowed from [from, to); false for none.
bool span(const placed_t& place, std::int64_t y, std::int64_t& from, std::int64_t& to) {
    if (place.shape == shape_t::NONE || y < place.top || y >= place.bottom) {
        return false;
    }
    from = std::max(from, place.left);
    to = std::min(to, place.right);
    if (from >= to || place.shape == shape_t::PLAIN) {
        return from < to;
    }
    if (place.shape == shape_t::MAPPED) {
        double u = 0.0;
        double v = 0.0;
        mapped(place, 0, y, u, v);
        return within(u, place.across_u, place.mask->width, from, to) &&
               within(v, place.across_v, place.mask->height, from, to);
    }
    // Where it's inside either triangle, a pixel more each way: each edge 0 or more.
    std::int64_t least = to;
    std::int64_t most = from;
    for (const triangle_t& triangle : place.triangles) {
        std::int64_t low = from;
        std::int64_t high = to;
        bool any = triangle.drawn;
        for (std::size_t edge = 0; edge < 3 && any; ++edge) {
            const double start = value(triangle, edge, middle(0), middle(y));
            const double step = -256.0 * triangle.along_y[edge];
            if (step == 0.0) {
                any = start >= 0.0;
            } else if (step > 0.0) {
                low = std::max(low, pixel_of(std::floor(-start / step)) - 1);
            } else {
                high = std::min(high, pixel_of(std::ceil(-start / step)) + 1);
            }
        }
        if (any && low < high) {
            least = std::min(least, low);
            most = std::max(most, high);
        }
    }
    from = least;
    to = most;
    return from < to;
}

/// The next pixel past `at` of row `y` where a placed mask's pixel may change, a pixel early, or
/// `to` if it doesn't before: where u or v reach a whole number, and for a SLANTED one, where it
/// goes into or out of a triangle that counts: the first it's in, and those before it.
std::int64_t changes(const placed_t& place, std::int64_t at, std::int64_t y, std::int64_t to) {
    double u = 0.0;
    double v = 0.0;
    double next_u = 0.0;
    double next_v = 0.0;
    if (place.shape == shape_t::MAPPED) {
        mapped(place, at, y, u, v);
        mapped(place, at + 1, y, next_u, next_v);
        return std::min(reaching(u, place.across_u, next_u, at, to),
                        reaching(v, place.across_v, next_v, at, to));
    }
    std::int64_t next = to;
    for (const triangle_t& triangle : place.triangles) {
        if (!triangle.drawn) {
            continue;
        }
        bool in = true;
        double right = 0.0;
        double bottom = 0.0;
        double right_step = 0.0;
        double bottom_step = 0.0;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const double now = value(triangle, edge, middle(at), middle(y));
            const double step = -256.0 * triangle.along_y[edge];
            in = in && (now > 0.0 || (now == 0.0 && triangle.top_left[edge]));
            next = std::min(next, crossing(now, step, at, to));
            right += triangle.right[edge] ? now : 0.0;
            right_step += triangle.right[edge] ? step : 0.0;
            bottom += triangle.bottom[edge] ? now : 0.0;
            bottom_step += triangle.bottom[edge] ? step : 0.0;
        }
        if (in) {
            const auto width = static_cast<double>(place.mask->width);
            const auto height = static_cast<double>(place.mask->height);
            u = (right * width) / triangle.total;
            v = (bottom * height) / triangle.total;
            next_u = ((right + right_step) * width) / triangle.total;
            next_v = ((bottom + bottom_step) * height) / triangle.total;
            return std::min({
                next,
                reaching(u, triangle.across_u, next_u, at, to),
                reaching(v, triangle.across_v, next_v, at, to),
            });
        }
    }
    return next;
}

/// Calls `visit(from, to, solid)` for each run of row `y`'s pixels from `from` up to `to` in one
/// of a placed mask's pixels, or outside it, stepping a pixel of the mask at a time, not a pixel of
/// the screen, so a scaled-up mask costs no more than its own pixels; true as soon as `visit` is.
template <typename Visit>
bool walk(const placed_t& place, std::int64_t y, std::int64_t from, std::int64_t to,
          const Visit& visit) {
    for (std::int64_t x = from; x < to;) {
        const std::int64_t next = changes(place, x, y, to);
        if (visit(x, next, solid_at(place, x, y))) {
            return true;
        }
        x = next;
    }
    return false;
}

/// The next row past `y` whose runs may differ from `y`'s: the next for a plain or slanted mask,
/// and for one on the screen's axes, the next in another of its rows or columns.
std::int64_t next_row(const placed_t& place, std::int64_t y, std::int64_t bottom) {
    if (place.shape != shape_t::MAPPED) {
        return y + 1;
    }
    double u = 0.0;
    double v = 0.0;
    double next_u = 0.0;
    double next_v = 0.0;
    mapped(place, 0, y, u, v);
    mapped(place, 0, y + 1, next_u, next_v);
    if (place.across_u == 0.0 && place.down_v == 0.0) {
        return reaching(u, place.down_u, next_u, y, bottom);
    }
    if (place.across_v == 0.0 && place.down_u == 0.0) {
        return reaching(v, place.down_v, next_v, y, bottom);
    }
    return y + 1;
}

/// Whether any of a plain mask's pixels from `from` up to `to` of row `y` is solid, a word of its
/// row at a time, and `other`'s too, if it's plain as well.
bool any_bits(const placed_t& place, const placed_t* other, std::int64_t y, std::int64_t from,
              std::int64_t to) {
    for (std::int64_t x = from; x < to; x += 64) {
        const std::int64_t count = std::min<std::int64_t>(64, to - x);
        std::uint64_t bits = place.mask->run(y - place.dy, x - place.dx, count);
        if (other != nullptr && bits != 0) {
            bits &= other->mask->run(y - other->dy, x - other->dx, count);
        }
        if (bits != 0) {
            return true;
        }
    }
    return false;
}

/// Whether any screen pixel of row `y` from `from` up to `to` is solid in `one`, and in `other` as
/// well, if it isn't null.
bool row_hit(const placed_t& one, const placed_t* other, std::int64_t y, std::int64_t from,
             std::int64_t to) {
    const bool plain = one.shape == shape_t::PLAIN;
    if (plain && (other == nullptr || other->shape == shape_t::PLAIN)) {
        return any_bits(one, other, y, from, to);
    }
    if (plain) {
        return row_hit(*other, &one, y, from, to);
    }
    return walk(one, y, from, to, [&](std::int64_t start, std::int64_t end, bool solid) {
        if (!solid) {
            return false;
        }
        if (other == nullptr) {
            return true;
        }
        if (other->shape == shape_t::PLAIN) {
            return any_bits(*other, nullptr, y, start, end);
        }
        return walk(*other, y, start, end,
                    [](std::int64_t, std::int64_t, bool also) { return also; });
    });
}

/// Whether a MAPPED mask is on the screen's axes, u along one and v along the other, so its rows
/// are the same down each of its pixels.
bool on_axes(const placed_t& place) {
    return (place.across_u == 0.0 && place.down_v == 0.0) ||
           (place.across_v == 0.0 && place.down_u == 0.0);
}

/// Whether a placed mask is turned other than by quarters, so each screen row of it differs from
/// the last.
bool slanted(const placed_t& place) {
    return place.shape == shape_t::SLANTED || (place.shape == shape_t::MAPPED && !on_axes(place));
}

/// A function of where a screen pixel is, from a pixel `ref`: value + across (x - ref's x) +
/// down (y - ref's y).
struct linear_t {
    double value = 0.0;
    double across = 0.0;
    double down = 0.0;
};

/// A convex polygon of screen pixels' middles, from a pixel `ref`, each corner x then y.
using polygon_t = std::vector<std::array<double, 2>>;

/// One of a placed mask's maps from the screen, from a pixel `ref`: where in its pixels a screen
/// pixel's middle is, and the sides of the screen it maps inside, a triangle's for a SLANTED mask,
/// none for a MAPPED one.
struct map_t {
    linear_t u;
    linear_t v;
    std::vector<linear_t> sides;
};

/// A placed mask's maps from the screen, from the pixel `ref_x`, `ref_y`.
std::vector<map_t> maps_of(const placed_t& place, std::int64_t ref_x, std::int64_t ref_y) {
    std::vector<map_t> maps;
    if (place.shape == shape_t::MAPPED) {
        map_t map;
        mapped(place, ref_x, ref_y, map.u.value, map.v.value);
        map.u.across = place.across_u;
        map.u.down = place.down_u;
        map.v.across = place.across_v;
        map.v.down = place.down_v;
        maps.push_back(map);
        return maps;
    }
    for (const triangle_t& triangle : place.triangles) {
        if (!triangle.drawn) {
            continue;
        }
        map_t map;
        inside(triangle, *place.mask, middle(ref_x), middle(ref_y), map.u.value, map.v.value);
        map.u.across = triangle.across_u;
        map.u.down = triangle.down_u;
        map.v.across = triangle.across_v;
        map.v.down = triangle.down_v;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            map.sides.push_back({
                .value = value(triangle, edge, middle(ref_x), middle(ref_y)),
                .across = -256.0 * triangle.along_y[edge],
                .down = 256.0 * triangle.along_x[edge],
            });
        }
        maps.push_back(map);
    }
    return maps;
}

/// `function` at `corner`.
double at_corner(const linear_t& function, const std::array<double, 2>& corner) {
    return function.value + (function.across * corner[0]) + (function.down * corner[1]);
}

/// `polygon` cut to where `side` is 0 or more, or a millionth of a pixel short of it, for what's
/// rounded.
polygon_t cut(const polygon_t& polygon, const linear_t& side) {
    const double slack = 1e-6 * (std::abs(side.across) + std::abs(side.down));
    polygon_t kept;
    for (std::size_t corner = 0; corner < polygon.size(); ++corner) {
        const std::array<double, 2>& from = polygon[corner];
        const std::array<double, 2>& to = polygon[(corner + 1) % polygon.size()];
        const double was = at_corner(side, from) + slack;
        const double is = at_corner(side, to) + slack;
        if (was >= 0.0) {
            kept.push_back(from);
        }
        if ((was >= 0.0) != (is >= 0.0)) {
            const double part = was / (was - is);
            kept.push_back(
                { from[0] + ((to[0] - from[0]) * part), from[1] + ((to[1] - from[1]) * part) });
        }
    }
    return kept;
}

/// The part of `polygon` that `map` draws its mask's pixel `column`, `row` on, inside its sides.
polygon_t part_of(polygon_t polygon, const map_t& map, std::int64_t column, std::int64_t row) {
    const auto across = static_cast<double>(column);
    const auto down = static_cast<double>(row);
    std::vector<linear_t> sides = map.sides;
    sides.push_back({ .value = map.u.value - across, .across = map.u.across, .down = map.u.down });
    sides.push_back(
        { .value = across + 1.0 - map.u.value, .across = -map.u.across, .down = -map.u.down });
    sides.push_back({ .value = map.v.value - down, .across = map.v.across, .down = map.v.down });
    sides.push_back(
        { .value = down + 1.0 - map.v.value, .across = -map.v.across, .down = -map.v.down });
    for (const linear_t& side : sides) {
        if (polygon.empty()) {
            break;
        }
        polygon = cut(polygon, side);
    }
    return polygon;
}

/// The columns and rows of a placed mask's pixels that its `maps` may draw screen pixels in
/// `polygon` from, a pixel more each way, kept to its own, [0] to [2] by [1] to [3]; false for
/// none.
bool cells_in(const placed_t& place, const std::vector<map_t>& maps, const polygon_t& polygon,
              std::array<std::int64_t, 4>& cells) {
    std::array<double, 4> reach{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (const map_t& map : maps) {
        for (const std::array<double, 2>& corner : polygon) {
            const double u = at_corner(map.u, corner);
            const double v = at_corner(map.v, corner);
            reach = {
                std::min(reach[0], u),
                std::min(reach[1], v),
                std::max(reach[2], u),
                std::max(reach[3], v),
            };
        }
    }
    const auto width = static_cast<double>(place.mask->width);
    const auto height = static_cast<double>(place.mask->height);
    if (!(reach[0] <= reach[2]) || reach[2] < -1.0 || reach[0] > width + 1.0 || reach[3] < -1.0 ||
        reach[1] > height + 1.0) {
        return false;
    }
    cells = {
        clamped(std::floor(reach[0]) - 1.0, place.mask->width),
        clamped(std::floor(reach[1]) - 1.0, place.mask->height),
        clamped(std::floor(reach[2]) + 1.0, place.mask->width),
        clamped(std::floor(reach[3]) + 1.0, place.mask->height),
    };
    return true;
}

/// Whether `test` is true of a screen pixel whose middle's in `polygon`, from the pixel `ref_x`,
/// `ref_y`, or a pixel from it, for what's rounded: first the one at its middle, as it is if it's
/// wide, then line by line along its narrower way, so a sliver costs its length.
template <typename Test>
bool any_middle(const polygon_t& polygon, std::int64_t ref_x, std::int64_t ref_y,
                const Test& test) {
    if (polygon.empty()) {
        return false;
    }
    std::array<double, 2> least = polygon[0];
    std::array<double, 2> most = polygon[0];
    std::array<double, 2> sum{};
    for (const std::array<double, 2>& corner : polygon) {
        for (std::size_t axis = 0; axis < 2; ++axis) {
            least[axis] = std::min(least[axis], corner[axis]);
            most[axis] = std::max(most[axis], corner[axis]);
            sum[axis] += corner[axis];
        }
    }
    const auto count = static_cast<double>(polygon.size());
    if (test(ref_x + std::llround(sum[0] / count), ref_y + std::llround(sum[1] / count))) {
        return true;
    }
    // Lines along x, a row each, if it's no taller than it's wide; else along y.
    const std::size_t line = (most[1] - least[1]) <= (most[0] - least[0]) ? 1 : 0;
    const std::size_t along = 1 - line;
    const auto first = static_cast<std::int64_t>(std::ceil(least[line])) - 1;
    const auto last = static_cast<std::int64_t>(std::floor(most[line])) + 1;
    for (std::int64_t at = first; at <= last; ++at) {
        // Where its sides cross the line, or the nearest line that crosses it.
        const double on = std::clamp(static_cast<double>(at), least[line], most[line]);
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (std::size_t corner = 0; corner < polygon.size(); ++corner) {
            const std::array<double, 2>& from = polygon[corner];
            const std::array<double, 2>& to = polygon[(corner + 1) % polygon.size()];
            if (std::min(from[line], to[line]) > on || std::max(from[line], to[line]) < on) {
                continue;
            }
            const double length = to[line] - from[line];
            const double part = length == 0.0 ? 0.0 : (on - from[line]) / length;
            const double where = from[along] + ((to[along] - from[along]) * part);
            const double end = length == 0.0 ? to[along] : where;
            low = std::min({ low, where, end });
            high = std::max({ high, where, end });
        }
        if (low > high) {
            continue;
        }
        const auto start = static_cast<std::int64_t>(std::floor(low)) - 1;
        const auto stop = static_cast<std::int64_t>(std::ceil(high)) + 1;
        for (std::int64_t step = start; step <= stop; ++step) {
            const std::int64_t x = line == 1 ? step : at;
            const std::int64_t y = line == 1 ? at : step;
            if (test(ref_x + x, ref_y + y)) {
                return true;
            }
        }
    }
    return false;
}

/// Whether `inner`'s solid pixels drawn on `part` (or `part` itself, if `inner`'s null) have a
/// screen pixel `test` is true of.
template <typename Test>
bool part_meets(const polygon_t& part, const placed_t* inner, const std::vector<map_t>& maps,
                std::int64_t ref_x, std::int64_t ref_y, const Test& test) {
    if (inner == nullptr) {
        return any_middle(part, ref_x, ref_y, test);
    }
    std::array<std::int64_t, 4> cells{};
    if (!cells_in(*inner, maps, part, cells)) {
        return false;
    }
    for (std::int64_t row = cells[1]; row <= cells[3]; ++row) {
        for (std::int64_t column = cells[0]; column <= cells[2]; ++column) {
            if (!inner->mask->test(column, row)) {
                continue;
            }
            for (const map_t& map : maps) {
                if (any_middle(part_of(part, map, column, row), ref_x, ref_y, test)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/// Whether a placed mask, `one`, and `other` too, if it isn't null, have a solid screen pixel in
/// [left, right) by [top, bottom), a pixel of theirs at a time: each solid one of the one whose
/// pixels are the smaller on the screen, the part of the screen it's drawn on, and each solid one
/// of the other's drawn there; so a turned mask scaled up costs its own pixels, not the screen's.
bool cells_meet(const placed_t& one, const placed_t* other, std::int64_t left, std::int64_t top,
                std::int64_t right, std::int64_t bottom) {
    const placed_t* outer = &one;
    const placed_t* inner = other;
    if (other != nullptr && other->texel < one.texel) {
        std::swap(outer, inner);
    }
    const auto both = [&](std::int64_t x, std::int64_t y) {
        return x >= left && x < right && y >= top && y < bottom && solid_at(one, x, y) &&
               (other == nullptr || solid_at(*other, x, y));
    };
    const auto across = static_cast<double>(right - 1 - left);
    const auto down = static_cast<double>(bottom - 1 - top);
    const polygon_t region{ { 0.0, 0.0 }, { across, 0.0 }, { across, down }, { 0.0, down } };
    const std::vector<map_t> outer_maps = maps_of(*outer, left, top);
    const std::vector<map_t> inner_maps =
        inner != nullptr ? maps_of(*inner, left, top) : std::vector<map_t>{};
    std::array<std::int64_t, 4> cells{};
    if (!cells_in(*outer, outer_maps, region, cells)) {
        return false;
    }
    for (std::int64_t row = cells[1]; row <= cells[3]; ++row) {
        for (std::int64_t column = cells[0]; column <= cells[2]; ++column) {
            if (!outer->mask->test(column, row)) {
                continue;
            }
            for (const map_t& map : outer_maps) {
                const polygon_t part = part_of(region, map, column, row);
                if (!part.empty() && part_meets(part, inner, inner_maps, left, top, both)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

extern "C" {

FBE_API uint64_t fbe_collide_mask(const uint8_t* pixels, int32_t width, int32_t height,
                                  int64_t stride, int32_t threshold) {
    if (width < 0 || height < 0 || width > largest || height > largest) {
        fbe::fail(std::format("a mask is 0 to {} pixels across and down, not {} by {}", largest,
                              width, height));
        return 0;
    }
    if (threshold < 1 || threshold > 255) {
        fbe::fail(std::format("a mask's threshold is 1 to 255, not {}", threshold));
        return 0;
    }
    const bool any_pixels = width > 0 && height > 0;
    if (any_pixels && (pixels == nullptr || stride < static_cast<std::int64_t>(width) * 4)) {
        fbe::fail("a mask's pixels are rows of 4 bytes a pixel, each its stride from the last");
        return 0;
    }
    auto made = std::make_unique<mask_t>();
    made->width = width;
    made->height = height;
    made->words = ((static_cast<std::size_t>(width) + 63U) / 64U) + 1U;
    made->bits.assign(made->words * static_cast<std::size_t>(height), 0);
    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* from = pixels + (static_cast<std::ptrdiff_t>(y) * stride);
        std::uint64_t* to = made->bits.data() + (static_cast<std::size_t>(y) * made->words);
        for (std::int32_t x = 0; x < width; ++x) {
            if (std::cmp_greater_equal(from[(static_cast<std::size_t>(x) * 4) + 3], threshold)) {
                const auto column = static_cast<std::uint64_t>(x);
                to[column >> 6U] |= std::uint64_t{ 1 } << (column & 63U);
                ++made->solid;
            }
        }
    }
    std::uint32_t slot = 0;
    if (!free_slots.empty()) {
        slot = free_slots.back();
        free_slots.pop_back();
    } else if (masks.size() < 0xFFFFFFFFU) {
        slot = static_cast<std::uint32_t>(masks.size());
        masks.emplace_back();
        generations.push_back(1);
    } else {
        fbe::fail("there are as many masks as there can be");
        return 0;
    }
    masks[slot] = std::move(made);
    return handle_of(slot);
}

FBE_API bool fbe_collide_free(uint64_t mask) {
    if (mask_of(mask) == nullptr) {
        return false;
    }
    const auto slot = static_cast<std::uint32_t>(mask);
    masks[slot].reset();
    if (++generations[slot] < last_generation) {
        free_slots.push_back(slot);
    }
    return true;
}

FBE_API int32_t fbe_collide_count(void) {
    return static_cast<std::int32_t>(
        std::ranges::count_if(masks, [](const auto& each) { return each != nullptr; }));
}

FBE_API int32_t fbe_collide_width(uint64_t mask) {
    const mask_t* found = mask_of(mask);
    return found != nullptr ? found->width : -1;
}

FBE_API int32_t fbe_collide_height(uint64_t mask) {
    const mask_t* found = mask_of(mask);
    return found != nullptr ? found->height : -1;
}

FBE_API int64_t fbe_collide_solid_count(uint64_t mask) {
    const mask_t* found = mask_of(mask);
    return found != nullptr ? found->solid : -1;
}

FBE_API bool fbe_collide_solid(uint64_t mask, int32_t x, int32_t y) {
    const mask_t* found = mask_of(mask);
    return found != nullptr && found->test(x, y);
}

FBE_API bool fbe_collide_bounds(uint64_t mask, const fbe_collide_place_t* place,
                                fbe_collide_rect_t* bounds) {
    const mask_t* found = mask_of(mask);
    if (found == nullptr || !finite(place)) {
        return false;
    }
    if (bounds == nullptr) {
        return fbe::fail("there's nowhere to put the bounds");
    }
    std::array<double, 8> at{};
    corners(*found, *place, at);
    double least_x = at[0];
    double most_x = at[0];
    double least_y = at[1];
    double most_y = at[1];
    for (std::size_t corner = 1; corner < 4; ++corner) {
        least_x = std::min(least_x, at[corner * 2]);
        most_x = std::max(most_x, at[corner * 2]);
        least_y = std::min(least_y, at[(corner * 2) + 1]);
        most_y = std::max(most_y, at[(corner * 2) + 1]);
    }
    // In pixels, from 256ths.
    bounds->x = static_cast<float>(least_x / 256.0);
    bounds->y = static_cast<float>(least_y / 256.0);
    bounds->width = static_cast<float>((most_x - least_x) / 256.0);
    bounds->height = static_cast<float>((most_y - least_y) / 256.0);
    return true;
}

FBE_API int32_t fbe_collide_masks(uint64_t first, const fbe_collide_place_t* first_place,
                                  uint64_t second, const fbe_collide_place_t* second_place) {
    const mask_t* one = mask_of(first);
    const mask_t* other = one != nullptr ? mask_of(second) : nullptr;
    if (other == nullptr || !finite(first_place) || !finite(second_place)) {
        return -1;
    }
    const placed_t a = placed(*one, *first_place);
    const placed_t b = placed(*other, *second_place);
    if (a.shape == shape_t::NONE || b.shape == shape_t::NONE) {
        return 0;
    }
    // Their bounds first, then each row both cover, rows that are the same taken once; or, turned
    // and scaled up, a pixel of theirs at a time.
    const std::int64_t top = std::max(a.top, b.top);
    const std::int64_t bottom = std::min(a.bottom, b.bottom);
    const std::int64_t left = std::max(a.left, b.left);
    const std::int64_t right = std::min(a.right, b.right);
    if (top >= bottom || left >= right) {
        return 0;
    }
    if (bottom - top > many_rows && a.texel >= cell_size && b.texel >= cell_size &&
        (slanted(a) || slanted(b))) {
        return cells_meet(a, &b, left, top, right, bottom) ? 1 : 0;
    }
    for (std::int64_t y = top; y < bottom && left < right;) {
        std::int64_t from = left;
        std::int64_t to = right;
        if (span(a, y, from, to) && span(b, y, from, to) && row_hit(a, &b, y, from, to)) {
            return 1;
        }
        y = std::min(next_row(a, y, bottom), next_row(b, y, bottom));
    }
    return 0;
}

FBE_API int32_t fbe_collide_rect(uint64_t mask, const fbe_collide_place_t* place,
                                 const fbe_collide_rect_t* rect) {
    const mask_t* found = mask_of(mask);
    if (found == nullptr || !finite(place)) {
        return -1;
    }
    if (rect == nullptr || !std::isfinite(rect->x) || !std::isfinite(rect->y) ||
        !std::isfinite(rect->width) || !std::isfinite(rect->height)) {
        fbe::fail("a rectangle is finite");
        return -1;
    }
    const placed_t at = placed(*found, *place);
    if (at.shape == shape_t::NONE || rect->width <= 0.0F || rect->height <= 0.0F) {
        return 0;
    }
    // The pixels whose area its inside reaches into.
    const std::int64_t left = std::max(at.left, pixel_of(std::floor(rect->x)));
    const std::int64_t right =
        std::min(at.right, pixel_of(std::ceil(static_cast<double>(rect->x) + rect->width)));
    const std::int64_t top = std::max(at.top, pixel_of(std::floor(rect->y)));
    const std::int64_t bottom =
        std::min(at.bottom, pixel_of(std::ceil(static_cast<double>(rect->y) + rect->height)));
    if (top >= bottom || left >= right) {
        return 0;
    }
    if (bottom - top > many_rows && at.texel >= cell_size && slanted(at)) {
        return cells_meet(at, nullptr, left, top, right, bottom) ? 1 : 0;
    }
    for (std::int64_t y = top; y < bottom && left < right;) {
        std::int64_t from = left;
        std::int64_t to = right;
        if (span(at, y, from, to) && row_hit(at, nullptr, y, from, to)) {
            return 1;
        }
        y = next_row(at, y, bottom);
    }
    return 0;
}

FBE_API int32_t fbe_collide_point(uint64_t mask, const fbe_collide_place_t* place, float x,
                                  float y) {
    const mask_t* found = mask_of(mask);
    if (found == nullptr || !finite(place)) {
        return -1;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        fbe::fail("a point is finite");
        return -1;
    }
    const placed_t at = placed(*found, *place);
    return solid_at(at, pixel_of(std::floor(x)), pixel_of(std::floor(y))) ? 1 : 0;
}

} // extern "C"
