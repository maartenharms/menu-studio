#pragma once

namespace MTB {
    // Vanilla third-person camera collision: ThirdPersonState's position
    // builder (RVA 0x850260, ID 49975) finishes by calling the collision
    // smoother (0x850A80, ID 49980), which runs the obstruction check
    // (0x84C870) and LERPs the camera from the last unobstructed spot toward
    // the corrected one - the visible pull-in when orbiting near walls in a
    // bubble menu (SPIM runs camera-orbit mode; declutter hides the wall's
    // RENDER but its havok stays). While the bubble is active we skip that
    // single call, leaving the camera exactly where the orbit math put it.
    // Gameplay is untouched - outside the bubble the original always runs.
    namespace CameraGate {
        void Install();  // SKSEPlugin_Load, after AllocTrampoline
    }
}
