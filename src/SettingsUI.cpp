#include "PCH.h"

#include "SettingsUI.h"

#include "Settings.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callback typedefs

#include "FUCK_API.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {
    // iDeclutterMode order: combo index == INI value (pre-r17 mode 2 with the
    // stage on migrates to 3 at load). Short labels; the explanation is a
    // hover tooltip (FLICK style, like Feet of Skyrim).
    constexpr std::array<const char*, 4> kViewModes{
        "Off", "Scene view", "Void", "Dressing room",
    };
    constexpr std::array<const char*, 4> kViewModeTips{
        "Menus open over the world exactly as you left it.",
        "The room stays visible with its own lighting; only NPCs and furniture "
        "leave the set.",
        "Everything around you hides into a coloured void, lit by the studio rig. "
        "No stage.",
        "The stage builds around you in the void: floor, dome and set pieces, all "
        "following the mood.",
    };

    // Capitalize the first letter for DISPLAY only. Preset .name fields double
    // as the INI key (ApplyXPreset / the saved-value comparisons use them), so
    // the key stays lowercase; only what the combo shows is title-cased.
    std::string TitleFirst(const char* a_name) {
        std::string s = a_name ? a_name : "";
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
            s[0] = static_cast<char>(s[0] - 'a' + 'A');
        }
        return s;
    }

    // Tooltip for the item just drawn (shown only on hover).
    void Tip(const char* a_desc) {
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip(a_desc);
        }
    }

    void ColorRow(const char* a_label, RE::Color& a_color, bool& a_dirty) {
        float rgb[3]{ a_color.red / 255.0f, a_color.green / 255.0f, a_color.blue / 255.0f };
        if (FUCK::ColorEdit3(a_label, rgb)) {
            a_color.red   = static_cast<std::uint8_t>(rgb[0] * 255.0f + 0.5f);
            a_color.green = static_cast<std::uint8_t>(rgb[1] * 255.0f + 0.5f);
            a_color.blue  = static_cast<std::uint8_t>(rgb[2] * 255.0f + 0.5f);
            a_dirty = true;
        }
    }

    // The panel body. FLICK's Combo is the array form only (no SMF-style
    // callback), and the preset .name fields are const char*, so each preset
    // combo gathers its names into a temp vector.
    void DrawPanel() {
        auto& cfg   = MTB::Settings::GetSingleton();
        bool  dirty = false;
        const ImVec4 kWarn{ 0.95f, 0.75f, 0.25f, 1.0f };

        // The 3-point studio rig for the Void / Dressing room; drawn in the
        // Lighting section below.
        const auto drawRig = [&] {
            dirty |= FUCK::Checkbox("Studio rig (key / fill / rim lights)",
                                    &cfg.studioRig, false, false);
            Tip("A three-point photo-studio light setup on the character: a "
                "bright key, a soft fill and a rim light.");
            if (cfg.studioRig) {
                if (FUCK::SliderFloat("Rig brightness", &cfg.rigBrightness, 0.0f, 3.0f, "%.2f")) {
                    dirty = true;
                }
                Tip("Overall strength of the studio rig. 0 turns it off.");
                const auto rigRow = [&](const char* a_label,
                                        MTB::Settings::RigLight& a_light) {
                    FUCK::Text("%s", a_label);
                    FUCK::SameLine(90.0f, 0.0f);
                    std::string id = std::string("##en") + a_label;
                    dirty |= FUCK::Checkbox(id.c_str(), &a_light.enabled, false, false);
                    FUCK::SameLine(0.0f, 8.0f);
                    id = std::string("Color##") + a_label;
                    ColorRow(id.c_str(), a_light.color, dirty);
                    id = std::string("Intensity##") + a_label;
                    if (FUCK::SliderFloat(id.c_str(), &a_light.intensity, 0.0f, 3.0f, "%.2f")) {
                        dirty = true;
                    }
                };
                rigRow("Key", cfg.rigKey);
                rigRow("Fill", cfg.rigFill);
                rigRow("Rim", cfg.rigRim);
            }
        };

        dirty |= FUCK::Checkbox("Enable Menu Studio", &cfg.enabled, false, false);
        Tip("Master switch for the whole mod. When off, menus behave normally.");

        FUCK::SeparatorText("View");
        if (int mode = cfg.declutterMode;
            FUCK::Combo("Space around you", &mode, kViewModes.data(),
                        static_cast<int>(kViewModes.size()))) {
            cfg.declutterMode = mode;
            dirty = true;
        }
        Tip(kViewModeTips[cfg.declutterMode]);
        dirty |= FUCK::Checkbox("Spin the character", &cfg.previewSpin, false, false);
        Tip("Drag with the right mouse or push the right stick to turn the "
            "character. Hair and cloth react with real physics.");

        // Colour filter (any view, off by default): a uniform colour grade over
        // the whole menu scene. Shown for every view mode.
        FUCK::SeparatorText("Colour filter");
        dirty |= FUCK::Checkbox("Colour filter", &cfg.colorFilter, false, false);
        Tip("Washes the whole menu scene in a colour, like a photo filter. Works "
            "in any view. It tints everything in frame, your character included. "
            "Off by default.");
        if (cfg.colorFilter) {
            ColorRow("Filter colour", cfg.tintColor, dirty);
            Tip("The colour washed over the scene.");
            if (FUCK::SliderFloat("Strength", &cfg.tintStrength, 0.0f, 1.0f, "%.2f")) {
                dirty = true;
            }
            Tip("How strongly the colour washes the scene (lower is greyer).");
            if (FUCK::SliderFloat("Saturation", &cfg.tintSaturation, 0.0f, 1.0f, "%.2f")) {
                dirty = true;
            }
            Tip("Lower is more faded and desaturated.");
            if (FUCK::SliderFloat("Brightness", &cfg.tintBrightness, 0.0f, 1.5f, "%.2f")) {
                dirty = true;
            }
            Tip("Overall brightness of the filtered scene (1.0 leaves it as-is).");
        }

        FUCK::SeparatorText("Background");
        const auto backgrounds = MTB::Settings::BackgroundPresets();
        std::vector<std::string> bgDisplay;
        std::vector<const char*> bgNames;
        bgDisplay.reserve(backgrounds.size());
        bgNames.reserve(backgrounds.size());
        int bgIdx = -1;
        for (int i = 0; i < static_cast<int>(backgrounds.size()); ++i) {
            bgDisplay.push_back(TitleFirst(backgrounds[i].name));
            if (cfg.backdropBackground == backgrounds[i].name) {
                bgIdx = i;
            }
        }
        for (const auto& d : bgDisplay) {
            bgNames.push_back(d.c_str());
        }
        if (FUCK::Combo("Background", &bgIdx, bgNames.data(),
                        static_cast<int>(bgNames.size())) &&
            bgIdx >= 0) {
            cfg.ApplyBackgroundPreset(backgrounds[bgIdx].name);
            dirty = true;
        }
        Tip("The backdrop behind you: a flat void colour, a starfield dome, or "
            "your own image.");
        if (cfg.declutterMode < 2) {
            FUCK::TextColored(kWarn, "Only shows in the Void and the Dressing room.");
        }
        if (FUCK::SliderFloat("Size", &cfg.backdropDomeRadius, 256.0f, 8000.0f, "%.0f")) {
            dirty = true;
        }
        Tip("How far away the backdrop sits.");
        if (FUCK::SliderFloat("Height", &cfg.backdropDomeZ, -2048.0f, 2048.0f, "%.0f")) {
            dirty = true;
        }
        Tip("Raises or lowers the backdrop. On a star dome this is the framing "
            "dial for the nebula.");
        dirty |= FUCK::Checkbox("Lock background angle", &cfg.backgroundFaceCamera, false, false);
        Tip("Frames a custom image the same way no matter which way you were "
            "facing when the menu opened.");
        if (cfg.backgroundFaceCamera) {
            if (FUCK::SliderFloat("Background rotation", &cfg.backgroundYawOffset,
                                  -180.0f, 180.0f, "%.0f")) {
                dirty = true;
            }
            Tip("Turns the locked image left or right.");
        }
        dirty |= FUCK::Checkbox("Custom void color", &cfg.voidColorOverride, false, false);
        Tip("Overrides the mood and paints the void a colour you pick.");
        if (cfg.voidColorOverride) {
            ColorRow("Void color", cfg.voidColor, dirty);
        }

        if (cfg.declutterMode == 3) {
            FUCK::SeparatorText("Stage");
            const auto stages = MTB::Settings::StagePresets();
            std::vector<std::string> stageDisplay;
            std::vector<const char*> stageNames;
            stageDisplay.reserve(stages.size());
            stageNames.reserve(stages.size());
            int stageIdx = -1;
            for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
                stageDisplay.push_back(TitleFirst(stages[i].name));
                if (cfg.backdropStage == stages[i].name) {
                    stageIdx = i;
                }
            }
            for (const auto& d : stageDisplay) {
                stageNames.push_back(d.c_str());
            }
            if (FUCK::Combo("Stage", &stageIdx, stageNames.data(),
                            static_cast<int>(stageNames.size())) &&
                stageIdx >= 0) {
                cfg.ApplyStagePreset(stages[stageIdx].name);
                dirty = true;
            }
            Tip("The floor and set pieces you stand on in the dressing room.");
            if (FUCK::SliderFloat("Floor size", &cfg.backdropFloorRadius, 64.0f,
                                  2048.0f, "%.0f")) {
                dirty = true;
            }
            Tip("How wide the stage floor is.");
            if (FUCK::SliderFloat("Floor height", &cfg.backdropFloorZ, -256.0f,
                                  256.0f, "%.0f")) {
                dirty = true;
            }
            Tip("Raises or lowers the floor to meet your feet.");
        }

        FUCK::SeparatorText("Lighting");
        dirty |= FUCK::Checkbox("Match time of day and season", &cfg.matchTimeAndSeason, false, false);
        Tip("The mood follows the in-game clock and season by itself.");
        if (cfg.matchTimeAndSeason) {
            FUCK::TextDisabled("Now: %s.", cfg.DescribeTimeAndSeason().c_str());
        }
        const auto presets = MTB::Settings::LightPresets();
        std::vector<std::string> moodDisplay;
        std::vector<const char*> moodNames;
        moodDisplay.reserve(presets.size());
        moodNames.reserve(presets.size());
        int presetIdx = -1;
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            moodDisplay.push_back(TitleFirst(presets[i].name));
            if (cfg.lightPreset == presets[i].name) {
                presetIdx = i;
            }
        }
        for (const auto& d : moodDisplay) {
            moodNames.push_back(d.c_str());
        }
        if (FUCK::Combo("Mood", &presetIdx, moodNames.data(),
                        static_cast<int>(moodNames.size())) &&
            presetIdx >= 0) {
            cfg.ApplyLightPreset(presets[presetIdx].name);
            dirty = true;
        }
        Tip("The lighting look. Sets the colour of the void and the studio rig.");
        drawRig();

        if (dirty) {
            cfg.Save();
        }
    }

    // FLICK sidebar entry: the user opens FUCK (hotkey / controller menu) and
    // picks "Menu Studio".
    class SettingsTool : public FUCK::ITool {
    public:
        const char* Name() const override { return "Menu Studio"; }
        void        Draw() override { DrawPanel(); }
    };

    SettingsTool g_settingsTool;  // process-lifetime; registered pointer stays valid
}

namespace MTB::SettingsUI {
    void Register() {
        // Soft dependency: without FUCK.dll the mod stays INI-only with one log
        // line. The name passed here is what FLICK shows in its sidebar / the
        // registered-plugin panel, so it must read "Menu Studio".
        if (!FUCK::Connect("Menu Studio")) {
            spdlog::info("SettingsUI: FUCK / FLICK not present; INI-only mode.");
            return;
        }
        FUCK::RegisterTool(&g_settingsTool);
        spdlog::info("SettingsUI: settings registered as a FLICK (FUCK) sidebar tool.");
    }
}
