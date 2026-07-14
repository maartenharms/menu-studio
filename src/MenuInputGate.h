#pragma once

namespace MTB {
    // While the bubble is active, starve Flash of RIGHT-mouse presses in the
    // bubble menus. The active UI stack (Vel'dun UI / Kome's Inventory Tweaks
    // swfs) binds right-click to quick item actions (buy/sell/equip), which
    // fire constantly while right-drag-rotating the character. Rotation mods
    // read raw input, so eating the Scaleform mouse event kills the purchase
    // without touching the rotation. Vanilla binds nothing to right-click in
    // these menus, so nothing vanilla is lost.
    namespace MenuInputGate {
        void Install();  // SKSEPlugin_Load, after SKSE::Init
    }
}
