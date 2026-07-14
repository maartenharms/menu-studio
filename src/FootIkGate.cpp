#include "PCH.h"

#include "FootIkGate.h"

#include <Windows.h>

namespace {
    // "Inverse Kinematics - Feet of Skyrim" (FeetOfSkyrim.dll), namespace FIF.
    // Verified against its shipped PDB: FIF::Settings::Config.enabled (bool,
    // offset 0) lives at the Settings singleton = FeetOfSkyrim.dll + 0xEBD90.
    // FootIkDriverUpdateHook and both ForceFootIkOn hooks early-out when it is 0
    // (0xEBD91 = forceEnableOnStairs, 0xEBD92 = forceEnableWhileMoving). Writing
    // the raw byte, unlike the GUI checkbox, does NOT clear or reseed toe
    // targets, so arm/disarm is cheap and fully reversible.
    constexpr std::uint32_t  kTimeDateStamp = 0x6A5266B1;
    constexpr std::uint32_t  kSizeOfImage   = 0xFD000;
    constexpr std::uintptr_t kEnabledRva    = 0xEBD90;

    std::uint8_t* g_flag = nullptr;
    bool          g_applied = false;
    std::uint8_t  g_saved = 1;
}

namespace MTB::FootIkGate {
    void Init() {
        const HMODULE mod = ::GetModuleHandleW(L"FeetOfSkyrim.dll");
        if (!mod) {
            spdlog::info("FootIkGate: FeetOfSkyrim.dll not loaded; nothing to suppress.");
            return;
        }
        const auto  base = reinterpret_cast<std::uintptr_t>(mod);
        const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kTimeDateStamp ||
            nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
            spdlog::error(
                "FootIkGate: FeetOfSkyrim.dll is an unknown build (stamp 0x{:08X} size 0x{:X}; "
                "expected 0x{:08X}/0x{:X}). Foot IK will NOT be suppressed in the bubble; "
                "everything else works.",
                nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
                kTimeDateStamp, kSizeOfImage);
            return;
        }
        auto* flag = reinterpret_cast<std::uint8_t*>(base + kEnabledRva);
        if (*flag > 1) {
            spdlog::error("FootIkGate: byte at FeetOfSkyrim.dll+0x{:X} is 0x{:02X}, not a bool; "
                          "skipping (build mismatch).", kEnabledRva, *flag);
            return;
        }
        g_flag = flag;
        spdlog::info("FootIkGate: recognized Feet of Skyrim; its foot IK will stand down in "
                     "the void / dressing room.");
    }

    bool IsAvailable() { return g_flag != nullptr; }

    void SetSuppressed(bool a_on) {
        if (!g_flag) {
            return;
        }
        if (a_on && !g_applied) {
            g_saved = *g_flag;   // remember the user's setting
            *g_flag = 0;         // FIF hooks early-out: vanilla / neutral feet
            g_applied = true;
        } else if (!a_on && g_applied) {
            *g_flag = g_saved;   // hand FIF back exactly as it was
            g_applied = false;
        }
    }
}
