#pragma once

namespace MTB::WeaponPumpPolicy {

    enum class Action {
        kContinue,
        kStopNothingToDrive,
        kStopTransitionSettled,
        kHoldBeforeIdle,
        kStopIdleRepicked,
    };

    struct Input {
        bool  bounded = false;
        bool  sawTransition = false;
        bool  transitionUnfinished = false;
        bool  idlePickChanged = false;
        int   stepsTaken = 0;
        int   blindStepLimit = 0;
        float transitionRemainingSeconds = -1.0f;  // negative = unavailable
        float stepSeconds = 0.0f;
    };

    [[nodiscard]] constexpr Action Decide(const Input& a_input) noexcept {
        if (!a_input.bounded) {
            return Action::kContinue;
        }
        if (a_input.sawTransition) {
            if (!a_input.transitionUnfinished) {
                return Action::kStopTransitionSettled;
            }
            if (a_input.transitionRemainingSeconds >= 0.0f &&
                a_input.stepSeconds > 0.0f &&
                a_input.transitionRemainingSeconds <= a_input.stepSeconds) {
                return Action::kHoldBeforeIdle;
            }
            return Action::kContinue;
        }
        if (a_input.idlePickChanged) {
            return Action::kStopIdleRepicked;
        }
        if (a_input.stepsTaken >= a_input.blindStepLimit) {
            return Action::kStopNothingToDrive;
        }
        return Action::kContinue;
    }

}  // namespace MTB::WeaponPumpPolicy
