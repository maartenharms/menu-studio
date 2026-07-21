#pragma once

namespace MTB::StudioRig {
    // Three-point studio lighting (key / fill / rim) around the player while
    // the bubble is armed in void mode. Pure render-side lights: engine
    // NiPointLights registered with the ShadowSceneNode - no LIGH forms, no
    // placed references, nothing a savegame could ever see.
    void Apply();     // arm edge, after StudioLight::Apply
    void Tick();      // per armed frame: re-assert transforms + bounds
    void PushFade();

    // r28d DIAGNOSTIC. Logs what each rig light looks like AT THE TOP of our
    // tick - i.e. what SURVIVED the engine's previous frame: bound radius,
    // culled flag, fade, parent, world position, and Transition::Value().
    //
    // Exists because the live-menu rig reports "3 light(s) up" while the field
    // sees nothing, brightness included ("even changing their brightness didn't
    // do anything"). Everything our own writes can prove is proven; what is
    // missing is what the LIVE engine does to the lights between our frames -
    // a pass the paused studio never runs, which would explain "works fine in
    // regular mode" exactly. Read before re-asserting, or the probe measures
    // our own writes instead of the engine's.
    void LogLiveState(const char* a_why);  // F-12: fade-only refresh for the teardown grace window
    void Remove();    // Disarm + ForceReset (all three exits per SPEC §4)
}
