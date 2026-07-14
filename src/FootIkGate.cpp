#include "PCH.h"

#include "FootIkGate.h"

#include <Windows.h>

namespace {
    // "Feet of Skyrim" (FeetOfSkyrim.dll). Its master enabled bool is what the
    // foot-IK update + force hooks cmp against 0 at entry - clear it and the mod
    // stands down (vanilla / neutral feet). Writing the raw byte, unlike the GUI
    // checkbox, does NOT reseed toe targets, so arm/disarm is cheap and fully
    // reversible. The namespace/layout was FIF:: on the SE build and FoS:: on the
    // AE v4 build, so the byte offset differs - key on the fingerprint. Each RVA
    // is the byte at the start of the driver update hook (PDB/decompile-verified).
    struct BuildProfile {
        std::uint32_t  timeDateStamp;
        std::uint32_t  sizeOfImage;
        std::uintptr_t enabledRva;
        const char*    name;
    };
    constexpr BuildProfile kBuilds[] = {
        { 0x6A5266B1, 0xFD000,  0xEBD90, "Feet of Skyrim (SE, FIF build)" },
        { 0x6A53F894, 0x10A000, 0xF4E40, "Feet of Skyrim v4 (AE/universal, FoS build)" },
    };

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
        const auto stamp = nt->FileHeader.TimeDateStamp;
        const auto size  = nt->OptionalHeader.SizeOfImage;

        const BuildProfile* build = nullptr;
        for (const auto& candidate : kBuilds) {
            if (candidate.timeDateStamp == stamp && candidate.sizeOfImage == size) {
                build = &candidate;
                break;
            }
        }
        if (!build) {
            spdlog::error(
                "FootIkGate: FeetOfSkyrim.dll is an unknown build (stamp 0x{:08X} size 0x{:X}). "
                "Foot IK will NOT be suppressed in the bubble; everything else works.",
                stamp, size);
            return;
        }
        auto* flag = reinterpret_cast<std::uint8_t*>(base + build->enabledRva);
        if (*flag > 1) {
            spdlog::error("FootIkGate: byte at FeetOfSkyrim.dll+0x{:X} is 0x{:02X}, not a bool; "
                          "skipping (build mismatch).", build->enabledRva, *flag);
            return;
        }
        g_flag = flag;
        spdlog::info("FootIkGate: recognized {}; its foot IK will stand down in the void / "
                     "dressing room.", build->name);
    }

    bool IsAvailable() { return g_flag != nullptr; }

    void SetSuppressed(bool a_on) {
        if (!g_flag) {
            return;
        }
        if (a_on && !g_applied) {
            g_saved = *g_flag;   // remember the user's setting
            *g_flag = 0;         // hooks early-out: vanilla / neutral feet
            g_applied = true;
        } else if (!a_on && g_applied) {
            *g_flag = g_saved;   // hand it back exactly as it was
            g_applied = false;
        }
    }
}
