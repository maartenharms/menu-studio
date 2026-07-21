#pragma once

#include <string>
#include <unordered_set>

namespace MTB {
    // The bubble: while a configured menu is open AND the game is actually
    // paused, tick the player's animation graph (and optionally FSMP) with a
    // real dt from our own QPC clock. Frame driver is a write_call<5> on the
    // player-update dispatch call inside Main::Update, so the tick lands right
    // after the engine's own (paused = skipped) player update.
    class Bubble : public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                   public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static Bubble& GetSingleton();

        static void InstallHook();  // SKSEPlugin_Load (after AllocTrampoline)
        static void Register();     // kDataLoaded: menu event sink
        void        ForceReset();   // kPreLoadGame / kNewGame backstop

        // Live bubble condition (menu open + game paused + enabled) - safe
        // from any main-thread context, no per-frame state required.
        [[nodiscard]] static bool IsBubbleActive();

        // Preview prompt: the last input device the sink saw, and whether the
        // try-on hint should render right now (armed + inventory-family menu +
        // dressing-room view mode + enabled).
        [[nodiscard]] bool UsingGamepad() const {
            return lastInputDevice_.load() == RE::INPUT_DEVICE::kGamepad;
        }
        [[nodiscard]] bool ShouldShowTryOnPrompt() const;

        // Raw bubble-menu open count (no pause/enable condition) - for
        // tripwires that must distinguish "gate off because no menu" from
        // "gate off DURING a menu" (TRACKER B-4).
        [[nodiscard]] static int OpenMenuCount() {
            return GetSingleton().menusOpen_.load();
        }

        // RaceSexMenu is bubbled EXPERIMENTALLY (F-9): the engine's own
        // racemenu mode already animates the player, so our anim ticks
        // skip, and the right-mouse gate stands down (RaceMenu owns its
        // input).
        [[nodiscard]] static bool IsRaceMenuOpen() {
            return GetSingleton().raceMenuOpen_.load();
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

        // F-14: the bubble's own drag input - right-mouse deltas accumulate
        // into the preview-spin target while armed (bPreviewSpin).
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override;

        void OnFrame(RE::Main* a_main);

        // B-7 v3 stutter fix: SPIM calls Update3DPosition (vfunc 0x3F) per
        // drag event, re-syncing the node to the pinned front AFTER our
        // per-tick compose - the vfunc hook calls this to re-apply the
        // spin on top of every sync. No-op unless armed with a live spin.
        void ReassertSpin();

    private:
        Bubble() = default;

        void Tick(RE::Main* a_main, bool a_paused);
        void Disarm();  // armed→idle edge: restore decluttered refs
        // F-15: force third + full framing when we own the view; a_force
        // (r45) bypasses the coverage check - tick-3 fallback only.
        void ArmOwnViewIfOurs(bool a_force = false);
        void LogTelemetry(RE::Main* a_main, bool a_paused, float a_dt);
        void CancelDipIfActive();          // F-12 v2: never leave the screen dark
        void FireDeferredExitMoveStart();  // B-8 v2: exit mirror, past the switch gap
        // F-26: the preview sheathe, past the switch gap. a_force pays it now
        // (paths that return before the timed fire site can ever run again).
        void FireDeferredWeaponRestore(bool a_force = false);

        std::atomic<int>  menusOpen_{ 0 };
        std::atomic<bool> raceMenuOpen_{ false };
        std::atomic<int>  graceFrames_{ 0 };    // deferred VISUAL teardown (short)
        std::atomic<int>  gateHoldFrames_{ 0 }; // gate continuity through menu switches (long)
        bool              armedLastFrame_ = false;
        bool              sentMoveStop_ = false;  // B-2: idle event sent at arm → mirror at exit
        bool              airFrozenArm_ = false;  // B-2: armed mid-air → anim tick frozen (vanilla look)
        // Was the player MOVING on solid ground when this menu opened? An
        // arm-edge latch, because the pause makes it unknowable afterwards.
        // A moving arm has an owner already - the settle-to-idle - so the
        // draw/sheathe hold stands aside for it (user: "if we unsheath and are
        // moving the period where we freeze the character applies, can we make
        // it so when we start moving this pause period is negated").
        bool              movingArm_ = false;
        bool              faceCallerSeen_ = false; // 0.7.1 probe: engine face caller (0x3D9440) alive?
        // THE BLINK, MADE COUNTABLE. A natural blink is ~0.2 s once every few
        // seconds, and "hard to tell" was the field verdict three rounds
        // running - which is how a changelog claimed a fix that did not work.
        // These count the engine's OWN blink machine (unk200: 0 wait /
        // 1 closing / 2 opening / 3-4 look-holds) and our mesh bakes, and the
        // disarm edge prints the totals. A number closes what an impression
        // cannot.
        std::uint32_t     faceMeshApplies_ = 0;   // mesh bakes this session
        std::uint32_t     blinkStarts_ = 0;       // machine entered state 1
        std::uint32_t     blinkCompletes_ = 0;    // machine left state 2
        std::uint32_t     blinkCaps_ = 0;         // diagnostic 0.5 s cap applied
        float             blinkLidPeak_ = 0.0f;   // highest composed lid value seen
        int               blinkStatePrev_ = -1;   // last unk200, for edge counting
        bool              faceNodeMissingLogged_ = false;  // one warning per session
        bool              faceHeldLogged_ = false;   // face-hold edge, logged on change
        bool              freezeDeclineLogged_ = false;  // freeze asked for, pause absent
        float             armedHeading_ = 0.0f;   // B-7: body heading snapshot at arm (pinned per tick)
        // B-7 v3 preview spin: SPIM's freeRotation writes are harvested as
        // input, the camera stays parked, and the smoothed yaw is composed
        // onto the player's root node above whatever the graph wrote.
        float             freeRotArm_ = 0.0f;     // camera park value (captured at arm)
        float             spinTarget_ = 0.0f;     // accumulated drag intent (radians)
        float             spinYaw_ = 0.0f;        // eased value applied to the node
        bool              spinBasisValid_ = false;
        std::atomic<bool> spinDragging_{ false };  // F-14: right mouse held (input sink)
        std::atomic<float> spinStickX_{ 0.0f };    // F-14 v3: right-stick X, direct (no hold button)
        std::atomic<RE::INPUT_DEVICE> lastInputDevice_{ RE::INPUT_DEVICE::kKeyboard };  // preview prompt label
        std::atomic<int>  inputEvidence_{ 0 };     // F-14 v3: raw input events left to log this arm
        std::string       currentMenuName_;        // last-opened bubble menu (own-view coverage)
        RE::NiMatrix3     rootBaseRotate_{};      // root local rotation at arm (pin heading)
        // r54: spin the HORSE with the rider (same yaw; a mounted rider
        // shares the horse's x,y, so a z-yaw about each node's own origin
        // keeps them together). Captured at arm, restored at disarm.
        RE::NiPointer<RE::NiAVObject> spinHorseRoot_;
        RE::NiMatrix3     horseBaseRotate_{};
        int               dipPhase_ = 0;          // F-12: 1 = black since open, fade-in due at arm
        int               dipHoldFrames_ = 0;     // F-12 v4: hold black N frames post-build, then reveal
        int               exitPhase_ = 0;         // F-12 v4 exit: 0 none, 1 hold (switch window), 2 fading to black
        std::uint64_t     exitQpc_ = 0;
        bool              pendingExitMoveStart_ = false;  // B-8 v2: close-time mirror deferred past the switch gap
        std::uint64_t     exitMoveQpc_ = 0;
        bool              pendingOwnViewRestore_ = false;  // F-15 r35: view restore deferred past the switch gap
        std::uint64_t     ownViewRestoreQpc_ = 0;
        bool              pendingWeaponRestore_ = false;  // F-26: preview sheathe deferred past the switch gap
        std::uint64_t     weaponRestoreQpc_ = 0;
        std::uint32_t     appliedRevision_ = 0;  // settings revision the studio look was built from
        int               appliedMode_ = 0;      // view mode the current culls were swept under (B-5)
        // r19c: the covered menus we actually COUNTED at their open event. The
        // close path decrements against this, never against the live predicate -
        // see the note at the menu event handler.
        std::unordered_set<std::string> countedMenus_;
        int               orphanFrames_ = 0;  // consecutive frames counted-but-not-actually-open
        bool              loggedUnpausedOnce_ = false;
        // r18: the arm decision is LATCHED per menu session, never re-read from
        // the global pause each frame. See the note in OnFrame.
        bool              sessionDormant_ = false;
        // r28b: the force-pause value the CURRENT session decided against. r18
        // latches the arm decision per session so an incidental pause change
        // cannot rebuild the scene ten times, and that is still right - but a
        // user ticking "keep these menus unpaused" IN the panel is not
        // incidental, and the panel lives inside the very menu whose session
        // did the latching. Without this the toggle silently does nothing until
        // you close and reopen, which is how it was reported.
        bool              lastForcePause_ = true;
        // r28g: latched at open. TRUE = this session is a menu Skyrim Souls
        // keeps live and the studio entered it for LIGHTING ONLY - never
        // paused, never armed, rig + colour filter and nothing else. Exists
        // because ShouldBubbleMenu drops Souls-live menus at the entry point,
        // which made every fresh open invisible to the r28 live-studio path:
        // the code lived in a dormant branch only reachable while a session is
        // counted, and no session was ever created. Two field runs produced
        // 25-line stub logs - the fingerprint of that silent early return.
        bool              sessionLiveOnly_ = false;
        std::uint64_t     lastQpc_ = 0;
        std::uint64_t     armedTicks_ = 0;
        std::uint64_t     telemetryCountdown_ = 0;
    };
}
