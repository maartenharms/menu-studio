#pragma once

namespace MTB {
    // Direct drive of FSMP (Faster HDT-SMP) 2.5.0 while the game is paused.
    //
    // FSMP suspends itself on either engine pause signal and its step decision
    // happens inside the same onEvent call (docs/SPIKE-B-FSMP.md), so the only
    // spike-sized unlock is to run its own public step path ourselves:
    //   m_suspended = false; per-system readTransform(dt); doUpdate2ndStep(...).
    // Addresses come from the PDB that ships with the Nolvus FSMP install and
    // are gated on an exact PE fingerprint of hdtSMP64.dll - any other build
    // logs one error and stays dormant (animation ticking is unaffected).
    namespace FsmpDrive {
        void Init();             // call at kDataLoaded (FSMP module resolvable)
        bool IsAvailable();
        void Step(float a_dt);   // main thread only, once per frame while armed

        // FSMP clamps the PLAYER's rotation speed (m_rotationSpeedLimit,
        // default 10 rad/s) and hard-resets the sim past a threshold angle
        // (m_unclampedResetAngle) - menu-drag rotation trips both, killing
        // momentum. true: save the originals and lift the thresholds;
        // false: restore. Idempotent, process-local (FSMP re-reads its own
        // config only at startup).
        void SetRotationFreedom(bool a_free);
    }
}
