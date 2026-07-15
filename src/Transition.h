#pragma once

namespace MTB::Transition {
    // F-12: one eased scalar fades the studio in at arm and back out
    // through the teardown grace - the rig lights and the backdrop pieces
    // multiply their visuals by Value() every frame. Deliberately NO
    // engine ImageSpaceModifiers: whether imod ramps advance in a paused
    // menu is unproven (a frozen mid-dip fade would black the screen for
    // the whole menu), and SPIM already owns the vanilla menu-blur imod
    // in this load order. Everything here is state we already own and
    // re-assert per frame, so no exit path can strand a half-faded look.
    void SetTarget(float a_target);  // 1 while armed, 0 from the close edge
    void Start(float a_value);       // fresh arm: put the ramp at a known point
    void Snap(float a_value);        // instant, no ramp (ForceReset / disarm backstop)
    void Tick(float a_dt);           // step toward the target on the caller's clock

    [[nodiscard]] float Value();      // eased 0..1
    [[nodiscard]] bool  FadingOut();  // exit ramp still running (holds the teardown)
}
