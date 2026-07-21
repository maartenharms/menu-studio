#include "PCH.h"

// The linker-provided base of THIS module. Used only to ask Windows for our own
// DLL path, so the load banner can report the file's real build time.
// PCH.h's comment claims RE/Skyrim.h pulls in <Windows.h>; it does not bring in
// these types here, so include it explicitly rather than trusting the comment.
#include <Windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;

#include "Bubble.h"
#include "CameraGate.h"
#include "CbpcDrive.h"
#include "FootIkGate.h"
#include "FsmpDrive.h"
#include "MenuInputGate.h"
#include "Settings.h"
#include "SettingsUI.h"
#include "VersionCheck.h"

#include <SimpleIni.h>

namespace {
    constexpr auto kLogName = "MenuStudio.log";
    constexpr auto kVersion = "0.7.2";

    enum class RuntimeGate {
        kAuto = 0,   // widen, and let the self-check decide
        kForce = 1,  // load even if the self-check fails
        kStrict = 2  // pre-0.7.2 behaviour: 1.5.97 or 1.6.1130+ only
    };

    // ⚠ NOT Settings. Settings::Load() runs at kDataLoaded, which is roughly
    // 25 seconds later in a field log - the same trap that left the diagnostic
    // probes reading a compiled-in default forever. The gate has to decide
    // here, so it reads the one key it needs itself.
    RuntimeGate ReadRuntimeGate() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(L"Data/SKSE/Plugins/MenuStudio.ini") < 0) {
            return RuntimeGate::kAuto;
        }
        switch (ini.GetLongValue("Compatibility", "iRuntimeGate", 0)) {
        case 1:
            return RuntimeGate::kForce;
        case 2:
            return RuntimeGate::kStrict;
        default:
            return RuntimeGate::kAuto;
        }
    }

    void SetupLog() {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= kLogName;

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            MTB::Settings::GetSingleton().Load();
            MTB::Bubble::Register();
            MTB::FsmpDrive::Init();
            MTB::CbpcDrive::Init();
            MTB::FootIkGate::Init();
            MTB::SettingsUI::Register();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            // A menu-open quickload never delivers the close event (AP lesson);
            // reset all armed state.
            MTB::Bubble::GetSingleton().ForceReset();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // §3.2: re-read the INI on every save load so edits apply
            // without relaunching. No menus are open here, so the sMenus
            // set swaps safely.
            MTB::Settings::GetSingleton().Load();
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    // ⚠ THE BUILD STAMP IS NOT DECORATION. MO2 profiles can point at any of
    // several Menu Studio mod folders (the plain dev one that build.bat
    // deploys to, plus a versioned folder per shipped release), and the
    // version string is IDENTICAL across them - a dev build and the release it
    // came from both say "0.7.0". A whole field round was read as "the fix did
    // not work" when the profile had simply loaded the release folder and the
    // new code was never in the process. __DATE__/__TIME__ are the compile
    // moment, so this line alone settles WHICH BINARY RAN, from the log, with
    // no access to the machine. Never remove it to tidy the header up.
    // ⚠ THE DLL'S OWN FILE TIME, NOT __DATE__/__TIME__.
    //
    // The first version of this used __DATE__ __TIME__ and was WRONG within an
    // hour: those expand at COMPILE time for THIS translation unit only, so a
    // build that changed Bubble.cpp and relinked left plugin.cpp untouched and
    // the stamp reported the previous build. A stamp that silently lags is
    // worse than none, because it is trusted. The module's last-write time is
    // the link moment and is always right.
    std::string built = "unknown";
    {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            SYSTEMTIME                st{};
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
                FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
                built = fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}Z", st.wYear, st.wMonth,
                                    st.wDay, st.wHour, st.wMinute, st.wSecond);
            }
        }
    }
    spdlog::info("MenuStudio {} loading (runtime {}) - built {}.", kVersion,
                 a_skse->RuntimeVersion().string(), built);

    // Universal DLL: SE 1.5.97 OR any AE from 1.6.317 up.
    //
    // ⚠ THE OLD GATE REFUSED EVERYTHING BETWEEN, AND THAT WAS THE BUG. Users
    // on the mid 1.6 builds saw SKSE's "reported as incompatible during load"
    // dialog, which is this function returning false - our own message, never
    // a crash. The refusal was written when the AE half of the Offsets.h table
    // had been verified on exactly one binary (1.6.1170), and "verified on one
    // build" was turned into "refuse every other build". Every engine address
    // goes through Address Library ids, which the database re-points per
    // build, so the refusal was mostly protecting us from a lack of evidence.
    //
    // The evidence is now gathered on the user's own machine instead:
    // VersionCheck::Run() checks every id for real membership in THIS build's
    // database and locates the frame-driver call site by matching its call
    // target, then this gate refuses only if that critical piece is missing.
    // The difference that matters is that we now decline on a MEASUREMENT
    // rather than on a version number, and either way we decline cleanly -
    // a plugin that loads and then misbehaves is far worse for a user than one
    // that says no.
    const auto ver     = a_skse->RuntimeVersion();
    const auto gate    = ReadRuntimeGate();
    const bool known   = ver == SKSE::RUNTIME_SSE_1_5_97 || ver >= REL::Version(1, 6, 317, 0);
    if (gate == RuntimeGate::kStrict) {
        // The pre-0.7.2 behaviour, kept as an escape hatch: if widening the
        // gate turns out to hurt someone, they can put it back without us
        // shipping a build.
        if (ver != SKSE::RUNTIME_SSE_1_5_97 && ver < REL::Version(1, 6, 1130, 0)) {
            spdlog::error("Unsupported Skyrim runtime {} and iRuntimeGate=2 (strict) - "
                          "not loading.", ver.string());
            return false;
        }
    } else if (!known) {
        spdlog::error("Unsupported Skyrim runtime {} - Menu Studio needs SE 1.5.97 or AE "
                      "1.6.317+; not loading.", ver.string());
        return false;
    }

    MTB::VersionCheck::Run();
    if (!MTB::VersionCheck::CriticalOk()) {
        if (gate != RuntimeGate::kForce) {
            spdlog::error("Address self-check FAILED on runtime {} - Menu Studio's frame driver "
                          "has nowhere to install, so the mod would load and do nothing. Not "
                          "loading. Send MenuStudio.log to the author; set "
                          "iRuntimeGate=1 under [Compatibility] to load anyway.", ver.string());
            return false;
        }
        spdlog::warn("Address self-check FAILED but iRuntimeGate=1 (force) - loading anyway. "
                     "Expect the pause features to do nothing.");
    }

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(64);

    MTB::Bubble::InstallHook();
    MTB::MenuInputGate::Install();
    MTB::CameraGate::Install();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        spdlog::error("Failed to register SKSE messaging listener; aborting load.");
        return false;
    }

    spdlog::info("MenuStudio loaded.");
    return true;
}
