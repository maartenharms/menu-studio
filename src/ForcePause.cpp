#include "PCH.h"

#include "ForcePause.h"

#include "Settings.h"
#include "ShadowPause.h"

#include <unordered_map>
#include <unordered_set>

namespace {
    // Covered menus we re-paused (names, not instances - instances die on
    // close). Menu events and the frame hook both run on the UI thread, so no
    // lock is needed.
    std::unordered_set<std::string> g_forced;

    // r14 ROOT CAUSE: THE MENU-OPEN EVENT IS NOT A RELIABLE PLACE TO READ THE
    // MENU. EnsurePaused ran once, off that event, and gave up permanently when
    // RE::UI::GetMenu returned null - "not up yet" was treated as "nothing to
    // do". On a menu SWITCH the open event can fire before the new menu is
    // registered in the UI map, so the lookup misses, we never take the pause,
    // Skyrim Souls keeps the menu live, and the bubble reads UNPAUSED and stays
    // dormant. Field 2026-07-19, switching Inventory -> Magic:
    //
    //   ForcePause: InventoryMenu opened UNPAUSED ... re-paused (counter now 2)
    //   Bubble menu open but game is UNPAUSED (Skyrim Souls?) - bubble dormant
    //   ForcePause: InventoryMenu closed balanced by the engine (counter 0)
    //
    // with NO MagicMenu line at all between them - the take never happened.
    // That is the black backdrop and the running clock in the magic menu.
    //
    // So a take that cannot be completed now becomes PENDING and is retried from
    // the per-frame settle until it lands, the menu turns out to be genuinely
    // pausing, or the budget runs out. One-shot work driven off an event with no
    // ordering guarantee is the bug; per-frame reconciliation against the
    // engine's actual state is the fix, and it is the same shape as the close
    // side already uses.
    //
    // ⚠ This is NOT the r6 re-assert. Pending only ever covers menus we have not
    // taken yet; the instant one is taken it moves to g_forced (whose flag is
    // only ever touched again by the BOUNDED r16 retake below).
    //
    // r16 FIELD NOTE (2026-07-20): the map-miss race this layer was built for
    // never fired in the verifying field run - every take succeeded first try,
    // and the switch bug turned out to live one step LATER (Souls strips the
    // flag after a successful take; see g_retakes). This stays as defense in
    // depth for the race it covers, but it is no longer believed to be the
    // switch bug's cause.
    std::unordered_map<std::string, int> g_pending;

    // ~1s at 60fps. Bounded so a menu that never materialises cannot be retried
    // forever; if this expires something is wrong and it says so.
    constexpr int kPendingFrames = 60;

    // r16 - THE SWITCH BUG'S REAL MECHANISM (field 2026-07-20, r15 log). The r14
    // theory ("the open event beats the UI map, the take never happens") was
    // field-REFUTED: the take succeeded on every switch ("re-paused ... counter
    // now 1" each time, zero "queued for retry" lines). What actually happens is
    // one step later: Skyrim Souls ANSWERS the take. On a switch the outgoing
    // menu's pause is already released before the incoming menu's open event, so
    // for 1-2 frames the world genuinely runs - and in that window Souls strips
    // the flag we just set and takes the counter back. Timings, one per switch:
    // take 36.595 -> stripped by 36.600; take 37.640 -> stripped by 37.646;
    // take 38.482 -> stripped by 38.486. On a FRESH open the take lands inside
    // an already-paused window (TweenMenu bridges it), and a set flag under a
    // paused world holds indefinitely - 2.1s and 9.8s in the same log. So the
    // fight only exists in the churn window; win one round and it is over.
    //
    // Why re-taking is safe where the r6 re-assert was catastrophic: r6 re-set
    // the FLAG ONLY, every frame, forever - Souls answered each set with a
    // balanced strip (flag + decrement), so the net was -1 per frame until the
    // unsigned counter wrapped. A RE-TAKE is flag + increment, and the same
    // field log proves Souls' answer is balanced (counter went 1 -> 0 exactly
    // when the flag vanished, landing on the engine invariant: "counter 0, 0
    // open pausing menus"). Balanced take vs balanced strip = zero drift per
    // round even if we lose every round. Bounded anyway: kMaxRetakes rounds,
    // then concede and go dormant, which is exactly today's behavior.
    std::unordered_map<std::string, int> g_retakes;
    constexpr int kMaxRetakes = 3;

    // r17 - THE FLAG WAR IS UNWINNABLE, SO STOP FIGHTING IT. The r16 retake ran
    // one field session (2026-07-20 09:14): 63 takes, 189 retakes, 63
    // concessions - Souls re-strips EVERY FRAME, even while the world is paused
    // (strips 6ms apart with the bubble armed). And the same log exposed that
    // the "fresh opens hold for seconds" evidence behind r16 was an illusion:
    // those sessions rode the TWEEN MENU's own rider pause while our flag was
    // long gone (the take said "counter now 2", the first armed tick already
    // read numPauses=1 - our +1 was stripped ~7ms in, every time, always).
    //
    // The decisive positive: 109 armed sessions ran FLAGLESS under rider pauses
    // with spin input, backdrop and declutter all working. The flag was never
    // the load-bearing part; the PAUSE is. So under Skyrim Souls we no longer
    // touch menu flags or numPausesGame at all - we hold RE::Main::freezeTime
    // (the `tfc 1` mechanism: world frozen, rendering/input/UI alive) for as
    // long as a covered menu is open, plus a short linger to bridge the
    // close->open gap of a menu switch. Souls has nothing to strip, there is no
    // counter to drift, and the r6 bug class cannot exist here.
    //
    // The hold is guarded: taken only if freezeTime is currently false (a `tfc
    // 1` user or another freezer keeps theirs), released only if we hold it,
    // reconciled per frame against the UI map (a menu that dies without a close
    // event cannot strand the freeze), and dropped immediately on the live
    // panel toggle, ReleaseAll and Reset.
    std::unordered_set<std::string> g_covered;   // covered menus currently open (Souls mode)
    bool g_freezeHeld  = false;                  // WE set Main::freezeTime (never clear another's)
    int  g_lingerLeft  = 0;                      // frames of freeze kept past the last close
    constexpr int kFreezeLingerFrames = 20;      // ~115ms @175fps, ~333ms @60fps - covers the
                                                 // field-measured 32ms switch gap with margin

    bool SoulsMode() {
        return MTB::Settings::GetSingleton().soulsLoaded;
    }

    // r19: prefer our own pausing menu over the raw clock freeze. freezeTime
    // stops the world but leaves the game in a NON-pausing-menu state, and the
    // field caught the consequence: after switching Inventory -> Magic the log
    // showed 11 ticks at numPauses=0, and gameplay input routing came back with
    // it (the mouse drove the camera at the screen edges). The borrowed pause
    // that made every other case look fine was the TWEEN MENU's - Souls leaves
    // bTweenMenu paused, so opening a menu from the world rides its pause, and
    // a switch drops it. ShadowPause supplies a real one that Souls never sees.
    bool UseShadowMenu() {
        return MTB::Settings::GetSingleton().shadowPause && MTB::ShadowPause::IsAvailable();
    }

    // r19b: "do we currently hold a pause, by ANY mechanism". This exists
    // because r19 shipped a release gated on g_freezeHeld alone, which the
    // shadow-menu path never sets - so the release branch could not fire, the
    // menu was never hidden, numPausesGame stayed at 2, and the world froze on
    // exit. Field-caught immediately ("ShadowPause: shown" with no matching
    // "hidden"). Any future mechanism MUST be added here, not just to
    // HoldFreeze/ReleaseFreeze: a hold whose release is keyed to the wrong
    // state variable is the r6 freeze wearing a different hat, and it costs the
    // player their session every time.
    bool HoldActive() {
        return g_freezeHeld || MTB::ShadowPause::IsUp();
    }

    void HoldFreeze(const char* a_why) {
        g_lingerLeft = kFreezeLingerFrames;
        if (UseShadowMenu()) {
            MTB::ShadowPause::Show();  // idempotent; the ENGINE moves numPausesGame
            return;
        }
        auto* main = RE::Main::GetSingleton();
        if (!main) {
            return;
        }
        if (g_freezeHeld || main->freezeTime) {
            return;  // already holding, or someone else's freeze - never stack
        }
        main->freezeTime = true;
        g_freezeHeld = true;
        spdlog::info("ForcePause: holding Main::freezeTime ({}) - Souls-live menu, "
                     "world frozen without touching menu flags.",
                     a_why);
    }

    void ReleaseFreeze(const char* a_why) {
        g_lingerLeft = 0;
        MTB::ShadowPause::Hide();  // unconditional: never strand it if the setting flips
        if (!g_freezeHeld) {
            return;
        }
        g_freezeHeld = false;
        if (auto* main = RE::Main::GetSingleton()) {
            main->freezeTime = false;
        }
        spdlog::info("ForcePause: released Main::freezeTime ({}).", a_why);
    }

    // Nothing legitimate stacks this many pausing menus; past it the counter has
    // wrapped, not grown. Skyrim's own nesting is a handful at worst.
    constexpr std::uint32_t kSanePauseCeiling = 64;

    bool g_loggedUnderflow = false;

    // numPausesGame is UNSIGNED, so ANY over-decrement wraps it to a huge value
    // that still satisfies "> 0" - and the game then believes it is paused
    // forever. The world never resumes, no menu will fix it, and only a reload
    // clears it. Snap it back to the engine's own invariant (the number of open
    // menus that actually carry kPausesGame) whenever it is implausible.
    //
    // Deliberately runs even when we hold nothing: a session that is ALREADY
    // stuck has no menu open, so gating this on g_forced would refuse to heal
    // the exact state it exists for. Cheap - a bail on one comparison in the
    // normal case, and a short menuStack walk only when already broken.
    void HealPauseUnderflow(RE::UI* a_ui) {
        if (a_ui->numPausesGame <= kSanePauseCeiling) {
            g_loggedUnderflow = false;  // healthy again - re-arm the one-shot log
            return;
        }
        std::uint32_t expected = 0;
        for (const auto& open : a_ui->menuStack) {
            if (open && open->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                ++expected;
            }
        }
        if (!g_loggedUnderflow) {
            g_loggedUnderflow = true;
            spdlog::warn(
                "ForcePause: numPausesGame was {} ({} as signed) - it UNDERFLOWED, so the "
                "game read as permanently paused and the world could not resume. Snapped to "
                "the engine invariant ({} open pausing menu(s)). If this repeats, something "
                "is decrementing the counter every frame - see the r6 note in ForcePause.cpp.",
                a_ui->numPausesGame, static_cast<std::int32_t>(a_ui->numPausesGame), expected);
        }
        a_ui->numPausesGame = expected;
    }
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

    // One attempt to take a menu's pause. Returns kTaken when we now hold it,
    // kSettled when there is legitimately nothing to do (already a pausing menu,
    // or we already hold it), and kNotYet when the menu is not in the UI map yet
    // and the caller should try again on a later frame.
    enum class TakeResult { kTaken, kSettled, kNotYet };

    TakeResult TryTake(RE::UI* a_ui, const std::string& a_menuName) {
        if (g_forced.contains(a_menuName)) {
            return TakeResult::kSettled;  // already holding this one - never double up
        }
        auto menu = a_ui->GetMenu(a_menuName);
        if (!menu) {
            return TakeResult::kNotYet;  // not registered yet - RETRY, do not give up
        }
        if (menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
            return TakeResult::kSettled;  // already pausing (vanilla / no Skyrim Souls)
        }
        menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
        a_ui->numPausesGame++;
        g_forced.insert(a_menuName);
        return TakeResult::kTaken;
    }

    void EnsurePaused(const std::string& a_menuName) {
        const auto& settings = Settings::GetSingleton();
        if (!settings.enabled || !settings.forcePause) {
            return;
        }
        // r17: Souls mode never touches flags - hold the engine freeze instead.
        // Taken HERE, in the open event itself, so the first OnFrame already
        // reads paused and the dormant branch can never fire in the gap.
        if (SoulsMode()) {
            if (settings.IsSoulsLiveMenu(a_menuName)) {
                return;  // F-27: the user listed this menu as live-under-Souls
            }
            g_covered.insert(a_menuName);
            HoldFreeze(a_menuName.c_str());
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        switch (TryTake(ui, a_menuName)) {
            case TakeResult::kTaken:
                g_pending.erase(a_menuName);
                g_retakes.erase(a_menuName);  // fresh menu session, fresh retake budget
                spdlog::info(
                    "ForcePause: {} opened UNPAUSED (Skyrim Souls?) - re-paused (flag set, "
                    "counter now {}). Settled against the engine after close.",
                    a_menuName, ui->numPausesGame);
                break;
            case TakeResult::kSettled:
                g_pending.erase(a_menuName);
                break;
            case TakeResult::kNotYet:
                // The open event beat the UI map. Hand it to the per-frame
                // settle rather than dropping it on the floor.
                g_pending[a_menuName] = kPendingFrames;
                spdlog::debug("ForcePause: {} not in the UI map at its own open event - "
                              "queued for retry.",
                              a_menuName);
                break;
        }
    }

    void OnMenuClosed(const std::string& a_menuName) {
        // A menu that closed before we ever managed to take it has nothing left
        // to retry, so stop looking for it. Everything else is intentionally
        // nothing: the g_forced record stays until the per-frame settle
        // (Reassert) sees the menu actually gone - the frame AFTER the pump
        // finished its close bookkeeping - and reconciles the counter there.
        // Settling at the close EVENT would mean guessing the pump's ordering,
        // and trusting the close edge is exactly what froze AE 1.6 + Souls.
        g_pending.erase(a_menuName);
        // r17: the freeze itself is NOT released here - the per-frame settle
        // owns that, through the linger, so a menu SWITCH (close then open a
        // few frames apart) never lets the world run in the gap.
        g_covered.erase(a_menuName);
    }

    void ReleaseAll() {
        // Hand every menu we hold back to its unpaused state RIGHT NOW (live
        // force-pause toggle-off, or a disarm). We clear the flag ourselves so
        // the engine's later close bookkeeping will NOT also decrement, then drop
        // our own increment - balanced, no double decrement.
        g_pending.clear();
        g_retakes.clear();
        g_covered.clear();
        ReleaseFreeze("ReleaseAll");  // live toggle-off / disarm: no linger
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
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        // FIRST, and deliberately above the g_forced gate: a session that is
        // already stuck has no menu open and holds nothing, so anything gated on
        // g_forced would decline to fix the one state this exists for. Pairs with
        // Bubble::OnFrame calling Reassert above ITS early returns.
        HealPauseUnderflow(ui);
        // `enabled` counts as well as `forcePause`: EnsurePaused refuses to take
        // a pause when the mod is off, so continuing to HOLD one after the user
        // switches it off mid-session would strand the world frozen with no way
        // back short of another menu open.
        const auto& settings = Settings::GetSingleton();
        if (!settings.forcePause || !settings.enabled) {
            g_pending.clear();
            ReleaseAll();
            return;
        }
        // r17 FREEZE SETTLE (Souls mode). g_covered is event-driven (open event
        // inserts, close event erases - the same events whose 109/109 balance
        // the 2026-07-20 log proved; Reset() covers loads, which tear menus
        // down without close events). Deliberately NOT reconciled against
        // ui->GetMenu here: a just-opened menu can lag the UI map (the r14
        // race), and a map-keyed purge would evict it with nothing left to
        // re-insert it. Hold while any covered menu is open, linger a few
        // frames past the last close (the field-measured 32ms switch gap),
        // then release.
        if (SoulsMode()) {
            if (!g_covered.empty()) {
                HoldFreeze("settle");  // also re-arms the linger
            } else if (g_lingerLeft > 0) {
                if (--g_lingerLeft == 0) {
                    ReleaseFreeze("last covered menu closed, linger elapsed");
                }
            } else if (HoldActive()) {
                // SELF-HEAL, and the reason this is a separate branch rather
                // than a tighter condition above: no covered menu is open and
                // the linger is spent, yet we are still holding a pause. That
                // state is always a bug, it is never recoverable by the player
                // (the world simply never resumes), and it has now happened
                // twice from two different causes. Reconcile it every frame
                // instead of trusting the accounting that put us here.
                ReleaseFreeze("self-heal: held a pause with no covered menu open");
            }
            return;  // no flags are ever taken in Souls mode - nothing below applies
        }
        // r14: PENDING RETRIES, and deliberately ABOVE the g_forced early-out.
        // The case this exists for is a menu SWITCH, where the outgoing menu has
        // already settled and g_forced is therefore EMPTY - gating the retry on
        // g_forced would skip precisely the frame that needs it and the bug
        // would survive the fix.
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            switch (TryTake(ui, it->first)) {
                case TakeResult::kTaken:
                    spdlog::info(
                        "ForcePause: {} re-paused ON RETRY (flag set, counter now {}) - its "
                        "open event fired before the menu was in the UI map.",
                        it->first, ui->numPausesGame);
                    it = g_pending.erase(it);
                    continue;
                case TakeResult::kSettled:
                    it = g_pending.erase(it);
                    continue;
                case TakeResult::kNotYet:
                    if (--it->second <= 0) {
                        spdlog::warn(
                            "ForcePause: gave up waiting for {} to appear in the UI map after "
                            "{} frames - it stays unpaused, so the studio will be dormant for "
                            "it. If this shows up in a field log, the menu name or the open "
                            "event is wrong, not the timing.",
                            it->first, kPendingFrames);
                        it = g_pending.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
            }
        }
        if (g_forced.empty()) {
            return;
        }
        for (auto it = g_forced.begin(); it != g_forced.end();) {
            if (auto menu = ui->GetMenu(*it)) {
                // STILL OPEN. If our flag is intact, leave it completely alone.
                //
                // This used to RE-ASSERT kPausesGame here every frame, on the
                // theory that "a mod may re-strip it mid-session". That theory
                // was right about the mechanism and catastrophically wrong about
                // the remedy: the mod that re-strips it is Skyrim Souls, whose
                // entire purpose is stripping that flag, and it answers a set
                // flag by decrementing numPausesGame. Re-asserting every frame
                // therefore bought one decrement per frame, forever.
                //
                // Field log 2026-07-19 caught it exactly: numPausesGame tracking
                // -tickCount one-for-one (-550 at tick 550, -1038 at tick 1038)
                // until it wrapped, and stopping dead the instant the user
                // toggled force-pause off in the panel - the loop confirmed from
                // both ends. The counter is UNSIGNED, so once wrapped it still
                // reads "> 0" and the game believes it is paused forever: the
                // world never resumes after the menu closes. That is the freeze.
                //
                // r16 draws the line the r6 fix over-drew. r6's sin was an
                // UNBALANCED move (flag only, no increment) repeated UNBOUNDED
                // (every frame, forever). A bounded re-TAKE (flag + increment,
                // kMaxRetakes rounds) shares neither property - see the note at
                // g_retakes for the field evidence that Souls' strip is balanced
                // and that winning one round (a paused world) ends the fight.
                // Losing every round costs kMaxRetakes balanced no-ops and lands
                // exactly where r6's "recoverable loss" landed: dormant.
                if (!menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
                    auto& rounds = g_retakes[*it];
                    if (rounds < kMaxRetakes) {
                        ++rounds;
                        menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
                        ui->numPausesGame++;
                        spdlog::info(
                            "ForcePause: {} had kPausesGame stripped while open (Skyrim "
                            "Souls answered the take) - RE-TOOK it (round {}/{}, counter "
                            "now {}).",
                            *it, rounds, kMaxRetakes, ui->numPausesGame);
                    } else {
                        spdlog::warn(
                            "ForcePause: {} was re-stripped {}x - conceding the fight "
                            "(bounded); the bubble goes dormant for this menu session.",
                            *it, kMaxRetakes);
                        g_retakes.erase(*it);
                        it = g_forced.erase(it);
                        continue;
                    }
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
            g_retakes.erase(*it);
            it = g_forced.erase(it);
        }
    }

    void Reset() {
        // A load tears menus down without close events and resets the engine's
        // pause counter to the loaded state, so just drop our bookkeeping.
        g_forced.clear();
        g_pending.clear();
        g_retakes.clear();
        g_covered.clear();
        // A load resets the engine's own freeze state; drop only our claim so a
        // held freeze can never leak across a load into gameplay.
        ReleaseFreeze("Reset (load/new game)");
    }
}
