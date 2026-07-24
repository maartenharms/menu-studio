#include "PCH.h"

#include "CameraGate.h"

#include "Bubble.h"
#include "CameraCollisionPolicy.h"
#include "Offsets.h"
#include "Settings.h"
#include "VersionCheck.h"

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

    // AE 1.6.x folds the smoother above into the position builder, but the
    // obstruction query remains a normal call. Returning false alone is not
    // enough: the inlined tail would otherwise interpolate from its remembered
    // collision point. Mark the raw proposed translation as settled too.
    struct InlineCollisionTestHook {
        static bool thunk(RE::TESCamera* a_camera, RE::NiPoint3* a_translation,
                          std::uint8_t a_mode) {
            using enum MTB::CameraCollisionPolicy::Action;
            const auto action = MTB::CameraCollisionPolicy::Decide({
                .enabled = MTB::Settings::GetSingleton().bypassCameraCollision,
                .bubbleActive = MTB::Bubble::IsBubbleActive(),
            });
            if (action == kBypass && a_translation) {
                auto* camera = RE::PlayerCamera::GetSingleton();
                auto* state = camera ? camera->currentState.get() : nullptr;
                if (state && state->id == RE::CameraState::kThirdPerson) {
                    auto* third = static_cast<RE::ThirdPersonState*>(state);
                    third->collisionPos = *a_translation;
                    third->collisionPosValid =
                        (std::numeric_limits<float>::max)();
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        spdlog::info(
                            "CameraGate: AE inline obstruction query bypassed "
                            "while the bubble is active.");
                    }
                    return false;
                }
            }
            return func(a_camera, a_translation, a_mode);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

namespace MTB::CameraGate {
    void Install() {
        // Located by call target (VersionCheck), not by the hand-measured
        // offset - see Offsets.h.
        const auto callOffset = VersionCheck::SmootherCallOffset();
        if (callOffset != 0) {
            REL::Relocation<std::uintptr_t> site{
                Offsets::CameraPositionBuilder, callOffset
            };
            if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
                byte != 0xE8) {
                spdlog::error(
                    "CameraGate: position-builder+0x{:X} is 0x{:02X}, expected E8 - "
                    "another mod patched the camera collision site incompatibly. "
                    "Gate NOT installed.",
                    callOffset, byte);
                return;
            }
            auto& trampoline = SKSE::GetTrampoline();
            CollisionSmootherHook::func = trampoline.write_call<5>(
                site.address(),
                reinterpret_cast<std::uintptr_t>(&CollisionSmootherHook::thunk));
            spdlog::info(
                "CameraGate: camera-collision bypass installed "
                "(position-builder+0x{:X}, full smoother).",
                callOffset);
            return;
        }

        const auto inlineOffset = VersionCheck::CollisionTestCallOffset();
        if (inlineOffset == 0) {
            spdlog::info(
                "CameraGate: no verified collision call site on this runtime; "
                "camera-collision bypass unavailable.");
            return;
        }
        REL::Relocation<std::uintptr_t> site{
            Offsets::CameraPositionBuilder, inlineOffset
        };
        if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
            byte != 0xE8) {
            spdlog::error(
                "CameraGate: inline obstruction site +0x{:X} is 0x{:02X}, "
                "expected E8. Gate NOT installed.",
                inlineOffset, byte);
            return;
        }
        auto& trampoline = SKSE::GetTrampoline();
        InlineCollisionTestHook::func = trampoline.write_call<5>(
            site.address(),
            reinterpret_cast<std::uintptr_t>(&InlineCollisionTestHook::thunk));
        spdlog::info(
            "CameraGate: camera-collision bypass installed "
            "(position-builder+0x{:X}, AE inline obstruction query).",
            inlineOffset);
    }
}
