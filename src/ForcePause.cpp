#include "PCH.h"

#include "ForcePause.h"

#include "Offsets.h"
#include "Settings.h"

#include <unordered_set>

namespace {
    // SE 1.5.97 decompile facts this unit stands on (mtb_pausecounter.c):
    // the UI message pump increments UI::numPausesGame on open and
    // decrements on close by RE-READING menuFlags & kPausesGame each time,
    // and the MenuOpenCloseEvent is queued AFTER the open bookkeeping. So
    // by the time our open sink runs, the engine's increment already
    // happened (or was skipped because Skyrim Souls stripped the flag in
    // the ctor), and one setter call here is engine-symmetric: whoever
    // holds the flag at close time gets the decrement.
    using SetMenuPausesGame_t = void(__fastcall*)(RE::UI*, const RE::BSFixedString&, bool);
    REL::Relocation<SetMenuPausesGame_t> g_setMenuPausesGame{ MTB::Offsets::SetMenuPausesGame };

    // Menus we re-flagged (names, not instances - instances die on close).
    // Menu events and the frame hook both run on the UI thread; no lock.
    std::unordered_set<std::string> g_forced;
    bool g_stripWarned = false;
}

namespace MTB::ForcePause {
    void EnsurePaused(const std::string& a_menuName) {
        const auto& settings = Settings::GetSingleton();
        if (!settings.enabled || !settings.forcePause) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        auto menu = ui->GetMenu(a_menuName);
        if (!menu || menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
            return;  // not up yet, or already pausing (vanilla / no Skyrim Souls)
        }

        const RE::BSFixedString name{ a_menuName };
        g_setMenuPausesGame(ui, name, true);
        if (menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
            g_forced.insert(a_menuName);
            spdlog::info(
                "ForcePause: {} opened UNPAUSED (Skyrim Souls?) - kPausesGame restored, "
                "pause counter now {}. Close-side decrement is engine-owned.",
                a_menuName, ui->numPausesGame);
        } else {
            spdlog::warn(
                "ForcePause: engine setter did not take on {} (menu missing from the "
                "registered map?) - bubble stays dormant for this menu session.",
                a_menuName);
        }
    }

    void OnMenuClosed(const std::string& a_menuName) {
        // The engine decrements numPausesGame itself (flag re-read at close);
        // only our bookkeeping dies here.
        g_forced.erase(a_menuName);
    }

    void Reassert() {
        if (g_forced.empty()) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        for (const auto& name : g_forced) {
            auto menu = ui->GetMenu(name);
            if (menu && !menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                // Someone re-stripped the flag AFTER our open-time restore.
                // Skyrim Souls only strips in menu ctors, so this should
                // never fire - but a lost flag means the close-time
                // decrement is lost with it (permanent pause leak). Repair
                // is flag-only: a flag-stripper never touched the counter.
                menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
                if (!g_stripWarned) {
                    g_stripWarned = true;
                    spdlog::warn(
                        "ForcePause: {} lost kPausesGame while open - re-set. Another mod "
                        "strips menu flags at runtime; report this mod combination.",
                        name);
                }
            }
        }
    }

    void Reset() {
        g_forced.clear();
    }
}
