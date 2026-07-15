#include "PCH.h"

#include "ForcePause.h"

#include "Settings.h"

#include <unordered_set>

namespace {
    // Covered menus we re-paused (names, not instances - instances die on
    // close). Menu events and the frame hook both run on the UI thread, so no
    // lock is needed.
    std::unordered_set<std::string> g_forced;
}

namespace MTB::ForcePause {
    // A covered menu that Skyrim Souls opened UNPAUSED is turned back into a real
    // paused menu: we set its kPausesGame flag AND bump RE::UI::numPausesGame by
    // hand. The flag matters for more than the frozen sim - the menu has to be a
    // pausing menu for input routing and the live-3D render path, which is what
    // the camera rotation and the backdrop ride on (with only the counter bumped
    // they break: rotation input is not captured, and the backdrop only shows
    // when a genuinely pausing menu like the console is also up).
    //
    // Souls strips kPausesGame exactly once, in CreateMenu, before the engine's
    // open-time pause bookkeeping - so the engine SKIPS its own increment, and
    // ours replaces it. We keep the flag set for the life of the menu (Reassert),
    // so the engine re-reads it at close and does the matching decrement itself;
    // the normal close path therefore needs no counter code from us. The bump is
    // done by hand rather than through the by-name engine setter, whose address
    // is version-specific (a wrong AE address there is a real hazard).

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
        if (g_forced.contains(a_menuName)) {
            return;  // already holding this one - never double up
        }
        menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
        ui->numPausesGame++;
        g_forced.insert(a_menuName);
        spdlog::info(
            "ForcePause: {} opened UNPAUSED (Skyrim Souls?) - re-paused (flag set, "
            "counter now {}). Close-side decrement is engine-owned.",
            a_menuName, ui->numPausesGame);
    }

    void OnMenuClosed(const std::string& a_menuName) {
        // The engine decrements numPausesGame at close by re-reading the flag we
        // kept set, so there is nothing to release here - just drop our record.
        g_forced.erase(a_menuName);
    }

    void ReleaseAll() {
        // Hand every menu we hold back to its unpaused state RIGHT NOW (live
        // force-pause toggle-off, or a disarm). We clear the flag ourselves so
        // the engine's later close bookkeeping will NOT also decrement, then drop
        // our own increment - balanced, no double decrement.
        if (g_forced.empty()) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            g_forced.clear();
            return;
        }
        for (const auto& name : g_forced) {
            if (auto menu = ui->GetMenu(name)) {
                menu->menuFlags.reset(RE::UI_MENU_FLAGS::kPausesGame);
            }
            if (ui->numPausesGame > 0) {
                ui->numPausesGame--;
            }
        }
        g_forced.clear();
    }

    void Reassert() {
        // Per-frame, UI thread. If force-pause was switched off live (e.g. the
        // FLICK panel, which flips the setting on the render thread) while we
        // still hold menus, release them now so the open menu un-freezes at once.
        // Otherwise keep our menus flagged as pausing - a belt-and-braces guard
        // against a mod that re-strips kPausesGame mid-session (Skyrim Souls only
        // strips once at creation, so under Souls alone this rarely does work).
        if (g_forced.empty()) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        if (!Settings::GetSingleton().forcePause) {
            ReleaseAll();
            return;
        }
        for (const auto& name : g_forced) {
            if (auto menu = ui->GetMenu(name);
                menu && !menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
            }
        }
    }

    void Reset() {
        // A load tears menus down without close events and resets the engine's
        // pause counter to the loaded state, so just drop our bookkeeping.
        g_forced.clear();
    }
}
