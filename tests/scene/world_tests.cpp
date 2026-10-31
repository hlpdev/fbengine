// Worlds and the tree of entities through the C API: the handle tables, the order of children and
// roots, what freeing leaves, and copies.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The handles of every entity freed since the last take, from the queue.
std::vector<std::uint64_t> take_freed() {
    std::vector<std::uint64_t> freed;
    std::array<std::uint64_t, 4> some{};
    for (;;) {
        const std::int32_t count = fbe_entity_freed(some.data(), some.size());
        freed.insert(freed.end(), some.begin(), some.begin() + count);
        if (std::cmp_less(count, some.size())) {
            return freed;
        }
    }
}

/// Empties the freed queue.
void drop_freed() noexcept {
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {
    }
}

std::vector<std::uint64_t> children(std::uint64_t entity) {
    std::vector<std::uint64_t> found(static_cast<std::size_t>(fbe_entity_child_count(entity)));
    fbe_entity_children(entity, found.data(), static_cast<std::int32_t>(found.size()));
    return found;
}

std::uint64_t pivot(std::uint64_t world, std::uint64_t parent = 0) {
    return fbe_entity_create(world, parent, FBE_ENTITY_PIVOT);
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
        drop_freed();
    }
};

} // namespace

TEST_CASE("a freed handle is never taken for the one made in its slot", "[scene]") {
    const test_world_t world;
    const std::uint64_t first = pivot(world.handle);
    REQUIRE(first != 0);
    REQUIRE(fbe_entity_free(first));
    const std::uint64_t second = pivot(world.handle);
    CHECK(static_cast<std::uint32_t>(second) == static_cast<std::uint32_t>(first));
    CHECK(second != first);
    CHECK_FALSE(fbe_entity_alive(first));
    CHECK(fbe_entity_alive(second));
    CHECK(fbe_entity_kind(first) == -1);
    CHECK(std::string_view(fbe_error()) == "the entity was freed");
    CHECK_FALSE(fbe_entity_set_name(first, "x"));
    CHECK(fbe_entity_name(first) == nullptr);
    CHECK(fbe_entity_create(world.handle, first, FBE_ENTITY_PIVOT) == 0);
    CHECK_FALSE(fbe_entity_alive(0));
    CHECK(fbe_entity_kind(0) == -1);
    CHECK(std::string_view(fbe_error()) == "no entity was given");
}

TEST_CASE("freeing an entity frees what's under it, and queues each", "[scene]") {
    const test_world_t world;
    take_freed();
    const std::uint64_t root = pivot(world.handle);
    const std::uint64_t child = pivot(world.handle, root);
    const std::uint64_t grandchild = pivot(world.handle, child);
    const std::uint64_t sibling = pivot(world.handle, root);
    REQUIRE(fbe_world_entity_count(world.handle) == 4);
    REQUIRE(fbe_entity_free(child));
    const std::vector<std::uint64_t> freed = take_freed();
    CHECK(freed.size() == 2);
    CHECK(std::ranges::find(freed, child) != freed.end());
    CHECK(std::ranges::find(freed, grandchild) != freed.end());
    CHECK(children(root) == std::vector{ sibling });
    CHECK(fbe_world_entity_count(world.handle) == 2);
    CHECK(take_freed().empty());
}

TEST_CASE("the freed queue hands out what doesn't fit the next time", "[scene]") {
    const test_world_t world;
    take_freed();
    const std::uint64_t root = pivot(world.handle);
    for (int i = 0; i < 9; ++i) {
        pivot(world.handle, root);
    }
    fbe_entity_free(root);
    std::array<std::uint64_t, 4> some{};
    CHECK(fbe_entity_freed(some.data(), 4) == 4);
    CHECK(fbe_entity_freed(some.data(), 4) == 4);
    CHECK(fbe_entity_freed(some.data(), 4) == 2);
    CHECK(fbe_entity_freed(some.data(), 4) == 0);
    CHECK(fbe_entity_freed(nullptr, 0) == 0);
}

TEST_CASE("children and roots keep their order through reparenting", "[scene]") {
    const test_world_t world;
    const std::uint64_t a = pivot(world.handle);
    const std::uint64_t b = pivot(world.handle);
    const std::uint64_t c = pivot(world.handle, a);
    std::array<std::uint64_t, 1> one{};
    CHECK(fbe_world_roots(world.handle, one.data(), 1) == 2);
    CHECK(one[0] == a);
    REQUIRE(fbe_entity_set_parent(b, a, true));
    CHECK(children(a) == std::vector{ c, b });
    REQUIRE(fbe_entity_set_parent(c, 0, true));
    std::array<std::uint64_t, 2> roots{};
    CHECK(fbe_world_roots(world.handle, roots.data(), 2) == 2);
    CHECK(roots == std::array{ a, c });
    CHECK(fbe_entity_child(a, 0) == b);
    CHECK(fbe_entity_child(a, 1) == 0);
    CHECK(fbe_entity_child(a, -1) == 0);
    CHECK_FALSE(fbe_entity_set_parent(a, b, true));
    CHECK(std::string_view(fbe_error()) == "an entity can't go under what's under it");
    CHECK_FALSE(fbe_entity_set_parent(a, a, true));
    CHECK(std::string_view(fbe_error()) == "an entity can't be its own parent");
}

TEST_CASE("finding goes depth first, parents before children", "[scene]") {
    const test_world_t world;
    const std::uint64_t root = pivot(world.handle);
    const std::uint64_t first = pivot(world.handle, root);
    const std::uint64_t deep = pivot(world.handle, first);
    const std::uint64_t second = pivot(world.handle, root);
    fbe_entity_set_name(deep, "bone");
    fbe_entity_set_name(second, "bone");
    fbe_entity_set_name(root, "bone");
    CHECK(fbe_entity_find(root, "bone") == deep);
    CHECK(fbe_world_find(world.handle, "bone") == root);
    CHECK(fbe_entity_find(root, "none") == 0);
    CHECK(fbe_entity_find(root, nullptr) == 0);
    CHECK(fbe_entity_set_name(root, nullptr));
    CHECK(std::string_view(fbe_entity_name(root)).empty());
}

TEST_CASE("a copy is a deep copy, listed before it's put anywhere", "[scene]") {
    const test_world_t world;
    const test_world_t other;
    const std::uint64_t root = pivot(world.handle);
    const std::uint64_t child = pivot(world.handle, root);
    fbe_entity_set_name(child, "child");
    fbe_entity_set_hidden(child, true);

    const std::uint64_t inside = fbe_entity_copy(root, 0, child);
    REQUIRE(inside != 0);
    CHECK(fbe_entity_parent(inside) == child);
    CHECK(fbe_entity_child_count(inside) == 1);
    const std::uint64_t copied_child = fbe_entity_child(inside, 0);
    CHECK(fbe_entity_child_count(copied_child) == 0);
    CHECK(std::string_view(fbe_entity_name(copied_child)) == "child");
    CHECK(fbe_entity_hidden(copied_child));
    CHECK(fbe_world_entity_count(world.handle) == 4);

    const std::uint64_t moved = fbe_entity_copy(root, other.handle, 0);
    CHECK(fbe_entity_world(moved) == other.handle);
    CHECK(fbe_entity_parent(moved) == 0);
    CHECK(fbe_world_entity_count(other.handle) == 4);
}

TEST_CASE("the main world stays, and a freed world takes nothing", "[scene]") {
    const std::uint64_t main = fbe_world_main();
    CHECK(main == fbe_world_main());
    CHECK_FALSE(fbe_world_free(main));
    CHECK(fbe_world_alive(main));
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t entity = pivot(world);
    REQUIRE(fbe_world_free(world));
    CHECK_FALSE(fbe_world_alive(world));
    CHECK_FALSE(fbe_entity_alive(entity));
    CHECK(pivot(world) == 0);
    CHECK(std::string_view(fbe_error()) == "the World was freed");
    CHECK(fbe_world_entity_count(world) == -1);
    CHECK(fbe_entity_create(main, 0, 99) == 0);
    take_freed();
}

TEST_CASE("an entity moved into another world takes what's under it", "[scene]") {
    const test_world_t first;
    const test_world_t second;
    const std::uint64_t moving = pivot(first.handle);
    const std::uint64_t under = pivot(first.handle, moving);
    const std::uint64_t there = pivot(second.handle);
    REQUIRE(fbe_entity_set_parent(moving, there, true));
    CHECK(fbe_entity_world(under) == second.handle);
    CHECK(fbe_world_entity_count(first.handle) == 0);
    CHECK(fbe_world_entity_count(second.handle) == 3);
}
