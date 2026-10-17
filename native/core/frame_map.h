#pragma once

// Fixed absolute mapping between Lily (EL world) and her Lilac puppet (EM world) (FR-MOV-5).
// Scale is 1 (both games use UU). Y is the side-scroll axis, Z is up; X is not mapped
// (EL keeps Lily's own plane, EM keeps Lilac's plane). Positions are FOOT positions
// (actor Z minus capsule half-height, both half-heights read at runtime), so differently sized
// capsules stand on the same surface.
//
//   guest = G0 + (host - H0)        host = H0 + (guest - G0)
//
// The Guest owns the mapping (it is the authority for Lily's body). A rebase declares "Lily's
// current foot <-> Lilac's current foot" and increments the epoch; a recenter shifts G0 together
// with Lily and the whole terrain pool (relative geometry unchanged, invisible to the player).

#include <cstdint>

namespace lie {

struct FrameMap {
    std::uint32_t epoch = 0;
    double h0_y = 0.0, h0_foot = 0.0; // EM anchor (Lilac foot)
    double g0_y = 0.0, g0_foot = 0.0; // EL anchor (Lily foot)

    bool valid() const { return epoch != 0; }

    double guest_y(double host_y) const { return g0_y + (host_y - h0_y); }
    double guest_z(double host_z) const { return g0_foot + (host_z - h0_foot); }
    double host_y(double guest_y) const { return h0_y + (guest_y - g0_y); }
    double host_z(double guest_z) const { return h0_foot + (guest_z - g0_foot); }

    // Actor-centre helpers: capsule half-heights come from the live capsules
    double guest_actor_z(double host_actor_z, double hh_em, double hh_el) const { return guest_z(host_actor_z - hh_em) + hh_el; }
    double host_actor_z(double guest_actor_z, double hh_el, double hh_em) const { return host_z(guest_actor_z - hh_el) + hh_em; }

    static FrameMap rebase(std::uint32_t prev_epoch, double host_y, double host_foot, double guest_y, double guest_foot) {
        return FrameMap{prev_epoch + 1, host_y, host_foot, guest_y, guest_foot};
    }

    // Shift the EL side by (dy, dz): Lily and every mirrored box move by the same offset.
    FrameMap recentered(double dy, double dz) const {
        FrameMap m = *this;
        ++m.epoch;
        m.g0_y += dy;
        m.g0_foot += dz;
        return m;
    }
};

} // namespace lie
