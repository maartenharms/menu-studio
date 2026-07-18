#include "PCH.h"

#include "CameraGate.h"

#include "Bubble.h"
#include "Offsets.h"
#include "Settings.h"

#include <cstring>
#include <limits>

namespace {
    // The unique E8 call to the collision smoother (ID 49980) inside the
    // third-person position builder (verified by exhaustive call-site scan).
    // IDs/offsets live in Offsets.h (§3.4 table).

    struct CollisionSmootherHook {
        static void thunk(RE::ThirdPersonState* a_this) {
            if (MTB::Settings::GetSingleton().bypassCameraCollision &&
                MTB::Bubble::IsBubbleActive()) {
                // Skip the correction - but keep the smoother's memory fresh
                // (collisionPos = last unobstructed spot, collisionPosValid =
                // its lerp timer; semantics verified in the 0x850A80
                // decompile). Left stale, the first call after the menu
                // closes LERPs the camera from wherever it was when the
                // bubble armed - the jarring glide reported on exit.
                a_this->collisionPos = a_this->translation;
                a_this->collisionPosValid = (std::numeric_limits<float>::max)();
                return;
            }
            // TRACKER B-4 tripwire (wall-open face camera): SPIM's boom is a
            // CONSTANT (fVanityMode min==max, spim_camera.c), so a pulled-in
            // menu camera means the engine clamp ran - i.e. this path
            // executed while a bubble menu was open and the gate thought the
            // bubble inactive (pause blip / arming gap). Convict or clear it.
            if (MTB::Settings::GetSingleton().bypassCameraCollision &&
                MTB::Bubble::OpenMenuCount() > 0) {
                static int hits = 0;
                if (hits++ % 120 == 0) {
                    auto* main = RE::Main::GetSingleton();
                    auto* ui = RE::UI::GetSingleton();
                    spdlog::warn(
                        "camera gate: smoother EXECUTED with a bubble menu open "
                        "(hit {}; freezeTime={} uiPaused={} numPauses={}) - B-4 evidence.",
                        hits, main && main->freezeTime, ui && ui->GameIsPaused(),
                        ui ? ui->numPausesGame : 0xFFFF);
                }
            }
            func(a_this);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

namespace MTB::CameraGate {
    void Install() {
        const auto callOffset = Offsets::SmootherCallOffset();
        if (callOffset == 0) {
            // AE: the engine INLINED the collision smoother into the position
            // builder - there is no standalone call to gate (Offsets.h). A
            // documented limitation, not an error (field logs previously
            // mis-blamed "another mod" here).
            spdlog::info("CameraGate: no smoother call site on this runtime (AE inlines it); "
                         "camera-collision bypass unavailable - everything else works.");
            return;
        }
        REL::Relocation<std::uintptr_t> site{ Offsets::CameraPositionBuilder, callOffset };
        if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
            byte != 0xE8) {
            spdlog::error(
                "CameraGate: position-builder+0x{:X} is 0x{:02X}, expected E8 - another mod "
                "patched the camera collision site incompatibly. Gate NOT installed.",
                callOffset, byte);
            return;
        }
        auto& trampoline = SKSE::GetTrampoline();
        CollisionSmootherHook::func = trampoline.write_call<5>(
            site.address(), reinterpret_cast<std::uintptr_t>(&CollisionSmootherHook::thunk));
        spdlog::info("CameraGate: camera-collision bypass installed (position-builder+0x{:X}).",
                     callOffset);
    }
}
