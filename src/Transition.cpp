#include "PCH.h"

#include "Transition.h"

#include "Settings.h"

#include <algorithm>

namespace {
    // Linear ramp position + target; Value() applies the easing. With
    // transitions disabled (or a zero duration) every entry point snaps,
    // which is exactly the pre-F-12 behavior.
    float g_value = 0.0f;
    float g_target = 0.0f;

    bool Disabled(float a_seconds) {
        return !MTB::Settings::GetSingleton().sleekTransitions || a_seconds <= 0.0f;
    }
}

namespace MTB::Transition {
    void SetTarget(float a_target) {
        g_target = a_target;
        const auto& cfg = Settings::GetSingleton();
        const float seconds = a_target > g_value ? cfg.transitionInSeconds
                                                 : cfg.transitionOutSeconds;
        if (Disabled(seconds)) {
            g_value = a_target;
        }
    }

    void Start(float a_value) {
        g_value = a_value;
    }

    void Snap(float a_value) {
        g_value = g_target = a_value;
    }

    void Tick(float a_dt) {
        if (g_value == g_target) {
            return;
        }
        const auto& cfg = Settings::GetSingleton();
        const float seconds = g_target > g_value ? cfg.transitionInSeconds
                                                 : cfg.transitionOutSeconds;
        if (Disabled(seconds)) {
            g_value = g_target;
            return;
        }
        const float step = a_dt / seconds;
        g_value = g_target > g_value ? (std::min)(g_value + step, g_target)
                                     : (std::max)(g_value - step, g_target);
    }

    float Value() {
        // Smoothstep: gentle takeoff and landing - the "sleek" half of the
        // request; the fade itself is the "minimum" half.
        const float t = std::clamp(g_value, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    bool FadingOut() {
        return g_target == 0.0f && g_value > 0.0f;
    }
}
