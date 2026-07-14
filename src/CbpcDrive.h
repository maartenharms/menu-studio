#pragma once

namespace MTB {
    // CBPC (cbp.dll) body physics under pause. CBPC self-clocks with QPC and
    // its updateActors() early-outs while the game is paused UNLESS its
    // internal `raceSexMenuOpen` flag is set - the author's own special case
    // that keeps body physics simulating inside the (paused) RaceMenu. While
    // the bubble is armed we set that flag, borrowing exactly that sanctioned
    // path; restored on disarm. Fingerprint-gated like the FSMP drive.
    namespace CbpcDrive {
        void Init();  // kDataLoaded
        bool IsAvailable();
        void SetSimulateWhilePaused(bool a_on);
    }
}
