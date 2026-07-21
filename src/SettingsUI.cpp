#include "PCH.h"

#include "SettingsUI.h"

#include "Settings.h"

#include "BackdropPacks.h"
#include "OwnView.h"

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
        "The room stays where it is and only the crowd and the furniture clear out.",
        "Everything around you drops away into a lit void.",
        "A stage builds itself around you, floor and dome and set pieces included.",
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

    // Skyrim Souls RE loaded? Its unpaused menus are the only reason the
    // force-pause control matters, so the panel only surfaces that control when
    // Souls is actually present. Checked once - a plugin cannot load mid-session.
    bool SkyrimSoulsPresent() {
        static const bool present = GetModuleHandleA("SkyrimSoulsRE.dll") != nullptr;
        return present;
    }

    // §4b: display order and labels for the per-menu Souls split. The order is
    // fixed here because Settings::menus is an unordered_set and a column of
    // checkboxes that reshuffles between frames is unusable. The ### suffix
    // keeps each checkbox's ImGui ID unique and stable while the visible text
    // stays short; FUCK sizes a label on the part in FRONT of the ###, so the
    // long id costs no width (see [[fuck-tab-item-sizing]]).
    constexpr std::array<std::pair<const char*, const char*>, 4> kSoulsMenuLabels{ {
        { "InventoryMenu", "Inventory###soulsLiveInventoryMenu" },
        { "BarterMenu", "Barter###soulsLiveBarterMenu" },
        { "ContainerMenu", "Container###soulsLiveContainerMenu" },
        { "MagicMenu", "Magic###soulsLiveMagicMenu" },
    } };

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
                                    &cfg.studioRig);
            Tip("Key, fill and rim lights on your character, like a photo studio.");
            if (cfg.studioRig) {
                dirty |= FUCK::Checkbox("Also light me in Off / Scene view",
                                        &cfg.rigWithoutSpace);
                Tip("Studio lights on your character with the world still behind you, "
                    "though anything standing close catches them too.");
                if (FUCK::SliderFloat("Rig brightness", &cfg.rigBrightness, 0.0f, 3.0f, "%.2f")) {
                    dirty = true;
                }
                Tip("Overall strength of the studio rig. 0 turns it off.");
                const auto rigRow = [&](const char* a_label,
                                        MTB::Settings::RigLight& a_light) {
                    FUCK::Text("%s", a_label);
                    FUCK::SameLine(90.0f, 0.0f);
                    std::string id = std::string("##en") + a_label;
                    // The ONE checkbox that keeps the manual args. This row
                    // hand-places a hidden-label tick between its own Text and
                    // the colour swatch, so alignFar would fling it to the far
                    // right and break the row. Every LABELLED checkbox in this
                    // panel now takes FLICK's defaults (alignFar + labelLeft)
                    // instead, which is what lines them up with the sliders.
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

        // Pulled out of the Colour filter section (r28b) so the Souls-live
        // panel can show the same controls without duplicating them. The
        // filter is a screen grade and hides nothing, so it is safe with the
        // world running for the same reason the rig is. Caller draws the
        // SeparatorText, since the two placements want different headings.
        const auto drawColorFilter = [&] {
            dirty |= FUCK::Checkbox("Colour filter", &cfg.colorFilter);
            Tip("Washes the whole shot in a colour, your character included.");
            if (!cfg.colorFilter) {
                return;
            }
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
        };

        dirty |= FUCK::Checkbox("Enable Menu Studio", &cfg.enabled);
        Tip("Master switch for the whole mod. When off, menus behave normally.");

        // Skyrim Souls RE runs these menus unpaused; Menu Studio re-pauses them so
        // the studio has a frozen scene to work with. Offer the opt-out only when
        // Souls is present, framed as "keep them unpaused" - the inverse of the
        // stored forcePause. Checked hands the menus back to Souls, and with no
        // pause the whole studio is inert, so the options below hide.
        const bool souls = SkyrimSoulsPresent();

        // TABS. The page had grown into one long scroll - view modes, then the
        // per-menu space list, then the character rows, then the filter, the
        // background, the stage and finally the lighting rig - and finding any
        // one control meant reading past all the others. Feet of Skyrim's panel
        // solves the same problem the same way.
        //
        // Each section is a lambda here and a tab at the bottom. Defined in
        // source order, drawn in tab order: the two are deliberately not the
        // same, so nothing has to move to be re-grouped.
        const auto drawSouls = [&] {
            bool keepUnpaused = !cfg.forcePause;
            if (FUCK::Checkbox("Keep ALL these menus unpaused (Skyrim Souls)",
                               &keepUnpaused)) {
                cfg.forcePause = !keepUnpaused;
                // r28f: NEVER SILENT. A field session loaded with
                // forcePause=false and opened its menu with it true, and the
                // log could not say who flipped it - this checkbox, a stray
                // click, or a bug - because the click did not log. One line
                // makes the next such log self-explaining.
                spdlog::info("Panel: 'Keep ALL these menus unpaused' clicked -> "
                             "forcePause={} (checkbox now {}).",
                             cfg.forcePause, keepUnpaused ? "TICKED" : "UNTICKED");
                dirty = true;
            }
            Tip("Hands these menus back to Skyrim Souls and keeps them live, with no "
                "studio and nothing frozen to pose against.");

            // §4b per-menu split. The studio needs a frozen scene and Souls
            // exists to keep menus live, so no single menu can be both - but
            // that is a per-menu conflict, not a global one, and this is where
            // it stops being all-or-nothing. Only worth showing while the
            // studio is on at all; when the master is checked every menu is
            // already live and there is nothing to divide.
            if (cfg.forcePause) {
                FUCK::TextDisabled("Or hand individual menus back to Souls:");
                // Fixed order, not set order: the set is unordered, and a list
                // of checkboxes that reshuffles between frames is unusable.
                for (const auto& [key, label] : kSoulsMenuLabels) {
                    if (!cfg.IsBubbleMenu(key)) {
                        continue;
                    }
                    bool live = cfg.soulsLiveMenus.contains(key);
                    if (FUCK::Checkbox(label, &live)) {
                        if (live) {
                            cfg.soulsLiveMenus.insert(key);
                        } else {
                            cfg.soulsLiveMenus.erase(key);
                        }
                        dirty = true;
                    }
                }
                // Every menu handed over is the same end state as the master
                // checkbox, reached a different way. Say so rather than leaving
                // a panel full of studio options that cannot apply anywhere,
                // but do NOT return: unchecking one of these is how they come
                // back, so the list has to stay reachable.
                std::size_t live = 0;
                for (const auto& m : cfg.menus) {
                    if (cfg.soulsLiveMenus.contains(m)) {
                        ++live;
                    }
                }
                if (!cfg.menus.empty() && live == cfg.menus.size()) {
                    FUCK::TextColored(kWarn,
                                      "Every menu is live, so the studio has nowhere to run.");
                }
            }
        };

        // Without a pause the studio cannot arm, so under Souls + keep-unpaused
        // most of the panel has nothing to configure. It used to say so and
        // RETURN, which cost a Souls user the whole page; now it says so once
        // above the tabs and the tabs that cannot apply simply do not appear.
        const bool studioInert = souls && !cfg.forcePause;
        if (studioInert) {
            // r28: this used to be the end of the panel. A Souls user who kept
            // their menus live lost the whole page, including the three-point
            // rig, which never needed the pause in the first place. The staging
            // half genuinely does need a still scene, so say which half is gone
            // rather than "the studio is off", and keep the half that works.
            FUCK::TextColored(kWarn,
                              "Skyrim Souls is keeping these menus live, so the void, "
                              "the backdrop and the live-physics posing are off.");
            FUCK::TextDisabled(
                "Untick \"Keep ALL these menus unpaused\" on the Skyrim Souls tab "
                "to pose in a frozen scene.");
            // ⚠ r28h: NO CHECKBOX for the live lighting. It had one, the field
            // turned it off while hunting for the missing lights, and the mod
            // then declined every menu in silence - a whole round lost to a
            // control that only existed to switch off the thing being tested.
            //
            // The user's standing instruction on this feature was "don't even
            // make them toggles, it works so just let it work with no settings
            // for it", and this is the second time ignoring that has cost a
            // round. bStudioInLiveMenus survives as an undocumented INI escape
            // hatch for a load order we have not seen; it is not a preference.
        }

        // CHARACTER - what your character does while a menu is up.
        const auto drawCharacter = [&] {
            dirty |= FUCK::Checkbox("Spin the character", &cfg.previewSpin);
            Tip("Turn your character with the right mouse or the right stick, hair "
                "and cloth swinging as they go.");
            // Only meaningful with SPIM loaded - it is the one mod known to
            // rotate on the same right-drag. Hidden otherwise, like the Souls
            // toggle.
            if (cfg.previewSpin && MTB::OwnView::SpimPresent()) {
                dirty |= FUCK::Checkbox("Override Show Player In Menus rotation",
                                        &cfg.overrideSpimRotation);
                Tip("Stops Show Player In Menus turning your character too, so one "
                    "drag no longer spins you twice as far.");
            }
            dirty |= FUCK::Checkbox("Always freeze the character", &cfg.freezeCharacter);
            // "Always" is load-bearing, not decoration: Menu Studio already
            // freezes on its own whenever a caught pose cannot settle (mid-air,
            // mid-attack, furniture, and a locomotion graph that will not leave
            // its walk clip). A plain "Freeze the character" read as "this is
            // the only time freezing happens", which is not true.
            Tip("Holds the caught frame in every menu, instead of only when a "
                "pose cannot settle on its own.");
            // The two per-case freeze toggles live on the Experimental tab. They
            // were here, under a one-line caveat that could not be worded both
            // unambiguously and short enough to survive the panel width. A tab
            // whose NAME is the caveat solves it without any caption at all.
            // "Show my weapon in hand" was here and is GONE (user, 2026-07-21:
            // "it doesn't do anything and i don't think we should have it
            // anyways"). The bWeaponPreviewInMenus key still exists and still
            // works for anyone who has set it - only the control is withdrawn,
            // so nobody's saved INI changes meaning under them.
        };

        // SCENE - the space around you, and what fills it.
        // EXPERIMENTAL - settings that trade a known-good behaviour for a
        // livelier one, and can look wrong. The tab name is the warning, which
        // is why nothing in here needs a caption explaining itself.
        const auto drawExperimental = [&] {
            FUCK::TextDisabled("These trade a reliable pose for a livelier one.");
            FUCK::SeparatorText("Freezing");
            dirty |= FUCK::Checkbox("Freeze poses that cannot settle",
                                    &cfg.freezeUnsettledPose);
            Tip("On, your character holds still when they are moving or turning "
                "and will not stop on their own. Off, they keep animating and "
                "can walk or step in the menu.");
            dirty |= FUCK::Checkbox("Freeze while drawing or sheathing",
                                    &cfg.freezeDrawSheathe);
            Tip("On, a menu opened part way through drawing or putting away a "
                "weapon holds that frame. Off, the animation finishes and can "
                "leave the wrong stance.");
            if (cfg.freezeCharacter) {
                FUCK::TextColored(kWarn, "\"Always freeze the character\" is on, so "
                                         "these do nothing.");
            }
        };

        const auto drawScene = [&] {
        // The CONFIGURED mode: while a menu that opted out of the space is
        // open, cfg.declutterMode is a temporary 0, so the panel must edit and
        // display declutterModeIni or it would fight the per-menu resolve.
        if (int mode = cfg.declutterModeIni;
            FUCK::Combo("Space around you", &mode, kViewModes.data(),
                        static_cast<int>(kViewModes.size()))) {
            cfg.declutterModeIni = mode;
            cfg.declutterMode    = mode;  // live for the menu already open
            dirty = true;
        }
        Tip(kViewModeTips[cfg.declutterModeIni]);
        // Per-menu space (NymerethRole): keep the backdrop for your own
        // character menus and drop it where you are looking at someone else's
        // things. Only the space is affected - the pause, the physics and the
        // live character still apply in every menu the mod covers.
        if (cfg.declutterModeIni != 0) {
            const auto spaceMenuRow = [&](const char* a_label, const char* a_menu,
                                          const char* a_tip) {
                bool on = cfg.spaceMenus.contains(a_menu);
                if (FUCK::Checkbox(a_label, &on)) {
                    if (on) {
                        cfg.spaceMenus.insert(a_menu);
                    } else {
                        cfg.spaceMenus.erase(a_menu);
                    }
                    dirty = true;
                }
                Tip(a_tip);
            };
            FUCK::TextDisabled("%s", "Show that space in:");
            spaceMenuRow("Inventory", "InventoryMenu",
                         "Your own inventory - the usual place to look at your character.");
            spaceMenuRow("Magic", "MagicMenu", "Your own magic menu.");
            spaceMenuRow("Container", "ContainerMenu",
                         "Chests, and looting followers or bodies. Turn this off if you "
                         "only want the backdrop for your own menus.");
            spaceMenuRow("Barter", "BarterMenu",
                         "Trading with merchants. Turn this off if you only want the "
                         "backdrop for your own menus.");
        }
        // NO PANEL ROW for the weapon-swap stance fix. It is correct behaviour,
        // not a preference: without it the character holds a new weapon in the
        // old weapon's stance, which nobody would choose. bLiveEquipNotifyInMenus
        // stays in the INI as an escape hatch for a load order we have not seen,
        // deliberately undocumented so it does not read as a supported choice.
        dirty |= FUCK::Checkbox("Camera ignores walls", &cfg.bypassCameraCollision);
        Tip("The camera slips through walls instead of shoving in close, which "
            "reads better in the void than in a real room.");

        // The colour filter moved to the Lighting tab - it is a look, not a
        // piece of the scene, and it belongs beside the mood and the rig.

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
        if (bgIdx >= 0) {
            const auto author = MTB::BackdropPacks::AuthorOf(backgrounds[bgIdx].name);
            if (!author.empty()) {
                Tip(("Backdrop pack by " + std::string{ author } + ".").c_str());
            } else {
                Tip("The backdrop behind you: a flat void colour, a starfield dome, or "
                    "your own image.");
            }
        } else {
            Tip("The backdrop behind you: a flat void colour, a starfield dome, or "
                "your own image.");
        }
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
        Tip("Raises or lowers the backdrop, which is how you frame the nebula on "
            "a star dome.");
        dirty |= FUCK::Checkbox("Lock background angle", &cfg.backgroundFaceCamera);
        Tip("Frames a custom image the same way no matter which way you were "
            "facing when the menu opened.");
        if (cfg.backgroundFaceCamera) {
            if (FUCK::SliderFloat("Background rotation", &cfg.backgroundYawOffset,
                                  -180.0f, 180.0f, "%.0f")) {
                dirty = true;
            }
            Tip("Turns the locked image left or right.");
        }
        dirty |= FUCK::Checkbox("Custom void color", &cfg.voidColorOverride);
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
            if (stageIdx >= 0) {
                const auto author = MTB::BackdropPacks::AuthorOf(stages[stageIdx].name);
                if (!author.empty()) {
                    Tip(("Stage from a backdrop pack by " + std::string{ author } + ".").c_str());
                } else {
                    Tip("The floor and set pieces you stand on in the dressing room.");
                }
            } else {
                Tip("The floor and set pieces you stand on in the dressing room.");
            }
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
        };

        // LIGHTING - the mood, the rig and the colour grade over the whole shot.
        const auto drawLighting = [&] {
        dirty |= FUCK::Checkbox("Match time of day and season", &cfg.matchTimeAndSeason);
        Tip("Lets the clock and the season pick the mood for you, which is why "
            "the choices below go quiet.");
        if (cfg.matchTimeAndSeason) {
            FUCK::TextDisabled("Now: %s.", cfg.DescribeTimeAndSeason().c_str());
            FUCK::TextDisabled(
                "The clock is choosing the mood and the rig - turn the box off "
                "to hand-tune them.");
        }
        // Everything from the mood picker down is COMPUTED while the clock
        // drives the look (CurrentLook ignores the manual fields), so editing
        // it would do nothing - the exact bad UX this disable exists to stop.
        FUCK::BeginDisabled(cfg.matchTimeAndSeason);
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
        FUCK::EndDisabled();
        // A screen grade, so it is safe with the world running for the same
        // reason the rig is: it hides nothing. That is also why both survive on
        // the Souls-live page while everything needing a frozen scene does not.
        FUCK::SeparatorText("Colour filter");
        drawColorFilter();
        };

        // THE TABS. Order is what a user reaches for most first: their
        // character, then the space around them, then how it is lit. The Souls
        // tab only exists when Souls does, and the two tabs that need a frozen
        // scene disappear when there is not going to be one - rather than
        // showing controls that silently cannot apply (r28h).
        if (FUCK::BeginTabBar("##MenuStudio")) {
            if (!studioInert && FUCK::BeginTabItem("Character")) {
                drawCharacter();
                FUCK::EndTabItem();
            }
            if (!studioInert && FUCK::BeginTabItem("Scene")) {
                drawScene();
                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("Lighting")) {
                drawLighting();
                FUCK::EndTabItem();
            }
            if (souls && FUCK::BeginTabItem("Skyrim Souls")) {
                drawSouls();
                FUCK::EndTabItem();
            }
            // Last on purpose: nobody should land on it by accident.
            if (!studioInert && FUCK::BeginTabItem("Experimental")) {
                drawExperimental();
                FUCK::EndTabItem();
            }
            FUCK::EndTabBar();
        }

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
            // ⚠ LOUD ON PURPOSE. FUCK/FLICK has no build for the mid 1.6
            // runtimes, so users there get a mod with NO VISIBLE SETTINGS and
            // no way to tell whether that is the intended fallback or a broken
            // install. Everything still works and every setting is in the INI -
            // this line is the only place that says so, so it says it in full
            // and at warn level rather than hiding in the info stream.
            spdlog::warn("SettingsUI: FLICK (FUCK) is not present, so Menu Studio has NO IN-GAME "
                         "SETTINGS PANEL on this setup. This is a supported fallback, not a "
                         "failure: every setting is available in "
                         "Data/SKSE/Plugins/MenuStudio.ini and is re-read each time you load a "
                         "save. FLICK has no build for the mid 1.6 runtimes (1.6.317-1.6.1129).");
            return;
        }
        FUCK::RegisterTool(&g_settingsTool);
        spdlog::info("SettingsUI: settings registered as a FLICK (FUCK) sidebar tool.");
    }
}
