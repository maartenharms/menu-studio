#include "PCH.h"

#include "AnimEventProbe.h"

namespace MTB::AnimEventProbe {

    namespace {
        // The graph raises events from its own threads, so this is read from a
        // sink callback that is NOT the main thread. Plain bool would be a data
        // race for a value whose whole job is to label the log correctly.
        std::atomic<bool> g_armed{ false };
        bool              g_installed = false;

        class Sink : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            static Sink* GetSingleton() {
                static Sink singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BSAnimationGraphEvent*                    a_event,
                RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                // kContinue ALWAYS, on every path. This sink sits in the same
                // chain the game's own listeners use; swallowing an event here
                // would change behaviour, and a diagnostic that changes
                // behaviour is worse than no diagnostic.
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const char* const tag = a_event->tag.c_str();
                if (!tag || !*tag) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const char* const payload = a_event->payload.c_str();
                spdlog::debug("anim event [{}] {}{}{}", g_armed.load() ? "MENU" : "live", tag,
                              (payload && *payload) ? " | " : "",
                              (payload && *payload) ? payload : "");
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void SetArmed(bool a_armed) { g_armed.store(a_armed); }

    bool IsArmed() { return g_armed.load(); }

    void Reset() { g_installed = false; }

    void Install() {
        if (g_installed) {
            return;
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) {
            return;  // no graph yet - retried next tick
        }
        RE::BSAnimationGraphManagerPtr manager;
        if (!player->GetAnimationGraphManager(manager) || !manager) {
            return;
        }
        int attached = 0;
        for (auto& graph : manager->graphs) {
            if (graph) {
                // BShkbAnimationGraph IS a BSTEventSource<BSAnimationGraphEvent>
                // (its 4th base). Adding a sink is the same supported path the
                // game's own listeners use - no vtable arithmetic, which is
                // deliberate: a wrong index on a secondary base is a crash, and
                // this is a throwaway diagnostic, not a reason to risk one.
                graph->AddEventSink(Sink::GetSingleton());
                ++attached;
            }
        }
        if (attached == 0) {
            return;  // manager exists but holds no graph yet - retry
        }
        g_installed = true;
        spdlog::info("anim event probe: attached to {} player graph(s). Events are logged "
                     "as [live] or [MENU] so a working swap can be diffed against a broken one.",
                     attached);
    }

}  // namespace MTB::AnimEventProbe
