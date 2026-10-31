// Masks: an image's solid pixels, a bit each, in rows of 64-bit words, and whether placed masks
// meet: their bounds first, then row by row, by whole words where neither is turned or scaled,
// and pixel by pixel, over the span of the row each covers, where one is.

#include "core/engine.hpp"
#include "fbengine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
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

/// A mask placed: the screen pixels' rows and columns it may cover, [left, right) by [top,
/// bottom), and how a screen pixel maps into it. Plain, neither turned nor scaled, a screen pixel
/// is its pixel moved by (dx, dy). Otherwise a pixel's middle maps back into its pixels by one
/// arithmetic everywhere, so every path agrees: from the position, turned back, scaled back, from
/// the handle; and a middle exactly on its edge is inside by the GPU's top-left rule, on a left or
/// top edge on the screen, and outside on a right or bottom one.
struct placed_t {
    const mask_t* mask = nullptr;
    bool empty = true;
    bool plain = false;
    std::int64_t dx = 0;
    std::int64_t dy = 0;
    double x = 0.0;
    double y = 0.0;
    double handle_x = 0.0;
    double handle_y = 0.0;
    double cosine = 1.0;
    double sine = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
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
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

/// `value` kept to the canvas's reach, as a screen pixel's row or column.
std::int64_t pixel_of(double value) {
    return static_cast<std::int64_t>(std::clamp(value, -farthest, farthest));
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

/// How many quarter turns `rotation` radians is, from 0 to 3, if it's one, within a few of a
/// float's steps, as a float pi over 2 is, which the GPU's snapped corners draw as one exactly; or
/// -1.
int quarters(float rotation) {
    const double turns = static_cast<double>(rotation) / (std::numbers::pi / 2.0);
    const double nearest = std::round(turns);
    const double off =
        std::abs(static_cast<double>(rotation) - (nearest * (std::numbers::pi / 2.0)));
    // Four of a float's steps at its size, or of 1's for one near 0.
    const double steps = 4.0 * std::max(std::abs(static_cast<double>(rotation)), 1.0) /
                         static_cast<double>(1U << 23U);
    if (off > steps || std::abs(nearest) > 1e15) {
        return -1;
    }
    return static_cast<int>(((static_cast<std::int64_t>(nearest) % 4) + 4) % 4);
}

/// The cosine and sine of `rotation` radians, exact for quarter turns, as the GPU's snapped
/// corners are.
void turned(float rotation, double& cosine, double& sine) {
    const int quarter = quarters(rotation);
    if (quarter >= 0) {
        constexpr std::array<double, 4> cosines{ 1.0, 0.0, -1.0, 0.0 };
        constexpr std::array<double, 4> sines{ 0.0, 1.0, 0.0, -1.0 };
        cosine = cosines[static_cast<std::size_t>(quarter)];
        sine = sines[static_cast<std::size_t>(quarter)];
        return;
    }
    const double turn = std::fmod(static_cast<double>(rotation), 2.0 * std::numbers::pi);
    cosine = std::cos(turn);
    sine = std::sin(turn);
}

/// The corners of `mask` placed as `place`, as the canvas puts a sprite's: from its handle,
/// scaled, turned clockwise, and moved; x then y of each.
std::array<double, 8> corners(const mask_t& mask, const fbe_collide_place_t& place) {
    double cosine = 1.0;
    double sine = 0.0;
    turned(place.rotation, cosine, sine);
    const std::array<std::array<double, 2>, 4> part{
        {
            { 0.0, 0.0 },
            { static_cast<double>(mask.width), 0.0 },
            { static_cast<double>(mask.width), static_cast<double>(mask.height) },
            { 0.0, static_cast<double>(mask.height) },
        },
    };
    std::array<double, 8> found{};
    for (std::size_t at = 0; at < part.size(); ++at) {
        const double from_x = (part[at][0] - place.handle_x) * static_cast<double>(place.scale_x);
        const double from_y = (part[at][1] - place.handle_y) * static_cast<double>(place.scale_y);
        found[at * 2] = place.x + (from_x * cosine) - (from_y * sine);
        found[(at * 2) + 1] = place.y + (from_x * sine) + (from_y * cosine);
    }
    return found;
}

/// Whether an edge whose inward normal on the screen is (`across`, `down`) is a left or a top one,
/// whose middles are inside.
bool top_left(double across, double down) {
    return across > 0.0 || (across == 0.0 && down > 0.0);
}

/// `mask` placed as `place`.
placed_t placed(const mask_t& mask, const fbe_collide_place_t& place) {
    placed_t made;
    made.mask = &mask;
    if (mask.width == 0 || mask.height == 0 || place.scale_x == 0.0F || place.scale_y == 0.0F) {
        return made;
    }
    made.empty = false;
    if (quarters(place.rotation) == 0 && place.scale_x == 1.0F && place.scale_y == 1.0F) {
        // A screen pixel's middle lands in the mask's pixel it's drawn from, floored.
        made.plain = true;
        made.dx = pixel_of(std::ceil(static_cast<double>(place.x) - place.handle_x - 0.5));
        made.dy = pixel_of(std::ceil(static_cast<double>(place.y) - place.handle_y - 0.5));
        made.left = std::max(made.dx, pixel_of(-farthest));
        made.right = std::min(made.dx + mask.width, pixel_of(farthest));
        made.top = std::max(made.dy, pixel_of(-farthest));
        made.bottom = std::min(made.dy + mask.height, pixel_of(farthest));
        return made;
    }
    const std::array<double, 8> found = corners(mask, place);
    double least_x = found[0];
    double most_x = found[0];
    double least_y = found[1];
    double most_y = found[1];
    for (std::size_t at = 1; at < 4; ++at) {
        least_x = std::min(least_x, found[at * 2]);
        most_x = std::max(most_x, found[at * 2]);
        least_y = std::min(least_y, found[(at * 2) + 1]);
        most_y = std::max(most_y, found[(at * 2) + 1]);
    }
    // The pixels whose middles are inside its corners' bounds, a pixel more each way for what's
    // rounded, kept to the canvas's reach.
    made.left = pixel_of(std::ceil(least_x - 0.5) - 1.0);
    made.right = pixel_of(std::floor(most_x - 0.5) + 2.0);
    made.top = pixel_of(std::ceil(least_y - 0.5) - 1.0);
    made.bottom = pixel_of(std::floor(most_y - 0.5) + 2.0);
    turned(place.rotation, made.cosine, made.sine);
    made.x = place.x;
    made.y = place.y;
    made.handle_x = place.handle_x;
    made.handle_y = place.handle_y;
    made.scale_x = place.scale_x;
    made.scale_y = place.scale_y;
    made.across_u = made.cosine / made.scale_x;
    made.down_u = made.sine / made.scale_x;
    made.across_v = -made.sine / made.scale_y;
    made.down_v = made.cosine / made.scale_y;
    made.u_low = top_left(made.across_u, made.down_u);
    made.u_high = top_left(-made.across_u, -made.down_u);
    made.v_low = top_left(made.across_v, made.down_v);
    made.v_high = top_left(-made.across_v, -made.down_v);
    return made;
}

/// Where the middle of the screen pixel `x`, `y` is in a placed mask's pixels, the one arithmetic
/// every path takes.
void local(const placed_t& place, std::int64_t x, std::int64_t y, double& u, double& v) {
    const double from_x = (static_cast<double>(x) + 0.5) - place.x;
    const double from_y = (static_cast<double>(y) + 0.5) - place.y;
    u = (((from_x * place.cosine) + (from_y * place.sine)) / place.scale_x) + place.handle_x;
    v = (((from_y * place.cosine) - (from_x * place.sine)) / place.scale_y) + place.handle_y;
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

/// Whether the screen pixel at `x`, `y` is one of a placed mask's solid ones.
bool solid_at(const placed_t& place, std::int64_t x, std::int64_t y) {
    if (place.empty) {
        return false;
    }
    if (place.plain) {
        return place.mask->test(x - place.dx, y - place.dy);
    }
    double u = 0.0;
    double v = 0.0;
    local(place, x, y, u, v);
    const std::int64_t column = index_of(u, place.mask->width, place.u_low, place.u_high);
    const std::int64_t row = index_of(v, place.mask->height, place.v_low, place.v_high);
    return column >= 0 && row >= 0 && place.mask->test(column, row);
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
    if (place.empty || y < place.top || y >= place.bottom) {
        return false;
    }
    from = std::max(from, place.left);
    to = std::min(to, place.right);
    if (from >= to) {
        return false;
    }
    if (place.plain) {
        return true;
    }
    double u = 0.0;
    double v = 0.0;
    local(place, 0, y, u, v);
    return within(u, place.across_u, place.mask->width, from, to) &&
           within(v, place.across_v, place.mask->height, from, to);
}

/// Calls `visit(from, to, solid)` for each run of row `y`'s pixels from `from` up to `to` in one
/// of a placed mask's pixels, or outside it, stepping a pixel of the mask at a time, not a pixel of
/// the screen, so a scaled-up mask costs no more than its own pixels; true as soon as `visit` is.
template <typename Visit>
bool walk(const placed_t& place, std::int64_t y, std::int64_t from, std::int64_t to,
          const Visit& visit) {
    for (std::int64_t x = from; x < to;) {
        double u = 0.0;
        double v = 0.0;
        local(place, x, y, u, v);
        const std::int64_t column = index_of(u, place.mask->width, place.u_low, place.u_high);
        const std::int64_t row = index_of(v, place.mask->height, place.v_low, place.v_high);
        const bool solid = column >= 0 && row >= 0 && place.mask->test(column, row);
        double next_u = 0.0;
        double next_v = 0.0;
        local(place, x + 1, y, next_u, next_v);
        const std::int64_t next = std::min(reaching(u, place.across_u, next_u, x, to),
                                           reaching(v, place.across_v, next_v, x, to));
        if (visit(x, next, solid)) {
            return true;
        }
        x = next;
    }
    return false;
}

/// The next row past `y` whose runs may differ from `y`'s: the next for a plain or turned mask,
/// and for one turned a quarter or not, the next in another of its rows or columns.
std::int64_t next_row(const placed_t& place, std::int64_t y, std::int64_t bottom) {
    if (place.plain) {
        return y + 1;
    }
    double u = 0.0;
    double v = 0.0;
    double next_u = 0.0;
    double next_v = 0.0;
    local(place, 0, y, u, v);
    local(place, 0, y + 1, next_u, next_v);
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
    if (one.plain && (other == nullptr || other->plain)) {
        return any_bits(one, other, y, from, to);
    }
    if (one.plain) {
        return row_hit(*other, &one, y, from, to);
    }
    return walk(one, y, from, to, [&](std::int64_t start, std::int64_t end, bool solid) {
        if (!solid) {
            return false;
        }
        if (other == nullptr) {
            return true;
        }
        if (other->plain) {
            return any_bits(*other, nullptr, y, start, end);
        }
        return walk(*other, y, start, end,
                    [](std::int64_t, std::int64_t, bool also) { return also; });
    });
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
    const std::array<double, 8> at = corners(*found, *place);
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
    bounds->x = static_cast<float>(least_x);
    bounds->y = static_cast<float>(least_y);
    bounds->width = static_cast<float>(most_x - least_x);
    bounds->height = static_cast<float>(most_y - least_y);
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
    if (a.empty || b.empty) {
        return 0;
    }
    // Their bounds first, then each row both cover, rows that are the same taken once.
    const std::int64_t top = std::max(a.top, b.top);
    const std::int64_t bottom = std::min(a.bottom, b.bottom);
    const std::int64_t left = std::max(a.left, b.left);
    const std::int64_t right = std::min(a.right, b.right);
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
    if (at.empty || rect->width <= 0.0F || rect->height <= 0.0F) {
        return 0;
    }
    // The pixels whose area its inside reaches into.
    const std::int64_t left = std::max(at.left, pixel_of(std::floor(rect->x)));
    const std::int64_t right =
        std::min(at.right, pixel_of(std::ceil(static_cast<double>(rect->x) + rect->width)));
    const std::int64_t top = std::max(at.top, pixel_of(std::floor(rect->y)));
    const std::int64_t bottom =
        std::min(at.bottom, pixel_of(std::ceil(static_cast<double>(rect->y) + rect->height)));
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
    if (at.empty) {
        return 0;
    }
    return solid_at(at, pixel_of(std::floor(x)), pixel_of(std::floor(y))) ? 1 : 0;
}

} // extern "C"
