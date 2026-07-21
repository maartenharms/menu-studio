#include "PCH.h"

#include "EquipNotifyGate.h"

#include "Settings.h"
#include "WeaponPreview.h"

namespace MTB::EquipNotifyGate {

    namespace {
        std::atomic<bool> g_armed{ false };
        std::atomic<bool> g_loggedThisSession{ false };
        bool              g_installed = false;

        struct SuppressCheckHook {
            // CommonLib declares this slot `void Unk_B3(void)`; it actually
            // returns a byte in al. Declaring the truth here is what makes the
            // override possible at all, and the ABI is identical either way.
            static bool thunk(RE::PlayerCharacter* a_this) {
                if (g_armed.load(std::memory_order_acquire) &&
                    Settings::GetSingleton().liveEquipNotifyInMenus) {
                    // Logged ONCE per menu session. This sits on the equip
                    // path and a per-call line would bury the very clip lines
                    // it exists to explain.
                    if (!g_loggedThisSession.exchange(true, std::memory_order_relaxed)) {
                        spdlog::info("equip notify gate: r25 ACTIVE - answering Unk_B3 'do not "
                                     "bail', so Actor::OnItemEquipped runs its re-parent and "
                                     "graph notifications instead of being short-circuited by "
                                     "our own kPausesGame menu.");
                    }
                    return true;
                }
                return func(a_this);
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };

        // r26. THE BOW FIX.
        //
        // r25 got the engine to start the RIGHT clip; nothing ran it. The field
        // saw Bow_Equip begin correctly and then play out over 1.16 s of real
        // frames, because NormalizeToTerminal only pumps a state caught
        // mid-draw and after an equip the state is already terminal.
        //
        // This is the only place the pump can go. The notification lands ~14 ms
        // AFTER our own swap handler has run, so nothing in WeaponPreview's
        // per-tick path is still around to carry it.
        struct OnItemEquippedHook {
            static void thunk(RE::PlayerCharacter* a_this, bool a_playAnim) {
                // ORIGINAL FIRST. It is the call that starts the clip, and
                // there is nothing to pump until it has run.
                func(a_this, a_playAnim);

                if (!a_this || !g_armed.load(std::memory_order_acquire) ||
                    !Settings::GetSingleton().liveEquipNotifyInMenus) {
                    return;
                }
                // ⚠ ARMED-ONLY, and the check above is why. Pumping the graph
                // while the world is live is the r8 shape that produced a
                // walking loop in a frozen scene.
                WeaponPreview::PumpEngineEquip(a_this);
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void SetArmed(bool a_armed) {
        g_armed.store(a_armed, std::memory_order_release);
        if (a_armed) {
            return;  // the once-per-session latch is cleared on DISARM, below
        }
        // Cleared at the closing edge, not the opening one. SetArmed(true) is
        // called every frame a menu is open, so clearing on arm would re-open
        // the latch every frame and log per call - which is precisely the bug
        // r24's own probe shipped with. Fourth time this shape has bitten this
        // codebase: latch once at the edge, never re-derive per frame.
        g_loggedThisSession.store(false, std::memory_order_relaxed);
    }

    void Install() {
        if (g_installed) {
            return;
        }
        // Unk_B3 is vfunc 0x0B3 on the PRIMARY vtable - the same one
        // Update3DPosition (0x3F) and Update (0x0AD) are already hooked on, so
        // [0] is right and no runtime vptr match is needed.
        //
        // write_vfunc, not write_branch, per the house rule.
        REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
        SuppressCheckHook::func = playerVtbl.write_vfunc(0x0B3, SuppressCheckHook::thunk);
        // 0x0B2 is the notification itself - hooked so the clip it starts can
        // be pumped to completion the moment it returns.
        OnItemEquippedHook::func = playerVtbl.write_vfunc(0x0B2, OnItemEquippedHook::thunk);
        g_installed              = true;
        spdlog::info("equip notify gate: hooked PlayerCharacter::Unk_B3 (0x0B3, un-suppress) "
                     "and OnItemEquipped (0x0B2, pump the engine's own equip clip). Armed "
                     "only inside a bubbled menu and only when bLiveEquipNotifyInMenus is "
                     "set.");
    }

}  // namespace MTB::EquipNotifyGate
