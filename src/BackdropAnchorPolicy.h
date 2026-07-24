#pragma once

namespace MTB::BackdropAnchorPolicy {

    enum class Anchor {
        kPlayer,
        kCamera,
    };

    inline constexpr unsigned kSettledRefitFrames = 4;

    // Transparent domes must not sit at camera distance zero: Skyrim sorts
    // them after the actor and their texture overlays the character. Keep all
    // pieces player-centred; the distinction is in the post-arm refit below.
    [[nodiscard]] constexpr Anchor Choose([[maybe_unused]] bool a_isDome) {
        return Anchor::kPlayer;
    }

    // Backdrop::Apply() can run while the scene parent's world transform still
    // belongs to the preceding gameplay frame. Refit only spherical pieces
    // across the first few settled menu ticks; stages need no such correction.
    [[nodiscard]] constexpr bool ShouldRefitAfterArm(
        bool a_isDome, unsigned a_framesRemaining) {
        return a_isDome && a_framesRemaining > 0;
    }

}  // namespace MTB::BackdropAnchorPolicy
