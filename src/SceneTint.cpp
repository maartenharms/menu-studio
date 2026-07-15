#include "PCH.h"

#include "SceneTint.h"

#include "Settings.h"

namespace {
    // WHY the override, not form edits: writing a TESImageSpace form's data does
    // NOTHING while a menu is paused - the ImageSpaceManager applies a CACHED
    // base each render and only re-reads the forms on a weather/cell change,
    // which rides Sky::Update, and that never runs while paused (field-proven,
    // ENB on AND off). The manager's base-data POINTER, however, is consulted on
    // EVERY rendered frame (rendering runs under pause), so pointing it at our
    // own buffer paints the whole screen live. Offsets from CommonLib-NG's
    // ImageSpaceManager (this build's CommonLib doesn't wrap it):
    //   singleton RELOCATION_ID(527731, 414660); currentBaseData +0xA8;
    //   overrideBaseData +0xB0; sizeof == 0x220.
    //
    // We drive `currentBaseData`, NOT `overrideBaseData`. Both are live per
    // frame (the world tints either way), but a NON-NULL overrideBaseData trips
    // the menu render path and the native menu UI vanishes (field: world tinted,
    // inventory UI gone). Repointing currentBaseData at our buffer tints the
    // world just the same while leaving overrideBaseData null, which the menu
    // pipeline expects - UI intact.
    constexpr std::ptrdiff_t kCurrentBaseOff = 0xA8;

    RE::ImageSpaceBaseData  g_tintData{};              // our base buffer (process-lifetime)
    RE::ImageSpaceBaseData* g_savedCurrent = nullptr;  // the manager's real currentBaseData pointer
    bool                    g_applied = false;

    std::uint8_t* ImageSpaceMgr() {
        REL::Relocation<std::uint8_t**> singleton{ RELOCATION_ID(527731, 414660) };
        return *singleton;  // the ImageSpaceManager instance
    }
    RE::ImageSpaceBaseData** CurrentSlot(std::uint8_t* a_mgr) {
        return reinterpret_cast<RE::ImageSpaceBaseData**>(a_mgr + kCurrentBaseOff);
    }

    void WriteTint(RE::ImageSpaceBaseData& a_d, const MTB::Settings::TintValues& a_t) {
        // Colour wash (TNAM). ImageSpace tint colour is a 0..1 ColorF.
        a_d.tint.amount      = a_t.strength;
        a_d.tint.color.red   = a_t.color.red / 255.0f;
        a_d.tint.color.green = a_t.color.green / 255.0f;
        a_d.tint.color.blue  = a_t.color.blue / 255.0f;
        // Faded, dimmable look (CNAM). Contrast left neutral.
        a_d.cinematic.saturation = a_t.saturation;
        a_d.cinematic.brightness = a_t.brightness;
        a_d.cinematic.contrast   = 1.0f;
        // Freeze eye-adaptation: a paused menu lets the exposure drift over the
        // tinted scene, which can blow / crush the whole composite. Pin the speed
        // so the exposure holds at its pre-arm value for the whole menu.
        a_d.hdr.eyeAdaptSpeed = 0.0f;
    }
}

namespace MTB::SceneTint {
    void Apply(const Settings::TintValues& a_tint) {
        auto* mgr = ImageSpaceMgr();
        if (!mgr) {
            spdlog::warn("scenetint: ImageSpaceManager singleton null - cannot filter.");
            return;
        }
        auto** slot = CurrentSlot(mgr);
        if (!g_applied) {
            g_savedCurrent = *slot;  // the manager's real current base pointer
            // Seed from the LIVE current base so HDR / bloom / adaptation stay
            // sane (a zeroed base blows exposure); only tint/saturation/brightness
            // + the adaptation freeze are overwritten on top.
            g_tintData = g_savedCurrent ? *g_savedCurrent : RE::ImageSpaceBaseData{};
            g_applied = true;
        }
        WriteTint(g_tintData, a_tint);
        *slot = &g_tintData;  // manager reads our tinted base every frame (override left null)
        spdlog::info(
            "scenetint: colour filter installed on mgr {:p} (tint {:.2f} "
            "({:.2f},{:.2f},{:.2f}) sat {:.2f} bright {:.2f}; saved base {:p}).",
            static_cast<void*>(mgr), a_tint.strength, a_tint.color.red / 255.0f,
            a_tint.color.green / 255.0f, a_tint.color.blue / 255.0f, a_tint.saturation,
            a_tint.brightness, static_cast<void*>(g_savedCurrent));
    }

    void LiveRefresh() {
        if (!g_applied) {
            return;
        }
        WriteTint(g_tintData, Settings::GetSingleton().CurrentTint());
        if (auto* mgr = ImageSpaceMgr()) {
            *CurrentSlot(mgr) = &g_tintData;  // keep it pointed across the arm
        }
        spdlog::debug("scenetint: live refresh.");
    }

    void Restore() {
        if (!g_applied) {
            return;
        }
        if (auto* mgr = ImageSpaceMgr()) {
            *CurrentSlot(mgr) = g_savedCurrent;  // hand the manager's own base back
        }
        g_applied = false;
        spdlog::debug("scenetint: colour filter restored.");
    }

    void Sync(bool a_enabled, const Settings::TintValues& a_tint) {
        if (a_enabled) {
            if (!g_applied) {
                Apply(a_tint);
            } else {
                LiveRefresh();
            }
        } else {
            Restore();
        }
    }
}
