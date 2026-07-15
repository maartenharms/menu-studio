#include "PCH.h"

#include "Settings.h"

#include "BackdropPacks.h"

#include <SimpleIni.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {
    constexpr auto kIniPath = L"Data/SKSE/Plugins/MenuStudio.ini";

    // Named [Lighting] presets - background + three-point rig as one vibe.
    // "studio" = the field-proven neutrals; "bright" carries the player
    // alone in light-poor cells; warm/cool/dusk trade fill for mood (dim
    // fill = harder shadows, hot rim = stronger silhouette).
    constexpr std::array<MTB::LightPreset, 5> kLightPresets{ {
        { "studio", { 96, 96, 100, 0 }, { 160, 155, 150, 0 }, { 13, 13, 15, 0 }, { 80, 80, 84, 0 },
          { { 255, 242, 222, 0 }, 1.0f }, { { 184, 199, 230, 0 }, 1.0f }, { { 255, 255, 255, 0 }, 1.0f } },
        { "bright", { 148, 148, 152, 0 }, { 255, 250, 242, 0 }, { 16, 16, 19, 0 }, { 132, 132, 138, 0 },
          { { 255, 250, 240, 0 }, 1.3f }, { { 235, 240, 250, 0 }, 1.1f }, { { 255, 255, 255, 0 }, 1.2f } },
        { "warm",   { 105, 88, 70, 0 }, { 200, 160, 110, 0 }, { 18, 12, 8, 0 },  { 96, 78, 60, 0 },
          { { 255, 190, 120, 0 }, 1.15f }, { { 200, 150, 100, 0 }, 0.7f }, { { 255, 220, 180, 0 }, 1.0f } },
        { "cool",   { 78, 88, 105, 0 }, { 140, 160, 200, 0 }, { 10, 12, 18, 0 }, { 70, 80, 96, 0 },
          { { 200, 220, 255, 0 }, 1.0f }, { { 120, 150, 220, 0 }, 0.65f }, { { 220, 235, 255, 0 }, 1.4f } },
        { "dusk",   { 70, 60, 80, 0 },  { 180, 120, 90, 0 },  { 14, 10, 16, 0 }, { 76, 64, 84, 0 },
          { { 255, 160, 90, 0 }, 1.2f }, { { 150, 110, 200, 0 }, 0.7f }, { { 255, 200, 150, 0 }, 1.3f } },
    } };

    const MTB::LightPreset* FindPreset(std::string_view a_name) {
        for (const auto& preset : kLightPresets) {
            if (a_name == preset.name) {
                return &preset;
            }
        }
        return nullptr;
    }

    // Case-insensitive equality of a saved name against a preset key. Built-in
    // keys are lowercase, but pack names keep the author's casing (e.g. "Example
    // Nebula"), so preset lookups must ignore case for a saved selection to
    // resolve back to its pack.
    bool NameMatches(std::string_view a_name, const char* a_key) {
        std::size_t i = 0;
        for (; i < a_name.size(); ++i) {
            const unsigned char k = static_cast<unsigned char>(a_key[i]);
            if (k == '\0') {
                return false;  // key shorter than a_name
            }
            if (std::tolower(static_cast<unsigned char>(a_name[i])) != std::tolower(k)) {
                return false;
            }
        }
        return a_key[i] == '\0';  // both ended together
    }

    const MTB::StagePreset* FindStage(std::string_view a_name) {
        for (const auto& stage : MTB::BackdropPacks::Stages()) {
            if (NameMatches(a_name, stage.name)) {
                return &stage;
            }
        }
        return nullptr;
    }

    const MTB::BackgroundPreset* FindBackground(std::string_view a_name) {
        for (const auto& bg : MTB::BackdropPacks::Backgrounds()) {
            if (NameMatches(a_name, bg.name)) {
                return &bg;
            }
        }
        return nullptr;
    }

    // Time-of-day clock mapping: base preset by hour…
    const char* TimePresetName(float a_hour) {
        if (a_hour < 5.0f) return "cool";    // deep night
        if (a_hour < 7.0f) return "dusk";    // dawn glow
        if (a_hour < 17.0f) return "studio"; // day
        if (a_hour < 19.0f) return "dusk";   // sunset
        if (a_hour < 22.0f) return "warm";   // candlelit evening
        return "cool";                       // night
    }
    // …tinted toward a season preset. Skyrim months: Morning Star=0 …
    // Evening Star=11.
    struct SeasonBlend {
        const char* name;
        const char* preset;  // nullptr = no tint
        float       t;
    };
    SeasonBlend SeasonFor(std::uint32_t a_month) {
        switch (a_month) {
        case 2: case 3: case 4:
            return { "spring", nullptr, 0.0f };
        case 5: case 6: case 7:
            return { "summer", "bright", 0.20f };
        case 8: case 9: case 10:
            return { "autumn", "warm", 0.20f };
        default:  // Sun's Dusk, Evening Star, Morning Star, Sun's Dawn
            return { "winter", "cool", 0.25f };
        }
    }

    float LerpF(float a_from, float a_to, float a_t) {
        return a_from + (a_to - a_from) * a_t;
    }
    RE::Color LerpColor(const RE::Color& a_from, const RE::Color& a_to, float a_t) {
        RE::Color out;
        out.red   = static_cast<std::uint8_t>(LerpF(a_from.red, a_to.red, a_t) + 0.5f);
        out.green = static_cast<std::uint8_t>(LerpF(a_from.green, a_to.green, a_t) + 0.5f);
        out.blue  = static_cast<std::uint8_t>(LerpF(a_from.blue, a_to.blue, a_t) + 0.5f);
        return out;
    }

    // "R,G,B" (0-255) -> Color; leaves a_out untouched on parse failure.
    void ParseColor(const CSimpleIniA& a_ini, const char* a_key, RE::Color& a_out,
                    const char* a_section = "Lighting") {
        const char* raw = a_ini.GetValue(a_section, a_key, nullptr);
        if (!raw || !*raw) {
            return;
        }
        int r = 0, g = 0, b = 0;
        if (std::sscanf(raw, " %d , %d , %d", &r, &g, &b) == 3) {
            a_out.red   = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            a_out.green = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
            a_out.blue  = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
        } else {
            spdlog::warn("Settings: [{}] {} = '{}' is not R,G,B - ignored.", a_section, a_key, raw);
        }
    }
}

namespace MTB {
    Settings& Settings::GetSingleton() {
        static Settings instance;
        return instance;
    }

    std::span<const LightPreset> Settings::LightPresets() {
        return kLightPresets;
    }

    std::span<const StagePreset> Settings::StagePresets() {
        return BackdropPacks::Stages();
    }

    std::span<const BackgroundPreset> Settings::BackgroundPresets() {
        return BackdropPacks::Backgrounds();
    }

    bool Settings::ApplyStagePreset(std::string_view a_name) {
        const auto* stage = FindStage(a_name);
        if (!stage) {
            return false;
        }
        backdropFloorMesh   = stage->floorMesh;
        backdropFloorRadius = stage->floorRadius;
        backdropFloorZ      = stage->floorZ;
        backdropStage       = stage->name;
        return true;
    }

    bool Settings::ApplyBackgroundPreset(std::string_view a_name) {
        const auto* bg = FindBackground(a_name);
        if (!bg) {
            return false;
        }
        backdropDomeMesh        = bg->mesh;
        backdropDomeRadius      = bg->radius;
        backdropDomeZ           = bg->z;
        backdropBackground      = bg->name;
        backdropBackgroundImage = bg->image ? bg->image : "";
        backgroundFaceCamera    = bg->faceCamera;
        backgroundYawOffset     = bg->yaw;
        return true;
    }

    std::span<const StagePiece> Settings::ActiveStageExtras() const {
        const auto* stage = FindStage(backdropStage);
        return stage ? stage->extras : std::span<const StagePiece>{};
    }

    bool Settings::ApplyLightPreset(std::string_view a_name) {
        for (const auto& preset : kLightPresets) {
            if (a_name == preset.name) {
                lightAmbient     = preset.ambient;
                lightDirectional = preset.directional;
                lightFog         = preset.fog;
                lightFill        = preset.fill;
                // The rig is part of the vibe: colors + intensities follow
                // the preset; the enable flags stay the user's layout call.
                rigKey.color      = preset.key.color;
                rigKey.intensity  = preset.key.intensity;
                rigFill.color     = preset.fillLight.color;
                rigFill.intensity = preset.fillLight.intensity;
                rigRim.color      = preset.rim.color;
                rigRim.intensity  = preset.rim.intensity;
                lightPreset       = preset.name;
                return true;
            }
        }
        return false;
    }

    Settings::LookValues Settings::CurrentLook() const {
        LookValues look{ lightAmbient, lightDirectional, lightFog, lightFill,
                       rigKey, rigFill, rigRim };
        if (!matchTimeAndSeason) {
            return look;
        }
        auto* calendar = RE::Calendar::GetSingleton();
        const auto* base = calendar ? FindPreset(TimePresetName(calendar->GetHour())) : nullptr;
        if (!base) {
            return look;
        }
        look.ambient     = base->ambient;
        look.directional = base->directional;
        look.fog         = base->fog;
        look.fill        = base->fill;
        look.key.color       = base->key.color;
        look.key.intensity   = base->key.intensity;
        look.fillLight.color     = base->fillLight.color;
        look.fillLight.intensity = base->fillLight.intensity;
        look.rim.color       = base->rim.color;
        look.rim.intensity   = base->rim.intensity;

        const auto season = SeasonFor(calendar->GetMonth());
        if (const auto* tint = season.preset ? FindPreset(season.preset) : nullptr) {
            const float t = season.t;
            look.ambient     = LerpColor(look.ambient, tint->ambient, t);
            look.directional = LerpColor(look.directional, tint->directional, t);
            look.fog         = LerpColor(look.fog, tint->fog, t);
            look.fill        = LerpColor(look.fill, tint->fill, t);
            look.key.color       = LerpColor(look.key.color, tint->key.color, t);
            look.key.intensity   = LerpF(look.key.intensity, tint->key.intensity, t);
            look.fillLight.color     = LerpColor(look.fillLight.color, tint->fillLight.color, t);
            look.fillLight.intensity = LerpF(look.fillLight.intensity, tint->fillLight.intensity, t);
            look.rim.color       = LerpColor(look.rim.color, tint->rim.color, t);
            look.rim.intensity   = LerpF(look.rim.intensity, tint->rim.intensity, t);
        }
        return look;
    }

    Settings::TintValues Settings::CurrentTint() const {
        return { tintColor, tintStrength, tintSaturation, tintBrightness };
    }

    std::string Settings::DescribeTimeAndSeason() const {
        auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return "no calendar";
        }
        const float hour = calendar->GetHour();
        const auto season = SeasonFor(calendar->GetMonth());
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%02d:%02d %s: %s%s%s",
                      static_cast<int>(hour),
                      static_cast<int>((hour - static_cast<int>(hour)) * 60.0f),
                      calendar->GetMonthName().c_str(), TimePresetName(hour),
                      season.preset ? " + " : "", season.preset ? season.name : "");
        return buf;
    }

    void Settings::Load() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(kIniPath) < 0) {
            spdlog::info("No MenuTimeBubble.ini - using defaults.");
        }

        enabled       = ini.GetBoolValue("General", "bEnabled", enabled);
        tickAnimation = ini.GetBoolValue("General", "bTickAnimation", tickAnimation);
        driveSmp      = ini.GetBoolValue("General", "bDriveSmp", driveSmp);
        tickFace      = ini.GetBoolValue("General", "bTickFace", tickFace);
        tickMagicCasters = ini.GetBoolValue("General", "bTickMagicCasters", tickMagicCasters);
        forcePause    = ini.GetBoolValue("General", "bForcePause", forcePause);
        blockRightMouse = ini.GetBoolValue("General", "bBlockRightMouse", blockRightMouse);
        bypassCameraCollision =
            ini.GetBoolValue("General", "bBypassCameraCollision", bypassCameraCollision);
        standardizeLighting =
            ini.GetBoolValue("Declutter", "bStandardizeLighting", standardizeLighting);
        verboseLog    = ini.GetBoolValue("General", "bVerboseLog", verboseLog);
        maxDeltaTime  = static_cast<float>(
            ini.GetDoubleValue("General", "fMaxDeltaTime", maxDeltaTime));
        if (maxDeltaTime < 0.001f || maxDeltaTime > 0.5f) {
            maxDeltaTime = 0.05f;
        }

        declutterMode = static_cast<int>(
            ini.GetLongValue("Declutter", "iDeclutterMode", declutterMode));
        // r17 split the old dressing room (2) into void (2) and stage (3):
        // a pre-r17 INI that had the stage enabled means mode 3 now.
        if (declutterMode == 2 && ini.GetBoolValue("Backdrop", "bBackdrop", false)) {
            declutterMode = 3;
        }
        declutterMode = std::clamp(declutterMode, 0, 3);  // 0 Off .. 3 Dressing room
        soloHideRadius = static_cast<float>(
            ini.GetDoubleValue("Declutter", "fSoloHideRadius", soloHideRadius));
        hideLightRefs = ini.GetBoolValue("Declutter", "bHideLightRefs", hideLightRefs);
        cutCellLights = ini.GetBoolValue("Declutter", "bCutCellLights", cutCellLights);
        voidEngine    = ini.GetBoolValue("Declutter", "bVoidEngine", voidEngine);
        if (const char* raw = ini.GetValue("General", "sFreezeGraphBools", nullptr);
            raw && *raw) {
            freezeGraphBools.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    freezeGraphBools.push_back(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }
        driveCbpc     = ini.GetBoolValue("General", "bDriveCbpc", driveCbpc);
        idleInMenus   = ini.GetBoolValue("General", "bIdleInMenus", idleInMenus);
        freezeHeadTracking =
            ini.GetBoolValue("General", "bFreezeHeadTracking", freezeHeadTracking);
        pinBodyHeading = ini.GetBoolValue("General", "bPinBodyHeading", pinBodyHeading);
        neutralExpression = ini.GetBoolValue("General", "bNeutralExpression", neutralExpression);
        previewSpin = ini.GetBoolValue("General", "bPreviewSpin", previewSpin);
        spinSensitivity = static_cast<float>(
            ini.GetDoubleValue("General", "fSpinSensitivity", spinSensitivity));
        spinSensitivity = std::clamp(spinSensitivity, 0.0005f, 0.05f);
        spinGamepadButton = static_cast<int>(
            ini.GetLongValue("General", "iSpinGamepadButton", spinGamepadButton));
        spinStickSensitivity = static_cast<float>(
            ini.GetDoubleValue("General", "fSpinStickSensitivity", spinStickSensitivity));
        spinStickSensitivity = std::clamp(spinStickSensitivity, 0.5f, 10.0f);
        showTryOnPrompt = ini.GetBoolValue("Preview", "bShowTryOnPrompt", showTryOnPrompt);
        tryOnLabelKbd   = ini.GetValue("Preview", "sTryOnLabelKbd", tryOnLabelKbd.c_str());
        tryOnLabelPad   = ini.GetValue("Preview", "sTryOnLabelPad", tryOnLabelPad.c_str());
        ownViewFirstPerson = ini.GetBoolValue("General", "bOwnViewFirstPerson",
                                              ownViewFirstPerson);
        ownViewUnmanaged = ini.GetBoolValue("General", "bOwnViewUnmanaged",
                                            ownViewUnmanaged);
        ownViewXOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewXOffset", ownViewXOffset));
        ownViewYOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewYOffset", ownViewYOffset));
        ownViewZOffset = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewZOffset", ownViewZOffset));
        ownViewPitch = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewPitch", ownViewPitch));
        ownViewRotation = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewRotation", ownViewRotation));
        ownViewMountRaise = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountRaise", ownViewMountRaise));
        ownViewMountBoom = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountBoom", ownViewMountBoom));
        ownViewMountPitch = static_cast<float>(
            ini.GetDoubleValue("General", "fOwnViewMountPitch", ownViewMountPitch));
        ownViewYOffset = std::clamp(ownViewYOffset, -200.0f, 140.0f);  // boom stays positive
        sleekTransitions = ini.GetBoolValue("General", "bSleekTransitions", sleekTransitions);
        transitionInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fTransitionIn", transitionInSeconds));
        transitionOutSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fTransitionOut", transitionOutSeconds));
        transitionInSeconds  = std::clamp(transitionInSeconds, 0.0f, 1.0f);
        transitionOutSeconds = std::clamp(transitionOutSeconds, 0.0f, 1.0f);
        dipToBlack = ini.GetBoolValue("General", "bDipToBlack", dipToBlack);
        dipOutSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fDipOutSeconds", dipOutSeconds));
        dipInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fDipInSeconds", dipInSeconds));
        sleekExit = ini.GetBoolValue("General", "bSleekExit", sleekExit);
        exitHoldSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitHoldSeconds", exitHoldSeconds));
        exitDipSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitDipSeconds", exitDipSeconds));
        exitInSeconds = static_cast<float>(
            ini.GetDoubleValue("General", "fExitInSeconds", exitInSeconds));
        // r48: 0 = cut at close (the default); anything above holds the
        // studio through menu switches. The fader durations are retired
        // (r47) - parsed for INI compat, consumed by nothing.
        exitHoldSeconds = std::clamp(exitHoldSeconds, 0.0f, 0.30f);
        exitDipSeconds  = std::clamp(exitDipSeconds, 0.05f, 0.50f);
        exitInSeconds   = std::clamp(exitInSeconds, 0.05f, 1.0f);
        dipOutSeconds = std::clamp(dipOutSeconds, 0.0f, 0.50f);
        dipInSeconds  = std::clamp(dipInSeconds, 0.05f, 1.00f);
        soloHideRadius = std::clamp(soloHideRadius, 512.0f, 16384.0f);

        // [Backdrop]: stage preset first, then per-key overrides on top
        // (the same layering as [Lighting]); an explicitly empty mesh
        // value drops that piece.
        // Discover backdrop packs so the saved sStage/sBackground can resolve
        // against them (and the menu can list them). No bubble menu is open here.
        BackdropPacks::Scan();
        if (const char* raw = ini.GetValue("Backdrop", "sStage", nullptr); raw && *raw) {
            backdropStage = raw;
            std::transform(backdropStage.begin(), backdropStage.end(), backdropStage.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!ApplyStagePreset(backdropStage)) {
            spdlog::warn("Settings: [Backdrop] sStage='{}' unknown - using starlight.",
                         backdropStage);
            ApplyStagePreset("starlight");
        }
        if (const char* raw = ini.GetValue("Backdrop", "sBackground", nullptr); raw && *raw) {
            backdropBackground = raw;
            std::transform(backdropBackground.begin(), backdropBackground.end(),
                           backdropBackground.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!ApplyBackgroundPreset(backdropBackground)) {
            spdlog::warn("Settings: [Backdrop] sBackground='{}' unknown (blank/"
                         "constellation) - using constellation.", backdropBackground);
            ApplyBackgroundPreset("constellation");
        }
        if (const char* raw = ini.GetValue("Backdrop", "sFloorMesh", nullptr); raw) {
            backdropFloorMesh = raw;
        }
        if (const char* raw = ini.GetValue("Backdrop", "sDomeMesh", nullptr); raw) {
            backdropDomeMesh = raw;
        }
        if (const char* raw = ini.GetValue("Backdrop", "sStarsMesh", nullptr); raw) {
            backdropStarsMesh = raw;  // empty string = stars off
        }
        if (const char* raw = ini.GetValue("Backdrop", "sShellMesh", nullptr); raw) {
            backdropShellMesh = raw;
        }
        // F-7 v3 migration: every pre-r31 shell candidate is field-convicted
        // (loadscreen sphere = inner wall; vampire dome = seams + per-cell
        // color + fake constellation) - a stale value in a panel-saved
        // overwrite INI must not pin the old look past the fix. Genuine
        // custom paths (anything not on this list) are honored.
        {
            std::string lowered = backdropShellMesh;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* stale :
                 { "dlc01\\interface\\intvampireperkskydome.nif",
                   "loadscreenart\\loadscreensphere.nif",
                   "architecture\\solitude\\interiors\\slgcdome01.nif",
                   "architecture\\markarth\\mrktempledome01.nif" }) {
                if (lowered == stale) {
                    spdlog::info("Settings: sShellMesh '{}' is a retired shell - "
                                 "migrated to the shipped voidshell.", backdropShellMesh);
                    backdropShellMesh = "mtb\\voidshell.nif";
                    break;
                }
            }
        }
        backdropFloorRadius = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fFloorRadius", backdropFloorRadius));
        backdropDomeRadius = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fDomeRadius", backdropDomeRadius));
        backdropFloorZ = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fFloorZ", backdropFloorZ));
        backdropDomeZ = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fDomeZ", backdropDomeZ));
        backdropBrightness = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fBrightness", backdropBrightness));
        voidBrightnessCap = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fVoidBrightnessCap", voidBrightnessCap));
        voidBrightnessCap = std::clamp(voidBrightnessCap, 0.02f, 0.35f);
        backgroundFaceCamera =
            ini.GetBoolValue("Backdrop", "bLockBackgroundAngle", backgroundFaceCamera);
        backgroundYawOffset = static_cast<float>(
            ini.GetDoubleValue("Backdrop", "fBackgroundYaw", backgroundYawOffset));
        backgroundYawOffset = std::clamp(backgroundYawOffset, -180.0f, 180.0f);
        backdropFloorRadius = std::clamp(backdropFloorRadius, 64.0f, 4096.0f);
        backdropDomeRadius  = std::clamp(backdropDomeRadius, 256.0f, 12000.0f);
        backdropFloorZ      = std::clamp(backdropFloorZ, -512.0f, 512.0f);
        backdropDomeZ       = std::clamp(backdropDomeZ, -4096.0f, 4096.0f);
        backdropBrightness  = std::clamp(backdropBrightness, 0.0f, 4.0f);

        // [Lighting]: preset first, then per-channel overrides on top.
        if (const char* raw = ini.GetValue("Lighting", "sPreset", nullptr); raw && *raw) {
            lightPreset = raw;
            std::transform(lightPreset.begin(), lightPreset.end(), lightPreset.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!ApplyLightPreset(lightPreset)) {
            spdlog::warn("Settings: [Lighting] sPreset='{}' unknown (studio/bright/warm/cool/"
                         "dusk) - using studio values.", lightPreset);
            ApplyLightPreset("studio");
        }
        ParseColor(ini, "rAmbient", lightAmbient);
        ParseColor(ini, "rDirectional", lightDirectional);
        ParseColor(ini, "rFog", lightFog);
        ParseColor(ini, "rFill", lightFill);
        voidColorOverride =
            ini.GetBoolValue("Lighting", "bVoidColorOverride", voidColorOverride);
        ParseColor(ini, "rVoidColor", voidColor);

        // [Filter] - optional colour grade over any view (off by default).
        colorFilter = ini.GetBoolValue("Filter", "bColorFilter", colorFilter);
        ParseColor(ini, "rTintColor", tintColor, "Filter");
        tintStrength = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fTintStrength", tintStrength)), 0.0f, 1.0f);
        tintSaturation = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fSaturation", tintSaturation)), 0.0f, 1.0f);
        tintBrightness = std::clamp(static_cast<float>(
            ini.GetDoubleValue("Filter", "fBrightness", tintBrightness)), 0.0f, 1.5f);
        lightFogNear = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fFogNear", lightFogNear));
        lightFogFar = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fFogFar", lightFogFar));
        lightFogNear = std::clamp(lightFogNear, 256.0f, 20000.0f);
        lightFogFar  = std::clamp(lightFogFar, lightFogNear, 40000.0f);
        studioRig = ini.GetBoolValue("Lighting", "bStudioRig", studioRig);
        rigBrightness = static_cast<float>(
            ini.GetDoubleValue("Lighting", "fRigBrightness", rigBrightness));
        rigBrightness = std::clamp(rigBrightness, 0.0f, 4.0f);
        const auto loadRigLight = [&](const char* a_prefix, RigLight& a_light) {
            std::string key = std::string("b") + a_prefix;
            a_light.enabled = ini.GetBoolValue("Lighting", key.c_str(), a_light.enabled);
            key = std::string("r") + a_prefix + "Color";
            ParseColor(ini, key.c_str(), a_light.color);
            key = std::string("f") + a_prefix + "Intensity";
            a_light.intensity = static_cast<float>(
                ini.GetDoubleValue("Lighting", key.c_str(), a_light.intensity));
            a_light.intensity = std::clamp(a_light.intensity, 0.0f, 4.0f);
        };
        loadRigLight("RigKey", rigKey);
        loadRigLight("RigFill", rigFill);
        loadRigLight("RigRim", rigRim);
        matchTimeAndSeason = ini.GetBoolValue("Lighting", "bMatchTimeAndSeason", matchTimeAndSeason);

        if (const char* raw = ini.GetValue("General", "sMenus", nullptr); raw && *raw) {
            menus.clear();
            std::string list{ raw };
            std::size_t pos = 0;
            while (pos <= list.size()) {
                auto comma = list.find(',', pos);
                if (comma == std::string::npos) {
                    comma = list.size();
                }
                auto item = list.substr(pos, comma - pos);
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    menus.insert(item.substr(first, last - first + 1));
                }
                pos = comma + 1;
            }
        }

        // r27 stabilization: the retired SPIM-harvest rotation key and the
        // black fade-in stay DISABLED IN CODE (the fader dip is parked on
        // the r26 input-capture suspicion). Forced after the INI read so a
        // stale panel-saved INI in MO2's overwrite can't re-enable them.
        // Rotation returned in r29 as bPreviewSpin - our OWN input sink,
        // opt-in, not this key.
        if (pinBodyHeading || dipToBlack || transitionInSeconds > 0.0f) {
            spdlog::info("Settings: bPinBodyHeading/bDipToBlack/fTransitionIn are "
                         "retired or parked in this build (values ignored).");
        }
        pinBodyHeading = false;
        dipToBlack = false;
        transitionInSeconds = 0.0f;

        ++revision;

        std::string names;
        for (const auto& m : menus) {
            names += names.empty() ? m : ", " + m;
        }
        spdlog::info("Settings: enabled={} forcePause={} tickAnimation={} driveSmp={} tickFace={} "
                     "tickMagic={} maxDt={:.3f} menus=[{}] space: mode={} solo(r={:.0f}) "
                     "bg={} stage={} (floor '{}' r={:.0f} z={:.0f}, dome '{}' r={:.0f} z={:.0f}) "
                     "light: preset={} amb=({},{},{}) fog=({},{},{})",
                     enabled, forcePause, tickAnimation, driveSmp, tickFace, tickMagicCasters,
                     maxDeltaTime, names, declutterMode, soloHideRadius, backdropBackground,
                     backdropStage,
                     backdropFloorMesh, backdropFloorRadius, backdropFloorZ, backdropDomeMesh,
                     backdropDomeRadius, backdropDomeZ, lightPreset,
                     lightAmbient.red, lightAmbient.green, lightAmbient.blue,
                     lightFog.red, lightFog.green, lightFog.blue);
    }

    void Settings::Save() {
        // Load-modify-save: only the panel-edited keys change; every other
        // key and the shipped comment blocks survive the round-trip.
        CSimpleIniA ini;
        ini.SetUnicode();
        ini.LoadFile(kIniPath);

        ini.SetBoolValue("General", "bEnabled", enabled);
        ini.SetBoolValue("General", "bForcePause", forcePause);
        ini.SetBoolValue("General", "bTickAnimation", tickAnimation);
        ini.SetBoolValue("General", "bTickFace", tickFace);
        ini.SetBoolValue("General", "bTickMagicCasters", tickMagicCasters);
        ini.SetBoolValue("General", "bIdleInMenus", idleInMenus);
        ini.SetBoolValue("General", "bFreezeHeadTracking", freezeHeadTracking);
        ini.SetBoolValue("General", "bNeutralExpression", neutralExpression);
        ini.SetBoolValue("General", "bPreviewSpin", previewSpin);
        ini.SetBoolValue("Preview", "bShowTryOnPrompt", showTryOnPrompt);
        ini.SetValue("Preview", "sTryOnLabelKbd", tryOnLabelKbd.c_str());
        ini.SetValue("Preview", "sTryOnLabelPad", tryOnLabelPad.c_str());
        ini.SetBoolValue("General", "bOwnViewFirstPerson", ownViewFirstPerson);
        ini.SetBoolValue("General", "bOwnViewUnmanaged", ownViewUnmanaged);
        ini.SetBoolValue("General", "bSleekTransitions", sleekTransitions);
        ini.SetDoubleValue("General", "fTransitionIn", transitionInSeconds);
        ini.SetDoubleValue("General", "fTransitionOut", transitionOutSeconds);
        ini.SetBoolValue("General", "bDipToBlack", dipToBlack);
        ini.SetBoolValue("General", "bSleekExit", sleekExit);
        ini.SetDoubleValue("General", "fExitHoldSeconds", exitHoldSeconds);
        ini.SetDoubleValue("General", "fExitDipSeconds", exitDipSeconds);
        ini.SetDoubleValue("General", "fExitInSeconds", exitInSeconds);
        ini.SetBoolValue("General", "bDriveSmp", driveSmp);
        ini.SetBoolValue("General", "bDriveCbpc", driveCbpc);
        ini.SetBoolValue("General", "bBlockRightMouse", blockRightMouse);
        ini.SetBoolValue("General", "bBypassCameraCollision", bypassCameraCollision);
        ini.SetLongValue("Declutter", "iDeclutterMode", declutterMode);
        ini.SetBoolValue("Declutter", "bStandardizeLighting", standardizeLighting);
        ini.SetBoolValue("Declutter", "bCutCellLights", cutCellLights);
        ini.SetBoolValue("Declutter", "bVoidEngine", voidEngine);
        {
            std::string joined;
            for (const auto& var : freezeGraphBools) {
                joined += joined.empty() ? var : ", " + var;
            }
            ini.SetValue("General", "sFreezeGraphBools", joined.c_str());
        }
        // bBackdrop retired in r17 (the stage is view mode 3 now); drop the
        // stale key so the 2→3 migration can't re-trigger.
        ini.Delete("Backdrop", "bBackdrop");
        ini.SetValue("Backdrop", "sStage", backdropStage.c_str());
        ini.SetValue("Backdrop", "sBackground", backdropBackground.c_str());
        // Backdrop keys carry only values differing from the active
        // presets (floor keys vs the stage, dome keys vs the background) -
        // a clean pick leaves the INI clean (the [Lighting] policy).
        const auto* activeStage = FindStage(backdropStage);
        const auto* activeBg = FindBackground(backdropBackground);
        const auto diffString = [&](const char* a_key, const std::string& a_value,
                                    const char* a_presetValue) {
            if (a_presetValue && a_value == a_presetValue) {
                ini.Delete("Backdrop", a_key);
            } else {
                ini.SetValue("Backdrop", a_key, a_value.c_str());
            }
        };
        const auto diffFloat = [&](const char* a_key, float a_value, bool a_hasPreset,
                                   float a_presetValue) {
            if (a_hasPreset && std::abs(a_value - a_presetValue) < 0.05f) {
                ini.Delete("Backdrop", a_key);
            } else {
                ini.SetDoubleValue("Backdrop", a_key, a_value);
            }
        };
        diffString("sFloorMesh", backdropFloorMesh,
                   activeStage ? activeStage->floorMesh : nullptr);
        diffString("sDomeMesh", backdropDomeMesh, activeBg ? activeBg->mesh : nullptr);
        diffFloat("fFloorRadius", backdropFloorRadius, activeStage != nullptr,
                  activeStage ? activeStage->floorRadius : 0.0f);
        diffFloat("fDomeRadius", backdropDomeRadius, activeBg != nullptr,
                  activeBg ? activeBg->radius : 0.0f);
        diffFloat("fFloorZ", backdropFloorZ, activeStage != nullptr,
                  activeStage ? activeStage->floorZ : 0.0f);
        diffFloat("fDomeZ", backdropDomeZ, activeBg != nullptr,
                  activeBg ? activeBg->z : 0.0f);
        ini.SetDoubleValue("Backdrop", "fBrightness", backdropBrightness);
        ini.SetBoolValue("Backdrop", "bLockBackgroundAngle", backgroundFaceCamera);
        ini.SetDoubleValue("Backdrop", "fBackgroundYaw", backgroundYawOffset);
        ini.SetValue("Lighting", "sPreset", lightPreset.c_str());
        ini.SetDoubleValue("Lighting", "fFogNear", lightFogNear);
        ini.SetDoubleValue("Lighting", "fFogFar", lightFogFar);
        ini.SetBoolValue("Lighting", "bStudioRig", studioRig);
        ini.SetDoubleValue("Lighting", "fRigBrightness", rigBrightness);
        ini.SetBoolValue("Lighting", "bMatchTimeAndSeason", matchTimeAndSeason);

        // Override keys carry only channels that differ from the active
        // preset - a clean preset pick leaves the INI clean.
        const LightPreset* active = nullptr;
        for (const auto& preset : kLightPresets) {
            if (lightPreset == preset.name) {
                active = &preset;
                break;
            }
        }

        const auto saveRigLight = [&](const char* a_prefix, const RigLight& a_light,
                                      const LightPreset::RigLook* a_presetLook) {
            std::string key = std::string("b") + a_prefix;
            ini.SetBoolValue("Lighting", key.c_str(), a_light.enabled);
            key = std::string("r") + a_prefix + "Color";
            if (a_presetLook && a_light.color == a_presetLook->color) {
                ini.Delete("Lighting", key.c_str());
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u,%u,%u", a_light.color.red,
                              a_light.color.green, a_light.color.blue);
                ini.SetValue("Lighting", key.c_str(), buf);
            }
            key = std::string("f") + a_prefix + "Intensity";
            if (a_presetLook && std::abs(a_light.intensity - a_presetLook->intensity) < 0.005f) {
                ini.Delete("Lighting", key.c_str());
            } else {
                ini.SetDoubleValue("Lighting", key.c_str(), a_light.intensity);
            }
        };
        saveRigLight("RigKey", rigKey, active ? &active->key : nullptr);
        saveRigLight("RigFill", rigFill, active ? &active->fillLight : nullptr);
        saveRigLight("RigRim", rigRim, active ? &active->rim : nullptr);
        const auto writeOverride = [&](const char* a_key, const RE::Color& a_value,
                                       const RE::Color& a_presetValue) {
            if (active && a_value == a_presetValue) {
                ini.Delete("Lighting", a_key);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u,%u,%u", a_value.red, a_value.green,
                              a_value.blue);
                ini.SetValue("Lighting", a_key, buf);
            }
        };
        writeOverride("rAmbient", lightAmbient, active ? active->ambient : lightAmbient);
        writeOverride("rDirectional", lightDirectional,
                      active ? active->directional : lightDirectional);
        writeOverride("rFog", lightFog, active ? active->fog : lightFog);
        writeOverride("rFill", lightFill, active ? active->fill : lightFill);
        ini.SetBoolValue("Lighting", "bVoidColorOverride", voidColorOverride);
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u,%u,%u", voidColor.red,
                          voidColor.green, voidColor.blue);
            ini.SetValue("Lighting", "rVoidColor", buf);
        }

        ini.SetBoolValue("Filter", "bColorFilter", colorFilter);
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u,%u,%u", tintColor.red,
                          tintColor.green, tintColor.blue);
            ini.SetValue("Filter", "rTintColor", buf);
        }
        ini.SetDoubleValue("Filter", "fTintStrength", tintStrength);
        ini.SetDoubleValue("Filter", "fSaturation", tintSaturation);
        ini.SetDoubleValue("Filter", "fBrightness", tintBrightness);

        if (ini.SaveFile(kIniPath) < 0) {
            spdlog::warn("Settings: could not write MenuStudio.ini.");
        } else {
            spdlog::info("Settings saved (panel).");
        }
        ++revision;
    }
}
