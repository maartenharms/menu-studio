#pragma once

#include <string>
#include <string_view>

namespace MTB::ForcePause {
    // §3.1 force-pause ownership: a bubble menu that arrives WITHOUT kPausesGame
    // (Skyrim Souls strips it) gets one pause held on RE::UI::numPausesGame
    // directly at open and released at close - a self-balanced pair we own, so a
    // flag-stripping mod cannot swallow the release and leave the world frozen.
    // Menus outside sMenus are never touched, so Skyrim Souls keeps its behavior
    // everywhere else.
    void EnsurePaused(const std::string& a_menuName);  // menu-open event, in-stack
    void OnMenuClosed(const std::string& a_menuName);  // release our held pause
    void Reassert();     // per-frame, UI thread: release live if force-pause was turned off
    void ReleaseAll();   // drop every pause we hold right now (live toggle / disarm)
    void Reset();        // load/new-game backstop (menu instances died with the load)
}
