// Primitive shapes, each one surface of radius 1 about the origin, with counterclockwise front
// faces, normals, and UVs from the top left of a texture.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cmath>
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

/// A disc of radius 1 at height `y`, facing up or down, with planar UVs.
void cap(builder_t& built, std::int32_t segments, float y, bool up) {
    const vec3_t normal(0.0F, up ? 1.0F : -1.0F, 0.0F);
    const std::uint32_t center = built.vertex({ 0.0F, y, 0.0F }, normal, 0.5F, 0.5F);
    const std::uint32_t first = center + 1;
    for (std::int32_t i = 0; i < segments; ++i) {
        const float angle = tau * static_cast<float>(i) / static_cast<float>(segments);
        const float x = std::cos(angle);
        const float z = -std::sin(angle);
        built.vertex({ x, y, z }, normal, (x * 0.5F) + 0.5F, (z * 0.5F) + 0.5F);
    }
    for (std::int32_t i = 0; i < segments; ++i) {
        const std::uint32_t a = first + static_cast<std::uint32_t>(i);
        const std::uint32_t b = first + static_cast<std::uint32_t>((i + 1) % segments);
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

/// A cylinder's or cone's side: rings at the top and bottom, the top's of radius `top`, each
/// vertex's normal leaning by `lean` up for every unit out.
void side(builder_t& built, std::int32_t segments, float top, float lean) {
    const auto first = static_cast<std::uint32_t>(built.surface.vertices.size());
    for (std::int32_t i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = tau * u;
        const float x = std::cos(angle);
        const float z = -std::sin(angle);
        const vec3_t normal = normalize(vec3_t(x, lean, z));
        built.vertex({ x * top, 1.0F, z * top }, normal, u, 0.0F);
        built.vertex({ x, -1.0F, z }, normal, u, 1.0F);
    }
    for (std::int32_t i = 0; i < segments; ++i) {
        const std::uint32_t at = first + (static_cast<std::uint32_t>(i) * 2);
        // Top, bottom, then the next column's bottom and top: counterclockwise from outside. A
        // cone's top is one point, so its quads are one triangle.
        built.triangle(at, at + 1, at + 3);
        if (top > 0.0F) {
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
    const std::int32_t around = segments * 2;
    for (std::int32_t ring = 0; ring <= segments; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(segments);
        const float down = std::numbers::pi_v<float> * v;
        for (std::int32_t i = 0; i <= around; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(around);
            const float angle = fbe::scene::tau * u;
            const vec3_t p(std::sin(down) * std::cos(angle), std::cos(down),
                           -std::sin(down) * std::sin(angle));
            built.vertex(p, p, u, v);
        }
    }
    const auto row = static_cast<std::uint32_t>(around + 1);
    for (std::int32_t ring = 0; ring < segments; ++ring) {
        for (std::int32_t i = 0; i < around; ++i) {
            const std::uint32_t top =
                (static_cast<std::uint32_t>(ring) * row) + static_cast<std::uint32_t>(i);
            const std::uint32_t bottom = top + row;
            // The poles' rows are one point each, so their quads are one triangle.
            if (ring != 0) {
                built.triangle(top, bottom, top + 1);
            }
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
    fbe::scene::side(built, segments, 1.0F, 0.0F);
    if (capped) {
        fbe::scene::cap(built, segments, 1.0F, true);
        fbe::scene::cap(built, segments, -1.0F, false);
    }
    return built.finish();
}

FBE_API uint64_t fbe_geometry_cone(int32_t segments, bool capped) {
    if (!fbe::scene::valid_segments(segments, 3)) {
        return 0;
    }
    builder_t built;
    // The side rises 2 for every 1 in, so its normal leans up by a half for every unit out.
    fbe::scene::side(built, segments, 0.0F, 0.5F);
    if (capped) {
        fbe::scene::cap(built, segments, -1.0F, false);
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
