#pragma once

#include <span>
#include <string_view>

#include "Settings.h"  // BackgroundPreset, StagePreset, StagePiece

namespace MTB::BackdropPacks {
    // Rebuild the combined (built-in + discovered) preset lists by re-scanning
    // Data\SKSE\Plugins\MenuStudio\Backdrops\*.ini. Call ONLY when no bubble menu
    // is open (Settings::Load at kDataLoaded / kPostLoadGame): the spans returned
    // below are invalidated by the next Scan().
    void Scan();

    std::span<const BackgroundPreset> Backgrounds();  // built-ins first, then packs
    std::span<const StagePreset>      Stages();        // built-ins first, then packs

    // Author of the pack that contributed a preset name ("" for built-ins /
    // unknown). Used for the menu tooltip. The returned view is invalidated by
    // the next Scan(), same as the spans above.
    std::string_view AuthorOf(std::string_view a_presetName);
}
