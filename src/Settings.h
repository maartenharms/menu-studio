#pragma once

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
        const char* mesh;  // empty = no dome piece
        float       radius, z;
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

        // View-mode family helper - kill the scattered magic numbers. The VOID
        // FAMILY (cull the world + studio rig + backdrop + gap occluder + studio
        // imagespace) is mode 2 (Void) and mode 3 (Dressing room). The colour
        // FILTER (bColorFilter) is independent of the view mode - it grades any
        // view - so it is not part of this family test.
        [[nodiscard]] bool IsVoidFamily() const {
            return declutterMode == 2 || declutterMode == 3;
        }

        bool  enabled = true;
        bool  tickAnimation = true;   // Spike A
        bool  driveSmp = true;        // Spike B/C
        bool  tickFace = true;        // facegen morphs: blinks, MFG expressions
        bool  tickMagicCasters = true;// equipped-spell hand art under pause (0.2)
        bool  idleInMenus = true;     // B-2: feed Speed=0 so the walk cycle settles to idle
        bool  freezeHeadTracking = true;  // B-3: pin the head-track target straight ahead
        bool  pinBodyHeading = true;  // B-7: retired key, force-disabled at load (rotation upstream)
        bool  neutralExpression = true;  // F-13: ramp the face to neutral at arm, restore at close

        // F-14: the bubble's own body spin (right-mouse drag / right
        // stick) - the ONLY rotation that moves the skeleton, so
        // hair/cloth physics swings with it (a camera orbit moves
        // nothing, so physics correctly stays still). Default ON since
        // r32 (user call after the controller rotation field-confirmed);
        // coexists with the SPII author's camera rotation.
        bool  previewSpin = true;
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
        bool  blockRightMouse = true; // eat right-mouse in bubble menus (UI quick-buy vs rotation)
        bool  bypassCameraCollision = true;  // skip the third-person camera pull-in while armed
        bool  standardizeLighting = true;    // neutral studio light + unified void color (interiors)
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
        float       backdropDomeRadius = 2200.0f;
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

        // Match time & season: pick the whole look (background + rig) from
        // the game clock - dawn/day/sunset/evening/night base preset,
        // tinted by the calendar season. Manual preset + overrides apply
        // when OFF. Default ON (the immersive mode is the product).
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

    private:
        Settings() = default;
    };
}
