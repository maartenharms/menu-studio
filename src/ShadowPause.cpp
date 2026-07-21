#include "PCH.h"

#include "ShadowPause.h"

namespace {
    bool g_registered = false;
    bool g_up = false;

    // A menu with NO Scaleform movie. That is the whole point - it must never
    // draw - but it makes every inherited entry point that assumes `uiMovie` a
    // null dereference waiting to happen. IMenu::uiMovie defaults to nullptr and
    // the base AdvanceMovie/PostDisplay/RefreshPlatform all use it, so EVERY one
    // of them is overridden here to do nothing. Nothing in this class may ever
    // touch uiMovie.
    //
    // The flags are deliberately minimal: kPausesGame is the entire purpose, and
    // anything else invites the engine to render, take input, own the cursor or
    // freeze-frame the background. In particular do NOT set
    // kRendersOffscreenTargets (it enables the PreDisplay path), kUsesCursor,
    // kModal or kTopmostRenderedMenu.
    class ShadowMenu : public RE::IMenu {
    public:
        ShadowMenu() {
            menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
            // Saving is normally blocked by a pausing menu. This one is invisible
            // and can be up while the player uses an inventory they CAN save
            // from, so hand that permission back rather than silently changing
            // when a save is allowed.
            menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving);
            // Bottom of the stack: never the topmost rendered menu, never a
            // candidate for cursor ownership or button-bar handling.
            depthPriority = 0;
            // No input context pushed - the player's real menu keeps ownership of
            // input exactly as it would without us.
            inputContext = Context::kNone;
        }

        ~ShadowMenu() override = default;

        static RE::IMenu* Creator() { return new ShadowMenu(); }

        // FxDelegateHandler - no callbacks, no movie to bind them to.
        void Accept(RE::FxDelegateHandler::CallbackProcessor*) override {}

        // Base implementations advance / display uiMovie. There is none.
        void AdvanceMovie(float, std::uint32_t) override {}
        void PostDisplay() override {}
        void RefreshPlatform() override {}

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override {
            using Type = RE::UI_MESSAGE_TYPE;
            // Handle the lifecycle messages ourselves and swallow everything
            // else. Passing them on would reach base handlers that expect a
            // movie. kShow/kHide are reported handled so the engine completes
            // its own open/close bookkeeping (which is what moves
            // numPausesGame) without asking this menu to draw anything.
            switch (a_message.type.get()) {
                case Type::kShow:
                case Type::kHide:
                case Type::kForceHide:
                case Type::kUpdate:
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                default:
                    return RE::UI_MESSAGE_RESULTS::kIgnore;
            }
        }
    };

    void Post(RE::UI_MESSAGE_TYPE a_type) {
        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(MTB::ShadowPause::kMenuName, a_type, nullptr);
        }
    }
}

namespace MTB::ShadowPause {
    void Register() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            spdlog::error("ShadowPause: UI singleton unavailable - the studio "
                          "cannot hold its own pause; Souls-live menus stay live.");
            return;
        }
        ui->Register(kMenuName, ShadowMenu::Creator);
        g_registered = true;
        spdlog::info("ShadowPause: registered '{}' (kPausesGame, movie-less). Skyrim "
                     "Souls works from a fixed list of vanilla menu names, so it "
                     "never sees this one.",
                     kMenuName);
    }

    void Show() {
        if (!g_registered || g_up) {
            return;
        }
        g_up = true;
        Post(RE::UI_MESSAGE_TYPE::kShow);
        spdlog::info("ShadowPause: shown - the studio now holds a genuine pausing "
                     "menu, so the engine owns the pause bookkeeping.");
    }

    void Hide() {
        if (!g_registered || !g_up) {
            return;
        }
        g_up = false;
        Post(RE::UI_MESSAGE_TYPE::kHide);
        spdlog::info("ShadowPause: hidden - pause handed back to the engine.");
    }

    bool IsUp() { return g_up; }
    bool IsAvailable() { return g_registered; }
}
