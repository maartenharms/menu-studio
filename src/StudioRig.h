#pragma once

namespace MTB::StudioRig {
    // Three-point studio lighting (key / fill / rim) around the player while
    // the bubble is armed in void mode. Pure render-side lights: engine
    // NiPointLights registered with the ShadowSceneNode - no LIGH forms, no
    // placed references, nothing a savegame could ever see.
    void Apply();     // arm edge, after StudioLight::Apply
    void Tick();      // per armed frame: re-assert transforms + bounds
    void PushFade();  // F-12: fade-only refresh for the teardown grace window
    void Remove();    // Disarm + ForceReset (all three exits per SPEC §4)
}
