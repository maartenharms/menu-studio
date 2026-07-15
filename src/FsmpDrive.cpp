#include "PCH.h"

#include "FsmpDrive.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>

namespace {
    // Supported hdtSMP64.dll builds, identified by PE fingerprint. Every field
    // was read from that build's own shipped PDB, so member offsets travel with
    // the profile (the SE 2.5-line layout, the AE 3.5 layout, and the AE 4.0
    // layout all differ). MO2 conflict rules decide which one loads, so support
    // every candidate.
    //
    //  - readTransformRva 0  => this build inlines the world read (AE); Step()
    //    then drives doUpdate2ndStep alone. While the menu is paused the skeleton
    //    is frozen, so the bone anchors from the last live frame stay valid and
    //    the cloth/hair sim still evolves under gravity/collision - the exact
    //    "physics keeps moving while paused" effect. (SE exposes a per-system
    //    SkyrimSystem::readTransform, so it loops the systems like the engine.)
    //  - isStasis 0         => this build has no m_isStasis member (AE 3.5/4.0
    //    dropped it); Step() skips that early-out.
    struct BuildProfile {
        std::uint32_t  timeDateStamp;
        std::uint32_t  sizeOfImage;
        std::uintptr_t worldRva;            // get()::g_World static object
        std::uintptr_t doUpdate2ndStepRva;  // SkyrimPhysicsWorld::doUpdate2ndStep
        std::uintptr_t readTransformRva;    // SkyrimSystem::readTransform, or 0 (AE)
        // member offsets into the g_World object (all PDB-verified per build):
        std::ptrdiff_t systems;             // std::vector<Ref<SkinnedMeshSystem>>
        std::ptrdiff_t isStasis;            // std::atomic_bool, or 0 (absent on AE)
        std::ptrdiff_t timeTick;            // float, 1/min-fps
        std::ptrdiff_t maxSubSteps;         // int
        std::ptrdiff_t rotationSpeedLimit;  // float, rad/s (clamp path)
        std::ptrdiff_t unclampedResetAngle; // float, degrees (reset path)
        std::ptrdiff_t disabled;            // bool
        std::ptrdiff_t resetPc;             // uint8_t
        std::ptrdiff_t suspended;           // std::atomic_bool
        std::ptrdiff_t loading;             // std::atomic_bool
        const char*    name;
    };
    constexpr BuildProfile kBuilds[] = {
        // SE 1.5.97 (2.5-line): the two builds Nolvus ships, identical layout.
        { 0x665702B5, 0x843000, 0x1A6270, 0xB8630, 0xA4CA0,
          0x210, 0x6D0, 0x7CC, 0x7D0, 0x7D8, 0x7E0, 0x7EC, 0x7ED, 0x858, 0x859,
          "Faster HDT-SMP 2.5.0 (SE, Nexus 57339)" },
        { 0x6813B696, 0x835000, 0x199B10, 0xB8940, 0xA50E0,
          0x210, 0x6D0, 0x7CC, 0x7D0, 0x7D8, 0x7E0, 0x7EC, 0x7ED, 0x858, 0x859,
          "HDT-SMP Slot 32 Fix 1.1 (SE, Nexus 119010)" },
        // AE next-gen (3.5.0): per-CPU builds; readTransform inlined, no m_isStasis.
        { 0x6A46CEC4, 0x3CD000, 0x399020, 0x76C20, 0,
          0x228, 0, 0x354, 0x358, 0x360, 0x368, 0x374, 0x375, 0x3E0, 0x3E1,
          "Faster HDT-SMP 3.5.0 (AE, SSE2)" },
        { 0x6A46CEE7, 0x3CC000, 0x398020, 0x76650, 0,
          0x228, 0, 0x354, 0x358, 0x360, 0x368, 0x374, 0x375, 0x3E0, 0x3E1,
          "Faster HDT-SMP 3.5.0 (AE, AVX2)" },
        // AE next-gen (4.0.1): tail members shifted vs 3.5.
        { 0x6A4A9FDB, 0x412000, 0x3DC180, 0x94860, 0,
          0x228, 0, 0x354, 0x358, 0x360, 0x368, 0x380, 0x381, 0x3E8, 0x3E9,
          "Faster HDT-SMP 4.0.1 (AE, SSE2)" },
        { 0x6A4A9FE0, 0x412000, 0x3DC180, 0x945B0, 0,
          0x228, 0, 0x354, 0x358, 0x360, 0x368, 0x380, 0x381, 0x3E8, 0x3E9,
          "Faster HDT-SMP 4.0.1 (AE, AVX2)" },
    };

    using DoUpdate2ndStepFn = void(__fastcall*)(void* a_world, float a_interval,
                                                float a_tick, float a_remaining);
    using ReadTransformFn = void(__fastcall*)(void* a_system, float a_timeStep);

    const BuildProfile* g_profile = nullptr;
    std::uint8_t* g_world = nullptr;
    DoUpdate2ndStepFn g_doUpdate2ndStep = nullptr;
    ReadTransformFn g_readTransform = nullptr;  // null on AE (inlined)
    std::uintptr_t g_moduleBase = 0;
    std::size_t g_moduleSize = 0;

    bool g_freedomApplied = false;
    float g_savedSpeedLimit = 0.0f;
    float g_savedResetAngle = 0.0f;
    bool g_resetPcLogged = false;  // one evidence line per arm session

    bool WorldConstructed() {
        if (!g_world) {
            return false;
        }
        const auto vtbl = *reinterpret_cast<std::uintptr_t*>(g_world);
        return vtbl >= g_moduleBase && vtbl < g_moduleBase + g_moduleSize;
    }
}

namespace MTB::FsmpDrive {
    void Init() {
        const HMODULE mod = ::GetModuleHandleW(L"hdtSMP64.dll");
        if (!mod) {
            spdlog::info("FSMP: hdtSMP64.dll not loaded - SMP drive disabled.");
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
                "FSMP: loaded hdtSMP64.dll is an unknown build (stamp 0x{:08X} size 0x{:X}). "
                "SMP drive disabled - animation ticking still works. Known: SE 2.5.0 / Slot 32 "
                "Fix, AE 3.5.0 / 4.0.1 (per-CPU builds).", stamp, size);
            return;
        }

        g_profile = build;
        g_moduleBase = base;
        g_moduleSize = size;
        g_world = reinterpret_cast<std::uint8_t*>(base + build->worldRva);
        g_doUpdate2ndStep = reinterpret_cast<DoUpdate2ndStepFn>(base + build->doUpdate2ndStepRva);
        g_readTransform = build->readTransformRva
            ? reinterpret_cast<ReadTransformFn>(base + build->readTransformRva)
            : nullptr;
        spdlog::info("FSMP: recognized {} - SMP drive armed (world @ {:p}).",
                     build->name, static_cast<void*>(g_world));
    }

    bool IsAvailable() {
        return g_world != nullptr;
    }

    void SetRotationFreedom(bool a_free) {
        // g_World is a lazily constructed function-local static: only touch it
        // once its vtable slot points into the module (=> constructor ran).
        if (!WorldConstructed()) {
            return;
        }
        auto* speedLimit = reinterpret_cast<float*>(g_world + g_profile->rotationSpeedLimit);
        auto* resetAngle = reinterpret_cast<float*>(g_world + g_profile->unclampedResetAngle);
        if (a_free && !g_freedomApplied) {
            g_savedSpeedLimit = *speedLimit;
            g_savedResetAngle = *resetAngle;
            *speedLimit = 1000.0f;   // effectively no clamp
            *resetAngle = 36000.0f;  // effectively no reset
            g_freedomApplied = true;
            g_resetPcLogged = false;
            spdlog::debug("FSMP: rotation thresholds lifted (were {:.1f} rad/s / {:.0f} deg).",
                          g_savedSpeedLimit, g_savedResetAngle);
        } else if (!a_free && g_freedomApplied) {
            *speedLimit = g_savedSpeedLimit;
            *resetAngle = g_savedResetAngle;
            g_freedomApplied = false;
            spdlog::debug("FSMP: rotation thresholds restored.");
        }
    }

    void Step(float a_dt) {
        if (!g_world || a_dt <= FLT_EPSILON) {
            return;
        }

        if (!WorldConstructed()) {
            return;
        }

        const auto& P = *g_profile;
        if (*reinterpret_cast<bool*>(g_world + P.disabled) ||
            (P.isStasis && *reinterpret_cast<bool*>(g_world + P.isStasis)) ||
            *reinterpret_cast<bool*>(g_world + P.loading)) {
            return;
        }

        auto** first = *reinterpret_cast<std::uint8_t***>(g_world + P.systems);
        auto** last = *reinterpret_cast<std::uint8_t***>(g_world + P.systems + 8);
        if (!first || first == last) {
            return;  // no active SMP systems
        }

        // FSMP re-suspends every frame while paused; doUpdate2ndStep early-outs
        // on m_suspended, so clear it right before our synchronous step. Plain
        // byte store on the atomic_bool is the same operation resume() does.
        *reinterpret_cast<bool*>(g_world + P.suspended) = false;

        // Momentum guard (field: hair "resets on every rotation" under the
        // SPII author's SmoothCam-API build): readTransform hard-resets the
        // player sim while m_resetPc > 0, and FSMP's SKSE camera-event
        // handler pumps it to 3 on first->third person transitions - those
        // events keep firing while paused. Zero the counter right before we
        // drive. Purely a transient event pulse - nothing to restore.
        auto* resetPc = reinterpret_cast<std::uint8_t*>(g_world + P.resetPc);
        if (*resetPc != 0) {
            if (!g_resetPcLogged) {
                g_resetPcLogged = true;
                spdlog::debug("FSMP: player reset counter was {} - suppressed while armed "
                              "(camera-state events must not reset menu physics).", *resetPc);
            }
            *resetPc = 0;
        }

        float tick = *reinterpret_cast<float*>(g_world + P.timeTick);
        if (!(tick > 0.0f) || tick > 1.0f / 30.0f) {
            tick = 1.0f / 60.0f;
        }
        tick = std::min(tick, a_dt);
        const int maxSub =
            std::clamp(*reinterpret_cast<int*>(g_world + P.maxSubSteps), 1, 60);
        const float remaining = std::min(a_dt, tick * static_cast<float>(maxSub));

        // SE builds expose a per-system readTransform (the world-level one was
        // inlined); replicate its loop. AE inlines even the per-system call, so
        // g_readTransform is null - the frozen-pose anchors carry the sim.
        if (g_readTransform) {
            for (auto** it = first; it != last; ++it) {
                if (*it) {
                    g_readTransform(*it, remaining);
                }
            }
        }

        // Synchronous on the main thread (FSMP normally runs this on a task
        // group and syncs next frame; while paused no task is in flight, and
        // the function takes FSMP's own world lock).
        g_doUpdate2ndStep(g_world, a_dt, tick, remaining);
    }
}
