#pragma once

namespace MTB {
    // "Dressing room" view: while the bubble is armed, hide what crowds or
    // occludes the player in the menu camera - nearby NPCs (radius around the
    // player) and Furniture/MovableStatic refs intersecting the camera→player
    // corridor. Purely visual (NiAVObject app-cull flag), fully restored on
    // disarm/menu close/ForceReset; nothing touches AI, cells or saves.
    namespace Declutter {
        // Re-scan and hide (idempotent, accumulates). Main thread, armed only.
        void Refresh();
        // Un-hide everything we hid. Idempotent; safe to call on any path.
        void RestoreAll();
        // r44: Sky::Update runs in the UNPAUSED close/switch/exit window and
        // un-culls its own branch every frame (field: "during the transition
        // we see the skybox") - re-assert the named world-feeder culls per
        // gap frame until the at-black restore. No-op unless this arm culled.
        void ReassertWorldFeederCulls();
    }
}
