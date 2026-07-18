#pragma once

#include <string>

namespace MTB::OwnView {
    // F-15 phase 2: the bubble owns the third-person preview framing
    // whenever no view mod does - the full SPIM RotateCamera() recipe
    // (clean-room from the author's MIT source, Tools/ShowPlayerInMenus),
    // which is exactly the "Show Player In Inventory" look the user knows.

    // Does the bubble own the view for this arm? First-person arms are the
    // r30-proven rule (a first-person camera means no view mod switched);
    // third-person arms are owned when no loaded view mod covers a_menuName.
    [[nodiscard]] bool ShouldOwn(const std::string& a_menuName, bool a_firstPersonArm);

    // Apply the framing (saves every original first). a_forcedThirdFromFirst
    // = this arm forced the camera out of first person (hand it back at exit).
    void ApplyFraming(bool a_forcedThirdFromFirst, bool a_mounted = false);

    // Menu-close exit: full restore, SPIM ResetCamera order (first person
    // handed back first, camera update, mouse-wheel zoom speed LAST).
    void Disarm();

    // ForceReset (load/new game): restore only the process-global surfaces
    // (INI Settings, FOV, the persistent third-person state object) - actor
    // data and the camera stack belong to the incoming save.
    void DropOnLoad();

    [[nodiscard]] bool Active();

    // Is Show Player In Menus loaded? (One module scan per session - DLLs
    // cannot hot-load.) SPIM rotates on RIGHT-MOUSE HELD, the same input as
    // our preview spin, so the spin tick needs to know it is there; see the
    // harvest branch in Bubble.cpp. Main-thread callers only, like the
    // coverage checks it shares its scan with.
    [[nodiscard]] bool SpimPresent();
}
