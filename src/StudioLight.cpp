#include "PCH.h"

#include "StudioLight.h"

#include "Offsets.h"
#include "Settings.h"

namespace {
    RE::FormID g_cellID = 0;
    RE::INTERIOR_DATA g_saved{};
    RE::FormID g_imageSpaceID = 0;
    RE::ImageSpaceBaseData g_savedImage{};
    RE::Sky::Mode g_savedSkyMode = RE::Sky::Mode::kNone;
    bool g_skyParked = false;

    using SkyRefresh_t = void(__fastcall*)(RE::Sky*, float);
    REL::Relocation<SkyRefresh_t> g_skyForceInteriorRefresh{
        MTB::Offsets::SkyForceInteriorRefresh
    };

    // The renderer never reads INTERIOR_DATA directly: the Sky ingests it
    // (blended with the lighting template) on cell ATTACH and caches the
    // result - form edits alone change nothing on screen (field evidence:
    // barebones sessions 0355/0407/0422). This drives the engine's own
    // forced re-ingest so edits (and restores) take effect immediately.
    void ReingestCellLighting() {
        auto* sky = RE::Sky::GetSingleton();
        if (!sky) {
            return;
        }
        const auto skyAddr = reinterpret_cast<std::uintptr_t>(sky);
        // Refresh throttle timer at Sky+0x1D4: the wrapper only fires once
        // the accumulated interval passes - max it for a deterministic run.
        *reinterpret_cast<float*>(skyAddr + 0x1D4) = 1.0e9f;
        // Second gate: the wrapper silently does NOTHING while
        // TESWaterSystem+0xB8 is nonzero (round-5 field sessions showed all
        // our calls "succeeding" with zero visual effect). Clear it around
        // the call and restore.
        REL::Relocation<RE::TESWaterSystem**> waterSingleton{
            MTB::Offsets::TESWaterSystemSingleton
        };
        auto* water = reinterpret_cast<std::uint8_t*>(*waterSingleton.get());
        std::uint8_t gate = water ? water[0xB8] : 0;
        if (water && gate) {
            water[0xB8] = 0;
        }
        // Ingest proof: one of the Sky's cell-fed color slots, before/after.
        const auto* slot = reinterpret_cast<const float*>(skyAddr + 0xB4);
        const float b0 = slot[0], b1 = slot[1], b2 = slot[2];
        g_skyForceInteriorRefresh(sky, 0.0f);
        spdlog::debug(
            "studio light: sky re-ingest forced (gate byte {}, slotB4 "
            "({:.3f},{:.3f},{:.3f}) -> ({:.3f},{:.3f},{:.3f})).",
            gate, b0, b1, b2, slot[0], slot[1], slot[2]);
        if (water && gate) {
            water[0xB8] = gate;
        }
    }
}

namespace {
    // Values from the effective look: manual preset + overrides, or the
    // auto time-of-day/season pick (Settings::CurrentLook decides).
    void WriteLook(RE::INTERIOR_DATA* a_lighting) {
        const auto& cfg = MTB::Settings::GetSingleton();
        const auto look = cfg.CurrentLook();
        a_lighting->ambient = look.ambient;
        a_lighting->directional = look.directional;  // rotation fields kept as-is
        a_lighting->fogColorNear = look.fog;
        a_lighting->fogColorFar = look.fog;
        a_lighting->fogNear = cfg.lightFogNear;
        a_lighting->fogFar = cfg.lightFogFar;
        a_lighting->fogPower = 1.0f;
        a_lighting->fogClamp = 1.0f;
        auto& dalc = a_lighting->directionalAmbientLightingColors;
        dalc.directional.x.max = look.fill;
        dalc.directional.x.min = look.fill;
        dalc.directional.y.max = look.fill;
        dalc.directional.y.min = look.fill;
        dalc.directional.z.max = look.fill;
        dalc.directional.z.min = look.fill;
    }
}

namespace MTB::StudioLight {
    void Apply() {
        const auto& cfg = Settings::GetSingleton();
        if (!cfg.standardizeLighting) {
            return;
        }
        // Void (2) and dressing room (3) only: Off / Scene view keep the cell's
        // own look - standardized studio light there reads as a bug. (The colour
        // filter is a separate, view-independent post-process, not this.)
        if (!cfg.IsVoidFamily()) {
            spdlog::debug("studio light: skipped - space mode {} keeps the cell's own look.",
                          cfg.declutterMode);
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        // r44 REWRITE of the F-7 park. The r42 discovery: Sky::mode is
        // consumed by Sky::Update, which never runs in a paused menu - the
        // park was RENDER-INERT in every armed menu ever; the sky's
        // disappearance is owned by the void engine's 'Sky'/'Weather'
        // branch culls now. What the mode VALUE still drives is the
        // interior FOG-INGEST chain (r26, slotB4-proven: kInterior keeps
        // INTERIOR_DATA fog alive, kNone kills it) - so INTERIORS still
        // park to kInterior for the fog, and EXTERIORS never touch the
        // mode again: nothing to park means the r40 weather-audio stop
        // latch is structurally impossible outdoors.
        if (!g_skyParked && cell && cell->IsInteriorCell()) {
            if (auto* sky = RE::Sky::GetSingleton()) {
                g_savedSkyMode = sky->mode.get();
                sky->mode = RE::Sky::Mode::kInterior;
                g_skyParked = true;
                spdlog::debug("studio light: sky mode parked to kInterior for "
                              "fog ingest (was mode {}).",
                              static_cast<int>(g_savedSkyMode));
            }
        }
        if (g_cellID != 0) {
            return;  // interior override already applied this arm
        }
        auto* lighting = cell && cell->IsInteriorCell() ? cell->GetLighting() : nullptr;
        if (!lighting) {
            // Exteriors have no INTERIOR_DATA to standardize - the parked
            // sky + the dome carry the look out there.
            return;
        }

        g_saved = *lighting;
        g_cellID = cell->GetFormID();

        // Cells can INHERIT channels from their lighting template (LTMP) -
        // the renderer then ignores the per-cell values entirely for those
        // channels. Field case: barebones 'Editor Smoke Test Cell' inherits
        // fog/ambient, so the spike's override never showed there (green
        // template murk instead of the void). Clear the inherit bits for
        // exactly the channels we set; the whole-struct save restores them.
        using Inherit = RE::INTERIOR_DATA::Inherit;
        lighting->lightingTemplateInheritanceFlags.reset(
            Inherit::kAmbientColor, Inherit::kDirectionalColor, Inherit::kFogColor,
            Inherit::kFogNear, Inherit::kFogFar, Inherit::kFogPower, Inherit::kFogMax);

        WriteLook(lighting);

        if (cfg.matchTimeAndSeason) {
            spdlog::debug("studio light: applied time-and-season look ({}) to cell {:08X} ('{}').",
                          cfg.DescribeTimeAndSeason(), g_cellID, cell->GetName());
        } else {
            spdlog::debug("studio light: applied '{}' to cell {:08X} ('{}').",
                          cfg.lightPreset, g_cellID, cell->GetName());
        }

        // The cell's IMAGESPACE post-processes the whole frame AFTER the
        // lighting above: cinematic tint, saturation/contrast, HDR eye
        // adaptation. On vanilla lighting stacks it dominates the void -
        // a green-tinted, auto-exposing imagespace crushes any fog color
        // into the same murk (field case: barebones 'Editor Smoke Test
        // Cell'). Neutralize it in place while armed; same save/restore
        // lifecycle as the cell lighting.
        auto* xImg = cell->extraList.GetByType<RE::ExtraCellImageSpace>();
        if (auto* img = xImg ? xImg->imageSpace : nullptr) {
            g_savedImage = img->data;
            g_imageSpaceID = img->GetFormID();
            auto& d = img->data;
            spdlog::debug(
                "studio light: neutralizing imagespace {:08X} (was: tint {:.2f} "
                "({:.2f},{:.2f},{:.2f}) sat {:.2f} bright {:.2f} contrast {:.2f} "
                "adaptStrength {:.2f} adaptSpeed {:.2f} white {:.2f}) - adaptation "
                "SPEED frozen at 0 while armed (exposure pinned pre-arm).",
                g_imageSpaceID, d.tint.amount, d.tint.color.red, d.tint.color.green,
                d.tint.color.blue, d.cinematic.saturation, d.cinematic.brightness,
                d.cinematic.contrast, d.hdr.eyeAdaptStrength, d.hdr.eyeAdaptSpeed,
                d.hdr.white);
            d.tint.amount = 0.0f;
            d.cinematic.saturation = 1.0f;
            d.cinematic.brightness = 1.0f;
            d.cinematic.contrast = 1.0f;
            // HDR adaptation STRENGTH stays untouched (r27: zeroing it
            // blew ENB exposure to white). r32 pins the adaptation SPEED
            // instead: the r31 Dragonsreach log proved everything else
            // neutral (tint 0, sat/bright/contrast 1, fog ingesting
            // charcoal, shell up) yet the void whited - that's the
            // auto-exposure DRIFTING up over the culled-dark studio
            // (adaptStrength 1.0 there, ENB amplifies). Speed 0 freezes
            // the running exposure at its pre-arm, world-lit value for
            // the whole menu; the whole-struct restore lets it resume.
            d.hdr.eyeAdaptSpeed = 0.0f;
        } else {
            // No XCIM: the engine's default imagespace applies and stays
            // untouched. If the void still reads tinted in such a cell,
            // that's the evidence for a manager-level override next round.
            spdlog::debug("studio light: cell {:08X} has no imagespace of its own - "
                          "tone left to the engine default.", g_cellID);
        }

        // r38 Dragonsreach-day-white evidence line (field: "still really
        // white during the day"): one correlatable row per arm - if the
        // white tracks the HOUR with identical imagespace/fog/adapt values,
        // the writer is ENB-side (its own day-interior adaptation/mist,
        // outside game data) and the next lever is the field A/B with
        // fVoidBrightnessCap, not another form write.
        {
            const auto* cal = RE::Calendar::GetSingleton();
            const float hour = cal ? cal->GetHour() : -1.0f;
            spdlog::info("void diag: cell='{}' interior={} hour={:.2f} xcim={} "
                         "adaptSpeed(was)={:.2f} adaptStrength={:.2f}",
                         cell->GetName() ? cell->GetName() : "?",
                         cell->IsInteriorCell() ? 1 : 0, hour,
                         g_imageSpaceID != 0 ? 1 : 0,
                         g_imageSpaceID != 0 ? g_savedImage.hdr.eyeAdaptSpeed : -1.0f,
                         g_imageSpaceID != 0 ? g_savedImage.hdr.eyeAdaptStrength : -1.0f);
        }

        ReingestCellLighting();
    }

    void LiveRefresh() {
        // Mid-arm settings change (panel edit / INI reload): rewrite the
        // preset values into the already-overridden cell and re-ingest.
        // The saved backup from Apply stays authoritative for Restore.
        if (g_cellID == 0) {
            return;
        }
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_cellID);
        auto* lighting = cell ? cell->GetLighting() : nullptr;
        if (!lighting) {
            return;
        }
        WriteLook(lighting);
        ReingestCellLighting();
        spdlog::debug("studio light: live refresh ('{}').",
                      Settings::GetSingleton().lightPreset);
    }

    // r40 (field: "rain sfx stops when i exit the menu"): the sleek exit
    // runs UNPAUSED frames between the close and the at-black restore -
    // Sky::Update ticks there with the parked mode (kNone outdoors),
    // treats the weather as gone and STOPS the rain loop, which only
    // re-triggers on a precipitation TRANSITION. Restoring the mode later
    // brings the visuals back but never the sound. So the sky mode alone
    // goes back AT THE CLOSE EDGE: visually free (the opaque shell keeps
    // occluding the sky through hold + dip), and the weather audio never
    // sees a kNone frame. The full Restore below skips the mode when this
    // already ran (g_skyParked guard); a menu-switch re-open re-parks.
    void RestoreSkyModeEarly() {
        if (!g_skyParked) {
            return;
        }
        if (auto* sky = RE::Sky::GetSingleton()) {
            sky->mode = g_savedSkyMode;
        }
        g_skyParked = false;
        spdlog::debug("studio light: sky mode restored at close edge "
                      "(weather/audio keeps running; shell occludes).");
    }

    void ReparkSkyMode() {
        if (g_skyParked) {
            return;
        }
        // r44: interiors only - exteriors never park the mode (the branch
        // culls own the visuals; the mode value only matters for interior
        // fog ingest).
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell || !cell->IsInteriorCell()) {
            return;
        }
        if (auto* sky = RE::Sky::GetSingleton()) {
            g_savedSkyMode = sky->mode.get();
            sky->mode = RE::Sky::Mode::kInterior;
            g_skyParked = true;
            spdlog::debug("studio light: sky mode re-parked (kInterior).");
        }
    }

    void Restore() {
        // Sky first: it exists in every cell kind (the interior block below
        // early-outs in exteriors).
        if (g_skyParked) {
            if (auto* sky = RE::Sky::GetSingleton()) {
                sky->mode = g_savedSkyMode;
            }
            g_skyParked = false;
            spdlog::debug("studio light: sky renderer restored (mode {}).",
                          static_cast<int>(g_savedSkyMode));
        }
        if (g_imageSpaceID != 0) {
            if (auto* img = RE::TESForm::LookupByID<RE::TESImageSpace>(g_imageSpaceID)) {
                img->data = g_savedImage;
                spdlog::debug("studio light: restored imagespace {:08X}.", g_imageSpaceID);
            }
            g_imageSpaceID = 0;
        }
        if (g_cellID == 0) {
            return;
        }
        if (auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_cellID)) {
            if (auto* lighting = cell->GetLighting()) {
                *lighting = g_saved;
                spdlog::debug("studio light: restored cell {:08X}.", g_cellID);
            }
        }
        g_cellID = 0;
        // Push the restored values through the same ingest path - otherwise
        // the studio values would linger on screen until the next cell load.
        ReingestCellLighting();
    }
}
