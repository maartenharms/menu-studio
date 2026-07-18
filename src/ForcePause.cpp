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
    // Souls strips kPausesGame in CreateMenu, before the engine's open-time
    // pause bookkeeping - so the engine SKIPS its own increment, and ours
    // replaces it. The bump is done by hand rather than through the by-name
    // engine setter, whose address is version-specific (a wrong AE address
    // there is a real hazard).
    //
    // CLOSE SIDE (reworked 2026-07-17, the 1.6.1170 freeze): 0.5.0 trusted the
    // engine to re-read our kept flag at close and decrement ("close-side
    // decrement is engine-owned"). Field-true on SE 1.5.97 + Skyrim Souls SE;
    // FALSE on AE 1.6.1170 + the "Skyrim Souls RE - Unpaused Menus 1.6" build -
    // the flag-keyed close decrement goes missing there, our +1 leaks, and the
    // world stays frozen after the menu closes (console still works, no other
    // menu opens - the field report). We no longer trust ANY close edge:
    // the record survives the close event, and the per-frame settle in
    // Reassert() reconciles the counter one frame later against the engine's
    // own invariant (numPausesGame == number of OPEN menus with kPausesGame),
    // taking our +1 back only if the engine did not. Runs off the frame
    // driver, which ticks while frozen - so it also self-heals a session that
    // is already stuck.

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
            "counter now {}). Settled against the engine after close.",
            a_menuName, ui->numPausesGame);
    }

    void OnMenuClosed(const std::string&) {
        // Intentionally nothing. The record stays until the per-frame settle
        // (Reassert) sees the menu actually gone - the frame AFTER the pump
        // finished its close bookkeeping - and reconciles the counter there.
        // Settling at the close EVENT would mean guessing the pump's ordering,
        // and trusting the close edge is exactly what froze AE 1.6 + Souls.
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
        // Per-frame, UI thread, ticks even while the sim is frozen. Three jobs:
        // (1) live force-pause toggle-off (FLICK panel) releases immediately;
        // (2) menus we hold and that are still open keep their flag asserted
        //     (a mod may re-strip kPausesGame mid-session);
        // (3) menus we hold that have LEFT the stack get SETTLED: the engine's
        //     invariant is numPausesGame == number of open menus with
        //     kPausesGame set. If the counter sits above that after one of our
        //     menus closed, the engine's flag-keyed close decrement missed our
        //     bump (AE 1.6 + Skyrim Souls 1.6 - the frozen-world field bug)
        //     and the excess +1 is ours to take back. On SE the engine
        //     balances it and the settle is a no-op. Corrections are bounded:
        //     one decrement per settled menu, only downward, only toward the
        //     invariant - a concurrent legitimate pause (another open pausing
        //     menu) raises `expected` and is never touched.
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
        for (auto it = g_forced.begin(); it != g_forced.end();) {
            if (auto menu = ui->GetMenu(*it)) {
                if (!menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                    menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
                }
                ++it;
                continue;
            }
            std::uint32_t expected = 0;
            for (const auto& open : ui->menuStack) {
                if (open && open->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                    ++expected;
                }
            }
            if (ui->numPausesGame > expected) {
                ui->numPausesGame--;
                spdlog::info(
                    "ForcePause: {} closed but the engine kept our pause (counter "
                    "{} > {} open pausing menus) - reclaimed, world resumes. (The "
                    "1.6 Souls build misses the flag-keyed close decrement.)",
                    *it, ui->numPausesGame + 1, expected);
            } else {
                spdlog::debug(
                    "ForcePause: {} closed balanced by the engine (counter {}, {} "
                    "open pausing menus).",
                    *it, ui->numPausesGame, expected);
            }
            it = g_forced.erase(it);
        }
    }

    void Reset() {
        // A load tears menus down without close events and resets the engine's
        // pause counter to the loaded state, so just drop our bookkeeping.
        g_forced.clear();
    }
}
