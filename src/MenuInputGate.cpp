#include "PCH.h"

#include "MenuInputGate.h"

#include "Bubble.h"
#include "Settings.h"

namespace {
    constexpr std::uint32_t kRightMouseButton = 1;  // GFx: 0 left, 1 right, 2 middle

    bool ShouldEatRightMouse(RE::UIMessage& a_message) {
        if (!MTB::Settings::GetSingleton().blockRightMouse ||
            !MTB::Bubble::IsBubbleActive()) {
            return false;
        }
        if (a_message.type != RE::UI_MESSAGE_TYPE::kScaleformEvent || !a_message.data) {
            return false;
        }
        const auto* data = static_cast<RE::BSUIScaleformData*>(a_message.data);
        const auto* ev = data->scaleformEvent;
        using ET = RE::GFxEvent::EventType;
        if (!ev || (ev->type != ET::kMouseDown && ev->type != ET::kMouseUp)) {
            return false;
        }
        // Eat both down and up so Flash never sees half a press.
        return static_cast<const RE::GFxMouseEvent*>(ev)->button == kRightMouseButton;
    }

    // One vfunc-0x4 (ProcessMessage) gate per bubble menu. Coexists with
    // other same-slot vfunc hooks (Apparel Preview's inspect gates) - each
    // chains through to the previous target.
    template <class MenuT>
    struct ProcessMessageGate {
        static RE::UI_MESSAGE_RESULTS thunk(MenuT* a_this, RE::UIMessage& a_message) {
            if (ShouldEatRightMouse(a_message)) {
                return RE::UI_MESSAGE_RESULTS::kHandled;
            }
            return orig(a_this, a_message);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;

        static void Install(const REL::VariantID& a_vtable0) {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtable0 };
            orig = vtbl.write_vfunc(0x4, thunk);
        }
    };

    // F-14 v3: while the bubble spins the character on the right stick, the
    // menu's own item-preview rotation must let go of that stick - the
    // engine maps it to the 'Rotate' user event, which MenuControls feeds
    // to Inventory3DManager. Blank the event ONLY while not inspecting
    // (zoomProgress == 0); inspect mode keeps vanilla item rotation. SPIM
    // ships this exact hook for its gamepad mode (Item3DControls,
    // Tools/ShowPlayerInMenus/src/Event.cpp, MIT - its 2.0.3 fix commit);
    // it is absent from this load order (SPIM disabled), so the bubble
    // carries its own. write_vfunc chains cleanly if both ever load.
    struct ItemRotateGate {
        static RE::BSEventNotifyControl thunk(RE::MenuControls* a_this,
                                              RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>* a_source) {
            if (a_event && MTB::Settings::GetSingleton().previewSpin &&
                MTB::Bubble::IsBubbleActive() && !MTB::Bubble::IsRaceMenuOpen()) {
                auto* inv = RE::Inventory3DManager::GetSingleton();
                if (!inv || inv->GetRuntimeData().zoomProgress == 0.0f) {
                    const auto* userEvents = RE::UserEvents::GetSingleton();
                    for (auto* event = *a_event; event; event = event->next) {
                        if (event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
                            event->HasIDCode()) {
                            auto* idEvent = static_cast<RE::IDEvent*>(event);
                            if (userEvents && idEvent->userEvent == userEvents->rotate) {
                                idEvent->userEvent = "";
                            }
                        }
                    }
                }
            }
            return orig(a_this, a_event, a_source);
        }
        static inline REL::Relocation<decltype(&thunk)> orig;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MenuControls[0] };
            orig = vtbl.write_vfunc(0x1, thunk);
        }
    };
}

namespace MTB::MenuInputGate {
    void Install() {
        ProcessMessageGate<RE::ContainerMenu>::Install(RE::VTABLE_ContainerMenu[0]);
        ProcessMessageGate<RE::BarterMenu>::Install(RE::VTABLE_BarterMenu[0]);
        ProcessMessageGate<RE::InventoryMenu>::Install(RE::VTABLE_InventoryMenu[0]);
        ProcessMessageGate<RE::MagicMenu>::Install(RE::VTABLE_MagicMenu[0]);
        ItemRotateGate::Install();
        spdlog::info("MenuInputGate: right-mouse gates (Container/Barter/Inventory/"
                     "Magic) + gamepad item-rotate gate installed.");
    }
}
