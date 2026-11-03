// Layers: which of a world's bodies collide with which, which Jolt asks of its broad phase and its
// narrow phase as it steps, and of casts.

#include "physics/physics.hpp"

#include <cstdint>

namespace fbe::physics {

namespace {

constexpr JPH::BroadPhaseLayer not_moving_broad_phase{ 0 };
constexpr JPH::BroadPhaseLayer moving_broad_phase{ 1 };

} // namespace

layers_t::layers_t() {
    m_collides.fill(~std::uint32_t{ 0 });
}

bool layers_t::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const {
    if (!moves(a) && !moves(b)) {
        return false;
    }
    return collides(layer_of(a), layer_of(b));
}

bool layers_t::collides(std::uint32_t a, std::uint32_t b) const {
    return ((m_collides[a] >> b) & 1U) != 0;
}

void layers_t::set_collides(std::uint32_t a, std::uint32_t b, bool collides) {
    if (collides) {
        m_collides[a] |= 1U << b;
        m_collides[b] |= 1U << a;
    } else {
        m_collides[a] &= ~(1U << b);
        m_collides[b] &= ~(1U << a);
    }
}

JPH::uint broad_phase_t::GetNumBroadPhaseLayers() const {
    return 2;
}

JPH::BroadPhaseLayer broad_phase_t::GetBroadPhaseLayer(JPH::ObjectLayer layer) const {
    return moves(layer) ? moving_broad_phase : not_moving_broad_phase;
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* broad_phase_t::GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const {
    return layer == moving_broad_phase ? "moving" : "not moving";
}
#endif

bool object_vs_broad_phase_t::ShouldCollide(JPH::ObjectLayer layer,
                                            JPH::BroadPhaseLayer broad_phase) const {
    return broad_phase == moving_broad_phase || moves(layer);
}

} // namespace fbe::physics
