#include "PCH.h"

#include "FsmpDrive.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>

namespace {
    // Supported hdtSMP64.dll builds, identified by PE fingerprint. Both are
    // 2.5-line builds with identical member layout (each verified against its
    // own shipped PDB - see docs/SPIKE-B-FSMP.md). MO2 conflict rules decide
    // which one actually loads, so support every candidate in the load order.
    struct BuildProfile {
        std::uint32_t  timeDateStamp;
        std::uint32_t  sizeOfImage;
        std::uintptr_t worldRva;            // `get()::g_World` static object
        std::uintptr_t doUpdate2ndStepRva;  // SkyrimPhysicsWorld::doUpdate2ndStep
        std::uintptr_t readTransformRva;    // SkyrimSystem::readTransform
        const char*    name;
    };
    constexpr BuildProfile kBuilds[] = {
        { 0x665702B5, 0x843000, 0x1A6270, 0xB8630, 0xA4CA0,
          "Faster HDT-SMP 2.5.0 (Nexus 57339)" },
        { 0x6813B696, 0x835000, 0x199B10, 0xB8940, 0xA50E0,
          "SMP Armor scanning bugfix 1.1 / Slot 32 Fix (Nexus 119010, 2.5-line)" },
    };

    // Member offsets, identical across the supported builds (PDB-verified
    // per build).
    constexpr std::ptrdiff_t kSystemsOffset = 0x210;     // std::vector<Ref<SkinnedMeshSystem>>
    constexpr std::ptrdiff_t kIsStasisOffset = 0x6D0;    // std::atomic_bool
    constexpr std::ptrdiff_t kTimeTickOffset = 0x7CC;    // float (1/min-fps)
    constexpr std::ptrdiff_t kMaxSubStepsOffset = 0x7D0; // int
    constexpr std::ptrdiff_t kRotationSpeedLimitOffset = 0x7D8;  // float, rad/s (clamp path)
    constexpr std::ptrdiff_t kUnclampedResetAngleOffset = 0x7E0; // float, degrees (reset path)
    constexpr std::ptrdiff_t kDisabledOffset = 0x7EC;    // bool
    constexpr std::ptrdiff_t kResetPcOffset = 0x7ED;     // uint8_t (PDB-verified, both builds)
    constexpr std::ptrdiff_t kSuspendedOffset = 0x858;   // std::atomic_bool
    constexpr std::ptrdiff_t kLoadingOffset = 0x859;     // std::atomic_bool

    using DoUpdate2ndStepFn = void(__fastcall*)(void* a_world, float a_interval,
                                                float a_tick, float a_remaining);
    using ReadTransformFn = void(__fastcall*)(void* a_system, float a_timeStep);

    std::uint8_t* g_world = nullptr;
    DoUpdate2ndStepFn g_doUpdate2ndStep = nullptr;
    ReadTransformFn g_readTransform = nullptr;
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
                "SMP drive disabled - animation ticking still works. Known builds: "
                "2.5.0 (0x665702B5) and Slot 32 Fix 1.1 (0x6813B696).",
                stamp, size);
            return;
        }

        g_moduleBase = base;
        g_moduleSize = size;
        g_world = reinterpret_cast<std::uint8_t*>(base + build->worldRva);
        g_doUpdate2ndStep = reinterpret_cast<DoUpdate2ndStepFn>(base + build->doUpdate2ndStepRva);
        g_readTransform = reinterpret_cast<ReadTransformFn>(base + build->readTransformRva);
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
        auto* speedLimit = reinterpret_cast<float*>(g_world + kRotationSpeedLimitOffset);
        auto* resetAngle = reinterpret_cast<float*>(g_world + kUnclampedResetAngleOffset);
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

        if (*reinterpret_cast<bool*>(g_world + kDisabledOffset) ||
            *reinterpret_cast<bool*>(g_world + kIsStasisOffset) ||
            *reinterpret_cast<bool*>(g_world + kLoadingOffset)) {
            return;
        }

        auto** first = *reinterpret_cast<std::uint8_t***>(g_world + kSystemsOffset);
        auto** last = *reinterpret_cast<std::uint8_t***>(g_world + kSystemsOffset + 8);
        if (!first || first == last) {
            return;  // no active SMP systems
        }

        // FSMP re-suspends every frame while paused; doUpdate2ndStep early-outs
        // on m_suspended, so clear it right before our synchronous step. Plain
        // byte store on the atomic_bool is the same operation resume() does.
        *reinterpret_cast<bool*>(g_world + kSuspendedOffset) = false;

        // Momentum guard (field: hair "resets on every rotation" under the
        // SPII author's SmoothCam-API build): readTransform hard-resets the
        // player sim while m_resetPc > 0, and FSMP's SKSE camera-event
        // handler pumps it to 3 on first→third person transitions - those
        // events keep firing while paused, and a camera-API rotation path
        // can raise them per drag. Our lifted thresholds can't cover this
        // route; zero the counter right before we drive. Purely a transient
        // event pulse - nothing to restore, normal behavior resumes with
        // the next camera event after disarm. The first-hit log line is the
        // field discriminator for WHERE the resets came from.
        auto* resetPc = reinterpret_cast<std::uint8_t*>(g_world + kResetPcOffset);
        if (*resetPc != 0) {
            if (!g_resetPcLogged) {
                g_resetPcLogged = true;
                spdlog::debug("FSMP: player reset counter was {} - suppressed while armed "
                              "(camera-state events must not reset menu physics).", *resetPc);
            }
            *resetPc = 0;
        }

        float tick = *reinterpret_cast<float*>(g_world + kTimeTickOffset);
        if (!(tick > 0.0f) || tick > 1.0f / 30.0f) {
            tick = 1.0f / 60.0f;
        }
        tick = std::min(tick, a_dt);
        const int maxSub =
            std::clamp(*reinterpret_cast<int*>(g_world + kMaxSubStepsOffset), 1, 60);
        const float remaining = std::min(a_dt, tick * static_cast<float>(maxSub));

        // SkinnedMeshWorld::readTransform got inlined in these builds; replicate
        // its loop: vector elements are Ref<SkinnedMeshSystem> = one pointer.
        for (auto** it = first; it != last; ++it) {
            if (*it) {
                g_readTransform(*it, remaining);
            }
        }

        // Synchronous on the main thread (FSMP normally runs this on a task
        // group and syncs next frame; while paused no task is in flight, and
        // the function takes FSMP's own world lock).
        g_doUpdate2ndStep(g_world, a_dt, tick, remaining);
    }
}
