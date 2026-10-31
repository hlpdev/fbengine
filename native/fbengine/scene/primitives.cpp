// Primitive shapes, each one surface of radius 1 about the origin, with counterclockwise front
// faces, normals, and UVs from the top left of a texture.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

constexpr float tau = 2.0F * std::numbers::pi_v<float>;
constexpr std::int32_t most_segments = 100;

/// A surface being built.
struct builder_t {
    surface_t surface;

    /// Adds a vertex, white, both UV sets the same, and gives its index.
    std::uint32_t vertex(vec3_t position, vec3_t normal, float u, float v) {
        const fbe_vertex_t made{
            .position = { .x = position.x, .y = position.y, .z = position.z },
            .normal = { .x = normal.x, .y = normal.y, .z = normal.z },
            .uv0 = { .x = u, .y = v },
            .uv1 = { .x = u, .y = v },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
        };
        surface.vertices.push_back(made);
        return static_cast<std::uint32_t>(surface.vertices.size() - 1);
    }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        surface.indices.insert(surface.indices.end(), { a, b, c });
    }

    /// Two triangles of a quad whose corners go counterclockwise seen from the front.
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }

    /// The geometry of the surface, held once, as its handle.
    std::uint64_t finish() {
        const std::uint32_t geometry = make_geometry();
        scene().geometries[geometry].surfaces.push_back(std::move(surface));
        return scene().geometry_slots.handle(geometry);
    }
};

/// Where each of `columns` columns around Y is, cos and sin of its angle, the first's again at the
/// end, so vertices at one place around are bit for bit the same wherever they're built.
std::vector<std::pair<float, float>> columns_around(std::int32_t columns) {
    std::vector<std::pair<float, float>> around;
    for (std::int32_t i = 0; i <= columns; ++i) {
        const float angle = tau * static_cast<float>(i % columns) / static_cast<float>(columns);
        around.emplace_back(std::cos(angle), std::sin(angle));
    }
    return around;
}

/// A disc of radius 1 at height `y`, facing up or down, with planar UVs that read as they do on
/// the top seen from above, and on the bottom seen from below.
void cap(builder_t& built, const std::vector<std::pair<float, float>>& around, float y, bool up) {
    const vec3_t normal(0.0F, up ? 1.0F : -1.0F, 0.0F);
    const std::uint32_t center = built.vertex({ 0.0F, y, 0.0F }, normal, 0.5F, 0.5F);
    const std::uint32_t first = center + 1;
    const auto segments = static_cast<std::uint32_t>(around.size() - 1);
    for (std::uint32_t i = 0; i < segments; ++i) {
        const float x = around[i].first;
        const float z = -around[i].second;
        const float u = up ? (x * 0.5F) + 0.5F : 0.5F - (x * 0.5F);
        built.vertex({ x, y, z }, normal, u, (z * 0.5F) + 0.5F);
    }
    for (std::uint32_t i = 0; i < segments; ++i) {
        const std::uint32_t a = first + i;
        const std::uint32_t b = first + ((i + 1) % segments);
        // The ring goes counterclockwise seen from above, so the bottom's winds the other way.
        if (up) {
            built.triangle(center, a, b);
        } else {
            built.triangle(center, b, a);
        }
    }
}

bool valid_segments(std::int32_t segments, std::int32_t least) {
    if (segments < least || segments > most_segments) {
        return fail(
            std::format("a shape has {} to {} segments, not {}", least, most_segments, segments));
    }
    return true;
}

/// A cylinder's side, or a cone's, whose top is a point: rings at the top and bottom, each vertex's
/// normal leaning by `lean` up for every unit out. A cone's tip is a vertex for each column, at its
/// middle, with the normal there.
void side(builder_t& built, const std::vector<std::pair<float, float>>& around, bool cone,
          float lean) {
    const auto first = static_cast<std::uint32_t>(built.surface.vertices.size());
    const auto segments = static_cast<std::uint32_t>(around.size() - 1);
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float x = around[i].first;
        const float z = -around[i].second;
        const vec3_t normal = normalize(vec3_t(x, lean, z));
        if (cone) {
            const float middle =
                tau * (static_cast<float>(i) + 0.5F) / static_cast<float>(segments);
            const vec3_t tip_normal = normalize(vec3_t(std::cos(middle), lean, -std::sin(middle)));
            const float tip_u = (static_cast<float>(i) + 0.5F) / static_cast<float>(segments);
            built.vertex({ 0.0F, 1.0F, 0.0F }, tip_normal, tip_u, 0.0F);
        } else {
            built.vertex({ x, 1.0F, z }, normal, u, 0.0F);
        }
        built.vertex({ x, -1.0F, z }, normal, u, 1.0F);
    }
    for (std::uint32_t i = 0; i < segments; ++i) {
        const std::uint32_t at = first + (i * 2);
        // Top, bottom, then the next column's bottom and top: counterclockwise from outside. A
        // cone's top is one point, so its quads are one triangle.
        built.triangle(at, at + 1, at + 3);
        if (!cone) {
            built.triangle(at, at + 3, at + 2);
        }
    }
}

} // namespace

} // namespace fbe::scene

using fbe::scene::builder_t;
using fbe::scene::vec3_t;

extern "C" {

FBE_API uint64_t fbe_geometry_cube(void) {
    builder_t built;
    // Each face by its normal and the axes going right and up across it, seen from outside.
    const auto face = [&built](vec3_t normal, vec3_t right, vec3_t up) {
        const std::uint32_t a = built.vertex(normal - right - up, normal, 0.0F, 1.0F);
        const std::uint32_t b = built.vertex(normal + right - up, normal, 1.0F, 1.0F);
        const std::uint32_t c = built.vertex(normal + right + up, normal, 1.0F, 0.0F);
        const std::uint32_t d = built.vertex(normal - right + up, normal, 0.0F, 0.0F);
        built.quad(a, b, c, d);
    };
    face({ 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F }, { 0.0F, 1.0F, 0.0F });
    face({ -1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F, 0.0F });
    face({ 0.0F, 1.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F });
    face({ 0.0F, -1.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F });
    face({ 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });
    face({ 0.0F, 0.0F, -1.0F }, { -1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });
    return built.finish();
}

FBE_API uint64_t fbe_geometry_sphere(int32_t segments) {
    if (!fbe::scene::valid_segments(segments, 3)) {
        return 0;
    }
    builder_t built;
    const std::int32_t columns = segments * 2;
    const std::vector<std::pair<float, float>> around = fbe::scene::columns_around(columns);
    for (std::int32_t ring = 0; ring <= segments; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(segments);
        const float down = std::numbers::pi_v<float> * v;
        const bool pole = ring == 0 || ring == segments;
        for (std::int32_t i = 0; i <= columns; ++i) {
            const auto column = static_cast<std::size_t>(i);
            if (pole) {
                // A pole is one point, exactly, each of its vertices at its column's middle.
                const vec3_t p(0.0F, ring == 0 ? 1.0F : -1.0F, 0.0F);
                const float middle = (static_cast<float>(i) + 0.5F) / static_cast<float>(columns);
                built.vertex(p, p, middle, v);
                continue;
            }
            const float u = static_cast<float>(i) / static_cast<float>(columns);
            const vec3_t p(std::sin(down) * around[column].first, std::cos(down),
                           -std::sin(down) * around[column].second);
            built.vertex(p, p, u, v);
        }
    }
    const auto row = static_cast<std::uint32_t>(columns + 1);
    for (std::int32_t ring = 0; ring < segments; ++ring) {
        for (std::int32_t i = 0; i < columns; ++i) {
            const std::uint32_t top =
                (static_cast<std::uint32_t>(ring) * row) + static_cast<std::uint32_t>(i);
            const std::uint32_t bottom = top + row;
            // The poles' rows are one point each, so their quads are one triangle, with the pole's
            // vertex of the quad's own column.
            if (ring == 0) {
                built.triangle(top, bottom, bottom + 1);
                continue;
            }
            built.triangle(top, bottom, top + 1);
            if (ring != segments - 1) {
                built.triangle(top + 1, bottom, bottom + 1);
            }
        }
    }
    return built.finish();
}

FBE_API uint64_t fbe_geometry_cylinder(int32_t segments, bool capped) {
    if (!fbe::scene::valid_segments(segments, 3)) {
        return 0;
    }
    builder_t built;
    const std::vector<std::pair<float, float>> around = fbe::scene::columns_around(segments);
    fbe::scene::side(built, around, false, 0.0F);
    if (capped) {
        fbe::scene::cap(built, around, 1.0F, true);
        fbe::scene::cap(built, around, -1.0F, false);
    }
    return built.finish();
}

FBE_API uint64_t fbe_geometry_cone(int32_t segments, bool capped) {
    if (!fbe::scene::valid_segments(segments, 3)) {
        return 0;
    }
    builder_t built;
    const std::vector<std::pair<float, float>> around = fbe::scene::columns_around(segments);
    // The side rises 2 for every 1 in, so its normal leans up by a half for every unit out.
    fbe::scene::side(built, around, true, 0.5F);
    if (capped) {
        fbe::scene::cap(built, around, -1.0F, false);
    }
    return built.finish();
}

FBE_API uint64_t fbe_geometry_plane(int32_t segments) {
    if (!fbe::scene::valid_segments(segments, 1)) {
        return 0;
    }
    builder_t built;
    const vec3_t up(0.0F, 1.0F, 0.0F);
    for (std::int32_t row = 0; row <= segments; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(segments);
        for (std::int32_t column = 0; column <= segments; ++column) {
            const float u = static_cast<float>(column) / static_cast<float>(segments);
            built.vertex({ (u * 2.0F) - 1.0F, 0.0F, (v * 2.0F) - 1.0F }, up, u, v);
        }
    }
    const auto line = static_cast<std::uint32_t>(segments + 1);
    for (std::int32_t row = 0; row < segments; ++row) {
        for (std::int32_t column = 0; column < segments; ++column) {
            const std::uint32_t at =
                (static_cast<std::uint32_t>(row) * line) + static_cast<std::uint32_t>(column);
            // Seen from above, with -Z away, the far row is at, at + 1, and the near one below it.
            built.quad(at, at + line, at + line + 1, at + 1);
        }
    }
    return built.finish();
}

} // extern "C"
