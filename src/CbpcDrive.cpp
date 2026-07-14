#include "PCH.h"

#include "CbpcDrive.h"

#include <Windows.h>

namespace {
    // CBPC "CBPC - Physics with Collisions" as shipped in Nolvus (cbp.dll,
    // link stamp 2023-08-30, ships cbp.pdb). Symbols verified against that
    // PDB: `raceSexMenuOpen` (bool) at +0xEC874 - read by updateActors()
    // (+0x3D8D0) when UI::numPausesGame > 0; nonzero continues simulating.
    constexpr std::uint32_t kTimeDateStamp = 0x64EF196F;
    constexpr std::uint32_t kSizeOfImage = 0x100000;
    constexpr std::uintptr_t kRaceSexMenuOpenRva = 0xEC874;

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
        if (nt->FileHeader.TimeDateStamp != kTimeDateStamp ||
            nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
            spdlog::error(
                "CBPC: cbp.dll is an unknown build (stamp 0x{:08X} size 0x{:X}; expected "
                "0x{:08X}/0x{:X}). Body-physics drive disabled - everything else works.",
                nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
                kTimeDateStamp, kSizeOfImage);
            return;
        }
        g_flag = reinterpret_cast<std::uint8_t*>(base + kRaceSexMenuOpenRva);
        spdlog::info("CBPC: recognized build - body physics will simulate while the bubble is armed.");
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
