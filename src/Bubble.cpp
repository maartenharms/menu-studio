#include "PCH.h"

#include "ActorTickProbe.h"
#include "AnimEventProbe.h"
#include "Backdrop.h"
#include "Bubble.h"
#include "CbpcDrive.h"
#include "ClipProbe.h"
#include "EquipNotifyGate.h"
#include "Declutter.h"
#include "FaceNeutral.h"
#include "ForcePause.h"
#include "FsmpDrive.h"
#include "FootIkGate.h"
#include "Offsets.h"
#include "OwnView.h"
#include "SceneTint.h"
#include "Settings.h"
#include "ShadowPause.h"
#include "StudioLight.h"
#include "StudioRig.h"
#include "Transition.h"
#include "VersionCheck.h"
#include "WeaponPreview.h"

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

    // The face MESH bake. Update above moves the DATA; this is the half that
    // reaches the geometry, and while a menu holds the pause the engine only
    // ever runs it for RaceSex Menu. See Offsets::FaceGenApplyMorphs.
    using FaceGenApplyMorphs_t = void(__fastcall*)(RE::BSFaceGenNiNode*, bool);
    REL::Relocation<FaceGenApplyMorphs_t> g_faceGenApplyMorphs{
        MTB::Offsets::FaceGenApplyMorphs
    };

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

    // TRUE when the blink machine is parked AND the lids are open, i.e. there
    // is nothing in flight on the face and it can be held still without
    // freezing a half-finished blink.
    //
    // The distinction matters. Holding the face the moment the POSE freezes
    // would recreate the shut-eyes bug this stint just fixed: whatever the
    // pause caught mid-blink would be the last thing baked. Settle first, hold
    // second - the same order the cancelled-effect lesson teaches.
    // unk200: 0 waiting / 1 closing / 2 opening / 3-4 look-holds.
    const char* WeaponStateName(int a_ws) {
        switch (a_ws) {
        case 0:  return "kSheathed";
        case 1:  return "kWantToDraw";
        case 2:  return "kDrawing";
        case 3:  return "kDrawn";
        case 4:  return "kWantToSheathe";
        case 5:  return "kSheathing";
        default: return "?";
        }
    }

    // ── POST-CLOSE WEAPON-STATE WATCH ──────────────────────────────────────
    //
    // Field 2026-07-21: "there is a chance that the character becomes stuck,
    // preventing them from drawing or sheathing the weapon entirely", after
    // opening the inventory mid-draw and swapping weapons inside it.
    //
    // ⚠ EVERY OTHER WEAPON LINE IN THIS PLUGIN IS ARMED-ONLY, so the log goes
    // silent at exactly the moment this symptom starts. A probe whose fuse
    // ends before the symptom cannot see it
    // ([[probe-fuse-must-outlive-the-symptom]]); this one OPENS at the close
    // edge and runs past it.
    //
    // WHAT IT DECIDES, in one run. WEAPON_STATE is a small state machine and
    // only kSheathed and kDrawn are terminal. Our pumps drive it by stepping
    // the graph until the state edge arrives, and they give up at a cap - so
    // a capped pump can hand the world back a player parked on kDrawing or
    // kSheathing with no clip left running to move them off it. The engine's
    // own draw toggle then has nothing to do, which IS the report.
    //   non-terminal here -> we stranded the state machine; the fix is at the
    //                        pump that capped.
    //   terminal here     -> the state machine is fine and the fault is in
    //                        which CLIP the graph latched, which is a
    //                        different search entirely.
    // A negative is as useful as a positive, which is the point.
    //
    // Costs nothing in normal play: it only runs in a bounded window after a
    // bubble menu closes, logs on CHANGE, and gives one verdict line.
    void WatchWeaponStateAfterClose(bool a_armed, std::uint64_t a_now,
                                    float (*a_since)(std::uint64_t, std::uint64_t)) {
        static bool          wasArmed{ false };
        static bool          watching{ false };
        static std::uint64_t closedAt{ 0 };
        static int           lastWs{ -1 };
        static bool          verdictDone{ false };

        const bool closedEdge = wasArmed && !a_armed;
        wasArmed              = a_armed;
        if (a_armed) {
            watching = false;  // a re-arm cancels the watch; the next close restarts it
            return;
        }
        if (closedEdge) {
            watching    = true;
            closedAt    = a_now;
            lastWs      = -1;
            verdictDone = false;
        }
        if (!watching) {
            return;
        }

        auto* const player = RE::PlayerCharacter::GetSingleton();
        // Gate on real 3D. The state machine means nothing without a graph,
        // and a member read before the 3D exists is the CTD shape in
        // [[engine-singletons-exist-before-their-arrays]].
        if (!player || !player->Get3D()) {
            return;
        }
        auto* const state = player->AsActorState();
        if (!state) {
            return;
        }

        const int   ws    = static_cast<int>(state->GetWeaponState());
        const float since = a_since(closedAt, a_now);
        if (ws != lastWs) {
            lastWs = ws;
            spdlog::info("weapon watch: +{:.2f}s after the last bubble menu closed - weapon "
                         "state {} ({}).", since, ws, WeaponStateName(ws));
        }
        // 1.5 s is past any real draw or sheathe clip, so anything still
        // mid-transition here is parked, not in progress.
        if (!verdictDone && since >= 1.5f) {
            verdictDone = true;
            if (ws == 0 || ws == 3) {
                spdlog::info("weapon watch: settled on {} - the world was handed a TERMINAL "
                             "weapon state, so a stuck draw/sheathe is not this.",
                             WeaponStateName(ws));
            } else {
                spdlog::warn("weapon watch: STILL {} ({}) 1.5s after the menu closed - the "
                             "weapon state machine was STRANDED mid-transition. The player "
                             "cannot draw or sheathe from here. Look at the last pump line "
                             "above: a CAPPED pump is the suspect.",
                             ws, WeaponStateName(ws));
            }
        }
        if (since > 6.0f) {
            watching = false;  // long past any transition; stop costing anything
        }
    }

    bool FaceAtRest(RE::BSFaceGenAnimationData* a_face) {
        if (!a_face) {
            return true;  // nothing to hold
        }
        if (a_face->unk200 != 0) {
            return false;  // mid-blink or parked in a look-hold
        }
        const auto& lids = a_face->unk100;
        if (!lids.values || lids.count < 2) {
            return true;
        }
        return lids.values[0] <= 0.01f && lids.values[1] <= 0.01f;
    }
}

namespace MTB {
    Bubble& Bubble::GetSingleton() {
        static Bubble instance;
        return instance;
    }

    void Bubble::InstallHook() {
        // The offset comes from VersionCheck, not from Offsets.h: on any build
        // other than the two we measured by hand, the hand-measured offset is
        // wrong and the site is found by matching the call's TARGET instead.
        const auto callOffset = VersionCheck::DispatchCallOffset();
        if (callOffset == 0) {
            spdlog::error("No player-dispatch call site inside Main::Update on this runtime; "
                          "Bubble NOT installed. (plugin.cpp normally refuses to load at all "
                          "in this state - reaching here means iRuntimeGate=1.)");
            return;
        }
        REL::Relocation<std::uintptr_t> site{ Offsets::MainUpdate, callOffset };

        // Never write_call a site that is no longer the vanilla E8 - another
        // mod may have claimed or patched it in an incompatible way. Kept even
        // though VersionCheck already read this byte: it re-reads it at the
        // moment of the write, so a mod that patched the site in between is
        // still caught.
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

        // r23 DIAGNOSTIC. Installed here with the other vtable writes rather
        // than per-tick: the hkbClipGenerator vtable is process-global and does
        // not depend on the player's 3D existing, unlike the graph SINK, which
        // does and is therefore retried every frame.
        // The equip gate is the FIX and always installs. The probes below are
        // diagnostics and install only when asked: see Settings::diagnosticProbes
        // for why they are not merely silenced.
        EquipNotifyGate::Install();
        // ⚠ THE PROBE INSTALLS USED TO LIVE HERE, AND THEY NEVER RAN.
        //
        // This function is part of plugin init; the INI is not read until the
        // save loads, ~25 s later in the field log. So `diagnosticProbes` was
        // ALWAYS false here and neither probe was ever installed from an INI
        // setting - only from a compiled-in default. Both are idempotent, so
        // they now sit in the per-frame retry beside AnimEventProbe::Install(),
        // which is the one that worked and the reason the discrepancy showed
        // up at all (its graph sink is retried per frame by necessity).
        //
        // ⚠ AND THIS INVALIDATES A MEASUREMENT THE DOCS TREAT AS SETTLED.
        // ActorTickProbe counts Actor::Update calls. With the hook never
        // installed the counter is trivially 0, and the disarm line still
        // prints "Actor::Update ran 0 time(s) ... ZERO: the actor does not
        // update while paused". That line is printed by Bubble, not by the
        // hook, so it reads identically whether the truth is "zero calls" or
        // "no hook". Any run whose log lacks `actor tick probe: hooked` proves
        // nothing about Actor::Update. Re-measure before citing it again.
    }

    void Bubble::Register() {
        // r19: register the studio's own pausing menu before the watcher, so a
        // menu that is already open at this point can still be covered.
        ShadowPause::Register();
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

    void Bubble::FireDeferredWeaponRestore(bool a_force) {
        // F-26 r2. Pays the sheathe Disarm deferred, once the close→open gap
        // of a menu switch has passed without a bubble menu re-opening. The
        // 0.085 s window is the same field-measured switch gap the other two
        // deferrals use (B-8 v2 measured 52-71 ms). If the field ever shows a
        // switch slow enough to still flicker, THIS is the number to raise -
        // gateHoldFrames_ (~0.5 s) exists because a switch can outlast the
        // 6-frame visual grace, so a slower gap is a known possibility.
        //
        // Cost of the window on a REAL exit: the weapon stays in hand ≤85 ms
        // before the sheathe starts, under the menu-close fade. That is the
        // same trade F-15 r35 and B-8 v2 both took, and the lesser residual.
        // DIAGNOSTIC (2026-07-20). The 12:18-12:20 field log shows this debt
        // pending for 2.2 s across a close, then cancelled by the next open,
        // with NO `sheathing (restore)` line anywhere in six menu sessions -
        // so the player walked out of every one of them still drawn. Both
        // halves read correct in isolation (Disarm sets the flag at :423 then
        // clears armedLastFrame_ at :509; this runs every frame from the
        // driver), so name the guard that actually blocks instead of reasoning
        // about it a second time. On CHANGE only - this runs every frame.
        {
            const int reason =
                !pendingWeaponRestore_                                              ? 0
                : (!a_force && armedLastFrame_)                                     ? 1
                : (!a_force && QpcSeconds(weaponRestoreQpc_, QpcNow()) < 0.085f)    ? 2
                                                                                    : 3;
            static int s_lastReason = -1;
            if (reason != s_lastReason) {
                s_lastReason = reason;
                if (reason != 0) {  // "nothing pending" is the common case, stay silent
                    spdlog::debug("weapon diag: deferred restore {} (force={} armed={} debt={})",
                                  reason == 1   ? "BLOCKED by armedLastFrame_"
                                  : reason == 2 ? "waiting out the 0.085s switch window"
                                                : "FIRING NOW",
                                  a_force, armedLastFrame_, WeaponPreview::HasDebt());
                }
            }
        }
        if (!pendingWeaponRestore_) {
            return;
        }
        // Gated on "are we previewing right now", NOT on "is a menu open".
        // Review caught two cases the menu-count version got wrong, both where
        // a menu is still OPEN but the bubble is dormant: the Souls unpause
        // path and Tick's missing-3D path both Disarm with the menu up, and a
        // menu-count guard held the sheathe for the whole remaining menu
        // session - leaving a force-drawn weapon through live unpaused
        // gameplay, and breaking the forgive-on-dead-3D path Restore()
        // documents. armedLastFrame_ is the honest question: a re-open cancels
        // this before it can ever be reached with the preview live again.
        if (!a_force && armedLastFrame_) {
            return;
        }
        if (!a_force && QpcSeconds(weaponRestoreQpc_, QpcNow()) < 0.085f) {
            return;
        }
        pendingWeaponRestore_ = false;
        // Restore() forgives the debt when the player or their 3D is gone, so
        // this is safe on every teardown edge, not just the graceful ones.
        WeaponPreview::Restore(RE::PlayerCharacter::GetSingleton());
    }

    void Bubble::ForceReset() {
        menusOpen_ = 0;
        countedMenus_.clear();  // r19c: menus died with the load, no close events coming
        raceMenuOpen_ = false;
        // F-26: the player 3D dies with the load - drop the restore debt rather
        // than trying to pay it against a stale actor. Reset() hard-clears the
        // debt, so the deferred sheathe must go with it or it would fire into
        // the new game against a weapon this preview never drew.
        WeaponPreview::Reset();
        pendingWeaponRestore_ = false;
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
        // The graph is destroyed and rebuilt across a load, so the sink from the
        // old one is gone with it. Drop the flag so Install() re-attaches.
        AnimEventProbe::Reset();
        AnimEventProbe::SetArmed(false);
        // Same rebuild, and this one is not merely stale but DANGEROUS if left:
        // the cached hkbCharacter addresses can be recycled by the new graph,
        // and a stale match would tag another actor's clips as the player's.
        ClipProbe::Reset();
        EquipNotifyGate::SetArmed(false);
        airFrozenArm_ = false;
        movingArm_ = false;
        graceFrames_ = 0;
        gateHoldFrames_ = 0;
        loggedUnpausedOnce_ = false;
        sessionDormant_ = false;
        sessionLiveOnly_ = false;  // r28g: dies with the session, like dormant
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
        Settings::GetSingleton().liveStudioActive = false;  // r28, same reason as in Disarm
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
        // Put the CONFIGURED space back, so nothing outside an arm (the panel,
        // a save load, the next open's own resolve) ever reads a menu-local 0.
        Settings::GetSingleton().declutterMode =
            Settings::GetSingleton().declutterModeIni;
        exitPhase_ = 0;  // r47: the exit machine is a pure hold - no fader state
        // F-26 r2: the sheathe is DEFERRED past the switch gap, exactly like
        // the B-8 v2 move mirror. Paying it inline sheathed on every menu
        // SWITCH: at close time a switch and a real exit are indistinguishable,
        // and on the DEFAULT settings (bSleekExit=1, space on,
        // fExitHoldSeconds=0) the r50 zero-frame cut calls this Disarm from
        // inside the close event's own call stack - so the weapon went away and
        // was drawn again a frame later (inventory->magic, ~2 s of animation).
        //
        // Mirror pendingExitMoveStart_, NOT pendingOwnViewRestore_: the
        // latter's cancel is gated on armedLastFrame_, which this very path has
        // already cleared by the time the next menu opens, so an
        // armedLastFrame_-gated cancel could never fire in the case being fixed.
        // The cancel in the open handler therefore sits outside that gate.
        //
        // Cancelling is safe because the debt outlives it: the gate checks its
        // weDrew branch FIRST and returns kNone while still armed, so the
        // re-opened menu holds the same drawn weapon and the same outstanding
        // sheathe (WeaponPreviewGate.h, and the "we drew, conditions still
        // hold" case in the gate suite).
        //
        // KEYED ON THE DEBT, NOT ON armedLastFrame_, and deliberately OUTSIDE
        // the armed block. Review caught the hole: a switch whose SECOND menu
        // fails to arm (the unpause race / missing 3D noted above) hits this
        // function with armedLastFrame_ already false, so an armed-gated
        // deferral would never re-take the debt the open event just cancelled -
        // and nothing else can pay it, because Update() only runs from an armed
        // Tick. The player would stay drawn for the rest of the session, which
        // is precisely the outcome WeaponPreview.cpp's own comment calls out.
        if (WeaponPreview::HasDebt()) {
            // r20b: STAMP ONCE, ON THE TAKING EDGE.
            //
            // Disarm() runs EVERY FRAME while no menu is open (see the call at
            // the bottom of the no-menus branch), so re-stamping here reset the
            // countdown every frame and the 0.085 s window could never elapse.
            // Field 2026-07-20: `waiting out the 0.085s switch window` held for
            // 2.2 s, then the next open cancelled it. The sheathe only ever
            // fired when the close fade happened to return early and suppress
            // Disarm for long enough - luck, not design.
            //
            // The cost was not cosmetic: an unpaid sheathe leaves a drawn state
            // WE manufactured standing in live gameplay, and the user saved on
            // top of it. On reload the graph rebuilds, the model returns to its
            // sheath node, and the save still says drawn - empty hands, weapon
            // on the hip.
            //
            // A switch still re-stamps correctly: the open handler clears
            // pendingWeaponRestore_, so the next close takes the debt afresh.
            //
            // Same shape as r19c: state that must be latched once was being
            // re-derived every frame.
            if (!pendingWeaponRestore_) {
                weaponRestoreQpc_ = QpcNow();
            }
            pendingWeaponRestore_ = true;
        }
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
            movingArm_ = false;
            Transition::Snap(0.0f);  // F-12 backstop: early disarms don't ramp
        }
        // B-14: the scene restore is deliberately OUTSIDE the armed gate. The
        // warm-occluder open path builds the void from the open EVENT itself
        // ("gap occluder WARM (same-frame, gap-free) - cull now"), BEFORE the
        // first armed tick ever runs - so a session that dies between build and
        // arm (field 2026-07-20 01:00:36.595-36.600: Skyrim Souls stripped the
        // pause in the switch churn, dormancy fired, no "Bubble ARMED" line)
        // reached this Disarm with armedLastFrame_ still false, the whole block
        // below was skipped, and the built void (166 hidden refs, 42 culled
        // nodes, water, imods) was ORPHANED into gameplay. That is the "void
        // bubble stays after exit" report. Every callee no-ops when its piece
        // was never built, so running them un-armed costs a handful of flag
        // checks. Spin/camera restores stay armed-gated above and below - they
        // undo state only an armed tick creates.
        FaceNeutral::Restore();
        Declutter::RestoreAll();
        Backdrop::Remove();
        Backdrop::OccluderHide();  // keep the occluder resident + warm for the next open
        // r28: drop the live-studio claim BEFORE the rig comes down, so nothing
        // downstream can see RigAllowed() still true against a removed rig and
        // put it straight back up.
        if (Settings::GetSingleton().liveStudioActive) {
            Settings::GetSingleton().liveStudioActive = false;
            // r28c: hand the fade back down with it. The armed path's snap sits
            // inside the armedLastFrame_ block below and a live-studio session
            // never armed, so without this a session that only ever lit the rig
            // would leave the transition parked at 1.
            Transition::Snap(0.0f);
        }
        StudioRig::Remove();
        StudioLight::Restore();
        SceneTint::Restore();  // instant restore at the close edge - no tint leaks to gameplay
        FsmpDrive::SetRotationFreedom(false);
        CbpcDrive::SetSimulateWhilePaused(false);
        FootIkGate::SetSuppressed(false);
        if (armedLastFrame_ && Settings::GetSingleton().tickFace &&
            !Settings::GetSingleton().freezeCharacter) {
            // THE SESSION IN NUMBERS. Blinks are too short and too rare to
            // judge by eye - this is the line that says whether the face
            // actually lived, and it is the line to quote before claiming the
            // blink is fixed. bakes>0 with blinks>0 and no visible blinking
            // means the bake is landing somewhere else; bakes==0 means the
            // call never ran and nothing below it is evidence of anything.
            spdlog::info("face session: {} mesh bakes, blinks {} started / {} "
                         "completed, peak composed lid {:.2f}, blink timer capped "
                         "{} times (bBlinkStressTest).",
                         faceMeshApplies_, blinkStarts_, blinkCompletes_,
                         blinkLidPeak_, blinkCaps_);
        }
        if (armedLastFrame_) {
            // The idle re-pick's pass/fail number, next to the face one. Before
            // AnimEventProbe::SetArmed(false) so nothing it counts is missed.
            ClipProbe::ArmedSessionReport();
            // F-15: the view was OURS - full SPIM ResetCamera-style restore
            // (first person handed back if we forced it, Settings originals,
            // interpolators snapped inside). Otherwise only snap the zoom/yaw
            // interpolators: SPIM restores the pre-menu camera on close and
            // they'd ease current -> target for half a second (exit glide).
            AnimEventProbe::SetArmed(false);  // diagnostic tag: back to live
            ActorTickProbe::MarkArmed(false);
            EquipNotifyGate::SetArmed(false);
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
                // r60 (user): the sign was backwards - dragging LEFT spun the
                // character clockwise from their own perspective. Negated, so
                // the body follows the hand: drag left, they turn to their
                // left. The stick path shares this convention (see the tick).
                spinTarget_ -= static_cast<float>(move->mouseInputX) *
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
        // §4b: ShouldBubbleMenu, not IsBubbleMenu. A menu the user has handed
        // back to Skyrim Souls drops out HERE, at the single entry point, so it
        // is left completely alone: no arm, no studio, and no force-pause,
        // since EnsurePaused is reached from below this line. Bailing here
        // rather than gating each consumer is the whole reason this stays one
        // line - a menu is either ours or it is Souls', and there is no
        // half-owned state to reason about later.
        // r19c - THE PREDICATE IS NOT STABLE ACROSS A MENU'S LIFETIME, SO THE
        // CLOSE MUST NOT CONSULT IT.
        //
        // ShouldBubbleMenu() reads live settings, and the settings panel can be
        // opened from inside a bubbled menu. Toggling the Souls split there
        // flips this predicate BETWEEN a menu's open and its close: the open
        // incremented menusOpen_, the close took the early-out above, and the
        // count was never given back. menusOpen_ stayed >= 1 with no menu open,
        // so OnFrame kept ticking an armed bubble straight into gameplay - the
        // field report was a void that survived into normal play, and the log
        // shows it exactly (11 opens, 10 closes, one InventoryMenu close eaten,
        // then thousands of armed ticks with the player walking around).
        //
        // The old comment here said "a menu is either ours or it is Souls', and
        // there is no half-owned state to reason about later". A menu that
        // OPENED as ours and CLOSES as Souls' is precisely that state, and it
        // exists the moment settings are mutable at runtime. So ownership is
        // decided once, at open, and remembered: close decrements if and only if
        // we counted this menu at ITS open, whatever the settings say now. A
        // handover therefore takes effect on the NEXT open, which is the only
        // edge where it can be applied consistently.
        if (a_event->opening) {
            if (!Settings::GetSingleton().ShouldBubbleMenu(name)) {
                // r28g: a Souls-live menu is no longer dropped outright when
                // the live lighting is on - it gets a LIGHTING-ONLY session.
                // Counted like any other (r19c: ownership decided at open,
                // remembered at close), but latched live-only so the arm
                // decision can never build the studio in it, no matter what
                // the pause counter happens to read on frame 1 - a borrowed
                // tween pause must not put the void into a menu the user
                // explicitly keeps live.
                //
                // !armedLastFrame_: a live menu opening on top of an ARMED
                // session (per-menu split mixes) keeps the old drop behaviour;
                // the mixed case has never existed and is not being invented
                // here as a side effect.
                const auto& s            = Settings::GetSingleton();
                const bool  liveLighting = s.studioInLiveMenus && s.IsBubbleMenu(name) &&
                                          s.IsSoulsLiveMenu(name) && !armedLastFrame_;
                if (!liveLighting) {
                    // ⚠ r28h: NEVER DECLINE SILENTLY. This early-out has now
                    // eaten two field rounds on its own - once as the original
                    // Souls drop (r28g) and once with bStudioInLiveMenus off,
                    // which produced a 25-line log identical to "the mod did
                    // nothing" and sent us hunting the lights instead of the
                    // gate. A decline that cannot be seen is indistinguishable
                    // from a crash, a bad build, or a broken feature.
                    //
                    // Logged per open, not deduped: menu opens are a handful
                    // per minute, and the previous "cheap" silence cost far
                    // more than these lines ever will.
                    if (s.IsBubbleMenu(name)) {
                        spdlog::info("Bubble menu DECLINED: {} - soulsLive={} "
                                     "studioInLiveMenus={} armedLastFrame={} -> no session "
                                     "(no pause, no scene, no lighting).",
                                     name, s.IsSoulsLiveMenu(name), s.studioInLiveMenus,
                                     armedLastFrame_);
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                sessionLiveOnly_ = true;
                spdlog::info("Bubble menu opened LIVE: {} (Souls keeps it unpaused) - "
                             "lighting-only session: no pause, no scene, rig + colour "
                             "filter only.",
                             name);
            } else {
                // A menu WE pause re-decides this at its own open: a live
                // session followed by a paused menu (per-menu split) must not
                // inherit the previous menu's live-only latch.
                sessionLiveOnly_ = false;
            }
            countedMenus_.insert(name);
            ++menusOpen_;
            currentMenuName_ = name;  // own-view coverage decides per menu at arm
            // Per-menu SPACE (NymerethRole): the backdrop/void is opt-in per
            // menu now, so resolve the EFFECTIVE mode for the menu being
            // opened. Everything else about the bubble (pause, physics, the
            // live character) is unchanged - only the space steps aside, and
            // only for menus the user left out of sSpaceMenus. Setting the
            // effective value here means all ~30 existing declutterMode /
            // IsVoidFamily consumers get the per-menu answer with no changes.
            // Restored to the configured value at Disarm.
            if (auto& s = Settings::GetSingleton();
                s.declutterModeIni != 0 && !s.MenuWantsSpace(name)) {
                if (s.declutterMode != 0) {
                    spdlog::info("space: {} is not in sSpaceMenus - opening without "
                                 "the backdrop (pause + physics unchanged).", name);
                }
                s.declutterMode = 0;
            } else {
                s.declutterMode = s.declutterModeIni;
            }
            graceFrames_ = 0;
            gateHoldFrames_ = 0;
            loggedUnpausedOnce_ = false;
            sessionDormant_ = false;  // r18: new menu session, new arm decision
            // r28b: baseline the toggle watcher against what this session is
            // actually deciding with, so the first frame cannot read a stale
            // default as a user toggle and re-decide a session that just began.
            lastForcePause_ = Settings::GetSingleton().forcePause;
            // B-8 v2: a bubble menu opened before the deferred exit mirror
            // fired - this close was a SWITCH, the walk must not restart.
            if (pendingExitMoveStart_) {
                pendingExitMoveStart_ = false;
                spdlog::debug("idle-in-menus: deferred moveStart cancelled (menu switch).");
            }
            // F-26 r2: same cancel, same reason - this open proves the close
            // was a SWITCH, so the weapon simply stays in hand and the debt
            // rides through to the next disarm. Deliberately NOT gated on
            // armedLastFrame_: the default zero-hold sleek exit disarms inside
            // the close event, so that flag is already false here - which is
            // exactly the case this fix exists for.
            if (pendingWeaponRestore_) {
                pendingWeaponRestore_ = false;
                spdlog::debug("weapon preview: deferred sheathe cancelled (menu switch) - "
                              "weapon stays in hand.");
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
                !raceMenuOpen_.load() && !Settings::GetSingleton().freezeCharacter) {
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
                if (s.CellLightAllowed() && !raceMenuOpen_.load()) {
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
            // Decrement against what we counted, not against the live predicate.
            if (countedMenus_.erase(name) == 0) {
                return RE::BSEventNotifyControl::kContinue;  // never ours - nothing to give back
            }
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
        // THE FORCE-PAUSE SETTLE RUNS FIRST, ABOVE EVERY EARLY RETURN BELOW.
        // This used to sit further down, past `if (menusOpen_ <= 0) { ... return; }`
        // and the whole exit choreography, which meant it only ever ran WHILE A
        // COVERED MENU WAS STILL OPEN. But settling closed menus is its entire
        // job: ForcePause takes a numPausesGame++ on open, OnMenuClosed is a
        // deliberate no-op, and Reassert is the ONLY thing that ever gives that
        // increment back. So closing the LAST covered menu dropped menusOpen_ to
        // 0, OnFrame returned before the settle, and the +1 was never reclaimed -
        // the world stayed frozen after the menu closed. Closing one of TWO open
        // menus always worked (the counter stayed above 0, so the settle still
        // ran), which is why this survived the 0.6.0 rework: the logic was right
        // and simply unreachable in the one case it was written for. It also
        // defeated that code's own "ticks while frozen, so it self-heals a stuck
        // session" intent, since a stuck session has no menu open by definition.
        //
        // Cheap unconditionally: Reassert early-outs when it holds nothing. Above
        // the `enabled` check too, so switching the mod off while holding a pause
        // releases it instead of stranding the world frozen.
        ForcePause::Reassert();
        if (!settings.enabled || !a_main) {
            graceFrames_ = 0;
            Disarm();
            // F-26 r2: this path returns every frame while the mod is off, so
            // the timed fire site below is unreachable - an outstanding sheathe
            // would hang here forever and leave the player permanently drawn.
            // No switch can follow a disable either, so pay it now.
            FireDeferredWeaponRestore(true);
            return;
        }
        // F-26 r2: OUTSIDE the menu-count block on purpose. The sheathe must
        // also be payable while a menu is still open but the bubble is dormant
        // (Souls unpause, missing 3D) - see the guard inside, which uses
        // armedLastFrame_ rather than the menu count.
        // DIAGNOSTIC (2026-07-20), OFF unless bDiagnosticProbes. Retried every
        // frame because both no-op once they have taken, and the graph does not
        // exist before the 3D loads and is rebuilt across a save load.
        // ClipProbe is NO LONGER a diagnostic - EquipClipInFlight() is the
        // signal the r36 freeze uses to tell "the draw clip is still running"
        // from "the weapon state says drawn", which r11 proved are not the same
        // thing. It installs and tracks unconditionally; only its LOGGING is
        // still gated on bDiagnosticProbes, so the cost when probes are off is
        // one pointer compare per clip activation.
        ClipProbe::Install();
        ClipProbe::TrackPlayerGraphs();
        if (Settings::GetSingleton().diagnosticProbes) {
            // Both sides of the arm flag on purpose: the whole question is what
            // these markers do in a menu that they do not do live, and a
            // measurement with no control has already misled this channel once
            // today (a 3.33 s idle turned out to fire live too).
            ClipProbe::LogStanceMarkers(armedLastFrame_);
        }
        if (Settings::GetSingleton().diagnosticProbes) {
            // Idempotent and no-op once taken. These moved here from plugin
            // init, where the INI had not been read yet and they could never
            // fire - see the note at Install().
            ActorTickProbe::Install();
            AnimEventProbe::Install();
        }
        FireDeferredWeaponRestore();
        // Runs disarmed as well as armed - see WatchWeaponStateAfterClose for
        // why the window has to outlive the menu.
        WatchWeaponStateAfterClose(armedLastFrame_, QpcNow(), &QpcSeconds);
        // DIAGNOSTIC (2026-07-20): the control sample. Disarmed only - an armed
        // frame is the case under investigation, and the whole point of this
        // call is to capture the world getting a weapon swap RIGHT so the menu
        // case has something real to be diffed against. Early-outs on the first
        // pointer compare when nothing changed.
        if (!armedLastFrame_) {
            WeaponPreview::ObserveUnarmed(RE::PlayerCharacter::GetSingleton());
        }
        // r19c SELF-HEAL. A menusOpen_ that never reaches 0 strands an armed
        // bubble in gameplay, and the player cannot recover from it without a
        // reload - the exact failure just field-reported. The counted-set fix
        // above closes the known cause (a predicate that changed mid-session),
        // but ANY missed close event produces the same unrecoverable state, so
        // reconcile against the UI itself rather than trusting the bookkeeping.
        //
        // Deliberately slow: a menu can lag the UI map right after its open
        // event (the r14 race), so a single absent frame proves nothing. Only a
        // menu absent for a full second is treated as gone.
        if (menusOpen_.load() > 0) {
            auto* ui = RE::UI::GetSingleton();
            bool anyReallyOpen = false;
            if (ui) {
                for (const auto& counted : countedMenus_) {
                    if (ui->IsMenuOpen(counted)) {
                        anyReallyOpen = true;
                        break;
                    }
                }
            }
            if (anyReallyOpen || !ui) {
                orphanFrames_ = 0;
            } else if (++orphanFrames_ >= 60) {
                spdlog::warn("Bubble: menusOpen_={} but none of the {} counted menu(s) has "
                             "been open for a second - a close event was missed. Reconciling "
                             "to 0 so the studio cannot outlive its menu.",
                             menusOpen_.load(), countedMenus_.size());
                orphanFrames_ = 0;
                countedMenus_.clear();
                menusOpen_ = 0;
            }
        } else {
            orphanFrames_ = 0;
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

        // (The force-pause reconcile moved to the TOP of OnFrame - see there.
        // Here it could never settle a menu that had already left the stack.)

        auto* ui = RE::UI::GetSingleton();
        const bool paused = a_main->freezeTime || (ui && ui->GameIsPaused());

        // r18 - THE ARM DECISION IS LATCHED PER SESSION, NOT POLLED PER FRAME.
        //
        // This branch used to re-read the GLOBAL pause every frame and Disarm()
        // whenever it read false. That coupled the whole visual scene (void,
        // backdrop, declutter, studio light) to a global that four other actors
        // write: Skyrim Souls, the close/open gap of a menu switch, our own
        // force-pause, and ANY unrelated pausing menu the player opens on top.
        // Field 2026-07-20 09:55-09:58 measured the consequence: ONE
        // InventoryMenu stayed open for three minutes with zero menu events,
        // and the bubble armed and tore down TEN times - every console, journal,
        // tween and system menu built the entire void and then destroyed it.
        // Nine of fifteen arms that session came from a pause we did not own
        // (freezeTime=false numPauses=1). That is the "janky as fuck" report,
        // and it is also why the void showed up behind the console.
        //
        // The scene's lifetime belongs to the MENU SESSION, not to the pause.
        // So the decision is made once, on the first frame of a session, and
        // held: dormant sessions stay dormant no matter what pauses later, and
        // armed sessions keep their scene no matter what unpauses later. The
        // per-session wording the old log line already used was the intent all
        // along - this makes the code match it.
        // r28b: AN EXPLICIT TOGGLE RE-DECIDES THE SESSION.
        //
        // r18's latch is about INCIDENTAL pause changes - a console, a tween, a
        // menu switch - and it stays. This is the one deliberate change: the
        // user reaching into the panel and flipping force-pause. The panel is
        // drawn inside the menu whose session did the latching, so without this
        // the toggle appears to do nothing at all until you close and reopen,
        // which is exactly how it was reported from the field.
        //
        // Tear down and clear the latch; the normal decision below runs next
        // frame and picks the armed or the live-studio path on its own. No new
        // transition, just the existing one allowed to happen again.
        if (const bool fp = Settings::GetSingleton().forcePause; fp != lastForcePause_) {
            lastForcePause_ = fp;
            spdlog::info("Force-pause toggled to {} while a menu was open - re-deciding this "
                         "session instead of waiting for the next menu.", fp);
            Disarm();
            sessionDormant_     = false;
            armedLastFrame_     = false;
            loggedUnpausedOnce_ = false;
            // r28g: the live-only latch is part of what the toggle re-decides.
            // Recomputed, not cleared: flipping TO keep-unpaused mid-menu makes
            // this session live-only right now, and flipping AWAY makes it a
            // normal armable one.
            {
                const auto& s    = Settings::GetSingleton();
                sessionLiveOnly_ = s.studioInLiveMenus && !currentMenuName_.empty() &&
                                   s.IsBubbleMenu(currentMenuName_) &&
                                   s.IsSoulsLiveMenu(currentMenuName_);
            }
            return;  // next frame decides cleanly against the new pause state
        }

        if (sessionDormant_) {
            // r28: a dormant session still gets the studio RIG and the COLOUR
            // FILTER if this is a Souls-live menu and the user kept them on.
            // Both are safe with the world running: the rig adds three lights
            // around the character, the filter is a screen grade. Neither hides
            // anything, which is the line this feature does not cross.
            //
            // StudioRig::Tick and SceneTint::Sync are both self-healing - they
            // apply when their setting turns on and restore when it turns off -
            // so these two calls are the whole per-frame cost and both no-op
            // when their feature is off.
            if (Settings::GetSingleton().liveStudioActive) {
                const auto& cfg = Settings::GetSingleton();
                // r28d: read back BEFORE Tick re-asserts anything, so the line
                // shows what survived the ENGINE's previous frame, not our own
                // writes. ~1 Hz; the counter is per-session state in spirit but
                // a static is fine - worst case the first line lands a beat
                // early on the next session.
                //
                // r29: GATED FOR RELEASE. This is three lines per second for as
                // long as a live menu is open, which is log spam a user would
                // report. It earned its keep - it is what proved the lights
                // were healthy - so it stays in the source behind the same
                // switch as the other probes rather than being deleted.
                if (cfg.diagnosticProbes) {
                    static std::uint32_t s_readbackCountdown = 0;
                    if (s_readbackCountdown == 0) {
                        StudioRig::LogLiveState("live-1Hz");
                        s_readbackCountdown = 120;
                    }
                    --s_readbackCountdown;
                }
                StudioRig::Tick();
                SceneTint::Sync(cfg.colorFilter, cfg.CurrentTint());
            }
            return;  // decided at session start; a later pause does not revive it
        }
        // r28g: sessionLiveOnly_ joins the condition. A lighting-only session
        // goes dormant even if frame 1 happens to READ paused - the tween
        // menu's borrowed pause rides under a fresh open often enough that
        // gating on the counter alone would build the void in a menu the user
        // explicitly keeps live, on a timing coin-flip.
        if ((!paused || sessionLiveOnly_) && !armedLastFrame_) {
            // Never armed this session and the world is live (or the session
            // is live-only by decision): stay out of it for the whole session.
            if (!loggedUnpausedOnce_) {
                spdlog::info("Bubble {} - bubble dormant for this menu session (latched: "
                             "a pause arriving later will NOT arm it).",
                             sessionLiveOnly_
                                 ? "menu is LIVE-ONLY by decision (lighting-only session)"
                                 : "menu open but game is UNPAUSED (Skyrim Souls?)");
                loggedUnpausedOnce_ = true;
            }
            sessionDormant_ = true;
            // r18b: a dormant session must hold NOTHING. Field 2026-07-20: with
            // force-pause on and Souls keeping the menu live, r17's freeze was
            // still held for a session the bubble had already declined - the
            // world froze with no studio in it ("the player is completely
            // paused" with no bubble), which is worse than either outcome on its
            // own. Hand the freeze back the moment we decide to sit this one out.
            ForcePause::ReleaseAll();
            // Clears anything the open event built before we got here (the warm
            // occluder cull); every callee no-ops when its piece was not built.
            Disarm();
            // r28 LIVE STUDIO, and it goes AFTER Disarm deliberately: Disarm
            // clears liveStudioActive and removes the rig, so setting the flag
            // first would be undone in the same breath. The next frame's
            // sessionDormant_ branch above is what actually raises the lights.
            //
            // The rig only. The void, backdrop and declutter stay out of a live
            // menu on purpose - see Settings::studioInLiveMenus - and the drives
            // are already suppressed by engineAnimates, which reads !paused.
            if (Settings::GetSingleton().studioInLiveMenus) {
                Settings::GetSingleton().liveStudioActive = true;
                // ⚠ r28c: SNAP THE TRANSITION UP, OR THE RIG EMITS NOTHING.
                //
                // StudioRig::PushConfig multiplies every light's fade by
                // Transition::Value(), and Disarm() above has just snapped it to
                // 0 (its F-12 backstop, so early disarms do not ramp). Without
                // this the lights are created, registered and correctly placed,
                // and put out 0.20 * 0.0 = zero photons.
                //
                // Field caught it exactly right: "changing their brightness
                // didn't do anything". It could not - brightness is one of the
                // factors being multiplied by zero. The log said "3 light(s) up"
                // the whole time, which is why this needed the CODE read and not
                // another round of looking at the log.
                Transition::Snap(1.0f);
                if (Settings::GetSingleton().studioRig) {
                    spdlog::info("Live studio: menu is unpaused, so the void and the drives "
                                 "stay out, but the studio rig stays on the character "
                                 "(bStudioInLiveMenus, fade snapped to 1).");
                }
            }
            return;
        }

        // Armed sessions fall through even when `paused` is false. Tick keeps
        // the scene alive and standing still; the drive is suppressed inside via
        // engineAnimates, exactly as it already is for RaceMenu.
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

        // NOT gated on diagnosticProbes - this one is the fix, not a probe. Here
        // rather than up beside the probes because it needs the REAL frame
        // delta: the marker has to age at wall-clock rate, exactly as it would
        // live, or it expires at the wrong moment.
        if (Settings::GetSingleton().clearIdleStartMarker) {
            ClipProbe::DriveStanceMarkers(armedLastFrame_, dt);
        }

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
            // F-26: evaluate the weapon preview at the arm edge. Cheap - it is
            // a no-op unless the decision changes. MUST run before the freeze
            // block below: it issues the draw, so that block would otherwise
            // see our own kDrawing and freeze the arm we just started. This
            // call runs ONCE per arm; the per-tick call further down is what
            // catches a weapon SWAP made inside the menu.
            //
            // 0.7.1: release the normalize latch FIRST. The normalize runs once
            // per arm now instead of once per tick, so this is what makes the
            // next session able to run it at all.
            WeaponPreview::ArmEdgeReset();
            WeaponPreview::Update(player, true, raceMenuOpen_.load());
            movingArm_ = false;
            if (Settings::GetSingleton().idleInMenus && !raceMenuOpen_.load() &&
                !Settings::GetSingleton().freezeCharacter) {
                auto* state = player->AsActorState();
                const bool locomoting = state && (state->actorState1.walking ||
                                                  state->actorState1.running ||
                                                  state->actorState1.sprinting);
                // The moving-arm latch. Grounded matters: "moving" in mid-air
                // or in water is not a case the settle owns, and those keep
                // their own freeze reasons below.
                // Budget for the locomotion settle further below. 1.0 s of graph
                // time is past any walk-to-stop in the field set; it stops the
                // instant an idle activates, so the full budget only runs when
                // the settle is failing, and the log says so when it does.
                constexpr float kSettleStep     = 1.0f / 30.0f;
                constexpr int   kSettleMaxSteps = 30;

                // ⚠ FIELD 2026-07-21 17:20. This latch is what puts a WALK
                // ANIMATION in a paused menu. Repro: be unsheathing while
                // moving, open the inventory, switch weapon.
                //
                //   idle-in-menus: armed MID-DRAW WHILE MOVING - the draw/sheathe
                //                  hold stands aside, the locomotion settle owns this arm.
                //
                // It then does not settle. The hold stands aside, the graph
                // keeps ticking, and the walk clip that was already running just
                // plays on for the whole menu. The 0.7.1 handoff already had it
                // as SUSPECT, NOT ENDORSED - it fired once in 19 field arms and
                // works by letting a mid-transition graph tick, which is the
                // exact state the hold exists to prevent.
                //
                // Behind a key rather than deleted outright, default OFF, so the
                // moving arm falls back to the ordinary draw/sheathe hold. If
                // the field confirms the walking is gone, delete the latch and
                // this key together rather than leaving them as decoration.
                movingArm_ = Settings::GetSingleton().movingArmStandsAside &&
                             locomoting && state && !state->actorState1.swimming &&
                             !player->IsInMidair();
                // r36 (user design): the graph ticks ONLY for standing
                // idles and the locomotion-to-still settle below -
                // EVERYTHING else holds its caught frame (mid-air r18,
                // attacks r34, and now draw/sheathe transitions, furniture
                // enter/exit, swimming, and scripted idles - interactions,
                // crafting - detected via bAnimationDriven at the arm edge,
                // the same flag non-idle clips raise, B-7). Face, hair and
                // body physics keep living either way.
                const auto weaponState = state->actorState2.weaponState;
                // F-26: a draw/sheathe THIS feature asked for is allowed to
                // animate - it is the one the user requested by opening the
                // menu, not an animation leaking in from gameplay. Every other
                // transition still holds its caught frame, which is the whole
                // point of r36 ("genuinely only the standing still idles should
                // be playing"). Without this exception the preview deadlocks:
                // we ask for a draw, the freeze latches, and the state machine
                // never leaves kDrawing.
                //
                // 0.7.1: a clip the normalize could NOT finish is treated the
                // same way as one of our own. Freezing it is what produced the
                // field report - a replaced draw animation runs past the pump
                // budget, so the arm froze mid-clip for the whole menu and the
                // half-played transition leaked into live gameplay on close.
                // Letting it animate costs a beat of movement in a paused menu
                // and leaves the actor in a state the engine can reconcile.
                //
                // ⚠ 0.7.1 - AND THE CLIP, NOT ONLY THE STATE. Every state below
                // reads TERMINAL for the whole back half of a draw, because
                // kDrawn is set by the weaponDraw annotation about halfway
                // through the clip (r11). 21 field arm edges all reported a
                // terminal state while the user was deliberately opening the
                // menu mid-unsheathe, so this test alone could never catch the
                // case it exists for. ClipProbe answers the question the state
                // cannot: is the animation still running? See ClipProbe.h for
                // why letting that clip finish inside a paused menu is what
                // loops a stance mod's idle.
                const bool weaponTransition =
                    !WeaponPreview::TransitionInFlight() &&
                    !WeaponPreview::NormalizeCapped() &&
                    (ClipProbe::EquipClipInFlight() ||
                     weaponState == RE::WEAPON_STATE::kWantToDraw ||
                     weaponState == RE::WEAPON_STATE::kDrawing ||
                     weaponState == RE::WEAPON_STATE::kWantToSheathe ||
                     weaponState == RE::WEAPON_STATE::kSheathing);
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
                // ⚠ A MOVING ARM ALREADY HAS AN OWNER, AND IT IS NOT THIS.
                //
                // Field (user): "if we unsheath and are moving the period where
                // we freeze the character applies, can we make it so when we
                // start moving this pause period is negated". Drawing while
                // running set weaponTransition, which wins this chain outright,
                // so the locomotion settle below was never reached and the
                // character held a half-drawn running frame for the length of
                // the clip plus a second.
                //
                // Standing still is where the lunge loop lives: the graph is
                // between idles, the stance framework's conditions are stale,
                // and a latched selector loops the step-into-stance clip. A
                // MOVING graph is not in that state - it has a locomotion pose
                // to leave and an explicit moveStop to leave it by - so the
                // settle is both the better answer and the one already written.
                // Only the weapon reason stands aside; mid-air, mid-attack,
                // furniture, dodges and scripted idles still freeze while
                // moving, because none of those has a settle to fall back on.
                const bool movingDraw = weaponTransition && movingArm_;
                const char* freezeReason =
                    player->IsInMidair()                                        ? "mid-air"
                    : state->actorState1.meleeAttackState !=
                          RE::ATTACK_STATE_ENUM::kNone                          ? "mid-attack"
                    : (weaponTransition && !movingDraw)
                                       ? (ClipProbe::EquipClipInFlight()
                                              ? "draw/sheathe CLIP still playing"
                                              : "draw/sheathe")
                    // ⚠ REVERTED - `ClipProbe::GraphSettling()` WAS HERE AND IT
                    // WAS TOO BROAD. It froze on ANY recent clip activity, and
                    // walking activates clips continuously, so opening a menu
                    // while moving froze the whole character - and a window
                    // after stopping did too. Field: "it's a nuclear fix ...
                    // even if i walked and stopped, there's a window where we
                    // are frozen."
                    //
                    // It was aimed at a SECOND-HAND report (Movement Behavior
                    // Overhaul: stop, open inventory, stop-anim loops) that
                    // could not be reproduced here, and it broke behaviour that
                    // demonstrably worked. Do not reinstate it in this form.
                    // The locomotion case already has an owner: the `locomoting`
                    // branch below sends moveStop and lets the graph settle,
                    // which is the design and which this pre-empted.
                    //
                    // If MBO is chased again, it needs a signal specific to a
                    // stop TRANSITION - not "the graph did something recently".
                    // GraphSettling() is left in ClipProbe unused for that work.
                    : furnitureTransition                                       ? "furniture/sleep transition"
                    : state->actorState1.swimming                               ? "swimming"
                    : scriptedIdle                                              ? "scripted idle (animation-driven)"
                                                                                : nullptr;
                // ⚠ 0.7.1 - THE ONE WAY THE CAPPED-CLIP FIX GETS DEFEATED.
                //
                // Letting an un-normalizable clip animate only works if nothing
                // ELSE in the chain below freezes the arm. A replaced draw with
                // root motion - which is exactly what "lunging forward"
                // describes - can raise bAnimationDriven, and that freezes for
                // "scripted idle" instead: same held clip, different reason,
                // and indistinguishable in the field from the fix simply not
                // working. Report it instead of leaving the next round to
                // guess. Both outcomes testify, because a silent decline is
                // indistinguishable from a dead feature (r28h).
                //
                // Evaluated BEFORE the chain, deliberately: every input it
                // needs is already known here, so it cannot disturb the
                // if/else-if structure - restructuring that chain silently
                // moved the locomotion settle under a frozen arm once already.
                if (WeaponPreview::NormalizeCapped()) {
                    const char* const wouldFreeze =
                        freezeReason ? freezeReason
                                     : (!dodgeFlag.empty() ? "mid-dodge" : nullptr);
                    if (wouldFreeze) {
                        spdlog::warn("idle-in-menus: the arm is freezing for '{}' even though "
                                     "the draw/sheathe clip could not be normalized - the clip "
                                     "is STILL held half-played. The capped-clip exception did "
                                     "not take; that reason needs exempting too.",
                                     wouldFreeze);
                    } else {
                        spdlog::info("idle-in-menus: NOT freezing - the draw/sheathe clip could "
                                     "not be normalized, so it is allowed to finish on real "
                                     "frames instead of being held half-played.");
                    }
                }
                if (movingDraw) {
                    // Name the branch. A silent decline is indistinguishable
                    // from a dead feature (r28h), and this one can only be
                    // told apart from the old behaviour by which log line ran.
                    spdlog::info("idle-in-menus: armed MID-DRAW WHILE MOVING - the "
                                 "draw/sheathe hold stands aside, the locomotion "
                                 "settle owns this arm.");
                }
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
                    // ⚠ CLEAR THE ACTOR'S MOVEMENT STATE, not just the graph's
                    // inputs. Two settle detectors in a row reported that the
                    // graph never leaves locomotion, and both were right: Speed,
                    // Direction and TurnDelta were pinned to zero for a full
                    // second of graph time and it still would not transition,
                    // because the ACTOR still reads as walking and the
                    // locomotion state machine keys on that, not on the blend
                    // variables.
                    //
                    // Field 2026-07-21: "the only time we can see our character
                    // idle now is when they are absolutely still" - the settle
                    // only ever worked when there was nothing to settle.
                    //
                    // Deliberately NOT restored at disarm: the engine re-derives
                    // these from input the moment the menu closes, so writing
                    // them back would be fighting it for one frame.
                    state->actorState1.walking   = false;
                    state->actorState1.running   = false;
                    state->actorState1.sprinting = false;
                    player->NotifyAnimationGraph("moveStop");
                    sentMoveStop_ = true;
                    // ⚠ moveStop ON ITS OWN DOES NOT SETTLE ANYTHING. This
                    // branch has always claimed "the locomotion settle owns this
                    // arm" and then only sent the event. Field 2026-07-21 17:46,
                    // the whole armed window after a moveStop:
                    //
                    //   clip [MENU] 1HM_TurnRight60.HKX  (node '1HM_TurnRight60')
                    //   clip [MENU] 1HM_TurnRight180.HKX (node '1HM_TurnRight180')
                    //
                    // Turn clips, no idle, ever. The graph leaves the walk state
                    // and enters another LOCOMOTION state, so the character goes
                    // on moving in a paused menu - "if i walk backwards i can
                    // still see them walking sometimes".
                    //
                    // The event is a request; nothing was driving the graph to
                    // act on it. So drive it, right here, once, until the graph
                    // actually reaches an idle. Bounded hard: this runs inside
                    // one frame and a runaway would be a hang, not a bug report.
                    int steps = 0;
                    for (; steps < kSettleMaxSteps; ++steps) {
                        player->UpdateAnimation(kSettleStep);
                        player->SetGraphVariableFloat("Speed", 0.0f);
                        player->SetGraphVariableFloat("Direction", 0.0f);
                        // ⚠ TurnDelta TOO, but ONLY inside this burst. Field
                        // 2026-07-21: "when walking backwards while we have
                        // camera rotation, then it can still happen" - a turning
                        // camera keeps TurnDelta non-zero, the graph sits in a
                        // TURN state (1HM_TurnRight60 / 1HM_TurnRight180), and
                        // an idle never activates, so the settle runs its whole
                        // budget and gives up.
                        //
                        // This does NOT reinstate the r19 bug. That one wrote
                        // TurnDelta on EVERY TICK FOR THE WHOLE MENU, which
                        // fought the preview spin's own input and produced
                        // "cannot rotate the player". This writes it inside a
                        // single arm-edge burst that ends the moment an idle
                        // activates; every tick after the arm leaves it alone,
                        // so the spin owns it exactly as before.
                        player->SetGraphVariableFloat("TurnDelta", 0.0f);
                        // Ask what the graph is PLAYING, not whether an idle
                        // happened to re-activate. See ClipProbe::InLocomotionClip.
                        if (!ClipProbe::InLocomotionClip()) {
                            ++steps;
                            break;
                        }
                    }
                    const bool reached = !ClipProbe::InLocomotionClip();
                    // ⚠ FALL BACK TO HOLDING THE FRAME. Field 2026-07-21 17:58:
                    // every single arm reported "30 step(s) (1.00s) but NO IDLE
                    // ACTIVATED". A full second of graph time with Speed,
                    // Direction and TurnDelta all pinned to zero and moveStop
                    // sent, and the graph still will not leave locomotion - so
                    // the actor's own movement state, not the graph variables,
                    // is what holds it there.
                    //
                    // Ticking a graph that cannot reach an idle can only ever
                    // show movement, which is the user's "they move in a
                    // different angle when we rotate differently" - the walk is
                    // steering with the camera. So stop ticking it. Holding the
                    // caught frame is what every other unsettleable arm already
                    // does (mid-air, mid-attack, furniture, dodges), and it is
                    // strictly better than a character who walks in a paused
                    // menu.
                    //
                    // Deliberately conditional on the MEASUREMENT rather than on
                    // "was locomoting": an arm that does settle keeps the live
                    // breathing idle, which is the whole point of the mod.
                    if (!reached && Settings::GetSingleton().freezeUnsettledPose) {
                        airFrozenArm_ = true;
                    }
                    spdlog::info("idle-in-menus: moveStop sent at arm (grounded locomotion) "
                                 "- settled in {} step(s) ({:.2f}s) {}.", steps,
                                 static_cast<float>(steps) * kSettleStep,
                                 reached ? "and the graph LEFT LOCOMOTION - ticking normally, "
                                           "so the idle stays alive"
                                         : "but it is STILL IN A LOCOMOTION CLIP - holding the "
                                           "caught frame instead, so it cannot walk in the menu");
                }
            }
            // F-13: papyrus expression mods are frozen by the pause (B-6) -
            // an arm can catch the face MID-SEQUENCE (half a blink) and
            // hold it all menu. Mode 2 (neutral): save the expression, ramp
            // to neutral via the engine's own facegen reset (our face tick
            // animates it), restore at disarm. Mode 1 (live): handled per
            // tick in the face block below (composed-buffer settle + blink
            // machine care); here only the arm-time CAUGHT snapshot is
            // logged - one line names the stuck channel when the field
            // reports a frozen face. RaceMenu owns its face; both modes
            // need the tick.
            if (Settings::GetSingleton().tickFace && !raceMenuOpen_.load()) {
                const int faceMode = Settings::GetSingleton().faceInMenus;
                if (faceMode == 2) {
                    FaceNeutral::Apply();
                } else if (faceMode == 1) {
                    if (auto* face = player->GetFaceGenAnimationData()) {
                        const auto val = [](const RE::BSFaceGenKeyframeMultiple& a_kf,
                                            std::uint32_t a_i) {
                            return (a_kf.values && a_kf.count > a_i) ? a_kf.values[a_i]
                                                                     : 0.0f;
                        };
                        float phonMax = 0.0f;
                        for (std::uint32_t i = 0;
                             face->unk140.values && i < face->unk140.count; ++i) {
                            phonMax = (std::max)(phonMax,
                                                 std::fabs(face->unk140.values[i]));
                        }
                        spdlog::info(
                            "face live: caught machine={} delay={:.2f} | composed "
                            "lids L/R={:.2f}/{:.2f} gaze8={:.2f} phonMax={:.2f} | "
                            "exprOverride={} eyesClosed21A={}",
                            face->unk200, face->blinkDelay, val(face->unk100, 0),
                            val(face->unk100, 1), val(face->unk100, 8), phonMax,
                            face->exprOverride, face->unk21A);

                        // F-16 r3 (user: "when we open the menu when they mid
                        // blink, we can see their eyes closed in the menu").
                        // ~9% of opens land inside a blink (field log
                        // 2026-07-18: 2 of 23, and the first telemetry sample
                        // is 0.35s AFTER the arm, so the real share is higher).
                        // The machine does finish the blink - but it finishes
                        // over the following ~0.2s WITH THE MENU ALREADY UP,
                        // and this is a posing/screenshot mod: a caught blink
                        // is exactly the frame you did not want.
                        //
                        // Release it AT THE ARM EDGE instead of waiting:
                        //   - park the state machine at 0. Per the decompile
                        //     (mtb_blinkgen.c 0x1403c2930) state 0 counts the
                        //     timer down and writes NOTHING, while states 1/2
                        //     rewrite the composed lids every Update - so
                        //     clearing lids without parking would just be
                        //     overwritten on the next tick.
                        //   - then clear the composed lid pair (the buffer the
                        //     bake reads) the way the machine writes it
                        //     (values + isUpdated=false), plus the input
                        //     keyframes so a later recompose cannot resurrect
                        //     the half-blink.
                        //   - hold the next blink off briefly so the menu does
                        //     not open straight into another close.
                        // ONE-SHOT, arm edge only: the per-tick path is left
                        // alone, because a per-tick pin is exactly what made
                        // r32-r34 "stop blinking" (r34.5 - our own pin held
                        // the machine mid-blink forever).
                        //
                        // unk21A gates it: state 4 drives the lids CLOSED while
                        // that flag is up (the engine's eyes-shut hold, e.g. a
                        // sleeping actor), so forcing those eyes open would
                        // fight a deliberate engine state.
                        const bool lidsShut = val(face->unk100, 0) > 0.05f ||
                                              val(face->unk100, 1) > 0.05f;
                        if (!face->unk21A && (lidsShut || face->unk200 != 0)) {
                            const int  caughtState = face->unk200;
                            face->unk200 = 0;  // waiting: the machine stops writing lids
                            if (auto& comp = face->unk100;
                                comp.values && comp.count > 1) {
                                comp.values[0] = 0.0f;
                                comp.values[1] = 0.0f;
                                comp.isUpdated = false;
                            }
                            if (auto& mod = face->modifierKeyFrame;
                                mod.values && mod.count > 1) {
                                mod.SetValue(0, 0.0f);
                                mod.SetValue(1, 0.0f);
                            }
                            if (face->blinkDelay < 0.6f) {
                                face->blinkDelay = 0.6f;
                            }
                            spdlog::info("face live: menu opened mid-blink "
                                         "(machine state {}) - eyes released open; "
                                         "natural blinking resumes in {:.2f}s.",
                                         caughtState, face->blinkDelay);
                        }
                    }
                }
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
            // Face counters are PER SESSION - the disarm edge reports them.
            faceMeshApplies_ = 0;
            blinkStarts_ = 0;
            blinkCompletes_ = 0;
            blinkCaps_ = 0;
            blinkLidPeak_ = 0.0f;
            blinkStatePrev_ = -1;
            faceNodeMissingLogged_ = false;
            faceHeldLogged_ = false;
            freezeDeclineLogged_ = false;
            ClipProbe::ArmedSessionBegin();  // clip counters are per session too
            spdlog::info("Bubble ARMED (paused menu, player 3D loaded).");
        }
        AnimEventProbe::SetArmed(true);  // diagnostic tag: events below are the MENU case
        ActorTickProbe::MarkArmed(true);
        EquipNotifyGate::SetArmed(true);
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
                if (!cfg.CellLightAllowed()) {
                    StudioLight::Restore();
                }
                if (cfg.CellLightAllowed()) {
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

        // F-26: re-evaluate every tick, not just at the arm edge. This is what
        // catches a weapon SWAP made inside the menu - the engine attaches the
        // newly equipped weapon to its SHEATH node, and only a later Update()
        // sees the FormID change and re-draws it into the hand. It is also what
        // clears inFlight once our own transition settles. The arm-edge call
        // above cannot do either job: it runs once, before the draw it issues
        // has even started.
        WeaponPreview::Update(player, true, raceMenuOpen_.load());

        // RaceSexMenu (F-9, experimental): the engine's own racemenu mode
        // already animates the player (the same special path CBPC keys
        // off) - our graph/face/caster ticks would double-step it. The
        // space/lighting/rig/stage and the FSMP drive still run.
        //
        // r18 folds the unpaused case into the SAME seam. When the world is
        // live the engine is ticking the player itself, which is the identical
        // situation - so suppress the drive and keep the scene, instead of
        // tearing the scene down (what the old OnFrame Disarm did). This is the
        // half of the r18 decoupling that makes an armed session survive losing
        // the pause: the void belongs to the menu, the ticking belongs to the
        // pause, and they are now independent.
        const bool engineAnimates = raceMenuOpen_.load() || !a_paused;

        // ⚠ NEVER DECLINE SILENTLY (r28h). Freeze holds the pose by not ticking
        // the graph - which only means anything while the world is paused and
        // WE are the only thing moving the player. With Skyrim Souls keeping a
        // menu live the engine animates the player itself, so there is nothing
        // here to withhold and the checkbox does exactly nothing. That is not a
        // conflict to fix, it is a case to name: a Souls user who ticks
        // "Freeze the character" and sees their character keep moving needs the
        // log to tell them which switch actually governs it.
        if (Settings::GetSingleton().freezeCharacter && engineAnimates &&
            !freezeDeclineLogged_) {
            freezeDeclineLogged_ = true;
            spdlog::info("freeze character: NOT APPLIED this session - {}. The engine is "
                         "animating the player itself, so holding our own tick withholds "
                         "nothing. Untick \"Keep these menus unpaused\" to pose in a "
                         "frozen scene.",
                         raceMenuOpen_.load()
                             ? "RaceSexMenu drives the player on its own path"
                             : "this menu is not paused (Skyrim Souls keeps it live)");
        }

        // ⚠ EquipClipInFlight() is checked LIVE here, not only at the arm edge.
        //
        // airFrozenArm_ is an arm-edge LATCH, so it can only ever describe the
        // state the menu OPENED in. The field showed the hole: equipping a
        // weapon INSIDE an already-open menu starts a fresh draw/sheathe that
        // the latch knows nothing about, and ticking ran it - plus the idle
        // pick after it - under the frozen VM. 98 of 110 footstep events in the
        // field log came from menus where an equip started after the arm.
        //
        // Same rule as the arm edge, evaluated continuously: while a
        // draw/sheathe clip or its idle settle is in flight, do not step the
        // graph. It resumes by itself the moment the hold expires.
        //
        // movingArm_ exempts the LIVE half of the hold for the same reason the
        // arm-edge half is exempted above: this menu opened on a moving graph,
        // the settle owns it, and holding it half-drawn mid-stride is the thing
        // the user reported. A menu that opened standing still is untouched, so
        // the lunge-loop fix keeps the case it was built for.
        const bool equipHold = Settings::GetSingleton().freezeDrawSheathe && !movingArm_ &&
                               ClipProbe::EquipClipInFlight();
        // ONE name for "the pose is being held", so the face can follow it.
        const bool animHeld = airFrozenArm_ || equipHold;
        if (Settings::GetSingleton().tickAnimation && !engineAnimates && !animHeld &&
            !Settings::GetSingleton().freezeCharacter) {
            // TESObjectREFR vfunc 0x7D - steps the behavior graph with our dt;
            // the PlayerCharacter override also refreshes 1st+3rd person
            // graphs. No pause gate inside (docs/SPIKE-A-RE.md).
            player->UpdateAnimation(dt);
        }

        // A HELD POSE HOLDS ITS FACE TOO. Field (user): "when the character is
        // on hold we can see them blinking still, it's slightly jarring" - a
        // statue that blinks reads as a bug, because the one thing still moving
        // draws the eye to everything that is not.
        //
        // But SETTLE BEFORE HOLDING. Stopping the face the instant the pose
        // freezes would bake whatever the pause caught - and a blink caught
        // halfway is exactly the shut-eyes defect this stint spent its whole
        // budget on. FaceAtRest gates the hold on the machine being parked with
        // the lids open, so an in-flight blink always finishes first and the
        // last frame baked is an open-eyed one.
        const bool faceHeld =
            animHeld && FaceAtRest(player->GetFaceGenAnimationData());
        if (faceHeld != faceHeldLogged_) {
            faceHeldLogged_ = faceHeld;
            spdlog::debug("face live: {} (the pose is {}).",
                          faceHeld ? "HELD - blinking stopped, eyes open"
                                   : "live again",
                          animHeld ? "held" : "animating");
        }
        if (Settings::GetSingleton().tickFace && !engineAnimates && !faceHeld &&
            !Settings::GetSingleton().freezeCharacter) {
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
                const int faceMode = Settings::GetSingleton().faceInMenus;
                const bool pinBlinks = faceMode == 2 && !engineAnimates;
                const auto zeroBlinks = [&face] {
                    if (auto& mod = face->modifierKeyFrame;
                        mod.values && mod.count > 1 &&
                        (mod.values[0] != 0.0f || mod.values[1] != 0.0f)) {
                        mod.SetValue(0, 0.0f);
                        mod.SetValue(1, 0.0f);
                    }
                };
                // F-16 mode 1 - LIVE face (the "Conditional Expressions
                // mid-blink" fix). The caught MOOD expression is kept:
                // exprOverride stays as the mod left it - when raised (CE
                // does), the engine's mood re-assert (the only thing +0x21E
                // gates, mtb_facegen.c:46/118) never wipes the authored
                // face. Everything below writes the COMPOSED buffers the
                // way the blink machine itself does (values + isUpdated
                // false) - the two-buffer lesson: input-buffer writes are
                // invisible while no channel composes.
                if (faceMode == 1) {
                    // Eyelids: the ambient machine (mtb_blinkgen.c) owns the
                    // composed pair and runs free - no pin. While it WAITS
                    // (state 0) it writes nothing, so visible residue there
                    // is a frozen writer's half-blink: pull the next natural
                    // blink close - one close-open cycle wipes it and ends
                    // eyes-open. One-shot by construction (post-blink lids
                    // are 0, the condition dies).
                    if (face->unk200 == 0) {
                        zeroBlinks();  // input hygiene: no recompose resurrection
                        if (const auto& comp = face->unk100;
                            comp.values && comp.count > 1 &&
                            (comp.values[0] > 0.05f || comp.values[1] > 0.05f) &&
                            face->blinkDelay > 0.3f) {
                            face->blinkDelay = 0.1f;
                            spdlog::debug("face live: composed lid residue - "
                                          "wipe blink pulled close.");
                        }
                        // ⚠ DIAGNOSTIC ONLY (bDiagnosticProbes) - THE BLINK IS
                        // UNVERIFIABLE BY EYE, AND THAT IS THE ACTUAL BLOCKER.
                        //
                        // Every measurable check on the blink fix passes: the
                        // machine is caught mid-blink with composed lids moving
                        // through real values, the engine's face caller runs
                        // while armed, and our change byte survives to be read.
                        // But a natural blink is ~0.2 s once every several
                        // seconds, and the field verdict was "hard to tell" -
                        // so the fix has sat unconfirmed, which is exactly how
                        // 0.6.0 shipped a changelog entry for a fix that did
                        // not work.
                        //
                        // This does not forge a blink or write a lid value. It
                        // only shortens the ENGINE's own countdown so its own
                        // machine blinks about twice a second. If the eyes
                        // visibly blink in a menu with probes on, the composed
                        // lids are reaching the mesh and natural blinking works
                        // too; if they stay open, the bake is still not landing
                        // and the fix is inert. Unmistakable either way.
                        //
                        // ⚠ It must testify to its own liveness: a probe that
                        // logs nothing is indistinguishable from a probe that
                        // never installed, and this one had never once been
                        // exercised with probes actually on (the run that was
                        // supposed to reached bDelay 7.5 s, proving the cap
                        // never applied). blinkCaps_ is that testimony.
                        if (Settings::GetSingleton().blinkStressTest &&
                            face->blinkDelay > 0.5f) {
                            face->blinkDelay = 0.5f;
                            ++blinkCaps_;
                        }
                    }
                    // Gaze (composed 8-11): frozen off-level gaze both looks
                    // stuck and can PARK the machine in its look-hold states
                    // (3/4 exit on a values[8] threshold test that nothing
                    // re-evaluates while gaze is pause-frozen -> no blinking
                    // all menu). Settle toward level; the machine un-parks
                    // itself on the next Update. A genuinely active gaze
                    // channel recomposes over this write and simply wins.
                    // Phonemes (composed unk140): a mouth caught mid-shape
                    // holds forever otherwise - settle it closed; the mood
                    // expression (unk0C0) carries the look and is untouched.
                    const float k = 1.0f - (std::min)(1.0f, dt * 8.0f);
                    const auto settle = [k](RE::BSFaceGenKeyframeMultiple& a_kf,
                                            std::uint32_t a_from,
                                            std::uint32_t a_to) {
                        if (!a_kf.values) {
                            return;
                        }
                        for (std::uint32_t i = a_from;
                             i <= a_to && i < a_kf.count; ++i) {
                            if (float& v = a_kf.values[i]; v != 0.0f) {
                                v *= k;
                                if (v > -0.01f && v < 0.01f) {
                                    v = 0.0f;
                                }
                                a_kf.isUpdated = false;
                            }
                        }
                    };
                    settle(face->unk100, 8, 11);
                    if (face->unk140.count > 0) {
                        settle(face->unk140, 0, face->unk140.count - 1);
                    }
                    // Input phonemes: cleared so a later recompose can't
                    // resurrect the caught mouth (the frozen writer re-drives
                    // itself after unpause anyway).
                    if (auto& ph = face->phenomeKeyFrame; ph.values) {
                        for (std::uint32_t i = 0; i < ph.count; ++i) {
                            if (ph.values[i] != 0.0f) {
                                ph.SetValue(i, 0.0f);
                            }
                        }
                    }
                }
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
                // ⚠ 0.7.1 - THE MIRROR STOPPED HALFWAY, AND THAT WAS THE WHOLE
                // BLINK BUG. The engine's caller (0x3D9440) does FOUR things:
                //
                //   1. entry gate - bail unless its own force arg is set OR
                //      +0x218 is non-zero;
                //   2. clear +0x218, call Update, KEEP its "changed" return;
                //   3. if changed, register the face MODEL into the re-bake
                //      queue (DAT_142f07d18) - this is what puts new lid
                //      values on the actual mesh;
                //   4. set +0x215 on the way out.
                //
                // We copied 2 and skipped 3. So every write here moved the
                // DATA and never reached the MESH: whatever was last baked
                // before the pause stayed on screen. Caught mid-blink that is
                // frozen shut eyes; caught open it is no blinking at all - one
                // defect, and both field reports are it.
                //
                // Pre-clearing +0x218 made it worse: the engine's own caller
                // then FAILED its entry gate (step 1), so we were also eating
                // refresh requests posted by other face mods.
                //
                // Step 3 is not ours to call - it wants the face model, not
                // this anim data, and takes the global face lock. So ask for it
                // instead: leave the pending flag SET whenever Update reports a
                // change, and the engine's caller does 1-4 itself.
                //
                // PROBE (0.7.1): +0x215 is set by that caller on every path
                // past its entry gate. Clearing it here means a set value next
                // tick PROVES the caller is alive inside a paused menu - which
                // is exactly what decides whether asking for a re-bake can work
                // at all. If this logs "is NOT running", the request cannot be
                // served and we have to drive the bake ourselves.
                // +0x215 sits inside unk214 (a uint16), so it is reached by
                // byte offset rather than by member.
                auto* const rawFace   = reinterpret_cast<std::uint8_t*>(face);
                const bool  callerRan = rawFace[0x215] != 0;
                rawFace[0x215]        = 0;
                // Did the change byte we set LAST tick survive? Update clears
                // +0x217 as its very first act and nothing else touches it, so
                // a 0 here proves the engine's caller ran Update ITSELF (its
                // path B) and clobbered our request. That single bit decides
                // the next move and cannot be read any other way.
                const bool ourFlagSurvived = face->unk217 != 0;
                if (callerRan != faceCallerSeen_) {
                    faceCallerSeen_ = callerRan;
                    spdlog::info("face live: engine face caller (0x3D9440) {} while armed "
                                 "(+0x215 {}); our change byte {} - {}.",
                                 callerRan ? "IS running" : "is NOT running",
                                 callerRan ? "came back set" : "stayed clear",
                                 ourFlagSurvived ? "SURVIVED" : "was CLEARED",
                                 ourFlagSurvived
                                     ? "the caller reads +0x217 (path A), so forcing it "
                                       "below is the fix"
                                     : "the caller re-runs Update itself (path B) and "
                                       "clobbers it; while paused its own dt is 0, which "
                                       "SKIPS the recompose entirely, so no write of ours "
                                       "can ever signal a change - the bake has to be "
                                       "driven off the face MODEL (+0x161) instead");
                }
                g_faceGenUpdate(face, dt, true);
                // ⚠ 0.7.1 - THE ACTUAL BAKE GATE. Read off the decompile, not
                // guessed: Update CLEARS +0x217 on entry, runs the blink
                // generator, and then ORs in a change bit ONLY when one of the
                // INPUT channels recomposed. The generator writes the COMPOSED
                // lid pair directly and never touches an input channel - so an
                // ordinary blink leaves +0x217 at 0, Update returns 0, and the
                // engine's caller skips the morph re-bake entirely.
                //
                // That is why the machine measurably blinks (bDelay counts down
                // and re-randomises on every completed blink, which only
                // happens on the state-2 exit) while the face on screen never
                // changes. The data was always fine. Nothing ever asked for the
                // mesh to be rebuilt.
                //
                // So say it changed, because it did. +0x217 is the caller's own
                // "something moved" signal and this is the one write that
                // reaches the mesh.
                face->unk217 = 1;
                face->unk218 = 1;  // and let it past the entry gate
                if (pinBlinks) {
                    zeroBlinks();  // other writers (MFG-style mods) stay covered
                }

                // Count the engine's own blink machine so the session can be
                // reported as numbers instead of an impression (Bubble.h).
                // Sampled AFTER Update, so these are the states the machine
                // actually advanced through on our dt.
                {
                    const int st = static_cast<int>(face->unk200);
                    if (blinkStatePrev_ != 1 && st == 1) {
                        ++blinkStarts_;
                    }
                    if (blinkStatePrev_ == 2 && st != 2) {
                        ++blinkCompletes_;
                    }
                    blinkStatePrev_ = st;
                    if (const auto& comp = face->unk100;
                        comp.values && comp.count > 1 && comp.values[0] > blinkLidPeak_) {
                        blinkLidPeak_ = comp.values[0];
                    }
                }

                // ⚠ 0.7.1 r2 - THE BAKE, AND THE ANSWER TO "CAN THE FACE MESH
                // REFRESH AT ALL WHILE THE WORLD IS PAUSED".
                //
                // It can. Nothing was ever asking it to.
                //
                // Everything above moves the DATA, and every measurement of the
                // data has passed for three rounds: the machine runs, blinks
                // complete, the change byte survives, the engine's face caller
                // is alive. The mesh still never moved, because setting the
                // change byte does not bake anything. Decompiling
                // BSFaceGenNiNode::UpdateDownwardPass end to end (Offsets.h,
                // Tools\re\research\mtb_facebake*.c) shows all it does with
                // that byte is REGISTER the face node in a global queue, and
                // the queue's only consumer runs from a parallel job batch on
                // Main::Update's NOT-PAUSED branch. A paused menu never drains
                // it, so the mesh keeps whatever was baked before the pause -
                // shut eyes if it caught a blink, no blinking at all if it did
                // not. One defect, and both field reports are it.
                //
                // Main::Update's PAUSED branch does bake one face, gated on a
                // single menu:
                //     if (UI::IsMenuOpen(InterfaceStrings::raceSexMenu))
                //         FaceGenApplyMorphs(player->GetFaceNodeSkinned(), true);
                // That is why RaceMenu has a live face in a paused game and
                // nothing else does. So do exactly what the engine does, for
                // our menus: same call, same force flag, same thread (this tick
                // runs from a write_call inside Main::Update).
                if (Settings::GetSingleton().faceMeshRefresh) {
                    auto* faceNode = player->GetFaceNodeSkinned();
                    if (faceNode) {
                        g_faceGenApplyMorphs(faceNode, true);
                        if (++faceMeshApplies_ == 1) {
                            spdlog::info("face live: MESH BAKE running - "
                                         "FaceGenApplyMorphs(force) per tick over {} "
                                         "face children. This is the half that "
                                         "reaches the geometry.",
                                         faceNode->GetChildren().capacity());
                        }
                    } else if (faceMeshApplies_ == 0 && !faceNodeMissingLogged_) {
                        // Never conclude from absent evidence: if the totals at
                        // disarm read zero, this line says whether it was the
                        // node or the call.
                        faceNodeMissingLogged_ = true;
                        spdlog::warn("face live: no skinned face node on the player - "
                                     "the mesh bake cannot run (eyes will hold "
                                     "whatever was baked before the pause).");
                    }
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
                        // r61 (user: "when we detect Show Player In Menus we
                        // want to disable their rotation and just use our
                        // rotation"). SPIM rotates on RIGHT-MOUSE HELD - the
                        // very input our spin sink uses (its Event.cpp: mouse
                        // button 1 arms allowRotation, then each MouseMove does
                        // SetRotationZ on the PLAYER *and* freeRotation.x -=
                        // amt on the camera; it is a body turn with a camera
                        // counter-turn, not an orbit). So one drag fed BOTH
                        // systems: our sink plus this harvest. Since the r60
                        // sign flip they push the same way = double-speed spin
                        // (before r60 they fought and part-cancelled) - neither
                        // is right.
                        // With SPIM present we keep the PIN and drop the DELTA:
                        // their player-heading write is already overwritten by
                        // the armedHeading_ pin above, and the reset below
                        // undoes their camera counter-turn, so their rotation
                        // is fully neutralised and our sink is the only thing
                        // that spins the character. previewSpin gates this
                        // whole block, so switching our spin off hands their
                        // rotation straight back.
                        // Without SPIM the harvest is unchanged (it no-ops on
                        // SPII builds, which never touch freeRot).
                        if (OwnView::SpimPresent() &&
                            Settings::GetSingleton().overrideSpimRotation) {
                            // One line per session: field-proof that the
                            // override actually engaged (it cannot be tested
                            // on a load order without SPIM).
                            static bool logged = false;
                            if (!logged) {
                                logged = true;
                                spdlog::info("preview spin: Show Player In Menus detected - "
                                             "its rotation is neutralised, our spin owns the "
                                             "character (bOverrideSpimRotation=1).");
                            }
                        } else {
                            spinTarget_ += delta;
                        }
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
                // Same convention as the right-drag (r60): pushing the stick
                // left turns the character to their left. Both direct inputs
                // flipped together so they can never disagree.
                spinTarget_ -= stickX *
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

        // r18: NOT gated on raceMenu (the F-9 note above keeps the FSMP drive
        // running there on purpose), but it MUST stand down when the world is
        // live - the engine steps SMP itself then, and stepping it again is a
        // double-step. This is the one drive site engineAnimates does not
        // already cover, because its two halves want different answers.
        if (Settings::GetSingleton().driveSmp && a_paused) {
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
        // F-16 discriminators. bState frozen at 0 with bDelay never
        // shrinking across rows = the ambient generator is globally gated
        // off (its enable byte) and we must drive blinks ourselves. bState
        // stuck at 3/4 = the look-hold parking (the gaze settle should
        // prevent it). bState cycling 0->1->2 with cLid moving while the
        // face visibly never blinks = a bake-side break. cPhon shrinking
        // to 0 = the mouth settle working.
        std::uint32_t blinkState = 0xFFFF;
        float cLidL = -1.0f, cGaze8 = -1.0f, cPhonMax = -1.0f;
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
                blinkState = face->unk200;
                if (face->unk100.values && face->unk100.count > 8) {
                    cLidL = face->unk100.values[0];
                    cGaze8 = face->unk100.values[8];
                }
                cPhonMax = 0.0f;
                for (std::uint32_t i = 0;
                     face->unk140.values && i < face->unk140.count; ++i) {
                    cPhonMax = (std::max)(cPhonMax, std::fabs(face->unk140.values[i]));
                }
            }
        }
        spdlog::debug(
            "tick #{:>5}: dt={:.4f} | engine slowDt={:.4f} realDt={:.4f} dt3={:.4f} | "
            "freezeTime={} numPauses={} uiPaused={} | fsmp={} cbpc={} | cam st={} "
            "pos=({:.1f},{:.1f},{:.1f}) | freeRot=({:.2f},{:.2f}) en={} animDriven={} "
            "heading={:.2f} pin={:.2f} | spin={:.2f}->{:.2f} park={:.2f} | T={:.2f} | "
            "blinkL={:.2f} bDelay={:.1f} bState={} cLid={:.2f} cGaze8={:.2f} "
            "cPhon={:.2f}",
            armedTicks_, a_dt, *g_slowDt.get(), *g_realDt.get(), *g_dtVariant3.get(),
            a_main->freezeTime, ui ? ui->numPausesGame : std::uint32_t(0xFFFF),
            a_paused, FsmpDrive::IsAvailable(), CbpcDrive::IsAvailable(), camId,
            camPos.x, camPos.y, camPos.z, freeRotX, freeRotY, freeRotEnabled, animDriven,
            heading, armedHeading_, spinYaw_, spinTarget_, freeRotArm_, Transition::Value(),
            blinkL, blinkDelay, blinkState, cLidL, cGaze8, cPhonMax);
    }
}
