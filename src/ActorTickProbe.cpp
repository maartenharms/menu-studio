#include "PCH.h"

#include "ActorTickProbe.h"

namespace MTB::ActorTickProbe {

    namespace {
        std::atomic<std::uint64_t> g_calls{ 0 };
        std::uint64_t              g_atArm       = 0;
        bool                       g_armedLatched = false;
        bool                       g_installed    = false;

        struct PlayerUpdateHook {
            static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
                // Counted BEFORE the original so a crash inside the engine's
                // own update still leaves an accurate count in the log. The
                // count is the entire point of this hook; the ordering costs
                // nothing because we add no behaviour either way.
                g_calls.fetch_add(1, std::memory_order_relaxed);
                func(a_this, a_delta);
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    std::uint64_t Count() { return g_calls.load(std::memory_order_relaxed); }

    void MarkArmed(bool a_armed) {
        const std::uint64_t now = Count();
        if (a_armed) {
            // ⚠ LATCH ON THE EDGE. r24 shipped this stamping EVERY FRAME,
            // because MarkArmed(true) is called per tick, not once at open. The
            // delta it then printed was measured from the previous frame and
            // was therefore ~0 no matter what the truth was - a diagnostic that
            // could only ever confirm its own hypothesis. It also spat 658 log
            // lines for one menu.
            //
            // The finding survived only because the ABSOLUTE count happened to
            // be printed too and sat pinned at 915 for 403 consecutive frames.
            // That was luck, not design.
            //
            // FOURTH time this exact shape has bitten this codebase - the
            // deferred-sheathe timer, r19c, the pause ownership, and now this.
            // State that must be latched once must not be re-derived per frame.
            if (g_armedLatched) {
                return;
            }
            g_armedLatched = true;
            g_atArm        = now;
            spdlog::debug("actor tick probe: menu ARMED at Actor::Update count {}.", now);
            return;
        }
        if (!g_armedLatched) {
            return;  // never opened - nothing to report, and no false zero
        }
        g_armedLatched = false;
        const std::uint64_t delta = now - g_atArm;
        // The interpretation is spelled out in the log line itself. A bare
        // number invites the next reader to guess which way round it means,
        // and this project has lost rounds to exactly that.
        spdlog::info("actor tick probe: menu DISARMED at count {} - Actor::Update ran {} "
                     "time(s) while the menu was open. {}",
                     now, delta,
                     delta == 0
                         ? "ZERO: the actor does not update while paused, so anything the "
                           "engine defers to it (a queued equip, the weapon replace) cannot "
                           "run either - consistent with r22/r23."
                         : "NON-ZERO: the actor DOES update while paused, so a queue that "
                           "drains from it is NOT the explanation - hypothesis refuted.");
    }

    void Install() {
        if (g_installed) {
            return;
        }
        // Actor::Update is vfunc 0x0AD on the PRIMARY vtable - the same one
        // Update3DPosition (0x3F) is already hooked on, so [0] is right here
        // and no runtime vptr match is needed.
        //
        // write_vfunc, not write_branch, per the house rule.
        REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = playerVtbl.write_vfunc(0x0AD, PlayerUpdateHook::thunk);
        g_installed            = true;
        spdlog::info("actor tick probe: hooked PlayerCharacter::Update (vfunc 0x0AD). "
                     "Counts whether the ACTOR updates while a menu holds the pause - "
                     "Bubble ticks the animation graph by hand but never this.");
    }

}  // namespace MTB::ActorTickProbe
