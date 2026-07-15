#include "PCH.h"

#include "CbpcDrive.h"

#include <Windows.h>

namespace {
    // CBPC (cbp.dll) recognised by PE fingerprint. `raceSexMenuOpen` (bool) is
    // read by updateActors() when UI::numPausesGame > 0; nonzero keeps CBPC
    // simulating (its own paused-RaceMenu path). We set it while armed. Each
    // offset was read from that build's shipped cbp.pdb - the SE offset differs
    // from the AE one, so support every build the mod ships for.
    struct BuildProfile {
        std::uint32_t  timeDateStamp;
        std::uint32_t  sizeOfImage;
        std::uintptr_t raceSexMenuOpenRva;
        const char*    name;
    };
    constexpr BuildProfile kBuilds[] = {
        { 0x64EF196F, 0x100000, 0xEC874,  "CBPC SE 1.5.97 (Nolvus)" },
        { 0x6597E7CC, 0x11A000, 0x105778, "CBPC AE 1.6.1130" },
        { 0x65A84242, 0x11A000, 0x105778, "CBPC AE 1.6.1170" },
        { 0x65CFBF82, 0x11A000, 0x105778, "CBPC GOG 1.6.1179" },
    };

    std::uint8_t* g_flag = nullptr;
    bool g_applied = false;
    std::uint8_t g_saved = 0;
}

namespace MTB::CbpcDrive {
    void Init() {
        const HMODULE mod = ::GetModuleHandleW(L"cbp.dll");
        if (!mod) {
            spdlog::info("CBPC: cbp.dll not loaded - body-physics drive disabled.");
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(mod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto stamp = nt->FileHeader.TimeDateStamp;
        const auto size = nt->OptionalHeader.SizeOfImage;

        const BuildProfile* build = nullptr;
        for (const auto& candidate : kBuilds) {
            if (candidate.timeDateStamp == stamp && candidate.sizeOfImage == size) {
                build = &candidate;
                break;
            }
        }
        if (!build) {
            spdlog::error(
                "CBPC: cbp.dll is an unknown build (stamp 0x{:08X} size 0x{:X}). Body-physics "
                "drive disabled - everything else works.", stamp, size);
            return;
        }
        g_flag = reinterpret_cast<std::uint8_t*>(base + build->raceSexMenuOpenRva);
        spdlog::info("CBPC: recognized {} - body physics will simulate while the bubble is armed.",
                     build->name);
    }

    bool IsAvailable() {
        return g_flag != nullptr;
    }

    void SetSimulateWhilePaused(bool a_on) {
        if (!g_flag) {
            return;
        }
        if (a_on && !g_applied) {
            g_saved = *g_flag;
            *g_flag = 1;
            g_applied = true;
        } else if (!a_on && g_applied) {
            *g_flag = g_saved;
            g_applied = false;
        }
    }
}
