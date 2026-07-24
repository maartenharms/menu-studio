#pragma once

namespace MTB::CameraCollisionPolicy {

    enum class Action {
        kRunOriginal,
        kBypass,
    };

    struct Input {
        bool enabled;
        bool bubbleActive;
    };

    [[nodiscard]] constexpr Action Decide(const Input& a_input) {
        return a_input.enabled && a_input.bubbleActive
                   ? Action::kBypass
                   : Action::kRunOriginal;
    }

}  // namespace MTB::CameraCollisionPolicy
