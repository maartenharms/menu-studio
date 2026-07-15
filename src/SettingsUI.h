#pragma once

namespace MTB::SettingsUI {
    // FLICK (FUCK) sidebar tool. Soft dependency: without FUCK.dll the mod
    // stays INI-only with one log line. (Was SKSE Menu Framework pre-2026-07-13.)
    void Register();  // kDataLoaded, after Settings::Load()
}
