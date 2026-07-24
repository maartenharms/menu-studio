#pragma once

#include "BackdropPolicy.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace MTB {
    // Named [Lighting] preset (shared by INI load and the SMF panel).
    // A preset is a complete vibe: background (ambient/directional/fog/
    // DALC fill) AND the three-point rig (per-light color + intensity).
    struct LightPreset {
        struct RigLook {
            RE::Color color;
            float     intensity;
        };
        const char* name;
        RE::Color   ambient, directional, fog, fill;
        RigLook     key, fillLight, rim;
    };

    // One extra set piece of a stage beyond floor + dome (braziers, props,
    // wall segments…). Offsets are world-axis units from the player's feet;
    // fitRadius > 0 rescales the mesh's bound to that size, otherwise the
    // explicit scale applies. tint = follow the look's emissive tint
    // (props like fire usually want their native look).
    struct StagePiece {
        const char* mesh;
        float       fitRadius;
        float       scale;
        float       x, y, z;
        float       yawDeg;
        bool        tint;
    };

    // The BACKGROUND is its own axis (user design, r18): the dome that
    // surrounds you in the Void AND the Dressing room - mix and match
    // with any stage. 'blank' = no dome, just the colored fog void.
    struct BackgroundPreset {
        const char* name;
        const char* mesh;   // the dome/sphere mesh (empty = no dome piece)
        float       radius, z;
        const char* image;  // non-empty: repoint the image sphere to this DDS
        bool        faceCamera;  // lock the background to the camera (images)
        float       yaw;         // extra rotation (degrees) when locked
    };

    // Named backdrop stage: the floor (panel-dialable via the fields
    // below) plus fixed extra set pieces - dressing room only. Picking
    // one copies the floor into the backdrop fields (same pattern as
    // LightPreset); hand-edits become per-key overrides on top.
    struct StagePreset {
        const char* name;
        const char* floorMesh;
        float       floorRadius, floorZ;
        std::span<const StagePiece> extras;
    };

    // INI-backed settings. Data/SKSE/Plugins/MenuStudio.ini
    // (MO2: overwrite/ wins). Loaded at kDataLoaded and re-read on every
    // save load; Save() writes the panel-edited keys back (load-modify-save
    // so the shipped comments survive).
    class Settings {
    public:
        static Settings& GetSingleton();

        static std::span<const LightPreset>      LightPresets();
        static std::span<const StagePreset>      StagePresets();
        static std::span<const BackgroundPreset> BackgroundPresets();

        // Copies the named stage's floor into the backdrop fields.
        bool ApplyStagePreset(std::string_view a_name);
        // Copies the named background's dome into the backdrop fields.
        bool ApplyBackgroundPreset(std::string_view a_name);

        // The active stage's extra set pieces (empty for unknown stages).
        [[nodiscard]] std::span<const StagePiece> ActiveStageExtras() const;

        void Load();
        void Save();

        // Sets the four colors + lightPreset from a named preset.
        bool ApplyLightPreset(std::string_view a_name);

        bool IsBubbleMenu(const std::string& a_menuName) const {
            return menus.contains(a_menuName);
        }

        // §4b: is this menu handed back to Skyrim Souls - live, unpaused, no
        // studio? Only ever true when Souls is actually loaded, because
        // force-pause exists solely to undo Souls' unpausing: without Souls
        // every covered menu already carries kPausesGame and there is nothing
        // to hand back.
        //
        // The conflict this settles is real and irreducible per menu - the
        // studio needs a frozen scene, Souls exists to keep menus live, and no
        // single menu can be both. What it does NOT have to be is all-or-
        // nothing, which is what it was before.
        //
        // Two layers on purpose, so no existing INI changes meaning on update:
        // bForcePause=0 is the OLD all-or-nothing opt-out and still hands over
        // every menu, while sSoulsLiveMenus refines things per menu when
        // force-pause is on. An empty list is exactly the pre-0.6.1 behaviour.
        [[nodiscard]] bool IsSoulsLiveMenu(const std::string& a_menuName) const {
            if (!soulsLoaded) {
                return false;
            }
            return !forcePause || soulsLiveMenus.contains(a_menuName);
        }

        // The single question the bubble asks when a menu opens. The two halves
        // stay separate above because the panel needs the CONFIGURED set to
        // draw its per-menu list, while this is the EFFECTIVE answer.
        [[nodiscard]] bool ShouldBubbleMenu(const std::string& a_menuName) const {
            return IsBubbleMenu(a_menuName) && !IsSoulsLiveMenu(a_menuName);
        }

        // View-mode family helper - kill the scattered magic numbers. The VOID
        // FAMILY (cull the world + studio rig + backdrop + gap occluder + studio
        // imagespace) is mode 2 (Void) and mode 3 (Dressing room). The colour
        // FILTER (bColorFilter) is independent of the view mode - it grades any
        // view - so it is not part of this family test.
        [[nodiscard]] bool IsVoidFamily() const {
            return declutterMode == 2 || declutterMode == 3;
        }

        // F-24 (field request: "custom lighting without a scene - Natural
        // world behind me + this mod's lighting"). The rig and the cell-light
        // override were both welded to the void family, so the only way to get
        // studio light was to lose the world. They are NOT one feature: the rig
        // ADDS three lights around the character and leaves the cell alone,
        // while the override REWRITES the room's own ambient/fog and flattens
        // its imagespace. Reading the ask as "the lighting, minus the void"
        // could mean either, so each piece gets its own opt-in instead of one
        // shared flag guessing which. Both default off - an existing install
        // sees no change on update.
        //
        // Safe outside the void BY CONSTRUCTION: bCutCellLights lives inside
        // Declutter::Refresh's IsVoidFamily branch (which returns before the
        // mode 0/1 path is reached), so freeing these can never drag the
        // light-source cull along and darken the world that is still visible.
        // r28: `liveStudioActive` is the LIVE-MENU case (Skyrim Souls keeping a
        // menu unpaused). It is a runtime flag Bubble sets for the duration of
        // such a session, never an INI key - see studioInLiveMenus below.
        //
        // The rig is the ONE piece that is safe here, and safe for the same
        // reason it is safe outside the void: it adds three lights around the
        // character and touches nothing else. No pause, no culling, no
        // rewriting the room. Nothing to restore but the lights.
        [[nodiscard]] bool RigAllowed() const {
            return studioRig && (IsVoidFamily() || rigWithoutSpace || liveStudioActive);
        }

        [[nodiscard]] bool CellLightAllowed() const {
            return standardizeLighting && (IsVoidFamily() || studioLightWithoutSpace);
        }

        // Does this menu get the SPACE (void / dressing room)? Field request
        // (NymerethRole, 2026-07-18): "would love it even more if it would be
        // possible to just have it when I open my character menu - with npcs
        // and followers I dont necessarily need it." So the backdrop becomes
        // per-menu while the rest of the bubble (pause, physics, the live
        // character) still applies everywhere in sMenus.
        [[nodiscard]] bool MenuWantsSpace(const std::string& a_menuName) const {
            return spaceMenus.contains(a_menuName);
        }

        // The CONFIGURED space mode (INI + panel). `declutterMode` below holds
        // the EFFECTIVE one for the menu currently open - the bubble sets it at
        // arm from this value and MenuWantsSpace(), and restores it at disarm.
        // Consumers keep reading `declutterMode` and get the per-menu answer
        // for free; only Load/Save/panel touch this one.
        int declutterModeIni = 2;
        // Menus that get the space; default = the same four the bubble covers.
        std::unordered_set<std::string> spaceMenus{ "ContainerMenu", "BarterMenu",
                                                    "InventoryMenu", "MagicMenu" };

        bool  enabled = true;
        bool  tickAnimation = true;   // Spike A
        bool  driveSmp = true;        // Spike B/C
        bool  tickFace = true;        // facegen morphs: blinks, MFG expressions
        bool  tickMagicCasters = true;// equipped-spell hand art under pause (0.2)
        bool  idleInMenus = true;     // B-2: feed Speed=0 so the walk cycle settles to idle
        // F-26: draw the equipped weapon while a bubbled menu is open so it is
        // visible in the character's hand, and sheathe it again on close. ON by
        // default (direct field request); off restores byte-identical behaviour.
        bool  weaponPreviewInMenus = true;
        // r20b: DRAW A SHEATHED WEAPON? OFF by default as of 2026-07-20.
        //
        // Auto-drawing is the only way this feature can ever OWE a sheathe, and
        // an unpaid sheathe wrote a broken weapon state into the user's SAVE:
        // reload showed empty hands with the weapon on the hip, because the
        // save recorded a drawn state we manufactured and the rebuilt animation
        // graph put the model back on its sheath node. Off, the preview only
        // MIRRORS - if the weapon was already out you see it, and if it was
        // sheathed it stays on the hip - so no debt exists to leak.
        //
        // Set 1 for the old always-draw behaviour. Weapon SWAP handling is
        // unaffected either way; it has never depended on us having drawn.
        bool  autoDrawInMenus = false;
        bool  freezeHeadTracking = true;  // B-3: pin the head-track target straight ahead
        bool  pinBodyHeading = true;  // B-7: retired key, force-disabled at load (rotation upstream)
        // F-13 / F-16: what happens to the FACE while a bubble menu is up
        // (expression mods run on Papyrus, which the pause freezes):
        //   0 = hold as caught (can stick a half-finished blink all menu)
        //   1 = LIVE (default): keep the caught expression (Conditional
        //       Expressions' exprOverride stays up), natural blinking runs,
        //       a blink the pause caught halfway is released
        //   2 = neutral: save + dissolve to neutral at arm, restore at close
        int   faceInMenus = 1;

        // F-14: the bubble's own body spin (right-mouse drag / right
        // stick) - the ONLY rotation that moves the skeleton, so
        // hair/cloth physics swings with it (a camera orbit moves
        // nothing, so physics correctly stays still). Default ON since
        // r32 (user call after the controller rotation field-confirmed);
        // coexists with the SPII author's camera rotation.
        bool  previewSpin = true;
        // r61: Show Player In Menus rotates on RIGHT-MOUSE HELD too (a player
        // turn plus a camera counter-turn), so on a SPIM setup one drag drove
        // both its rotation and our spin. When SPIM is loaded, neutralise its
        // rotation and let our spin own the character. Escape hatch for a
        // SPIM user who wants the old combined behavior back.
        bool  overrideSpimRotation = true;
        float spinSensitivity = 0.005f;       // radians per mouse count
        int   spinGamepadButton = 274;        // hold to rotate (274 = left shoulder)
        float spinStickSensitivity = 3.0f;    // radians/second at full right-stick deflection

        // Preview-item prompt (2026-07-13): a persistent, device-aware on-screen
        // hint that the highlighted item can be tried on in the dressing room.
        // The try-on itself is Apparel Preview's (it owns the key / R3 press); this
        // is only the visible affordance. Labels are DISPLAY-ONLY text - set them to
        // match your Apparel Preview binding (keyboard default = its inspect key,
        // fallback C; gamepad default = R3 / right-stick click).
        bool        showTryOnPrompt = true;
        std::string tryOnLabelKbd = "C";
        std::string tryOnLabelPad = "R3";

        // F-15: a first-person arm has no character to preview (the SPII
        // build misses this on barter). The bubble provides the third-
        // person view itself and puts first person back at the exit.
        bool  ownViewFirstPerson = true;
        // F-15 phase 2: own the framing on third-person arms too whenever
        // no loaded view mod covers the menu (this profile: SPII Preview
        // skips barter + container). The framing itself is SPIM's recipe;
        // offsets below match the Nolvus SPIM preset and are superseded by
        // SPIM's own MCM files when those exist in the VFS.
        bool  ownViewUnmanaged = true;
        float ownViewXOffset = -20.0f;   // over-shoulder X = -this - 75
        float ownViewYOffset = 50.0f;    // boom length = 155 - this
        float ownViewZOffset = 0.0f;     // over-shoulder Z = this - 50
        float ownViewPitch = 0.0f;       // player pitch = 0.2 + this
        float ownViewRotation = 0.0f;    // face-the-player yaw = pi + this - 0.5
        // Mounted-only framing deltas, applied ON TOP of the standing framing
        // (a rider sits ~120u up on the horse, so the standing offset frames
        // the legs). Raise the look-at to the rider's torso, add boom to fit,
        // optionally tilt. Boom default trimmed from the r53 +140 (which read
        // as a tiny rider lost above a big horse) toward the standing look.
        float ownViewMountRaise = 120.0f;  // over-shoulder Z += this when mounted
        float ownViewMountBoom  = 50.0f;   // boom (vanity dist) += this when mounted
        float ownViewMountPitch = 0.0f;    // player pitch += this when mounted

        // F-12: fade the studio (stage pieces, rig lights) in at arm and
        // out through the teardown grace instead of popping. Durations in
        // seconds; 0 = instant (identical to the pre-F-12 behavior).
        bool  sleekTransitions = true;
        float transitionInSeconds = 0.35f;
        float transitionOutSeconds = 0.15f;

        // F-12 v3: cut the screen to black IN the menu-open call stack (the
        // skills-menu / RaceMenu look) - the UI never appears over the
        // dressed world; the studio builds behind the black and blooms in
        // under the fade-in. Engine FaderMenu (UI-clocked, pause-immune).
        // Void + dressing room only. Out-duration 0 = instant cut (field
        // r24: any visible world under the menu UI reads as jarring).
        bool  dipToBlack = true;
        float dipOutSeconds = 0.0f;
        float dipInSeconds = 0.35f;

        // r38 SLEEK EXIT (F-12's close half, unparked on user ask "the exit
        // transition is shit"): hold the studio through the switch window,
        // engine fader to black, ALL restores under the black, fade back
        // in - the skills menu's own exit choreography (its ProcessMessage
        // close path drives the same fader). Close-side only; the open dip
        // stays parked (r27). Switches cancel in the hold phase for free.
        bool  sleekExit = true;
        // r48 (user spec: "not even visible the moment we switch out"):
        // hold 0 = the cut happens AT the close event - nothing of the
        // studio survives into a single post-menu frame. The cost is a
        // few frames of world between menu SWITCHES; raise the hold to
        // ~0.085 to trade back (gaps measure 52-71 ms).
        float exitHoldSeconds = 0.0f;
        float exitDipSeconds = 0.12f;    // retired r47 (fader deleted); parsed, unused
        float exitInSeconds = 0.20f;     // retired r47 (fader deleted); parsed, unused
        bool  forcePause = true;      // §3.1: re-pause bubble menus Skyrim Souls unpaused
        // SkyrimSoulsRE.dll present this session (read-only; set during Load).
        // Drives the force-pause default when the INI does not say, and makes
        // "is Souls even loaded" answerable from a log or the settings panel.
        bool  soulsLoaded = false;
        // r19: under Skyrim Souls, hold the pause with our OWN pausing menu
        // (Souls only knows vanilla menu names) instead of Main::freezeTime.
        // freezeTime freezes the clock but leaves the game in a non-pausing-menu
        // state, which leaks gameplay camera input. Set bShadowPause=0 to fall
        // back to the r17 freeze if this misbehaves.
        bool  shadowPause = true;
        bool  blockRightMouse = true; // eat right-mouse in bubble menus (UI quick-buy vs rotation)
        bool  bypassCameraCollision = true;  // skip the third-person camera pull-in while armed
        bool  standardizeLighting = true;    // neutral studio light + unified void color (interiors)
        bool  studioLightWithoutSpace = false;  // F-24: also standardize in Off / Scene view
        bool  verboseLog = true;      // per-second instrumentation while armed
        float maxDeltaTime = 0.05f;   // dt clamp (seconds)

        // The space around the player while armed.
        // 0 = off, 1 = SCENE VIEW (room stays with its own lighting; NPCs
        // + furniture hidden), 2 = VOID (everything hides; studio lighting
        // + rig, NO stage), 3 = DRESSING ROOM (the stage builds in the
        // void). Migration: pre-r17 mode 2 with bBackdrop=1 becomes 3.
        int   declutterMode = 2;   // r45 (user): the VOID is the default view
        float soloHideRadius = 4096.0f;    // modes 1+2: units around the player
        bool  hideLightRefs = true;        // mode 2: hide flame/smoke art on light refs too
        bool  cutCellLights = true;        // r33: self-cull cell NiLights (illumination off)
        bool  voidEngine = true;           // r37 F-20: cull grass/land/LOD/precip at their
                                           // engine roots + zero frozen imods (modes >=2)

        // [Filter] - an optional colour grade over the whole menu scene, in ANY
        // view (a uniform imagespace wash: colour tint + saturation + brightness,
        // delivered through the ImageSpaceManager base override in SceneTint.cpp).
        // OFF by default. It grades everything in frame, the character INCLUDED -
        // a true world-not-character filter needs a stencil / two-layer
        // compositor pass the plugin can't do (deferred). Defaults to a warm,
        // gently-faded look for whoever wants it.
        bool      colorFilter = false;             // master toggle (off by default)
        RE::Color tintColor{ 200, 170, 120, 0 };   // warm sepia wash
        float     tintStrength = 0.5f;             // 0..1 tint amount (TNAM)
        float     tintSaturation = 0.6f;           // 0..1 (CNAM, washed look)
        float     tintBrightness = 0.9f;           // 0..1.5 (CNAM); 1.0 = neutral

        // r39: graph BOOLs that freeze the arm like an attack when true -
        // dodge mods read as plain locomotion otherwise (TK Dodge RE/TUDM
        // publish bIsDodging; DMCO spellings carried; unknown names are
        // free no-ops). INI sFreezeGraphBools, comma-separated.
        std::vector<std::string> freezeGraphBools{ "bIsDodging",
                                                   "DMCO_IsDodging",
                                                   "bDMCO_IsDodging" };

        // DIAGNOSTIC (2026-07-20). Extra graph variables dumped at every pump
        // boundary and on a live weapon change. The variable that selects the
        // VISIBLE idle is demonstrably NOT iRightHandEquipped - field 12:20:22
        // read `iRightHandEquipped=2 weaponType=2` while the character stood in
        // the two-handed idle holding a dagger - so this is a net cast for one
        // that does track it. Names below are CANDIDATES, not confirmed: an
        // unknown name fails all three typed reads and costs nothing, which is
        // exactly what makes widening the list safe. INI sDiagGraphVars,
        // comma-separated, so a new name can be tried without a rebuild.
        std::vector<std::string> diagGraphVars{ "iLeftHandEquipped",
                                                "iState",
                                                "iSyncIdleLocomotion",
                                                "iWantBlockBash",
                                                "bEquipOk",
                                                "bIsEquipping",
                                                "bIsUnequipping",
                                                "bMotionDriven",
                                                "bIsSynced",
                                                "bWantCastLeft",
                                                "bWantCastRight",
                                                "bIsStaggering",
                                                "bIsBashing",
                                                "bAllowRotation",
                                                "bHeadTracking",
                                                "IsBlocking",
                                                "IsBashing",
                                                "bBowDrawn",
                                                "bIsAttacking" };
        // r20 EXPERIMENT - REFUTED 2026-07-20, kept OFF and kept around.
        //
        // Runs a cross-class swap across REAL frames instead of pumping it
        // inside one, to test whether the in-frame pump was what stopped the
        // graph re-picking the idle. It was not: the field ran a full 146-frame
        // sheathe and 105-frame redraw, neither capped, and the pose was still
        // wrong. Frame boundaries are eliminated.
        //
        // DEFAULT FALSE because with the theory dead this is strictly worse
        // than r13 - same wrong pose, plus ~2s of visible animation per swap.
        // Kept rather than deleted because the code is the cheap half of
        // re-running it if the OAR line of enquiry gives a reason to.
        bool  slowSwapExperiment = false;  // INI bSlowSwapExperiment

        // r22 A/B. TRUE is r13's behaviour: a cross-class swap sheathes fully
        // and redraws, both pumped. FALSE skips the sheathe and falls through
        // to PumpSwap, the path a same-class swap already takes.
        //
        // This exists because r21's event probe found the only difference
        // between a working live swap and a broken menu swap, and it is this
        // detour. The engine NEVER sheathes on a weapon swap: it goes
        // tailCombatIdle -> BeginWeaponDraw -> WeapEquip_Out and stays drawn.
        // Ours drops through MTState / tailMTIdle, the sheathed movement idle,
        // on the way past. The draw prologues differ too - ours raises
        // AnimObjectUnequip + IdleOffsetStop (a draw FROM SHEATHED), the
        // engine's raises DisableBumper (a REPLACE WHILE DRAWN). Two different
        // transitions, not one with extra steps in front.
        //
        // ⚠ DEFAULT FLIPPED TO FALSE IN r26, ON FIELD EVIDENCE. With r25 on,
        // the log shows every cross-class swap running TWICE: our detour goes
        // first and STILL picks the wrong clip (2HW_Equip for an Iron Dagger),
        // then ~14 ms later the engine's own notification plays the right one
        // (Dag_Equip). r13 is not merely redundant now, it is a wrong-clip
        // transition that runs ahead of the correct one.
        //
        // Kept rather than deleted because it is the fallback if r25 ever has
        // to be turned off, and because deleting it would erase the record of
        // why it existed. r13's original premise - "the engine queues its own
        // replace and it never gets to finish" - is REFUTED: it is not queued
        // and it never starts. See EquipNotifyGate.h for the real chain.
        bool  crossClassSheatheRedraw = false;  // INI bCrossClassSheatheRedraw

        // r25. Let the engine's own equip notification run inside a menu, by
        // answering Unk_B3 "do not bail" while armed. See EquipNotifyGate.h for
        // the whole chain - the short version is that our own kPausesGame menu
        // increments UI::numPausesGame, which is the counter the engine reads
        // to decide to SKIP the notification, so the graph is never told the
        // weapon changed and a forced draw plays the old weapon's clip.
        //
        // ⚠ DEFAULT FLIPPED TO TRUE IN r26. Field-confirmed, and confirmed by
        // the LOG rather than by the symptom: `equip notify gate: r25 ACTIVE`
        // followed by `clip [MENU] Dag_Equip.hkx` where every previous run had
        // `2HW_Equip.hkx`. The path provably ran and provably changed the clip.
        //
        // r26 pairs it with a pump of the engine's own equip clip (see
        // EquipNotifyGate's OnItemEquipped hook) - without that, r25 starts the
        // right animation and lets it play out visibly, which is what the field
        // saw as a bow unsheathing "sometimes".
        bool  liveEquipNotifyInMenus = true;  // INI bLiveEquipNotifyInMenus

        // r27. The animation-event / clip / actor-tick probes that found the
        // wrong-idle bug. OFF for release and not merely quiet: when false the
        // hooks are never INSTALLED at all, so they cost nothing.
        //
        // That matters for the clip probe in particular - it sits on
        // hkbClipGenerator::Activate, which every actor in the cell goes
        // through. Cheap per call, but it is not a thing to leave in a shipped
        // build for no reason. The event probe and the tick probe also write a
        // line per graph event, which is log spam a user would report.
        //
        // Kept in the source because they are the only reason r21-r26 got
        // anywhere: the bug was invisible to every value we could read, and
        // these read the graph's actual behaviour instead.
        bool  diagnosticProbes = false;  // INI bDiagnosticProbes

        // Bake the face MESH each armed tick (Offsets::FaceGenApplyMorphs).
        // Without this the face DATA moves and the mesh never does - the
        // paused-menu blink bug. Kill-switch only: it calls an engine
        // function on every armed frame, so a way to turn it off without a
        // rebuild is worth six lines.
        bool  faceMeshRefresh = true;   // INI bFaceMeshRefresh

        // Blink stress test: caps the ENGINE's own blink countdown to 0.5 s so
        // the character blinks ~twice a second. Forges nothing - it only
        // shortens a timer - and it is what made the paused-menu blink fix
        // confirmable by eye instead of by impression.
        //
        // It used to ride on bDiagnosticProbes, which was wrong: the probes are
        // the animation instrumentation and this is a face stress test, so
        // turning the first on to chase a graph bug also made the face strobe.
        // Undocumented, its own key.
        bool  blinkStressTest = false;  // INI bBlinkStressTest

        // Stop each weapon-preview pump the moment the draw/sheathe it is
        // driving reaches an idle, instead of running a fixed step budget past
        // it. See the pump helper in WeaponPreview.cpp for the field evidence.
        //
        // ⚠ DEFAULT OFF ON PURPOSE, for now. The measurement that selects this
        // fix (idle picks inside pumps vs on ticked frames) has not been taken
        // on the repro yet, and this channel has shipped a broad fix for an
        // unreproduced symptom twice. Run the repro once with it off, read the
        // "idle session:" line, THEN turn it on and run the same repro again -
        // the same build answers both, so it costs one launch and no rebuild.
        bool  pumpStopsAtIdle = true;  // INI bPumpStopsAtIdle

        // Let a menu armed mid-draw WHILE MOVING skip the draw/sheathe hold.
        // Default OFF: it hands the arm to a "locomotion settle" that does not
        // settle, and the caught walk clip plays on for the whole menu. See the
        // latch's own note in Bubble.cpp.
        bool  movingArmStandsAside = false;  // INI bMovingArmStandsAside

        // ── The two freezes a user might reasonably want to switch off. ──────
        //
        // Menu Studio freezes a caught pose whenever letting it animate would
        // look worse than holding it. Mid-air, mid-attack, furniture and
        // scripted idles are not negotiable: there is nothing to settle into and
        // ticking them produces nonsense. These two ARE arguable, because both
        // trade a live pose for a still one to dodge an engine behaviour, and
        // some people would rather have the live pose and take the jank.
        bool  freezeUnsettledPose = true;  // INI bFreezeUnsettledPose
        bool  freezeDrawSheathe   = true;  // INI bFreezeDrawSheathe

        // FREEZE THE CHARACTER (opt-in): hold the exact frame the menu caught -
        // no behaviour graph, no settle-to-idle, no face. Hair and cloth, body
        // physics, spell hand art and the weapon preview all stay live, so the
        // preview still reacts to a spin and still shows what you equip; only
        // the character's own ANIMATION stops. Off by default: the live preview
        // is the mod's whole point, and this is for people who want a still.
        bool  freezeCharacter = false;  // INI bFreezeCharacter
        bool  driveCbpc = true;            // CBPC body physics while paused (fingerprinted)

        // Studio-light look, resolved at load from [Lighting] sPreset +
        // per-channel overrides (StudioLight consumes these; only applies
        // in void mode - scene view keeps the cell's own lighting).
        RE::Color lightAmbient{ 96, 96, 100, 0 };
        RE::Color lightDirectional{ 160, 155, 150, 0 };
        RE::Color lightFog{ 13, 13, 15, 0 };      // = the void color
        RE::Color lightFill{ 80, 80, 84, 0 };     // DALC all-axis fill

        // r45 (user ask: "color picker to change the voidsphere color"):
        // when the override is on, the shell tints to EXACTLY this color -
        // no vibe hue, no luminance cap (the pick is the pick). Off = the
        // capped fog+ambient-hue vibe flow, unchanged.
        bool      voidColorOverride = false;
        RE::Color voidColor{ 13, 13, 15, 0 };
        float     lightFogNear = 3000.0f;
        float     lightFogFar = 6000.0f;
        std::string lightPreset = "studio";
        bool      studioRig = true;        // key/fill/rim point lights in the void
        bool      rigWithoutSpace = false;  // F-24: also light the character in Off / Scene view

        // r28. Keep the studio rig in menus Skyrim Souls holds LIVE.
        //
        // Before this, a live menu made the bubble dormant and the whole panel
        // collapsed to "the studio is off" - a Souls user lost the three-point
        // lighting along with everything else, which is what they actually
        // wanted from the mod. The lighting never needed the pause.
        //
        // Only the RIG. Deliberately NOT the void, the backdrop or the
        // declutter: those hide the world, and a live menu is live precisely so
        // the player can still see what is happening to them. Blanking the
        // scene while a dragon is landing on you is not a feature. The drives
        // (animation, physics, face) are not needed either - they exist only to
        // undo a pause, and Bubble's engineAnimates already stands them down.
        //
        // DEFAULT TRUE: it only ever adds back something that was previously
        // lost, and it costs nothing when the rig itself is off.
        bool      studioInLiveMenus = true;  // INI bStudioInLiveMenus

        // RUNTIME ONLY, never read from or written to the INI. TRUE while a
        // Souls-live menu session is being lit. Same pattern as declutterMode's
        // per-menu override: Bubble owns it for the length of a session.
        bool      liveStudioActive = false;
        float     rigBrightness = 0.2f;    // multiplies each rig light's fade (r59: dimmer default)

        // Per-light three-point controls (colors 0-255; intensity multiplies
        // that light's base fade). Applied LIVE every armed tick.
        struct RigLight {
            bool      enabled;
            RE::Color color;
            float     intensity;
        };
        RigLight rigKey{ true, { 255, 242, 222, 0 }, 1.0f };
        RigLight rigFill{ true, { 184, 199, 230, 0 }, 1.0f };
        RigLight rigRim{ true, { 255, 255, 255, 0 }, 1.0f };

        // Backdrop pieces, demand-loaded from the game's own meshes and
        // cloned render-side - no plugin, no placed refs, no save surface.
        // BACKGROUND (dome fields) surrounds you in Void + Dressing room;
        // STAGE (floor fields + extras) builds in the Dressing room. The
        // fields hold the ACTIVE presets' values (+ INI/panel overrides);
        // empty mesh = piece dropped. Radii are the world size each mesh
        // is SCALED to (bound radius).
        std::string backdropStage = "starlight";
        std::string backdropBackground = "constellation";
        std::string backdropFloorMesh = "clutter\\nightingale\\nightingaleplatform.nif";
        std::string backdropDomeMesh = "interface\\intperkskydome.nif";
        // F-7 v3: opaque SHELL behind the (additive, see-through) star dome
        // - the void's skin, colored per the vibe. Default: OUR OWN shipped
        // mesh (dist/meshes/mtb/voidshell.nif - inverted sphere, unlit
        // effect shader, flat white texture): texturally FLAT, so the void
        // reads one solid color in every cell. The r29/r30 vanilla
        // candidates are convicted - loadscreen sphere (2nd geometry = wall
        // at fit scale), vampire dome (seams + per-cell color shift + its
        // starry texture masquerading as the constellation). Empty = off.
        std::string backdropShellMesh = "mtb\\voidshell.nif";
        // r49 shipped the vanilla stars mesh here; r50 pulled it back OFF
        // by default - fitted to our sphere it renders as a white VEIL
        // over the whole view (field screenshot), not star points. The key
        // stays for experiments (sStarsMesh in [Backdrop]).
        std::string backdropStarsMesh = "";
        float       backdropFloorRadius = 600.0f;
        float       backdropDomeRadius = BackdropPolicy::kBackgroundRadiusDefault;
        float       backdropFloorZ = -10.0f;
        float       backdropDomeZ = 0.0f;
        float       backdropBrightness = 1.0f;  // emissive tint strength
        // r33: hard luminance ceiling on the dome/shell tint - the void is
        // dark in EVERY look and cell; the vibe varies only its hue.
        float       voidBrightnessCap = 0.10f;

        // r57 custom-image composition: lock the background sphere to FACE
        // THE CAMERA so a hand-made texture on the void shell presents its
        // front consistently, no matter which way the player faced when the
        // menu opened (and it tracks if the camera orbits). Off (default) =
        // the world-locked dynamic look the user likes for the star domes.
        // The yaw offset (degrees) rotates the image to taste.
        bool        backgroundFaceCamera = false;
        float       backgroundYawOffset = 0.0f;
        // The active background's image (DDS) wrapped on the image sphere; empty
        // means use the mesh's own baked texture. ApplyBackgroundPreset copies it
        // from the selected background preset.
        std::string backdropBackgroundImage = "";

        // Match time & season ("the mood"): pick the whole look (background +
        // rig) from the game clock - dawn/day/sunset/evening/night base
        // preset, tinted by the calendar season. Manual preset + overrides
        // apply when OFF. Default ON for fresh installs; an explicit saved
        // value still wins when the INI is loaded. The panel disables manual
        // mood and three-point rig controls while this is on.
        bool matchTimeAndSeason = true;

        // What consumers actually render: manual = the fields above;
        // auto = computed from Calendar. Enable flags always follow the
        // user's rig layout.
        struct LookValues {
            RE::Color ambient, directional, fog, fill;
            RigLight  key, fillLight, rim;
        };
        [[nodiscard]] LookValues    CurrentLook() const;
        [[nodiscard]] std::string DescribeTimeAndSeason() const;  // "21:12 Frostfall: warm + autumn"

        // The colour-filter values the SceneTint module renders. A plain snapshot
        // of the fields above (no time/season computation - the filter is manual),
        // mirroring LookValues / CurrentLook so consumers stay decoupled.
        struct TintValues {
            RE::Color color;
            float     strength;
            float     saturation;
            float     brightness;
        };
        [[nodiscard]] TintValues CurrentTint() const;

        // Bumped on every Load()/Save(); the bubble re-applies the studio
        // look mid-arm when it changes (live panel edits).
        std::uint32_t revision = 0;
        std::unordered_set<std::string> menus{ "ContainerMenu", "BarterMenu", "InventoryMenu",
                                               "MagicMenu" };
        // §4b: the menus left LIVE for Skyrim Souls, a subset of `menus`.
        // EMPTY by default, which is precisely the pre-0.6.1 behaviour, so an
        // existing setup is untouched until the user asks for a split.
        std::unordered_set<std::string> soulsLiveMenus;

    private:
        Settings() = default;
    };
}
