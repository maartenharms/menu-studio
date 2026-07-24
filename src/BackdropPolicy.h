#pragma once

namespace MTB::BackdropPolicy {

    inline constexpr float kBackgroundRadiusDefault = 800.0f;
    inline constexpr float kBackgroundRadiusMin     = 500.0f;
    inline constexpr float kBackgroundRadiusMax     = 1200.0f;

    [[nodiscard]] constexpr float ClampBackgroundRadius(float a_radius) {
        return a_radius < kBackgroundRadiusMin ? kBackgroundRadiusMin :
               a_radius > kBackgroundRadiusMax ? kBackgroundRadiusMax :
                                                  a_radius;
    }

}  // namespace MTB::BackdropPolicy
