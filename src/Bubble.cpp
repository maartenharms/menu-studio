#include "PCH.h"

#include "Backdrop.h"
#include "Bubble.h"
#include "CbpcDrive.h"
#include "Declutter.h"
#include "FaceNeutral.h"
#include "ForcePause.h"
#include "FsmpDrive.h"
#include "FootIkGate.h"
#include "Offsets.h"
#include "OwnView.h"
#include "SceneTint.h"
#include "Settings.h"
#include "StudioLight.h"
#include "StudioRig.h"
#include "Transition.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace {
    // SE 1.5.97: at Main::Update+0x28E sits the unique E8 call to the
    // per-frame player-update dispatch (ID 35578). Verified by decompile +
    // exhaustive call-site scan (docs/SPIKE-A-RE.md). IDs/offsets live in
    // Offsets.h (§3.4 table).

    // Engine frame-dt globals, updated every frame even while paused
    // (Main::UpdateTimers, RVA 0x5B3E40). Logged for telemetry only - the
    // bubble runs on its own QPC clock.
    REL::Relocation<float*> g_slowDt{ MTB::Offsets::SlowDt };
    REL::Relocation<float*> g_realDt{ MTB::Offsets::RealDt };
    REL::Relocation<float*> g_dtVariant3{ MTB::Offsets::DtVariant3 };

    // BSFaceGenAnimationData::Update(float a_dt, bool a_force) - ID 25983,
    // RVA 0x3C4030. Advances expression/modifier/phoneme keyframe ramps and
    // the blink timers; the engine's only caller (0x3D9440, render-side face
    // model update) passes (g_slowDt, true) after clearing the +0x218 pending
    // flag - mirrored exactly in Tick. Returns whether anything changed.
    using FaceGenUpdate_t = bool(__fastcall*)(RE::BSFaceGenAnimationData*, float, bool);
    REL::Relocation<FaceGenUpdate_t> g_faceGenUpdate{ MTB::Offsets::FaceGenUpdate };

    // Game.FadeOutGame's engine core (Offsets.h: FaderMenu-driven, UI-
    // clocked ⇒ animates while paused). ABI byte-verified from the native
    // wrapper: (fadingOut, blackFade, duration→XMM2, unk=false, secsBefore).
    using FadeOutGame_t = void (*)(bool a_fadingOut, bool a_blackFade, float a_duration,
                                   bool a_unk, float a_secsBeforeFade);
    REL::Relocation<FadeOutGame_t> g_fadeOutGame{ MTB::Offsets::FadeOutGame };

    struct PlayerDispatchHook {
        static void thunk(RE::Main* a_main) {
            func(a_main);
            MTB::Bubble::GetSingleton().OnFrame(a_main);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // B-7 v3 stutter (field r25: "it can rotate but it stutters like
    // crazy… some logic keeps resetting the player to face the front"):
    // SPIM calls this vfunc on the player after EVERY drag event
    // (spim_input.c tail: vtable+0x1F8), and it re-syncs the node from
    // data.angle - the pinned front - AFTER our per-tick compose when the
    // input pump lands later in the frame. Re-apply the spin on top of
    // every sync; the r25 telemetry proved the accumulator itself is
    // silky (park rock-steady, spin easing cleanly).
    struct Update3DPositionHook {
        static void thunk(RE::PlayerCharacter* a_this, bool a_warp) {
            func(a_this, a_warp);
            MTB::Bubble::GetSingleton().ReassertSpin();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    float QpcSeconds(std::uint64_t a_from, std::uint64_t a_to) {
        static const double freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        return static_cast<float>(static_cast<double>(a_to - a_from) / freq);
    }

    std::uint64_t QpcNow() {
        LARGE_INTEGER t{};
        ::QueryPerformanceCounter(&t);
        return static_cast<std::uint64_t>(t.QuadPart);
    }

    RE::ThirdPersonState* GetThirdPersonState() {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* state = camera ? camera->currentState.get() : nullptr;
        if (state && state->id == RE::CameraState::kThirdPerson) {
            return static_cast<RE::ThirdPersonState*>(state);
        }
        return nullptr;
    }

    // F-14 v3: SPIM's own inspect-mode gate - the right stick belongs to
    // the ITEM while its 3D preview is zoomed (Inventory3DManager
    // zoomProgress > 0) and to the CHARACTER otherwise.
    bool ItemPreviewZoomedOut() {
        auto* inv = RE::Inventory3DManager::GetSingleton();
        return !inv || inv->GetRuntimeData().zoomProgress == 0.0f;
    }
}

namespace MTB {
    Bubble& Bubble::GetSingleton() {
        static Bubble instance;
        return instance;
    }

    void Bubble::InstallHook() {
        const auto callOffset = Offsets::DispatchCallOffset();
        REL::Relocation<std::uintptr_t> site{ Offsets::MainUpdate, callOffset };

        // Never write_call a site that is no longer the vanilla E8 - another
        // mod may have claimed or patched it in an incompatible way.
        if (const auto byte = *reinterpret_cast<std::uint8_t*>(site.address());
            byte != 0xE8) {
            spdlog::error(
                "Main::Update+0x{:X} is 0x{:02X}, expected E8 (call) - another mod "
                "patched the player-dispatch site incompatibly. Bubble NOT installed.",
                callOffset, byte);
            return;
        }

        auto& trampoline = SKSE::GetTrampoline();
        PlayerDispatchHook::func =
            trampoline.write_call<5>(site.address(),
                                     reinterpret_cast<std::uintptr_t>(&PlayerDispatchHook::thunk));
        spdlog::info("Frame driver installed at Main::Update+0x{:X}.", callOffset);

        // Update3DPosition (TESObjectREFR vfunc 0x3F) on the PLAYER vtable -
        // the spin re-assert (see Update3DPositionHook). write_vfunc, per
        // the house rule: never write_branch a non-branch site.
        REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
        Update3DPositionHook::func = playerVtbl.write_vfunc(0x3F, Update3DPositionHook::thunk);
        spdlog::info("Preview-spin re-assert installed on PlayerCharacter::Update3DPosition.");
    }

    void Bubble::Register() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&GetSingleton());
            spdlog::info("Menu watcher registered.");
        } else {
            spdlog::error("UI singleton unavailable; menu watcher NOT registered.");
        }
        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink<RE::InputEvent*>(&GetSingleton());
            spdlog::info("Input watcher registered (preview spin).");
        } else {
            spdlog::error("Input device manager unavailable; preview spin has no input.");
        }
    }

    void Bubble::CancelDipIfActive() {
        // F-12 exit discipline: no exit path may leave the screen dark.
        // Only touches the fader when OUR dip is mid-flight (black cut or
        // the post-build hold) - an unconditional fade-in here would fight
        // the engine's own fades (loading screens ride the same FaderMenu).
        if (dipPhase_ != 0 || dipHoldFrames_ > 0) {
            dipPhase_ = 0;
            dipHoldFrames_ = 0;
            g_fadeOutGame(false, true, 0.15f, false, 0.0f);
            spdlog::debug("dip: cancelled (exit before the reveal) - fading back in.");
        }
    }

    void Bubble::ArmOwnViewIfOurs(bool a_force) {
        // F-15: own the view when no view mod does - first-person arms (a
        // first-person camera means nothing switched it) and menus outside
        // every installed view mod's coverage. Applies the FULL SPIM
        // framing; the CLOSE EVENT restores it (r33: restoring at the
        // deferred Disarm kept the framed camera up through the grace
        // window - a jarring split-second third-person angle after
        // first-person barters). No-op when already framed (menu switch).
        // r45: a_force bypasses the coverage check - the tick-3 fallback
        // fires it when a COVERED menu is still first person and unframed
        // (the covering mod bowed out; before r45 the fallback re-ran the
        // coverage check and silently declined - the rare "first-person
        // barter, no player" field case).
        if (raceMenuOpen_.load() || OwnView::Active()) {
            return;
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->currentState) {
            return;
        }
        const auto stateId = camera->currentState->id;
        const bool firstPerson = stateId == RE::CameraState::kFirstPerson;
        // r52 (field: "show player in inventory doesn't even work when we
        // are on the horse" - SPII declines mounted, source-confirmed, so
        // it's ours). A mounted arm's camera is the MOUNT state (kMount),
        // NOT kThirdPerson - so before r52 we neither forced third nor
        // framed the active mount camera, and the void came up EMPTY. Treat
        // it like a first-person arm: force the switch to third person,
        // which shows the seated player AND the horse (both keep rendering).
        const bool mounted = stateId == RE::CameraState::kMount;
        if (!a_force && !OwnView::ShouldOwn(currentMenuName_, firstPerson)) {
            return;
        }
        bool forcedThird = false;
        if (firstPerson || mounted) {
            forcedThird = true;
            spdlog::debug("own view: {} arm - switching to third person.",
                          mounted ? "mounted" : "first-person");
        }
        // ApplyFraming does the transition itself (SPIM's direct SetState -
        // the ForceThirdPerson request path stalled on COMBAT arms, r32
        // field: "in combat we don't see the character enter the view").
        OwnView::ApplyFraming(forcedThird, mounted);
        if (mounted) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto p = player ? player->GetPosition() : RE::NiPoint3{};
            spdlog::info("own view: MOUNTED arm - framing raised for the rider; "
                         "player at ({:.0f},{:.0f},{:.0f}). If the void/constellation "
                         "reads wrong here, the backdrop centers on this point.",
                         p.x, p.y, p.z);
        }
        if (auto* tps = GetThirdPersonState()) {
            freeRotArm_ = tps->freeRotation.x;  // the spin park adopts the framed view
        }
    }

    void Bubble::ReassertSpin() {
        if (!armedLastFrame_ || !spinBasisValid_ || spinYaw_ == 0.0f) {
            return;
        }
        if (raceMenuOpen_.load() || !Settings::GetSingleton().previewSpin) {
            return;
        }
        if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsOnMount()) {
            return;  // r55: no spin while mounted
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        RE::NiMatrix3 spin;
        spin.EulerAnglesToAxesZXY(0.0f, 0.0f, spinYaw_);
        if (auto* root = player ? player->Get3D(false) : nullptr) {
            root->local.rotate = spin * rootBaseRotate_;
        }
        if (auto* horse = spinHorseRoot_.get()) {  // r54: keep the horse with the rider
            horse->local.rotate = spin * horseBaseRotate_;
        }
    }

    void Bubble::FireDeferredExitMoveStart() {
        // B-8 v2: the close-time mirror fired into the close→open gap of
        // every menu SWITCH (field r23: "switch to magic menu and I see the
        // player walk for a bit" - measured gaps 52-71 ms), and at close
        // time a switch is indistinguishable from a real exit. Defer the
        // mirror just past the gap; a bubble re-open cancels it. On a real
        // close the held-key slide this re-introduces (~85 ms) hides under
        // the menu-close fade + the F-12 dissolve (r20 moved the mirror to
        // the close event to kill a 100 ms slide with NO cover - the cover
        // exists now).
        if (!pendingExitMoveStart_) {
            return;
        }
        if (QpcSeconds(exitMoveQpc_, QpcNow()) < 0.085f) {
            return;
        }
        pendingExitMoveStart_ = false;
        sentMoveStop_ = false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* state = player ? player->AsActorState() : nullptr;
        const bool locomoting = state && (state->actorState1.walking ||
                                          state->actorState1.running ||
                                          state->actorState1.sprinting);
        if (locomoting && !player->IsInMidair() && !state->actorState1.swimming) {
            player->NotifyAnimationGraph("moveStart");
            spdlog::debug("idle-in-menus: deferred moveStart fired (real close, input held).");
        }
    }

    void Bubble::ForceReset() {
        menusOpen_ = 0;
        raceMenuOpen_ = false;
        sentMoveStop_ = false;   // graph state dies with the load - no exit mirror
        pendingExitMoveStart_ = false;
        pendingOwnViewRestore_ = false;  // DropOnLoad below owns the globals
        CancelDipIfActive();
        exitPhase_ = 0;  // r47: the exit machine is a pure hold - no fader state
        // B-7 v3: drop the spin state; the node dies with the load (if the
        // 3D is still up, un-compose so nothing spun survives into a save
        // made this frame - cheap and deterministic).
        if (spinBasisValid_) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (auto* root = player ? player->Get3D(false) : nullptr) {
                root->local.rotate = rootBaseRotate_;
            }
            spinBasisValid_ = false;
        }
        if (auto* horse = spinHorseRoot_.get()) {  // r54: un-spin the mount
            horse->local.rotate = horseBaseRotate_;
        }
        spinHorseRoot_.reset();
        spinTarget_ = 0.0f;
        spinYaw_ = 0.0f;
        // F-15: the load restores the save's own camera - put only the
        // process-global surfaces back (INI Settings survive loads).
        OwnView::DropOnLoad();
        airFrozenArm_ = false;
        graceFrames_ = 0;
        gateHoldFrames_ = 0;
        loggedUnpausedOnce_ = false;
        armedLastFrame_ = false;
        // Restore NOW, not via the next Disarm edge (there is none after
        // this): handles die with the load, form-side edits (cell lighting,
        // FSMP thresholds, CBPC flag) survive it and must not leak. The
        // backdrop's shader writes hit the SHARED model templates - they
        // outlive the load too.
        // F-12 exit discipline: a quickload never ramps - snap the
        // transition dead before the removes so nothing re-applies a
        // faded value on the next arm.
        Transition::Snap(0.0f);
        FaceNeutral::Drop();  // face data dies with the load - nothing to write back
        Declutter::RestoreAll();
        Backdrop::Remove();
        Backdrop::OccluderDrop();  // scene parent dies with the load - detach the resident occluder
        StudioRig::Remove();
        StudioLight::Restore();
        SceneTint::Restore();  // guarded: no-op unless the colour filter was on
        FsmpDrive::SetRotationFreedom(false);
        CbpcDrive::SetSimulateWhilePaused(false);
        FootIkGate::SetSuppressed(false);
        // Forced-pause bookkeeping only: menu instances (and their flags)
        // die with the load; the engine owns the counter either way.
        ForcePause::Reset();
        spdlog::info("ForceReset (load/new game).");
    }

    void Bubble::Disarm() {
        // Unconditional: a dip can be live before the first armed tick
        // (open event cuts to black; an unpause race or missing player 3D
        // disarms without ever arming) - the screen must never stay dark.
        CancelDipIfActive();
        exitPhase_ = 0;  // r47: the exit machine is a pure hold - no fader state
        if (armedLastFrame_) {
            // B-7 v3: un-compose the preview spin from the root node - the
            // engine re-syncs from data.angle on unpause, but one spun
            // frame in the handoff would read as a flicker.
            if (spinBasisValid_) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (auto* root = player ? player->Get3D(false) : nullptr) {
                    root->local.rotate = rootBaseRotate_;
                    RE::NiUpdateData ctx;
                    ctx.time = 0.0f;
                    ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                    root->Update(ctx);
                }
                spinBasisValid_ = false;
            }
            if (auto* horse = spinHorseRoot_.get()) {  // r54: un-spin the mount
                horse->local.rotate = horseBaseRotate_;
                RE::NiUpdateData ctx;
                ctx.time = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                horse->Update(ctx);
            }
            spinHorseRoot_.reset();
            spinTarget_ = 0.0f;
            spinYaw_ = 0.0f;
            // B-2 exit mirror fallback: the deferred close-time mirror
            // (B-8 v2) owns the normal exit; this only covers disarms that
            // never saw a close event (dormant unpause, player 3D gone).
            if (sentMoveStop_ && !pendingExitMoveStart_) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* state = player ? player->AsActorState() : nullptr;
                const bool locomoting = state && (state->actorState1.walking ||
                                                  state->actorState1.running ||
                                                  state->actorState1.sprinting);
                if (locomoting && !player->IsInMidair() && !state->actorState1.swimming) {
                    player->NotifyAnimationGraph("moveStart");
                    spdlog::debug("idle-in-menus: moveStart sent at exit (input still held).");
                }
            }
            sentMoveStop_ = false;
            airFrozenArm_ = false;
            Transition::Snap(0.0f);  // F-12 backstop: early disarms don't ramp
            FaceNeutral::Restore();
            Declutter::RestoreAll();
            Backdrop::Remove();
            Backdrop::OccluderHide();  // keep the occluder resident + warm for the next open
            StudioRig::Remove();
            StudioLight::Restore();
            SceneTint::Restore();  // instant restore at the close edge - no tint leaks to gameplay
            FsmpDrive::SetRotationFreedom(false);
            CbpcDrive::SetSimulateWhilePaused(false);
            FootIkGate::SetSuppressed(false);
            // F-15: the view was OURS - full SPIM ResetCamera-style restore
            // (first person handed back if we forced it, Settings originals,
            // interpolators snapped inside). Otherwise only snap the zoom/yaw
            // interpolators: SPIM restores the pre-menu camera on close and
            // they'd ease current -> target for half a second (exit glide).
            if (OwnView::Active()) {
                OwnView::Disarm();
            } else if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                if (auto* state = camera->currentState.get();
                    state && state->id == RE::CameraState::kThirdPerson) {
                    auto* tps = static_cast<RE::ThirdPersonState*>(state);
                    tps->currentZoomOffset = tps->targetZoomOffset;
                    tps->currentYaw = tps->targetYaw;
                }
            }
        }
        armedLastFrame_ = false;
    }

    bool Bubble::IsBubbleActive() {
        auto& self = GetSingleton();
        if (!Settings::GetSingleton().enabled) {
            return false;
        }
        if (self.menusOpen_.load() <= 0) {
            // Hold the gates through a menu switch (the pause counter dips to
            // zero between close and open; the switch can outlast the visual
            // grace, so gate lifetime gets its own longer window).
            return self.gateHoldFrames_.load() > 0;
        }
        auto* main = RE::Main::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        return (main && main->freezeTime) || (ui && ui->GameIsPaused());
    }

    bool Bubble::ShouldShowTryOnPrompt() const {
        const auto& cfg = Settings::GetSingleton();
        if (!cfg.showTryOnPrompt) {
            return false;
        }
        if (!IsBubbleActive() || cfg.declutterMode < 2) {
            return false;  // only the void / dressing room, only while armed
        }
        // Inventory-family only - you try on gear, not spells (MagicMenu excluded).
        const auto& m = currentMenuName_;
        return m == "InventoryMenu" || m == "BarterMenu" || m == "ContainerMenu";
    }

    RE::BSEventNotifyControl Bubble::ProcessEvent(RE::InputEvent* const* a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>*) {
        // F-14: right-drag spins the PREVIEW BODY. The log proved the SPII
        // author's SmoothCam-API build rotates the CAMERA only (heading +
        // freeRot constant through every drag) - physics correctly sees
        // nothing move. This is the rotation that moves the SKELETON, so
        // hair/cloth swings with real momentum; it lives on its own input
        // and coexists with the author's camera orbit. MenuInputGate
        // already eats right-mouse in the UI (no quick-buy on drags).
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // Preview prompt: record the last-seen input device on every call,
        // even while idle, so the keyboard/gamepad label stays fresh across
        // the arm edge rather than only updating while armed.
        for (auto* e = *a_event; e; e = e->next) {
            lastInputDevice_.store(e->device.get());
        }
        if (!IsBubbleActive() || raceMenuOpen_.load()) {
            spinDragging_ = false;
            spinStickX_ = 0.0f;
            return RE::BSEventNotifyControl::kContinue;
        }
        const auto& s = Settings::GetSingleton();
        for (auto* event = *a_event; event; event = event->next) {
            // F-14 v3 input evidence (field r30: "controller rotation
            // doesn't work" with zero input facts in the log): the first
            // button/stick events of each arm get logged raw, so the next
            // session's log PROVES whether thumbstick events reach this
            // sink at all - and with which idCodes.
            if (s.verboseLog && inputEvidence_.load() > 0 &&
                event->eventType.get() != RE::INPUT_EVENT_TYPE::kMouseMove) {
                --inputEvidence_;
                if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    const auto* stick = static_cast<const RE::ThumbstickEvent*>(event);
                    spdlog::debug(
                        "input evidence: THUMBSTICK id={} right={} x={:.2f} y={:.2f}",
                        stick->idCode, stick->IsRight(), stick->xValue, stick->yValue);
                } else {
                    const auto* id = event->AsIDEvent();
                    spdlog::debug(
                        "input evidence: type={} device={} id={} userEvent='{}'",
                        static_cast<int>(event->eventType.get()),
                        static_cast<int>(event->GetDevice()),
                        id ? id->idCode : 0xFFFFFFFF,
                        id ? id->userEvent.c_str() : "");
                }
            }
            if (!s.previewSpin) {
                continue;  // evidence still collects above
            }
            if (const auto* button = event->AsButtonEvent(); button) {
                if (button->device.get() == RE::INPUT_DEVICE::kMouse &&
                    button->GetIDCode() == 1) {
                    spinDragging_ = button->Value() > 0.0f;
                }
            } else if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kMouseMove &&
                       spinDragging_.load()) {
                const auto* move = static_cast<const RE::MouseMoveEvent*>(event);
                spinTarget_ += static_cast<float>(move->mouseInputX) *
                               Settings::GetSingleton().spinSensitivity;
            } else if (event->eventType.get() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                // F-14 v3: DIRECT right-stick rotation (the design SPIM
                // itself ships with iGamepadTurnMethod=0 - no hold button;
                // r30's hold-gate also compared idCode against 274 while
                // the engine delivers shoulder buttons as 9/10, so the
                // hold could never register). The tick integrates the
                // stored deflection; the inspect gate lives there too.
                const auto* stick = static_cast<const RE::ThumbstickEvent*>(event);
                if (stick->IsRight()) {
                    spinStickX_ = stick->xValue;
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl Bubble::ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const std::string name{ a_event->menuName.c_str() };
        if (!Settings::GetSingleton().IsBubbleMenu(name)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (a_event->opening) {
            ++menusOpen_;
            currentMenuName_ = name;  // own-view coverage decides per menu at arm
            graceFrames_ = 0;
            gateHoldFrames_ = 0;
            loggedUnpausedOnce_ = false;
            // B-8 v2: a bubble menu opened before the deferred exit mirror
            // fired - this close was a SWITCH, the walk must not restart.
            if (pendingExitMoveStart_) {
                pendingExitMoveStart_ = false;
                spdlog::debug("idle-in-menus: deferred moveStart cancelled (menu switch).");
            }
            if (name == "RaceSexMenu") {
                raceMenuOpen_ = true;
                spdlog::info("RaceSexMenu bubbled (EXPERIMENTAL, F-9): anim/face/caster "
                             "ticks defer to the engine's racemenu mode; input gate off.");
            }
            spdlog::info("Bubble menu opened: {} (open count {}).", name, menusOpen_.load());
            // §3.1 force-pause FIRST: if Skyrim Souls stripped kPausesGame,
            // restore it now so the pause is live before anything below
            // reads IsBubbleActive() - the whole arm path then behaves
            // exactly like a vanilla paused menu.
            ForcePause::EnsurePaused(name);
            // Menu SWITCH: this open lands with the bubble still armed
            // (grace bridges the gap), so the arm-edge settle won't re-run -
            // but the close-side mirror a frame ago just restarted
            // locomotion for a menu that died immediately (field r20:
            // "switch to magic menu and we are walking again"). Re-settle.
            if (armedLastFrame_ && Settings::GetSingleton().idleInMenus &&
                !raceMenuOpen_.load()) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* state = player ? player->AsActorState() : nullptr;
                const bool locomoting = state && (state->actorState1.walking ||
                                                  state->actorState1.running ||
                                                  state->actorState1.sprinting);
                if (locomoting && !player->IsInMidair() && !state->actorState1.swimming) {
                    player->SetGraphVariableFloat("Speed", 0.0f);
                    player->SetGraphVariableFloat("Direction", 0.0f);
                    player->SetGraphVariableFloat("TurnDelta", 0.0f);
                    player->NotifyAnimationGraph("moveStop");
                    sentMoveStop_ = true;
                    spdlog::debug("idle-in-menus: re-settled on menu switch.");
                }
            }
            // F-15 r35: a re-open inside the switch gap cancels the pending
            // restore - the framing never came down, the switch is seamless
            // (ArmOwnViewIfOurs below no-ops while OwnView stays active; it
            // still frames the new menu when the previous one wasn't ours).
            if (armedLastFrame_) {
                if (pendingOwnViewRestore_) {
                    pendingOwnViewRestore_ = false;
                    spdlog::debug("own view: restore cancelled (menu switch) - framing stays.");
                }
                ArmOwnViewIfOurs();
            }
            // F-12 v3 + declutter, in the SAME call stack as the menu open,
            // before this frame renders. The r24 timed dip showed the world
            // for a few frames under the menu UI (field: "when we see the
            // inventory UI it should already have been faded in") - the cut
            // to black now happens HERE, so the first rendered menu frame
            // is already dark and the cull swap hides behind it; the first
            // armed tick builds the studio and starts the fade-in.
            if (IsBubbleActive() && Settings::GetSingleton().declutterMode != 0) {
                const auto& s = Settings::GetSingleton();
                // r40: the close edge hands the sky mode back so weather audio
                // survives the exit's unpaused window - a switch re-open re-parks
                // it (idempotent for fresh arms; Apply's own park then no-ops).
                if (s.IsVoidFamily() && !raceMenuOpen_.load()) {
                    StudioLight::ReparkSkyMode();
                }
                // BLUE-VOID FLASH - DEFINITIVE FIX (RE Option P). The world-feeder
                // cull is immediate, but a freshly-attached shell can't occlude
                // until the NEXT frame (one-frame publish latency of new geometry
                // into the batch renderer - attaching earlier does NOT help; RE-
                // confirmed). Un-culling an already-RESIDENT node, however, is
                // render-side and immediate (the mod's own world-culls prove it).
                // So a persistent gap occluder is kept resident + AppCulled and
                // UN-CULLED here, in the open event, before the frame renders: when
                // it is warm it blocks the world the SAME frame the cull drops it →
                // gap-free, so cull now. When it had to build cold (first open per
                // session / after a cell change) it lags one frame like any fresh
                // node, so keep the no-fader interim (defer the cull to the Tick - a
                // brief real-world frame, never blue); it is warm from the next open.
                // (No fader - that captures menu input, HANDOFF l.14.) `Warm()`
                // preheats the real shell so it draws in promptly behind the
                // occluder. A menu SWITCH keeps the previous occluder up → no gap.
                if (!armedLastFrame_ && s.IsVoidFamily() && !raceMenuOpen_.load()) {
                    Backdrop::Warm();
                    const bool occWarm = Backdrop::OccluderShow();
                    spdlog::debug("open: gap occluder {} - cull {}.",
                                  occWarm ? "WARM (same-frame, gap-free)" : "cold (interim)",
                                  occWarm ? "now" : "deferred to Tick");
                    if (occWarm) {
                        Declutter::Refresh();
                    }
                } else {
                    Declutter::Refresh();
                }
            }
        } else {
            const int now = std::max(0, menusOpen_.load() - 1);
            menusOpen_ = now;
            if (name == "RaceSexMenu") {
                raceMenuOpen_ = false;
            }
            ForcePause::OnMenuClosed(name);
            // B-2/B-8 v2 exit mirror: DEFERRED past the switch window. At
            // close time a real exit and a menu switch look identical, and
            // firing here restarted the walk on every switch (field r23;
            // gaps measured 52-71 ms). OnFrame fires it at ~85 ms if no
            // bubble menu re-opened; conditions re-checked live there.
            if (now == 0 && armedLastFrame_ && sentMoveStop_) {
                pendingExitMoveStart_ = true;
                exitMoveQpc_ = QpcNow();
            }
            // r40 (field: "rain sfx stops when i exit the menu"): the sky
            // mode goes back RIGHT HERE, before a single unpaused frame -
            // Sky::Update ticking kNone in the exit's hold+dip window made
            // the engine stop the weather's rain loop, and steady rain has
            // no transition to re-trigger it. Visually free: the shell
            // keeps occluding the sky until the at-black teardown. A
            // switch re-open re-parks (open path above).
            if (now == 0 && armedLastFrame_) {
                StudioLight::RestoreSkyModeEarly();
                // r50: with hold 0 the cut happens IN THIS CALL STACK -
                // the OnFrame machine ran one frame late (field: "quite
                // literally one frame where i see this after i close").
                // Same mechanism as the open-side r25 lesson: mutate
                // before the frame renders, and there is no frame to see.
                const auto& s = Settings::GetSingleton();
                if (s.sleekExit && s.declutterMode >= 2 &&
                    s.exitHoldSeconds <= 0.0f) {
                    if (pendingOwnViewRestore_) {
                        pendingOwnViewRestore_ = false;
                        OwnView::Disarm();
                    }
                    Transition::Snap(0.0f);
                    Disarm();
                    spdlog::debug("sleek exit: cut in the close event "
                                  "(zero-frame).");
                }
            }
            if (now == 0 && armedLastFrame_) {
                // Deferred teardown: menu SWITCHES (inventory -> magic) close
                // one bubble menu and open the next a frame or two apart.
                // Tearing down in that gap let the collision smoother press
                // the camera onto the player and churned declutter/lighting.
                // A short grace bridges the gap for the visual teardown; the
                // camera gate holds longer (the close->open gap of a switch
                // can span more frames than the grace - field-measured).
                graceFrames_ = 6;
                gateHoldFrames_ = 30;  // ~0.5 s
                // F-15 r35: the r33 close-event restore fixed the EXIT
                // flash but created a SWITCH flash - the unpaused gap
                // frames rendered the restored heading, then the re-frame
                // snapped back ("a frame where our character is not in the
                // right rotation"). Defer the restore past the measured
                // switch gap; a re-open cancels it and the SAME framing
                // simply stays up (seamless switch). A real exit keeps the
                // framed pose ≤85 ms under the menu-close fade - the
                // lesser residual of the two.
                if (OwnView::Active()) {
                    pendingOwnViewRestore_ = true;
                    ownViewRestoreQpc_ = QpcNow();
                }
                // F-12: start the exit dissolve - UNLESS the v4 black-fade
                // exit will run (it holds the studio at full and covers the
                // restore with the fader instead; a dissolve underneath it
                // would blank the pieces early). With transitions off (or
                // out=0) nothing is started either: the grace holds the
                // pieces exactly as pre-F-12.
                const auto& s = Settings::GetSingleton();
                const bool dipExit = s.sleekTransitions && s.dipToBlack &&
                                     s.declutterMode >= 2;
                if (!dipExit && s.sleekTransitions && s.transitionOutSeconds > 0.0f) {
                    Transition::SetTarget(0.0f);
                }
            }
            spdlog::info("Bubble menu closed: {} (open count {}).", name, now);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    void Bubble::OnFrame(RE::Main* a_main) {
        const auto& settings = Settings::GetSingleton();
        if (!settings.enabled || !a_main) {
            graceFrames_ = 0;
            Disarm();
            return;
        }
        if (menusOpen_.load() <= 0) {
            FireDeferredExitMoveStart();
            if (gateHoldFrames_.load() > 0) {
                --gateHoldFrames_;
            }
            const bool graceLive = graceFrames_.load() > 0;
            if (graceLive) {
                --graceFrames_;
            }
            if (armedLastFrame_) {
                const auto& s = Settings::GetSingleton();
                // r38 SLEEK EXIT: the r26 exit machine returns on its own
                // switch (bSleekExit), decoupled from the parked open-side
                // dip. r37 field made the need visible in exteriors: the
                // legacy path ran every restore in FULL VIEW - terrain/LOD
                // popped back as black holes for the frames between the UI
                // closing and Disarm ("for a few frames i can see this").
                // The machine is also the SKILLS MENU's own exit
                // choreography (StatsMenu::ProcessMessage case 3 calls the
                // same fader on close - mtb_statsmenu.c), which is the
                // presentation the user keeps pointing at.
                const bool exitMachine = s.sleekExit && s.declutterMode >= 2;
                // F-15 r35: the deferred view restore fires once the switch
                // window passed - this close was a real exit. With the exit
                // machine on it fires AT BLACK instead (the first-person
                // snap was visible in the first darkening frames).
                if (!exitMachine && pendingOwnViewRestore_ &&
                    QpcSeconds(ownViewRestoreQpc_, QpcNow()) >= 0.085f) {
                    pendingOwnViewRestore_ = false;
                    OwnView::Disarm();
                }
                // r31 field: menu SWITCHES stutter the spun character for a
                // split frame - the close→open gap runs UNPAUSED, so the
                // engine's own player update re-syncs the node from
                // data.angle (the pinned entry heading) and our armed tick
                // isn't running to re-compose. This hook site sits right
                // after that engine update: re-assert before the frame
                // renders. (The vfunc-0x3F hook covers explicit
                // Update3DPosition calls; the unpaused frame path isn't
                // guaranteed to route through it.)
                ReassertSpin();
                // r44 (field: "during the transition we see the skybox"):
                // these gap/exit frames run UNPAUSED - Sky::Update un-culls
                // its branch each frame; pin the world-feeder culls back
                // down until the at-black restore hands the sky over for
                // the fade-in.
                Declutter::ReassertWorldFeederCulls();
                // Closed mid-arm-dip (black since open, insta-closed): the
                // build never ran - bring the light back before anything.
                CancelDipIfActive();
                const auto now = QpcNow();
                const float dt = std::clamp(QpcSeconds(lastQpc_, now), 0.0f,
                                            Settings::GetSingleton().maxDeltaTime);
                lastQpc_ = now;
                if (exitMachine) {
                    // r47 EXIT v3 - THE INSTANT CUT (user spec: "the
                    // constellation and void should not even be visible
                    // the moment we switch out of the menu"). The fader
                    // choreography is DEAD: four rounds of field evidence
                    // say its black never reliably covers under ENB -
                    // every "restored under the black" was partially
                    // visible (r38 terrain holes, r43 skybox, r46 "entire
                    // menu view"). Hold the studio through the switch
                    // window (switches cancel for free, r36 seamlessness
                    // untouched), then restore EVERYTHING in one frame:
                    // world, studio, camera, all before the next render.
                    // Vanilla menus cut; so do we.
                    if (exitPhase_ == 0) {
                        exitPhase_ = 1;
                        exitQpc_ = now;
                    }
                    if (exitPhase_ == 1 &&
                        QpcSeconds(exitQpc_, now) >= s.exitHoldSeconds) {
                        exitPhase_ = 0;
                        if (pendingOwnViewRestore_) {
                            pendingOwnViewRestore_ = false;
                            OwnView::Disarm();
                        }
                        Transition::Snap(0.0f);
                        Disarm();
                        spdlog::debug("sleek exit: instant cut at {:.0f} ms "
                                      "(hold window passed).",
                                      s.exitHoldSeconds * 1000.0f);
                        return;
                    }
                    return;  // holding through the switch window
                }
                // Legacy exit (no dip): the dissolve runs THROUGH the grace
                // window - only fade values move; teardown holds until the
                // ramp lands (QPC-bounded).
                Transition::Tick(dt);
                StudioRig::PushFade();
                Backdrop::PushFade();
                if (graceLive || Transition::FadingOut()) {
                    return;  // bridging a switch / dissolving - hold state
                }
            }
            Disarm();
            return;
        }
        graceFrames_ = 0;

        // Tripwire for a mod re-stripping kPausesGame mid-session: a lost
        // flag would lose the engine's close-time decrement with it. Runs
        // regardless of pause state (the leak hazard is exactly when the
        // flag vanished).
        ForcePause::Reassert();

        auto* ui = RE::UI::GetSingleton();
        const bool paused = a_main->freezeTime || (ui && ui->GameIsPaused());

        if (!paused) {
            // Skyrim Souls (or similar) is running this menu unpaused - the
            // engine ticks the player itself; the bubble must not double-tick.
            if (!loggedUnpausedOnce_) {
                spdlog::info("Bubble menu open but game is UNPAUSED (Skyrim Souls?) - "
                             "bubble dormant for this menu session.");
                loggedUnpausedOnce_ = true;
            }
            Disarm();
            return;
        }

        Tick(a_main, paused);
    }

    void Bubble::Tick(RE::Main* a_main, bool a_paused) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) {
            Disarm();
            return;
        }

        // r47: a re-open cancelled the exit machine - phase 1 is a pure
        // hold (nothing visible changed), so cancelling costs nothing.
        exitPhase_ = 0;

        const auto now = QpcNow();
        float dt = armedLastFrame_ ? QpcSeconds(lastQpc_, now) : (1.0f / 60.0f);
        lastQpc_ = now;
        dt = std::clamp(dt, 0.0f, Settings::GetSingleton().maxDeltaTime);

        if (!armedLastFrame_) {
            armedTicks_ = 0;
            telemetryCountdown_ = 0;
            // TRACKER B-7: the heading the player brought into the menu is
            // the one the whole session previews from - snapshot it here,
            // re-asserted after every graph tick below.
            armedHeading_ = player->data.angle.z;
            // B-7 v3 preview spin: park the camera where SPIM's open-time
            // setup put it and capture the root's engine-made rotation for
            // that heading - the spin composes on this immutable basis
            // every tick (never on the node's live value: whether the
            // engine re-syncs node←angle per tick or only on dirty flags,
            // an absolute basis cannot compound).
            spinTarget_ = 0.0f;
            spinYaw_ = 0.0f;
            spinDragging_ = false;
            spinStickX_ = 0.0f;
            spinBasisValid_ = false;
            inputEvidence_ = 24;  // F-14 v3: raw input events logged this arm
            // F-15 phase 2: the own-view decision + framing live in
            // ArmOwnViewIfOurs(), called after the B-4 camera refresh below
            // (and re-used by menu switches + the late-arm fallback).
            if (auto* tps = GetThirdPersonState()) {
                freeRotArm_ = tps->freeRotation.x;
            }
            if (auto* root = player->Get3D(false)) {
                rootBaseRotate_ = root->local.rotate;
                spinBasisValid_ = true;
            }
            // r54: capture the mount's root so the spin rotates it too.
            spinHorseRoot_.reset();
            if (RE::ActorPtr mount; player->GetMount(mount) && mount) {
                auto* horseRoot = mount->Get3D(false);
                if (horseRoot) {
                    spinHorseRoot_ = RE::NiPointer<RE::NiAVObject>{ horseRoot };
                    horseBaseRotate_ = horseRoot->local.rotate;
                }
                spdlog::info("spin: mount '{}' - 3D root {} (spin will {}rotate it).",
                             mount->GetName() ? mount->GetName() : "?",
                             horseRoot ? "captured" : "NULL",
                             horseRoot ? "" : "NOT ");
            }
            // Menu-drag rotation exceeds FSMP's 10 rad/s player clamp, which
            // clamps or hard-resets the sim ("momentum isn't kept"). Lift the
            // thresholds while armed; Disarm() restores the originals.
            FsmpDrive::SetRotationFreedom(true);
            if (Settings::GetSingleton().driveCbpc) {
                // CBPC keeps body physics simulating in paused menus via its
                // own RaceMenu special case; borrow it for the bubble.
                CbpcDrive::SetSimulateWhilePaused(true);
            }
            // Feet of Skyrim forces foot IK onto the (now hidden) real ground;
            // stand it down in the void / dressing room so the feet keep the
            // neutral animated pose on the stage. Restored on disarm / reset.
            // Void / dressing room hide the real ground, so foot IK must stand
            // down. Oblivion keeps the real ground visible, so foot IK stays ON
            // - gate on the void family, not >= 2.
            FootIkGate::SetSuppressed(Settings::GetSingleton().IsVoidFamily());
            // F-12 v4: if the open event cut to black, the world is already
            // culled behind it - build the studio FINISHED (T snapped to 1:
            // the r25 ramps under the reveal read as "harsh popping"), hold
            // the black a few frames so the light re-ingest and first-time
            // mesh demands land, then reveal a static, fully lit studio
            // with one clean fade. Otherwise the v1 bloom.
            {
                if (dipPhase_ == 1) {
                    dipPhase_ = 0;
                    Transition::Snap(1.0f);
                    StudioLight::Apply();
                    StudioRig::Apply();
                    Backdrop::Apply();
                    dipHoldFrames_ = 3;
                    spdlog::debug("dip: studio built at black - revealing in {} frames.",
                                  dipHoldFrames_);
                } else {
                    // Instant open: SNAP the studio + backdrop to full so the
                    // additive constellation dome shows the SAME frame as the
                    // opaque void shell. Fading in from 0 renders the shell (a
                    // solid dark sphere even at t=0) first and pops the emissive
                    // stars in a beat later, which read as an "empty void" flash
                    // on open. Close still dissolves (its edge sets the target 0).
                    Transition::Snap(1.0f);
                    StudioLight::Apply();
                    StudioRig::Apply();
                    Backdrop::Apply();
                }
            }
            // Colour filter (any view, off by default): grade the whole menu
            // scene through the imagespace base override. Sync applies it when on
            // and restores when off, so this is a guarded no-op when the filter is
            // off. Mode-independent - a view switch below leaves it untouched.
            {
                const auto& cfg = Settings::GetSingleton();
                SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
            }
            // TRACKER B-2: the walk→idle TRANSITION is event-driven (the
            // r17 float route field-failed) - send the engine's own
            // input-stopped edge ONCE at arm, exactly what releasing the
            // keys produces. NOT r11's banned state-forcing; gated to
            // grounded locomotion so air/swim states are never touched.
            // sentMoveStop_ mirrors at exit: the controller only emits
            // moveStart on an input EDGE, and a key held across the menu
            // has none (field r18: exit slide) - Disarm sends it back.
            sentMoveStop_ = false;
            airFrozenArm_ = false;
            if (Settings::GetSingleton().idleInMenus && !raceMenuOpen_.load()) {
                auto* state = player->AsActorState();
                const bool locomoting = state && (state->actorState1.walking ||
                                                  state->actorState1.running ||
                                                  state->actorState1.sprinting);
                // r36 (user design): the graph ticks ONLY for standing
                // idles and the locomotion-to-still settle below -
                // EVERYTHING else holds its caught frame (mid-air r18,
                // attacks r34, and now draw/sheathe transitions, furniture
                // enter/exit, swimming, and scripted idles - interactions,
                // crafting - detected via bAnimationDriven at the arm edge,
                // the same flag non-idle clips raise, B-7). Face, hair and
                // body physics keep living either way.
                const auto weaponState = state->actorState2.weaponState;
                const bool weaponTransition =
                    weaponState == RE::WEAPON_STATE::kWantToDraw ||
                    weaponState == RE::WEAPON_STATE::kDrawing ||
                    weaponState == RE::WEAPON_STATE::kWantToSheathe ||
                    weaponState == RE::WEAPON_STATE::kSheathing;
                const auto sitSleep = state->GetSitSleepState();
                const bool furnitureTransition =
                    sitSleep != RE::SIT_SLEEP_STATE::kNormal &&
                    sitSleep != RE::SIT_SLEEP_STATE::kIsSitting;
                bool scriptedIdle = false;
                player->GetGraphVariableBool("bAnimationDriven", scriptedIdle);
                // r39 (field: "TK dodge and dodge mod animations aren't
                // paused"): a dodge reads as plain grounded locomotion -
                // none of the states below catch it, the graph keeps
                // ticking, and the dodge clip plays out in the menu. Dodge
                // mods PUBLISH their state as a graph bool though
                // (bIsDodging = the TK Dodge RE / TUDM convention; DMCO
                // spellings carried too - unknown names read false, free).
                // Any true flag freezes the arm exactly like an attack;
                // sFreezeGraphBools extends the list without a rebuild.
                std::string dodgeFlag;
                for (const auto& var : Settings::GetSingleton().freezeGraphBools) {
                    bool set = false;
                    if (player->GetGraphVariableBool(var.c_str(), set) && set) {
                        dodgeFlag = var;
                        break;
                    }
                }
                const char* freezeReason =
                    player->IsInMidair()                                        ? "mid-air"
                    : state->actorState1.meleeAttackState !=
                          RE::ATTACK_STATE_ENUM::kNone                          ? "mid-attack"
                    : weaponTransition                                          ? "draw/sheathe"
                    : furnitureTransition                                       ? "furniture/sleep transition"
                    : state->actorState1.swimming                               ? "swimming"
                    : scriptedIdle                                              ? "scripted idle (animation-driven)"
                                                                                : nullptr;
                if (freezeReason) {
                    airFrozenArm_ = true;
                    spdlog::debug("idle-in-menus: armed {} - anim tick frozen this arm.",
                                  freezeReason);
                } else if (!dodgeFlag.empty()) {
                    airFrozenArm_ = true;
                    spdlog::debug("idle-in-menus: armed mid-dodge ('{}' set) - "
                                  "anim tick frozen this arm.", dodgeFlag);
                } else if (locomoting && !state->actorState1.swimming) {
                    // Blend inputs to "no input" ONCE - writing these per
                    // tick fought the preview's turn input all menu long
                    // (field r19: "cannot rotate the player").
                    player->SetGraphVariableFloat("Speed", 0.0f);
                    player->SetGraphVariableFloat("Direction", 0.0f);
                    player->SetGraphVariableFloat("TurnDelta", 0.0f);
                    player->NotifyAnimationGraph("moveStop");
                    sentMoveStop_ = true;
                    spdlog::debug("idle-in-menus: moveStop sent at arm (grounded locomotion).");
                }
            }
            // F-13: papyrus expression mods are frozen by the pause (B-6) -
            // an arm can catch the face MID-SEQUENCE (half a blink) and
            // hold it all menu. Save the expression, ramp to neutral via
            // the engine's own facegen reset (our face tick animates it),
            // restore at disarm. RaceMenu owns its face; needs the tick.
            if (Settings::GetSingleton().neutralExpression &&
                Settings::GetSingleton().tickFace && !raceMenuOpen_.load()) {
                FaceNeutral::Apply();
            }
            // TRACKER B-4: SPIM pushes exactly ONE camera update at menu
            // open and then only moves the camera on input (spim_camera.c:
            // RotateCamera tail vfunc call). If that update beat our
            // open-event bookkeeping, the collision smoother was still
            // ungated and clamped the camera onto the player at close
            // walls - where it then FROZE for the whole menu (r15 log:
            // one arm at horizontal 69 vs the healthy arms' constant 126).
            // Re-run the update now that the gate is provably live: the
            // builder recomputes the raw orbit position through the
            // (culled) wall.
            if (auto* camera = RE::PlayerCamera::GetSingleton();
                camera && camera->currentState &&
                camera->currentState->id == RE::CameraState::kThirdPerson) {
                camera->Update();
                spdlog::debug("camera refreshed at arm (B-4 re-extend).");
            }
            // F-15 phase 2: force third + apply the FULL SPIM framing when
            // the bubble owns this menu's view (originals saved inside; the
            // close event restores).
            ArmOwnViewIfOurs();
            appliedRevision_ = Settings::GetSingleton().revision;
            appliedMode_ = Settings::GetSingleton().declutterMode;
            spdlog::info("Bubble ARMED (paused menu, player 3D loaded).");
        }
        armedLastFrame_ = true;
        ++armedTicks_;

        // F-12 v4: the post-build black hold - reveal once it elapses. Dormant
        // now (nothing sets dipPhase_ since the fader open-cover was reverted);
        // the dip machinery is left intact for the definitive fix to reuse if it
        // needs a NON-fader build-then-show.
        if (dipHoldFrames_ > 0 && --dipHoldFrames_ == 0) {
            g_fadeOutGame(false, true, Settings::GetSingleton().dipInSeconds, false, 0.0f);
            spdlog::debug("dip: revealing ({:.2f}s).", Settings::GetSingleton().dipInSeconds);
        }

        // F-15 r33 fallback: rare field case - still FIRST person a few
        // ticks in and nobody framed the view (a covered view mod skipped
        // its own conditions for this menu). ownViewFirstPerson's contract
        // is "you always see your character": late-arm our view.
        if (armedTicks_ == 3 && !OwnView::Active() && !raceMenuOpen_.load() &&
            Settings::GetSingleton().ownViewFirstPerson) {
            if (auto* camera = RE::PlayerCamera::GetSingleton();
                camera && camera->currentState &&
                camera->currentState->id == RE::CameraState::kFirstPerson) {
                spdlog::info("own view: still first person at tick 3 - the covering "
                             "view mod skipped this menu; late-arming our view.");
                ArmOwnViewIfOurs(true);  // r45: bypass coverage - nobody framed
            }
        }

        // SPIM's drag handler refuses ALL rotation while the actor-state
        // direction bits are set (spim_input.c: movingBack/Forward/Right/
        // Left gates every rotation path) - a menu opened mid-walk freezes
        // them ON for the whole pause (the release edge never reaches the
        // paused movement controller), which is why the preview wouldn't
        // rotate after walking in (field r25). They are per-frame INPUT
        // state, not graph state: clear them while armed; the movement
        // controller re-derives them from live input the moment the world
        // unpauses. No graph events, no behavior patch. Gated with the
        // preview spin (it serves SPIM-driven rotation setups; our own
        // input sink doesn't read these bits).
        if (Settings::GetSingleton().previewSpin) {
            if (auto* state = player->AsActorState()) {
                state->actorState1.movingBack = 0;
                state->actorState1.movingForward = 0;
                state->actorState1.movingRight = 0;
                state->actorState1.movingLeft = 0;
            }
        }

        // Declutter: the INITIAL cull for a fresh VOID open is deferred to HERE
        // (the open handler skips it when the occluder covers the swap), gated to
        // armedTicks_ 2 so the shell (attached by the arm block on tick 1) is up
        // before the sky is dropped. Then re-cull ~4x/s (the paused SPIM camera
        // can move/zoom, shifting the occluder corridor). Switches / non-void
        // opens already culled in the open event; culling again here is a harmless
        // idempotent top-up. (armedTicks_ is unsigned - guard the subtraction.)
        if (armedTicks_ >= 2 && (armedTicks_ - 2) % 15 == 0) {
            Declutter::Refresh();
        }

        // Live settings: the panel bumps the revision on every save (it can
        // be open on top of a bubble menu). Rig values flow per-tick inside
        // StudioRig::Tick; the cell look re-applies + re-ingests once per
        // change here.
        if (const auto rev = Settings::GetSingleton().revision; rev != appliedRevision_) {
            appliedRevision_ = rev;
            const auto& cfg = Settings::GetSingleton();
            if (const int mode = cfg.declutterMode; mode != appliedMode_) {
                appliedMode_ = mode;
                // B-5: a view-mode switch must UNDO the old mode's culls
                // right away - the periodic sweep only ever ADDS hides, so
                // without this the change waits for the next menu open.
                Declutter::RestoreAll();
                if (mode != 0) {
                    Declutter::Refresh();
                }
                // StudioLight owns the void's cell lighting: restore it when
                // leaving the void family, apply it when entering (self-gated to
                // 2/3). The colour filter is view-INDEPENDENT, so a mode switch
                // leaves SceneTint exactly as the filter toggle set it.
                if (!cfg.IsVoidFamily()) {
                    StudioLight::Restore();
                }
                if (cfg.IsVoidFamily()) {
                    StudioLight::Apply();
                }
                spdlog::info("view mode switched to {} mid-menu (live).", mode);
            }
            StudioLight::LiveRefresh();
            // Colour filter: apply / refresh / restore per the toggle, so turning
            // it on or off mid-menu (and slider drags) all land live.
            SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
        }
        // F-12: keep the ramp aimed up while armed (a menu switch flips it
        // down at the close edge; landing back here reverses it mid-flight)
        // and step it BEFORE the consumers read Value() this frame.
        Transition::SetTarget(1.0f);
        Transition::Tick(dt);
        StudioRig::Tick();
        Backdrop::Tick();

        if (Settings::GetSingleton().freezeHeadTracking) {
            // TRACKER B-3: SPIM already turns head tracking off in menus
            // (IsNPC=false, spim_camera.c), but another mod (TDM's
            // headtracking rides the very graph tick below) keeps
            // re-aiming the head at a stale world direction while the
            // body rotates. Pin the head-track target straight ahead of
            // the BODY every tick - neutral head regardless of who
            // drives, nothing to restore (gameplay recomputes).
            if (auto* process = player->GetActorRuntimeData().currentProcess) {
                // B-7 v3: "ahead" follows the VISUAL body - the preview
                // spin rotates the node, so the head target spins with it
                // (else a spun body twists its neck toward the old front).
                // F-13: target z at EYE level (the engine's own eye
                // position), not the old feet+120 constant - the eye
                // look-up/down modifiers track this target, and a target
                // above the eyes pins the gaze upward ("stuck eyes" lead 2;
                // the constant also missed on scaled skeletons).
                const float heading = player->data.angle.z + spinYaw_;
                const float eyeZ = player->GetLookingAtLocation().z;
                RE::NiPoint3 ahead{ player->GetPositionX() + std::sin(heading) * 500.0f,
                                    player->GetPositionY() + std::cos(heading) * 500.0f,
                                    eyeZ };
                process->SetHeadtrackTarget(player, ahead);
            }
        }

        // RaceSexMenu (F-9, experimental): the engine's own racemenu mode
        // already animates the player (the same special path CBPC keys
        // off) - our graph/face/caster ticks would double-step it. The
        // space/lighting/rig/stage and the FSMP drive still run.
        const bool engineAnimates = raceMenuOpen_.load();

        if (Settings::GetSingleton().tickAnimation && !engineAnimates && !airFrozenArm_) {
            // TESObjectREFR vfunc 0x7D - steps the behavior graph with our dt;
            // the PlayerCharacter override also refreshes 1st+3rd person
            // graphs. No pause gate inside (docs/SPIKE-A-RE.md).
            player->UpdateAnimation(dt);
        }

        if (Settings::GetSingleton().tickFace && !engineAnimates) {
            // Facegen is a separate system from the behavior graph: blink
            // timers and MFG expression/phoneme/modifier ramps only advance
            // in BSFaceGenAnimationData::Update, whose engine caller is
            // render-model-side and pause-gated.
            if (auto* face = player->GetFaceGenAnimationData()) {
                // r33's post-update blink zero STILL blinked in the field -
                // order of operations: Update() BAKES the morph geometry
                // from the keyframe values as they stand, so a value some
                // mod wrote since our last tick was already baked before
                // the post-zero cleaned the keyframe. Pin BEFORE the bake
                // (and keep the post-zero so samplers between ticks read
                // clean). If eyes STILL blink with both pins, the writer
                // runs INSIDE Update (MFG Fix re-drive) - the telemetry
                // blink column is the discriminator.
                const bool pinBlinks =
                    Settings::GetSingleton().neutralExpression && !engineAnimates;
                const auto zeroBlinks = [&face] {
                    if (auto& mod = face->modifierKeyFrame;
                        mod.values && mod.count > 1 &&
                        (mod.values[0] != 0.0f || mod.values[1] != 0.0f)) {
                        mod.SetValue(0, 0.0f);
                        mod.SetValue(1, 0.0f);
                    }
                };
                if (pinBlinks) {
                    // r34.5 - DECOMPILE VERDICT (mtb_blinkgen.c, the ambient
                    // blink generator 0x1403c2930): +0x200 is a STATE machine
                    // (0 waiting / 1 closing / 2 opening / 3-4 holds) and
                    // +0x204 (blinkDelay) is its TIMER. In state 0 the timer
                    // just counts down and NOTHING is written - but an arm
                    // that catches state 1/2 with our timer pin holds the
                    // machine MID-BLINK forever, rewriting the eyelids inside
                    // every Update (baked before any post-zero could clean
                    // it). The r32-r34 "still blinking" was OUR OWN pin.
                    // State 0 + fat timer = the machine idles silently.
                    face->unk200 = 0;         // blink state: WAITING
                    face->blinkDelay = 2.0f;  // its countdown, never reaches 0
                    zeroBlinks();
                }
                face->unk218 = 0;  // pending-force flag, cleared by the engine caller too
                g_faceGenUpdate(face, dt, true);
                if (pinBlinks) {
                    zeroBlinks();  // other writers (MFG-style mods) stay covered
                }
            }
        }

        if (Settings::GetSingleton().tickMagicCasters && !engineAnimates) {
            // Equipped-spell hand art attaches in ActorMagicCaster::Update
            // (vfunc 0x1D), which runs on the paused world clock - so a
            // spell equipped in MagicMenu shows nothing until unpause. The
            // charge/drain paths inside are casting-state-gated (states 2/6
            // can't occur in a menu), leaving art maintenance + art-3D tick,
            // which is exactly what we want (decompile: mtb_magiccaster.c).
            for (const auto source : { RE::MagicSystem::CastingSource::kLeftHand,
                                       RE::MagicSystem::CastingSource::kRightHand }) {
                if (auto* caster = player->GetMagicCaster(source)) {
                    static_cast<RE::ActorMagicCaster*>(caster)->Update(dt);
                }
            }
        }

        // THE PREVIEW CONTRACT: any non-idle clip sets bAnimationDriven,
        // and the third-person camera locks free rotation while animation-
        // driven (the vanilla killmove/scripted-idle rule; SPIM fights the
        // same thing via m_shouldDisableAnimCam, spim_rotate.c). Cleared
        // AFTER the graph tick so the flag reads false for the rest of the
        // frame. The r22 forensics acquitted this flag as the LIVE rotation
        // blocker (animDriven=false in every sampled row) - the clear stays
        // as contract hygiene: animations play, they never drive.
        player->SetGraphVariableBool("bAnimationDriven", false);

        // THE PREVIEW SPIN (F-14, opt-in). Rotation at the one level
        // nothing recomputes while paused: the player's ROOT NODE, set
        // absolutely from the arm-time basis - above whatever eats body
        // rotation below (non-idle clips, r20-22), under any animation,
        // with real hair/cloth physics (the skeleton actually moves - a
        // camera orbit moves nothing). Input: our own right-drag sink
        // accumulates spinTarget_; the easing below makes it silk. The
        // SPIM freeRot harvest stays for setups where SPIM still drives
        // (its writes read as drag intent; SPII camera builds never touch
        // freeRot, so it no-ops there). data.angle stays pinned while
        // spinning: the basis holds and the exit heading is the entry
        // heading. Disarm/ForceReset un-compose the node.
        // r55: preview spin DISABLED while mounted (user: "disable rotation
        // when we are on a horse for now"). The horse is a separate actor
        // and rotating the player root alone spun the rider on a still
        // horse; the shared-yaw mount rotation (r54) didn't take. Revisit
        // with the mount 3D root diagnostic if mounted spin is wanted.
        if (Settings::GetSingleton().previewSpin && !engineAnimates &&
            !player->IsOnMount()) {
            player->data.angle.z = armedHeading_;
            if (auto* tps = GetThirdPersonState()) {
                const float delta = freeRotArm_ - tps->freeRotation.x;
                if (delta != 0.0f) {
                    if (std::fabs(delta) > 1.0f) {
                        // Not a drag: SPIM re-ran its open-time camera
                        // setup (menu switch re-park) - adopt the new park
                        // instead of spinning through the jump.
                        freeRotArm_ = tps->freeRotation.x;
                    } else {
                        spinTarget_ += delta;
                        tps->freeRotation.x = freeRotArm_;
                    }
                }
            }
            // Controller (F-14 v3): DIRECT right-stick rotation - no hold
            // button (the design SPIM itself ships; its Nolvus preset runs
            // iGamepadTurnMethod=0). The item-preview conflict is solved by
            // SPIM's own inspect gate: the stick spins the character only
            // while the item 3D is NOT zoomed; MenuInputGate blanks the
            // menu's item-rotate user event in exactly that window, so one
            // input means one thing per mode. Deadzone for stick drift.
            if (const float stickX = spinStickX_.load();
                std::fabs(stickX) > 0.15f && ItemPreviewZoomedOut()) {
                spinTarget_ += stickX *
                               Settings::GetSingleton().spinStickSensitivity * dt;
            }
            spinYaw_ += (spinTarget_ - spinYaw_) * (std::min)(1.0f, dt * 14.0f);
            if (spinBasisValid_) {
                RE::NiMatrix3 spin;
                spin.EulerAnglesToAxesZXY(0.0f, 0.0f, spinYaw_);
                if (auto* root = player->Get3D(false)) {
                    root->local.rotate = spin * rootBaseRotate_;
                }
                if (auto* horse = spinHorseRoot_.get()) {  // r54: rider + horse together
                    horse->local.rotate = spin * horseBaseRotate_;
                }
            }
        }

        if (Settings::GetSingleton().tickAnimation || Settings::GetSingleton().tickFace ||
            Settings::GetSingleton().tickMagicCasters || spinYaw_ != 0.0f) {
            // The paused scene graph may never run a downward pass, leaving
            // new local transforms/morphs invisible (the preview spin's
            // root write needs the same propagation). Same kick AP uses
            // after its paused-menu equipment rebuild.
            if (auto* third = player->Get3D(false)) {
                RE::NiUpdateData ctx;
                ctx.time = 0.0f;
                ctx.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
                third->Update(ctx);
            }
        }

        if (Settings::GetSingleton().driveSmp) {
            FsmpDrive::Step(dt);
        }

        if (Settings::GetSingleton().verboseLog && telemetryCountdown_-- == 0) {
            telemetryCountdown_ = 60;  // roughly once a second
            LogTelemetry(a_main, a_paused, dt);
        }
    }

    void Bubble::LogTelemetry(RE::Main* a_main, bool a_paused, float a_dt) {
        auto* ui = RE::UI::GetSingleton();
        // Camera columns: state id + world position - evidence for the
        // parked camera-collision work (does the pull-in happen live?).
        auto* camera = RE::PlayerCamera::GetSingleton();
        const auto* camState = camera ? camera->currentState.get() : nullptr;
        const auto camId = camState ? static_cast<int>(camState->id) : -1;
        RE::NiPoint3 camPos;
        if (auto* root = camera ? camera->cameraRoot.get() : nullptr) {
            camPos = root->world.translate;
        }
        // B-7 rotation forensics (kept until the pin is field-confirmed):
        // heading == pin in every row means the body pin holds against
        // whatever rode the graph tick; freeRot moving freely on drag with
        // the sum no longer constant = the orbit is really orbiting. If
        // heading stays pinned in the LOG but the body still visibly
        // rotates, the writer is node-side (below data.angle) - that would
        // be the next lead, and this line is the discriminator.
        float freeRotX = 0.0f, freeRotY = 0.0f;
        bool freeRotEnabled = false;
        if (camState && camState->id == RE::CameraState::kThirdPerson) {
            const auto* tps = static_cast<const RE::ThirdPersonState*>(camState);
            freeRotX = tps->freeRotation.x;
            freeRotY = tps->freeRotation.y;
            freeRotEnabled = tps->freeRotationEnabled;
        }
        bool animDriven = false;
        float heading = 0.0f;
        float blinkL = -1.0f, blinkDelay = -1.0f;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            player->GetGraphVariableBool("bAnimationDriven", animDriven);
            heading = player->data.angle.z;
            // F-13 discriminator: a 0.00 here with visible blinking on
            // screen convicts a writer INSIDE the face update (bake-time).
            if (auto* face = player->GetFaceGenAnimationData()) {
                if (face->modifierKeyFrame.values && face->modifierKeyFrame.count > 0) {
                    blinkL = face->modifierKeyFrame.values[0];
                }
                blinkDelay = face->blinkDelay;
            }
        }
        spdlog::debug(
            "tick #{:>5}: dt={:.4f} | engine slowDt={:.4f} realDt={:.4f} dt3={:.4f} | "
            "freezeTime={} numPauses={} uiPaused={} | fsmp={} cbpc={} | cam st={} "
            "pos=({:.1f},{:.1f},{:.1f}) | freeRot=({:.2f},{:.2f}) en={} animDriven={} "
            "heading={:.2f} pin={:.2f} | spin={:.2f}->{:.2f} park={:.2f} | T={:.2f} | "
            "blinkL={:.2f} bDelay={:.1f}",
            armedTicks_, a_dt, *g_slowDt.get(), *g_realDt.get(), *g_dtVariant3.get(),
            a_main->freezeTime, ui ? ui->numPausesGame : std::uint32_t(0xFFFF),
            a_paused, FsmpDrive::IsAvailable(), CbpcDrive::IsAvailable(), camId,
            camPos.x, camPos.y, camPos.z, freeRotX, freeRotY, freeRotEnabled, animDriven,
            heading, armedHeading_, spinYaw_, spinTarget_, freeRotArm_, Transition::Value(),
            blinkL, blinkDelay);
    }
}
