#include "PCH.h"

#include "Bubble.h"
#include "CameraGate.h"
#include "CbpcDrive.h"
#include "FootIkGate.h"
#include "FsmpDrive.h"
#include "MenuInputGate.h"
#include "Settings.h"
#include "SettingsUI.h"

namespace {
    constexpr auto kLogName = "MenuStudio.log";
    constexpr auto kVersion = "0.4.0";

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
    spdlog::info("MenuStudio {} loading (runtime {}).", kVersion,
                 a_skse->RuntimeVersion().string());

    // Universal DLL: SE 1.5.97 OR next-gen AE 1.6.1130+. The Offsets.h table
    // carries a decompile-verified AE (1.6.1170) id for every engine address
    // (SPEC §3.4). Every hook byte-checks E8 before install,
    // so an unrecognised runtime fails SAFE. Refuse anything older/between.
    if (const auto ver = a_skse->RuntimeVersion();
        ver != SKSE::RUNTIME_SSE_1_5_97 && ver < REL::Version(1, 6, 1130, 0)) {
        spdlog::error("Unsupported Skyrim runtime {} - Menu Studio needs SE 1.5.97 "
                      "or next-gen AE 1.6.1130+; not loading.", ver.string());
        return false;
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
