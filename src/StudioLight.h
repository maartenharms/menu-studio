#pragma once

namespace MTB {
    // "Studio lighting": while the bubble is armed in an interior, override
    // the cell's INTERIOR_DATA with the [Lighting] preset values - even
    // ambient/DALC fill, soft directional, fog (the fog color IS the void's
    // backdrop color) - clear the lighting-template inherit flags for the
    // overridden channels, neutralize the cell's imagespace (tint/HDR), and
    // force the Sky's interior re-ingest (the renderer only ingests cell
    // lighting at cell attach; docs/STATUS.md has the chain). Original data
    // saved on arm and restored on disarm/close/ForceReset.
    namespace StudioLight {
        void Apply();        // arm edge (no-op outside interiors / non-void modes)
        void LiveRefresh();  // mid-arm settings change: rewrite + re-ingest
        void Restore();      // disarm edge / ForceReset
        // r40: the sky MODE alone crosses the exit's unpaused window early -
        // a kNone frame there makes the engine stop the weather's rain loop
        // for good (transition-triggered audio). Close edge restores it;
        // a switch re-open parks it again.
        void RestoreSkyModeEarly();  // bubble-menu close edge
        void ReparkSkyMode();        // bubble-menu open while armed (switch)
    }
}
