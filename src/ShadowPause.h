#pragma once

#include <string_view>

namespace MTB::ShadowPause {
    // r19 - A REAL PAUSING MENU THAT SKYRIM SOULS DOES NOT KNOW ABOUT.
    //
    // Every earlier attempt tried to make one of the PLAYER'S menus pause, and
    // Skyrim Souls exists to stop exactly that. It works from an explicit list
    // of vanilla menu names in SkyrimSoulsRE.ini (bInventoryMenu, bMagicMenu,
    // bTweenMenu, ...); a menu whose name is not in that list is never touched.
    // So instead of fighting for a menu Souls owns, we bring our own.
    //
    // This registers a menu with kPausesGame and no movie, and shows it for the
    // duration of a bubble session. The ENGINE then does all the pause
    // bookkeeping itself - numPausesGame is incremented on show and decremented
    // on hide, by the same code that handles every vanilla menu - so there is no
    // counter to hand-balance and the r6 underflow class cannot occur here.
    //
    // It also restores what freezeTime could not: with a genuinely pausing menu
    // on the stack the game IS in a pausing-menu state, so input routing and the
    // live-3D render path behave exactly as they do in vanilla. That is what the
    // preview spin and the backdrop ride on (see the note in ForcePause.cpp).
    void Register();                 // once, at plugin init
    void Show();                     // idempotent
    void Hide();                     // idempotent
    [[nodiscard]] bool IsUp();
    [[nodiscard]] bool IsAvailable();  // registration succeeded

    inline constexpr std::string_view kMenuName{ "MTB_StudioPause" };
}
