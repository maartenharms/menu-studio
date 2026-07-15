#pragma once

namespace MTB::FootIkGate {
    // "Inverse Kinematics - Feet of Skyrim" (FeetOfSkyrim.dll) forces foot IK
    // onto the real ground. In the void / dressing room that ground is hidden
    // under the stage, so the feet plant at the wrong height; stand FIF down
    // while armed so they keep the neutral animated pose. Soft dependency:
    // inert when the DLL is absent or an unrecognized build.
    void Init();               // kDataLoaded
    bool IsAvailable();
    void SetSuppressed(bool a_on);  // true = disable FIF foot IK; false = restore
}
