#include "PCH.h"

#include "OwnView.h"

#include "Settings.h"

#include <SimpleIni.h>
#include <Windows.h>

#include <array>
#include <filesystem>

namespace {
    constexpr float kPi = 3.14159265f;

    // ---------------------------------------------------------------- //
    // View-mod presence - who else could own the view?                  //
    // ---------------------------------------------------------------- //
    // DLLs can't hot-load mid-session, so one scan per game session is
    // the truth. The SPII builds (classic and the author's Preview) share
    // one module name; the Preview's own INI says whether barter is
    // covered (Nolvus ships it bBarterMenu=0 - the exact gap the r30
    // field session hit).
    struct Coverage {
        bool checked = false;
        bool spim = false;        // ShowPlayerInMenus.dll (menus-wide)
        bool spii = false;        // ShowPlayerInInventory.dll (classic / Preview)
        bool spiiBarter = false;  // the SPII build's INI opts into barter
        // SPII's OWN camera values ([Camera] in its INI). Field reports
        // 2026-07-18 ("Barter/Pickpocketing Menu is Zoomed in Strangely",
        // "the FOV and character position are completely off"): SPII 1.4
        // covers Inventory/Magic (+Barter when bBarterMenu=1) and NEVER
        // ContainerMenu - its DLL contains no "ContainerMenu" string at all -
        // so looting/pickpocketing, and barter with the toggle off, fall to
        // US. We were framing those with the SPIM preset (FOV 90, offsets
        // -20/50/0) while the player's inventory used SPII's (FOV 60,
        // -46.7/-12/-20): the same character, two completely different
        // cameras. Mirror SPII's values so the menus it does not cover look
        // like the ones it does.
        float spiiOffsetX = -46.7f;
        float spiiOffsetY = -12.0f;
        float spiiOffsetZ = -20.0f;
        float spiiFov     = 60.0f;
        std::filesystem::path spiiIniPath;
    };
    Coverage g_cov;

    // SPII's INI, re-read on demand. bBarterMenu is togglable from its SKSE
    // Menu Framework panel AT RUNTIME, so a value cached once per session goes
    // stale the moment the user flips it - and a stale "SPII does not cover
    // barter" makes BOTH mods frame that menu. Cheap: one small INI, once per
    // menu open.
    void RefreshSpiiIni() {
        if (!g_cov.spii || g_cov.spiiIniPath.empty()) {
            return;
        }
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(g_cov.spiiIniPath.c_str()) != SI_OK) {
            return;
        }
        const bool wasBarter = g_cov.spiiBarter;
        g_cov.spiiBarter = ini.GetBoolValue("General", "bBarterMenu", false);
        g_cov.spiiOffsetX = static_cast<float>(
            ini.GetDoubleValue("Camera", "fOffsetX", g_cov.spiiOffsetX));
        g_cov.spiiOffsetY = static_cast<float>(
            ini.GetDoubleValue("Camera", "fOffsetY", g_cov.spiiOffsetY));
        g_cov.spiiOffsetZ = static_cast<float>(
            ini.GetDoubleValue("Camera", "fOffsetZ", g_cov.spiiOffsetZ));
        g_cov.spiiFov = static_cast<float>(
            ini.GetDoubleValue("Camera", "fFOV", g_cov.spiiFov));
        if (wasBarter != g_cov.spiiBarter) {
            spdlog::info("own view: SPII barter coverage changed to {} (its panel "
                         "writes the INI live) - barter framing follows it now.",
                         g_cov.spiiBarter);
        }
    }

    std::filesystem::path GameDataPath() {
        wchar_t buf[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path{ buf }.parent_path() / L"Data";
    }

    void RefreshCoverage() {
        if (g_cov.checked) {
            return;
        }
        g_cov.checked = true;
        g_cov.spim = ::GetModuleHandleW(L"ShowPlayerInMenus.dll") != nullptr;
        g_cov.spii = ::GetModuleHandleW(L"ShowPlayerInInventory.dll") != nullptr;
        if (g_cov.spii) {
            CSimpleIniA ini;
            ini.SetUnicode();
            // r43: resolve the INI FROM THE LOADED MODULE's real on-disk
            // path - exactly how ivy's own Settings.cpp finds it
            // (GetModuleFileNameW on their DLL → sibling file in the MOD
            // folder; MO2 reports the real mod path for loaded modules).
            // The old exe-relative Data\ read was never served by the VFS:
            // LoadFile failed SILENTLY every session and barter coverage
            // always read the default false - invisible while the shipped
            // flag was 0, split-brain the moment the flag flipped (r41
            // field: SPII framed barter AND we framed barter).
            std::filesystem::path iniPath;
            if (HMODULE mod = ::GetModuleHandleW(L"ShowPlayerInInventory.dll")) {
                wchar_t buf[MAX_PATH]{};
                if (const DWORD len = ::GetModuleFileNameW(mod, buf, MAX_PATH);
                    len > 0 && len < MAX_PATH) {
                    iniPath = std::filesystem::path{ buf }.parent_path()
                              / L"ShowPlayerInInventory.ini";
                }
            }
            if (iniPath.empty()) {
                iniPath = GameDataPath() / L"SKSE" / L"Plugins"
                          / L"ShowPlayerInInventory.ini";
            }
            g_cov.spiiIniPath = iniPath;
            if (ini.LoadFile(iniPath.c_str()) == SI_OK) {
                g_cov.spiiBarter = ini.GetBoolValue("General", "bBarterMenu", false);
                RefreshSpiiIni();  // also pick up its [Camera] values
            } else {
                spdlog::warn("own view: could not read '{}' - assuming SPII "
                             "barter coverage OFF (defer will not happen).",
                             iniPath.string());
            }
        }
        spdlog::info("own view: view-mod scan - SPIM={} SPII={} (SPII barter={}).",
                     g_cov.spim, g_cov.spii, g_cov.spiiBarter);
    }

    bool MenuCovered(const std::string& a_menu) {
        RefreshCoverage();
        RefreshSpiiIni();  // bBarterMenu is live-togglable in SPII's own panel
        if (g_cov.spim) {
            // Menus-wide by design; its MCM decides per menu - defer, never
            // double-frame (two owners writing the same INI Settings would
            // corrupt each other's restores).
            return true;
        }
        if (g_cov.spii) {
            // SPII's own Start() DECLINES combat and horseback (source:
            // Tools/Show-Player-In-Inventory/src/MenuCamera.cpp v1.3, MIT)
            // - during those, the menus it nominally covers are really
            // unmanaged. This was the r32-33 field mystery ("in combat we
            // don't see the character enter the view"): we deferred to a
            // mod that had already bowed out.
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && (player->IsInCombat() || player->IsOnMount())) {
                return false;
            }
            if (a_menu == "InventoryMenu" || a_menu == "MagicMenu") {
                // Classic SPII covers both; the Preview build at least
                // inventory - conservative (a wrong "covered" costs a
                // framing gap, a wrong "unmanaged" costs a double-frame).
                return true;
            }
            if (a_menu == "BarterMenu") {
                return g_cov.spiiBarter;
            }
        }
        return false;  // ContainerMenu: covered by neither SPII build
    }

    // ---------------------------------------------------------------- //
    // Framing values                                                    //
    // ---------------------------------------------------------------- //
    // Our INI defaults are the Nolvus SPIM preset (X -20 / Y 50 - boom
    // 105, the framing the user plays with); when SPIM's own MCM files
    // are present in the VFS we adopt them, so the bubble's framing and
    // a live SPIM install can never drift apart.
    struct Framing {
        float x, y, z, pitch, rotation;
    };

    void Overlay(CSimpleIniA& a_ini, Framing& a_f) {
        a_f.x = static_cast<float>(
            a_ini.GetDoubleValue("PositionSettings", "fXOffset", a_f.x));
        a_f.y = static_cast<float>(
            a_ini.GetDoubleValue("PositionSettings", "fYOffset", a_f.y));
        a_f.z = static_cast<float>(
            a_ini.GetDoubleValue("PositionSettings", "fZOffset", a_f.z));
        a_f.pitch = static_cast<float>(
            a_ini.GetDoubleValue("PositionSettings", "fPitch", a_f.pitch));
        a_f.rotation = static_cast<float>(
            a_ini.GetDoubleValue("PositionSettings", "fRotation", a_f.rotation));
    }

    Framing ReadFraming() {
        const auto& s = MTB::Settings::GetSingleton();
        Framing f{ s.ownViewXOffset, s.ownViewYOffset, s.ownViewZOffset,
                   s.ownViewPitch, s.ownViewRotation };
        const auto data = GameDataPath();
        const char* source = "MenuStudio.ini";
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(
                (data / L"MCM/Config/ShowPlayerInMenus/settings.ini").c_str()) == SI_OK) {
            Overlay(ini, f);
            source = "SPIM MCM defaults";
        }
        ini.Reset();
        if (ini.LoadFile((data / L"MCM/Settings/ShowPlayerInMenus.ini").c_str()) == SI_OK) {
            Overlay(ini, f);
            source = "SPIM MCM settings";
        }
        spdlog::debug("own view: framing X={:.1f} Y={:.1f} Z={:.1f} pitch={:.2f} "
                      "rot={:.2f} (source: {}).",
                      f.x, f.y, f.z, f.pitch, f.rotation, source);
        return f;
    }

    // ---------------------------------------------------------------- //
    // Saved originals                                                   //
    // ---------------------------------------------------------------- //
    struct SettingSlot {
        const char*  name;
        RE::Setting* setting = nullptr;
        float        original = 0.0f;
    };
    constexpr std::size_t kMouseWheelSlot = 8;  // restored AFTER the camera update

    struct Saved {
        bool         active = false;
        bool         forcedThird = false;
        // r52: the camera state we switched AWAY from (kFirstPerson OR
        // kMount) - restored verbatim at Disarm. Was hardcoded to first
        // person, which put a dismounted first-person view back after a
        // MOUNTED arm.
        RE::CameraState savedStateId = RE::CameraState::kFirstPerson;
        bool         headtrackingWasEnabled = false;
        bool         toggleAnimCam = false;
        bool         freeRotationEnabled = false;
        float        angleX = 0.0f;
        float        angleZ = 0.0f;
        float        targetZoomOffset = 0.0f;
        float        pitchZoomOffset = 0.0f;
        float        worldFOV = 0.0f;
        RE::NiPoint2 freeRotation{};
        RE::NiPoint3 posOffsetExpected{};
        // The vanilla menu-blur imod (ISMTween 0x434BB): SPIM parks it so
        // the character isn't blurred - r31 field confirmed the blur IS
        // live on own-view arms ("blurring effect around the sides").
        // Mechanism per SPIM source: the radial-blur STRENGTH interpolator
        // pointer is swapped out, the blur-RADIUS interpolator's held
        // float zeroed in place.
        bool                               blurParked = false;
        RE::NiPointer<RE::NiFloatInterpolator> radialBlurStrength;
        float                              blurRadiusValue = 0.0f;
        std::array<SettingSlot, 9> ini{ { { "fOverShoulderCombatPosX:Camera" },
                                          { "fOverShoulderCombatAddY:Camera" },
                                          { "fOverShoulderCombatPosZ:Camera" },
                                          { "fOverShoulderPosX:Camera" },
                                          { "fOverShoulderPosZ:Camera" },
                                          { "fAutoVanityModeDelay:Camera" },
                                          { "fVanityModeMinDist:Camera" },
                                          { "fVanityModeMaxDist:Camera" },
                                          { "fMouseWheelZoomSpeed:Camera" } } };
    };
    Saved g_state;

    RE::ThirdPersonState* ThirdStateObject() {
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera) {
            return nullptr;
        }
        return static_cast<RE::ThirdPersonState*>(
            camera->cameraStates[RE::CameraState::kThirdPerson].get());
    }

    void RestoreBlurImod() {
        if (!g_state.blurParked) {
            return;
        }
        g_state.blurParked = false;
        if (auto* blurMod =
                RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000434BB)) {
            blurMod->radialBlur.strength = g_state.radialBlurStrength;
            if (blurMod->blurRadius) {
                blurMod->blurRadius->floatValue = g_state.blurRadiusValue;
            }
        }
        g_state.radialBlurStrength = nullptr;
    }
}

namespace MTB::OwnView {
    bool ShouldOwn(const std::string& a_menuName, bool a_firstPersonArm) {
        const auto& s = Settings::GetSingleton();
        // r41 (user: "we should use SPII and make it work in the barter
        // menu"): COVERAGE precedes the first-person rule now. The r30 rule
        // ("still first person at arm = no view mod switched it") predates
        // reading ivy's source - their ApplyCameraValues does the direct
        // SetState(third) unconditionally, first-person arms included, and
        // wasFirstPerson restores at their Stop. With their INI now opting
        // barter in (bBarterMenu=1, flipped in the installed mod), a
        // covered menu is THEIRS from any camera. Combat/mounted arms
        // still fall through to us (MenuCovered's carve-out - their
        // Start() declines those), and the tick-3 late-arm fallback
        // backstops any other decline.
        if (MenuCovered(a_menuName)) {
            return false;
        }
        if (a_firstPersonArm) {
            return s.ownViewFirstPerson;
        }
        return s.ownViewUnmanaged;
    }

    void ApplyFraming(bool a_forcedThirdFromFirst, bool a_mounted) {
        if (g_state.active) {
            return;  // already framed (menu switch keeps the arm alive)
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* iniCol = RE::INISettingCollection::GetSingleton();
        auto* third = ThirdStateObject();
        if (!camera || !player || !iniCol || !third) {
            spdlog::warn("own view: framing skipped (camera/player/INI unavailable).");
            return;
        }

        // SAVE every original the recipe touches (SPIM leaks pitchZoomOffset
        // by omission - we keep it).
        g_state.forcedThird = a_forcedThirdFromFirst;
        g_state.angleX = player->data.angle.x;
        // r31 field: "on exit the player faces a completely different
        // direction" - leaving the forced third person bakes the camera
        // yaw into the player heading. SPIM saves/restores angle.z for
        // exactly this; nothing in the recipe writes it, but the exit
        // transition does.
        g_state.angleZ = player->data.angle.z;
        g_state.targetZoomOffset = third->targetZoomOffset;
        g_state.pitchZoomOffset = third->pitchZoomOffset;
        g_state.freeRotation = third->freeRotation;
        g_state.posOffsetExpected = third->posOffsetExpected;
        g_state.toggleAnimCam = third->toggleAnimCam;
        g_state.freeRotationEnabled = third->freeRotationEnabled;
        g_state.worldFOV = camera->worldFOV;
        player->GetGraphVariableBool("IsNPC", g_state.headtrackingWasEnabled);
        for (auto& slot : g_state.ini) {
            slot.setting = iniCol->GetSetting(slot.name);
            if (slot.setting) {
                slot.original = slot.setting->data.f;
            } else {
                spdlog::warn("own view: INI Setting '{}' not found - skipped.", slot.name);
            }
        }

        // APPLY - SPIM's RotateCamera() recipe (Tools/ShowPlayerInMenus/
        // src/Event.cpp:315-412, MIT): the character sits screen-side of
        // the UI panels via the over-shoulder offsets, the boom length is
        // zoom-proof (vanity min == max), the camera faces the player.
        // The transition itself is SPIM's too: a DIRECT SetState on the
        // third-person state - r32 field showed COMBAT first-person arms
        // never left first person through the request-based
        // ForceThirdPerson; SPIM's direct switch is field-proven across
        // its user base including combat menus.
        if (a_forcedThirdFromFirst) {
            // r52: remember what we're leaving (first person OR the mount
            // camera) so Disarm hands the SAME view back. SetState(third)
            // while physically mounted just changes the camera - the
            // player stays seated and the horse keeps rendering, so the
            // over-shoulder framing shows both.
            if (camera->currentState) {
                g_state.savedStateId = camera->currentState->id;
            }
            camera->SetState(third);
        }
        // WHOSE LOOK are we copying? With SPII installed (and no SPIM) the
        // player's inventory is framed by SPII, so the menus SPII does not
        // cover - ContainerMenu always, BarterMenu while bBarterMenu=0 - must
        // use SPII's camera or they read as a different mod (field 2026-07-18:
        // "the FOV and character position are completely off", "zoomed in
        // strangely"). SPII applies its offsets RAW and its own FOV; our SPIM
        // recipe transforms them (-x-75, z-50) and hardcodes FOV 90, which is
        // what made barter/loot look nothing like the inventory.
        const bool spiiLook = g_cov.spii && !g_cov.spim;
        const Framing f = ReadFraming();
        const float newX = -f.x - 75.0f;
        // r53: a mounted rider sits ~120 units up and the horse spans down
        // to the ground - the standing-torso offset framed the LEGS (field:
        // "goofy… saddling empty space", zoomed on the boots). Raise the
        // look-at to the rider's torso and (optionally) pull the boom back.
        // Follow-up (user: mounted view "very different from the standing
        // view"): the raise/boom/pitch are now INI knobs (fOwnViewMount* in
        // [General]) so the mounted framing can be dialled to match the
        // standing look without a rebuild - re-read every save load. The boom
        // default is trimmed from the r53 +140 (which read as a tiny rider
        // above a big horse) toward the standing look.
        const auto& s = Settings::GetSingleton();
        const float riderRaise = a_mounted ? s.ownViewMountRaise : 0.0f;
        const float mountBoom  = a_mounted ? s.ownViewMountBoom : 0.0f;
        const float newZ = f.z - 50.0f + riderRaise;
        const auto set = [&](std::size_t a_i, float a_v) {
            if (g_state.ini[a_i].setting) {
                g_state.ini[a_i].setting->data.f = a_v;
            }
        };
        // SPII's own recipe, read from its source (Tools/Show-Player-In-
        // Inventory, MenuCamera::ApplyCameraValues) and its live INI: offsets
        // go in RAW, offsetY is the combat AddY, angle.x is 0.1, FOV is its
        // fFOV. Mounted deltas still apply on top - SPII declines mounted
        // arms entirely, so that framing stays ours either way.
        const float sx = spiiLook ? g_cov.spiiOffsetX : newX;
        const float sy = spiiLook ? g_cov.spiiOffsetY : 0.0f;
        const float sz = spiiLook ? g_cov.spiiOffsetZ + riderRaise : newZ;
        set(0, sx);              // fOverShoulderCombatPosX
        set(1, sy);              // fOverShoulderCombatAddY
        set(2, sz);              // fOverShoulderCombatPosZ
        set(3, sx);              // fOverShoulderPosX
        set(4, sz);              // fOverShoulderPosZ
        set(5, 10800.0f);        // fAutoVanityModeDelay: no vanity cam mid-frame
        // THE BOOM. Field 2026-07-18, after the offsets/FOV above already
        // matched: "the barter/loot camera is more zoomed in than in the
        // inventory". The previous note here read SPII as exposing no distance
        // and kept ours. That was half right and wholly misleading: SPII's INI
        // ships no distance key because the value is COMPILE-TIME, not because
        // it has none - Settings.h:13, `inline constexpr float distance =
        // 145.0f`, pinned to min AND max at MenuCamera.cpp:309-310, the same
        // equal-pin trick we use. We were pinning to 155 - fOwnViewYOffset =
        // 105, i.e. 40 units CLOSER than the inventory the player is comparing
        // against, and that gap is the entire report. Mirror the constant while
        // we are wearing SPII's look; our own boom still drives every menu that
        // is genuinely ours (mounted, or with SPII absent).
        constexpr float kSpiiBoom = 145.0f;  // SPII include/Settings.h:13
        const float boom = (spiiLook ? kSpiiBoom : 155.0f - f.y) + mountBoom;
        set(6, boom);  // fVanityModeMinDist ─┐ equal = boom never
        set(7, boom);  // fVanityModeMaxDist ─┘ derives from zoom
        set(kMouseWheelSlot, 10000.0f);  // restore-time zoom transition = instant

        third->toggleAnimCam = true;       // free the camera from drawn-weapon anim cam
        third->freeRotationEnabled = true;
        third->freeRotation.x = kPi + f.rotation - 0.5f;  // face the player (2.64)
        third->freeRotation.y = 0.0f;
        third->pitchZoomOffset = 0.1f;     // distance independent of camera pitch
        third->posOffsetExpected = third->posOffsetActual =
            RE::NiPoint3{ sx, sy, sz };
        player->data.angle.x = (spiiLook ? 0.1f : 0.2f) + f.pitch +
                               (a_mounted ? s.ownViewMountPitch : 0.0f);
        camera->worldFOV = spiiLook ? g_cov.spiiFov : 90.0f;
        // Headtracking off while framed (SPIM's own move); the B-3 head pin
        // keeps the head sane when another mod re-drives tracking through
        // our graph tick.
        player->SetGraphVariableBool("IsNPC", false);
        // Park the vanilla menu-blur imod exactly like SPIM (r31 field:
        // edge blur on own-view arms; NG's TESImageSpaceModifier carries
        // the same members SPIM's build used - pointer swap on the radial
        // strength, in-place zero on the radius interpolator).
        g_state.blurParked = false;
        if (auto* blurMod =
                RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000434BB)) {
            g_state.radialBlurStrength = blurMod->radialBlur.strength;
            blurMod->radialBlur.strength = nullptr;
            if (blurMod->blurRadius) {
                g_state.blurRadiusValue = blurMod->blurRadius->floatValue;
                blurMod->blurRadius->floatValue = 0.0f;
            }
            g_state.blurParked = true;
        }
        camera->Update();
        g_state.active = true;
        // boom is logged because it is the one term with no INI to read back:
        // a field report of "zoomed in differently" is otherwise unfalsifiable.
        spdlog::info("own view: {} framing applied ({}; X={:.1f} Y={:.1f} Z={:.1f} "
                     "FOV={:.0f} boom={:.1f}).",
                     spiiLook ? "Show Player In Inventory" : "SPIM",
                     a_forcedThirdFromFirst ? "third person forced from first"
                                            : "unmanaged third-person arm",
                     sx, sy, sz, camera->worldFOV, boom);
    }

    void Disarm() {
        if (!g_state.active) {
            return;
        }
        g_state.active = false;
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* third = ThirdStateObject();
        if (!camera || !player || !third) {
            return;
        }
        // SPIM ResetCamera order: first person handed back FIRST (when we
        // forced the switch), then every original written back, one camera
        // update, and the mouse-wheel zoom speed only AFTER the update (the
        // update itself consumes the instant-transition value). Direct
        // SetState like SPIM - the request-based Force* path defers, and a
        // deferred switch is how r32's exit frames leaked.
        if (g_state.forcedThird) {
            // r52: back to whatever we left - first person for a foot arm,
            // the mount camera for a mounted arm (kFirstPerson was baked in
            // before and dismounted the view on exit).
            if (auto* prior = camera->cameraStates[g_state.savedStateId].get()) {
                camera->SetState(prior);
            }
        }
        player->data.angle.x = g_state.angleX;
        // r31 field fix: the exit transition bakes the preview camera yaw
        // into the player heading - put the entry heading back (SPIM does
        // the same, in this same spot of its ResetCamera order).
        player->data.angle.z = g_state.angleZ;
        third->toggleAnimCam = g_state.toggleAnimCam;
        third->freeRotationEnabled = g_state.freeRotationEnabled;
        third->targetZoomOffset = g_state.targetZoomOffset;
        third->pitchZoomOffset = g_state.pitchZoomOffset;
        third->freeRotation = g_state.freeRotation;
        third->posOffsetExpected = third->posOffsetActual = g_state.posOffsetExpected;
        camera->worldFOV = g_state.worldFOV;
        for (std::size_t i = 0; i < g_state.ini.size(); ++i) {
            if (i != kMouseWheelSlot && g_state.ini[i].setting) {
                g_state.ini[i].setting->data.f = g_state.ini[i].original;
            }
        }
        player->SetGraphVariableBool("IsNPC", g_state.headtrackingWasEnabled);
        RestoreBlurImod();
        camera->Update();
        if (auto* s = g_state.ini[kMouseWheelSlot].setting) {
            s->data.f = g_state.ini[kMouseWheelSlot].original;
        }
        // Exit-glide guard: the third-person interpolators would ease
        // current -> target over half a second after the restore - snap them.
        third->currentZoomOffset = third->targetZoomOffset;
        third->currentYaw = third->targetYaw;
        spdlog::info("own view: framing restored{}.",
                     g_state.forcedThird ? " (first person handed back)" : "");
    }

    void DropOnLoad() {
        if (!g_state.active) {
            return;
        }
        g_state.active = false;
        // Only the surfaces that survive a load: the INI Settings and the
        // camera/state singletons. Actor data, graph variables and the
        // camera state stack belong to the incoming save.
        for (auto& slot : g_state.ini) {
            if (slot.setting) {
                slot.setting->data.f = slot.original;
            }
        }
        if (auto* third = ThirdStateObject()) {
            third->toggleAnimCam = g_state.toggleAnimCam;
            third->freeRotationEnabled = g_state.freeRotationEnabled;
            third->targetZoomOffset = g_state.targetZoomOffset;
            third->pitchZoomOffset = g_state.pitchZoomOffset;
            third->freeRotation = g_state.freeRotation;
            third->posOffsetExpected = third->posOffsetActual = g_state.posOffsetExpected;
        }
        if (auto* camera = RE::PlayerCamera::GetSingleton()) {
            camera->worldFOV = g_state.worldFOV;
        }
        RestoreBlurImod();  // the imod form is process-global too
        spdlog::info("own view: globals restored on load (camera stack left to the save).");
    }

    bool Active() {
        return g_state.active;
    }

    bool SpimPresent() {
        RefreshCoverage();
        return g_cov.spim;
    }
}
