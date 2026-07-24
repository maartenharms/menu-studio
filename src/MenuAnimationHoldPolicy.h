#pragma once

namespace MTB::MenuAnimationHoldPolicy {

    struct Input {
        bool armEdgeHeld{ false };
        bool freezeDrawSheathe{ false };
        bool equipClipInFlight{ false };
        bool equipOccurredThisSession{ false };
    };

    // A paused actor cannot finish the framework state changes associated with
    // an equip. Once an equip clip has activated in this menu, keep the body
    // graph on its caught frame until the world resumes. Releasing the hold
    // merely because the clip's wall-clock cap elapsed lets the next menu tick
    // select an idle against stale state and latches the lunge loop.
    [[nodiscard]] constexpr bool ShouldHold(const Input& a_input) {
        return a_input.armEdgeHeld ||
               (a_input.freezeDrawSheathe &&
                (a_input.equipClipInFlight || a_input.equipOccurredThisSession));
    }

}  // namespace MTB::MenuAnimationHoldPolicy
