// Geometry through the C API: how long it lives as handles and meshes hold it, what surfaces take,
// and the flat arrays physics reads.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace {

fbe_vertex_t at(float x, float y, float z) {
    return {
        .position = { .x = x, .y = y, .z = z },
        .normal = { .x = 0.0F, .y = 0.0F, .z = 1.0F },
        .uv0 = { .x = 0.0F, .y = 0.0F },
        .uv1 = { .x = 0.0F, .y = 0.0F },
        .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
    };
}

/// A geometry of one surface with one triangle.
std::uint64_t triangle() {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    const fbe_vertex_t top = at(0.0F, 1.0F, 0.0F);
    const std::array<fbe_vertex_t, 3> vertices{ at(0.0F, 0.0F, 0.0F), at(1.0F, 0.0F, 0.0F), top };
    fbe_surface_add_vertices(geometry, surface, vertices.data(), 3);
    const std::array<std::uint32_t, 3> indices{ 0, 1, 2 };
    fbe_surface_add_triangles(geometry, surface, indices.data(), 3);
    return geometry;
}

/// A world of the test's own, freed at its end.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
        std::array<std::uint64_t, 16> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    }
};

} // namespace

TEST_CASE("a geometry lives while its handle is held or a mesh draws it", "[scene]") {
    const test_world_t world;
    const std::uint64_t geometry = triangle();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK(fbe_mesh_geometry(mesh) == 0);
    REQUIRE(fbe_mesh_set_geometry(mesh, geometry));
    const std::uint64_t copy = fbe_entity_copy(mesh, 0, 0);
    CHECK(fbe_mesh_geometry(copy) == geometry);

    REQUIRE(fbe_geometry_release(geometry));
    CHECK(fbe_geometry_alive(geometry));
    fbe_entity_free(mesh);
    CHECK(fbe_geometry_alive(geometry));
    CHECK(fbe_surface_vertex_count(geometry, 0) == 3);
    fbe_entity_free(copy);
    CHECK_FALSE(fbe_geometry_alive(geometry));
    CHECK(fbe_surface_vertex_count(geometry, 0) == -1);
    CHECK(std::string_view(fbe_error()) == "the geometry was freed");
}

TEST_CASE("a mesh given its own geometry again keeps it", "[scene]") {
    const test_world_t world;
    const std::uint64_t geometry = triangle();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    REQUIRE(fbe_mesh_set_geometry(mesh, geometry));
    CHECK(fbe_geometry_alive(geometry));
    REQUIRE(fbe_mesh_set_geometry(mesh, 0));
    CHECK_FALSE(fbe_geometry_alive(geometry));
    CHECK_FALSE(fbe_mesh_set_geometry(mesh, geometry));
    CHECK_FALSE(fbe_mesh_set_geometry(world.handle, 0));
}

TEST_CASE("a surface takes only triangles of vertices it has", "[scene]") {
    const std::uint64_t geometry = triangle();
    const std::array<std::uint32_t, 3> past{ 0, 1, 3 };
    CHECK(fbe_surface_add_triangles(geometry, 0, past.data(), 3) == -1);
    CHECK(std::string_view(fbe_error()) == "the surface's vertices are 0 to 2, not 3");
    CHECK(fbe_surface_add_triangles(geometry, 0, past.data(), 2) == -1);
    CHECK(fbe_surface_add_triangles(geometry, 1, past.data(), 0) == -1);
    CHECK(std::string_view(fbe_error()) == "the geometry's surfaces are 0 to 0, not 1");
    CHECK(fbe_surface_triangle_count(geometry, 0) == 1);
    std::array<fbe_vertex_t, 2> two{};
    CHECK_FALSE(fbe_surface_vertices(geometry, 0, 2, two.data(), 2));
    CHECK(fbe_surface_vertices(geometry, 0, 1, two.data(), 2));
    CHECK(two[1].position.y == 1.0F);
    CHECK(fbe_surface_add_vertices(geometry, 0, two.data(), -1) == -1);
    CHECK(fbe_geometry_sphere(2) == 0);
    CHECK(fbe_geometry_plane(101) == 0);
    fbe_geometry_release(geometry);
}

TEST_CASE("positions and indices run on across surfaces", "[scene]") {
    const std::uint64_t geometry = triangle();
    const std::uint64_t other = triangle();
    const fbe_mat4_t identity{ .m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1 } };
    REQUIRE(fbe_geometry_add(geometry, other, &identity));
    std::vector<fbe_vec3_t> positions(
        static_cast<std::size_t>(fbe_geometry_positions(geometry, nullptr, 0)));
    fbe_geometry_positions(geometry, positions.data(), static_cast<std::int32_t>(positions.size()));
    REQUIRE(positions.size() == 6);
    CHECK(positions[3].x == 2.0F);
    std::vector<std::uint32_t> indices(
        static_cast<std::size_t>(fbe_geometry_indices(geometry, nullptr, 0)));
    fbe_geometry_indices(geometry, indices.data(), static_cast<std::int32_t>(indices.size()));
    CHECK(indices == std::vector<std::uint32_t>{ 0, 1, 2, 3, 4, 5 });
    const std::uint64_t version = fbe_geometry_version(geometry);
    fbe_geometry_flip(geometry);
    CHECK(fbe_geometry_version(geometry) > version);
    fbe_geometry_release(geometry);
    fbe_geometry_release(other);
}

TEST_CASE("fitting takes only a box the right way round", "[scene]") {
    const std::uint64_t geometry = fbe_geometry_cube();
    const fbe_aabb_t backward{
        .min = { .x = 1.0F, .y = 0.0F, .z = 0.0F },
        .max = { .x = 0.0F, .y = 1.0F, .z = 1.0F },
    };
    CHECK_FALSE(fbe_geometry_fit(geometry, &backward, false));
    const fbe_aabb_t point{
        .min = { .x = 3.0F, .y = 3.0F, .z = 3.0F },
        .max = { .x = 3.0F, .y = 3.0F, .z = 3.0F },
    };
    REQUIRE(fbe_geometry_fit(geometry, &point, true));
    fbe_aabb_t bounds{};
    fbe_geometry_bounds(geometry, &bounds);
    CHECK(bounds.min.x == 3.0F);
    CHECK(bounds.max.z == 3.0F);
    fbe_geometry_release(geometry);
}

namespace {

/// How many edges, by welded index, aren't shared by exactly two triangles.
std::size_t open_edges(std::uint64_t geometry) {
    std::vector<std::uint32_t> indices(
        static_cast<std::size_t>(fbe_geometry_indices(geometry, nullptr, 0)));
    fbe_geometry_indices(geometry, indices.data(), static_cast<std::int32_t>(indices.size()));
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t a = indices[i + k];
            const std::uint32_t b = indices[i + ((k + 1) % 3)];
            ++edges[{ std::min(a, b), std::max(a, b) }];
        }
    }
    return static_cast<std::size_t>(
        std::ranges::count_if(edges, [](const auto& edge) { return edge.second != 2; }));
}

} // namespace

TEST_CASE("welded positions close every closed primitive", "[scene]") {
    const std::uint64_t cube = fbe_geometry_cube();
    CHECK(fbe_geometry_positions(cube, nullptr, 0) == 8);
    CHECK(fbe_geometry_indices(cube, nullptr, 0) == 36);
    CHECK(open_edges(cube) == 0);
    for (const std::int32_t segments : { 3, 8, 16 }) {
        const std::uint64_t sphere = fbe_geometry_sphere(segments);
        const std::uint64_t cylinder = fbe_geometry_cylinder(segments, true);
        const std::uint64_t cone = fbe_geometry_cone(segments, true);
        const std::array<std::uint64_t, 3> closed{ sphere, cylinder, cone };
        for (const std::uint64_t shape : closed) {
            CHECK(open_edges(shape) == 0);
            fbe_geometry_release(shape);
        }
    }
    // A plane's edges are its rim, 4 squares of 4.
    const std::uint64_t plane = fbe_geometry_plane(4);
    CHECK(open_edges(plane) == 16);
    fbe_geometry_release(plane);
    fbe_geometry_release(cube);
}
