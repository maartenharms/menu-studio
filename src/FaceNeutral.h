#pragma once

namespace MTB::FaceNeutral {
    // F-13: standardize the face while a bubble menu is open. Papyrus
    // expression mods are frozen in paused menus (B-6) - an arm can catch
    // the face MID-SEQUENCE (half a blink, a stuck brow) and hold it there
    // for the whole session. At arm the current expression/modifier/
    // phoneme keyframes are SAVED and the engine's own facegen reset ramps
    // the face to neutral (our face tick animates the ramp - a dissolve,
    // not a snap; the engine's ambient blink generator keeps running).
    // The saved values come back at disarm; a load drops them with the 3D.
    void Apply();    // arm edge: save + ramp to neutral
    void Restore();  // Disarm: write the saved expression back
    void Drop();     // ForceReset: face data dies with the load
}
