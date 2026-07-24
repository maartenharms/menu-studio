#pragma once

namespace MTB::MovementArmPolicy {
    struct Input {
        bool     locomoting = false;
        bool     grounded = false;
        bool     swimming = false;
        unsigned directionMask = 0;
    };

    struct Plan {
        bool freezeCaughtPose = false;
        bool runSyntheticSettle = false;
        bool createExitRestartDebt = false;
        bool preserveDirectionBits = false;
    };

    // A moving arm keeps the exact pose and movement state the live world
    // supplied. Advancing a synthetic stop transition inside the paused menu
    // made direction-change momentum affect preview placement, then required a
    // delayed moveStart that visibly slid on exit. Standing arms remain live.
    [[nodiscard]] constexpr Plan Choose(const Input& a_input) noexcept {
        // Direction bits are part of the same live movement transaction as the
        // graph. Erasing them directly in a paused menu does not emit Skyrim's
        // normal input-release edge, so the graph can remain in locomotion
        // after close and select a looping weapon stance-start idle later.
        const bool preserveDirectionBits =
            a_input.locomoting || a_input.directionMask != 0;
        if (!a_input.locomoting || !a_input.grounded || a_input.swimming) {
            return {
                .preserveDirectionBits = preserveDirectionBits,
            };
        }
        return {
            .freezeCaughtPose = true,
            .runSyntheticSettle = false,
            .createExitRestartDebt = false,
            .preserveDirectionBits = preserveDirectionBits,
        };
    }
}
