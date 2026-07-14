#pragma once

#include <string>
#include <string_view>

namespace MTB::ForcePause {
    // §3.1 force-pause ownership: a bubble menu that arrives WITHOUT
    // kPausesGame (Skyrim Souls strips it in the menu ctor, before the UI
    // pump's pause bookkeeping) gets the flag restored and the pause counter
    // bumped through the engine's own by-name setter. The close side needs
    // no code at all: the UI pump re-reads the flag at close time and
    // decrements symmetrically. Menus outside sMenus are never touched, so
    // Skyrim Souls keeps its behavior everywhere else.
    void EnsurePaused(const std::string& a_menuName);  // menu-open event, in-stack
    void OnMenuClosed(const std::string& a_menuName);  // bookkeeping only
    void Reassert();  // per-frame tripwire while bubble menus are open
    void Reset();     // load/new-game backstop (menu instances died with the load)
}
