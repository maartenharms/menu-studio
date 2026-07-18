# Changelog - Menu Studio

## 0.6.0 (2026-07-18)

- Fixed bartering, pickpocketing and looting being zoomed in strangely, with the camera and your character in the wrong place. Show Player In Inventory frames your own inventory and magic menus, but it never handles containers, and it only handles bartering when you switch that on in its settings. The menus it leaves alone fall to Menu Studio, which was framing them with a different mod's camera preset: a 90 degree field of view against its 60, and different offsets. Menu Studio now reads Show Player In Inventory's own camera settings and uses them, so the menus it frames look like the ones it does not.
- Turning bartering on in Show Player In Inventory's settings now takes effect straight away. Its setting was only read once at startup, so switching it on mid-session left both mods framing the barter menu at the same time.
- Opening a menu while your character is blinking no longer catches them with their eyes closed. Roughly one open in ten landed inside a blink, and since the world freezes for the menu you were left looking at shut eyes until the blink finished. The blink is now released the instant the menu opens, so you always get open eyes, and natural blinking carries on a moment later. Sleeping characters, whose eyes are meant to be shut, are left alone.
- You can now choose which menus get the backdrop. Keep the void and the dressing room for your own inventory and magic menus, and let containers and merchants open over the normal world; every menu still pauses and still keeps your character alive and posed. Set it per menu in the settings panel, or with sSpaceMenus in the INI. Unchanged by default.
- Facial expressions now stay on your character in the menus. Expression mods (Conditional Expressions and similar) run on Papyrus, which the menu pause freezes; Menu Studio used to standardize the face to neutral to keep it from sticking. It now keeps the mood expression you walked in with and finishes whatever the pause caught mid-motion instead of letting it stick: a half-blink releases, a mouth frozen mid-shape settles closed, off-center eyes come back to level, and natural blinking continues. The old neutral-face behavior is still available (`iFaceInMenus=2`), and everything hands back to your expression mods the moment the menu closes.
- Fixed the world staying frozen after closing a menu on Skyrim 1.6 with Skyrim Souls installed. The pause Menu Studio adds for unpaused menus was released by the game itself on SE, but the 1.6 Skyrim Souls build changes the close path and the release went missing, leaving time stopped (console still worked, other menus would not open). Menu Studio now verifies and releases its own pause after every close, on every runtime. This also un-freezes a session that is already stuck the moment you update.
- Hair and cloth physics now work in menus with HDT-SMP Slot 32 Fix for Skyrim 1.6. Menu Studio only drives physics builds it recognises, and that one was missing from the list, so it stepped aside and everything sat still. If a build is still unrecognised the log now prints its id and asks you to report it, so support can be added.
- Fixed the preview spin turning your character the wrong way: dragging left now turns them to their left instead of away from your hand. The right stick was inverted the same way and matches the mouse again.
- With Show Player In Menus installed, Menu Studio now owns the rotation in its menus. SPIM turns your character on the same right-drag, so both rotations used to run at once and the character span about twice as far as you dragged. Its framing is untouched and its rotation returns the moment the menu closes. Set bOverrideSpimRotation=0 (or untick it in the panel) for the old behavior.
- You can now use the studio lighting without the void. Turn the rig on outside the void and the world stays exactly as it is behind you while your character picks up the three studio lights: tick "Also light me in Off / Scene view" under the studio rig, or set bRigWithoutSpace in the INI. These are real lights, so anything standing close by catches them too, and the rig brightness slider dials that back. There is a separate INI switch, bStudioLightWithoutSpace, for the full room treatment, which takes the studio ambient, fog and image space into a room you can still see. Both are off by default, so nothing changes unless you ask for it.
- The third vanilla perk-dome background is now listed as "aurora". It used to carry the name of Bethesda's own mesh file, which really is spelled "teat", and that is not a word anyone wants in a menu. The dome itself is unchanged, and if you already had it picked your choice carries over.
- The settings panel lines up properly now. Every tick box sat on the wrong side of its row and never matched the sliders above and below it, because the mod was overriding FLICK's own layout instead of leaving it alone. Thanks to Fuzzles for catching it.
- "Match time of day and season" now starts off on fresh installs (a setting you saved earlier is respected), so the mood picker and the studio rig respond to your changes out of the box. When you do turn it on, the panel greys out the mood and the three-point rig controls instead of letting you drag sliders that do nothing.

## 0.5.0 (2026-07-15)

Backdrop packs: add your own backgrounds and stages.

- Drop a pack (a small INI plus its meshes) into
  `Data\SKSE\Plugins\MenuStudio\Backdrops\` and it shows up in the Background and
  Stage lists in the menu. A background is a dome mesh (point its texture at your
  own image); a stage is a floor mesh with optional props.
- Ships an example pack as a template, plus a "make your own backdrop" guide in
  the readme.
- Packs are picked by name and fall back to a built-in if you remove one.
- Fixed a crash on Anniversary Edition when opening a menu outdoors: the
  scene-declutter sweep now iterates cell references directly instead of through
  a CommonLib path whose worldspace offset is wrong on AE.
- Physics in menus now works on Anniversary Edition. The driver recognizes every
  CPU variant of the supported FSMP builds, and cloth and hair follow the
  character instead of freezing in place or stretching away as you rotate the
  preview.
- Skyrim Souls RE: the forced re-pause of the posing menus now sets the pause
  flag and adjusts the pause counter by hand instead of through a
  version-specific engine call, so the camera rotation and the live backdrop
  work under Souls and the pause stays balanced at close.
- When Skyrim Souls RE is installed, the settings panel shows a live "Keep these
  menus unpaused" toggle. Checked, it hands the posing menus back to Souls and
  hides the studio options (the studio needs a paused scene); unchecked, Menu
  Studio pauses them and the studio runs. The toggle is hidden when Souls is not
  installed, since menus pause on their own.

## 0.4.0 (2026-07-15)

Now runs on Anniversary Edition. One download for both Skyrim versions.

- **Anniversary Edition support.** Menu Studio is now a single DLL that loads on
  Skyrim SE 1.5.97 and Anniversary Edition (1.6.1130 and newer). It checks the
  runtime and resolves the right addresses for your version.
- **Physics in menus on AE.** The paused-menu physics drives (Faster HDT-SMP hair
  and cloth, CBPC body, Feet of Skyrim foot IK) were ported to AE, so hair, cloth,
  body and feet keep simulating under the pause on both versions. Each drive
  recognises the common current builds and sits a newer one out gracefully.
- On AE the camera-collision easing is off, because the engine inlined the step
  the SE build hooks. The camera behaves as your "show player in menus" mod leaves
  it; everything else is the same on both versions.

## 0.3.0 (2026-07-14)

Backdrops, moods, a colour filter, and a clean instant open. Skyrim SE 1.5.97 only.

- **View modes.** Pick how the menu scene looks: leave the world as it is, keep
  just the room, clear everything into a clean void, or build a full dressing
  room around you.
- **Backdrops.** For the void and dressing room: a flat tintable colour, a
  starfield constellation, or your own 2:1 image (drop it in as
  `textures\mtb\voidshell_g.dds`). An optional lock keeps a custom image framed
  the same way no matter which way you were facing on open.
- **Moods.** Studio-lighting presets, with per-light colour and intensity, that
  can follow the in-game time of day and season, plus a custom void-colour
  picker.
- **Colour filter.** An optional whole-scene grade (colour, saturation and
  brightness), like a photo filter, that works in any view. Off by default; it
  colours the character along with the scene.
- **Clean open.** Opening a menu no longer flashes the world or the sky for a
  frame before the void appears; a resident occluder covers the gap while the
  backdrop draws in.
- **Settings on FLICK.** The config panel moved to the FLICK in-game UI
  framework; the INI still works without it.
- **Feet of Skyrim** foot IK is stood down in the void so the feet keep their
  neutral pose instead of reaching for the hidden ground.
- Mounted support, opt-in inspect spin, and studio rig brightness.

## 0.1.0 (2026-07-11)

First packaged release. Skyrim SE 1.5.97 only (AE port structured, pending).

While a supported menu is open (Container, Barter, Inventory, Magic by default)
the world stays **paused** but **you** stay alive: behavior-graph animation, face
(blinks and expression ramps), HDT-SMP hair and cloth and CBPC body physics all
keep ticking on a real clock, in a decluttered solo view with studio lighting and
a free-orbiting camera.

New in 0.1.0:

- **Force-pause ownership** (`bForcePause`, default on): covered menus pause even
  with Skyrim Souls installed, with no more SkyrimSoulsRE.ini edits. Only the
  menus in `sMenus` are touched; every other menu keeps your Skyrim Souls
  behavior. Implemented through the engine's own by-name pause bookkeeping, so
  open and close stay perfectly symmetric.
- INI schema frozen; settings reload on every save load (edit the INI, load a
  save, done, no relaunch).
- All engine addresses restructured into one SE/AE table for the future AE pass;
  non-SE runtimes are refused loudly instead of crashing.

Carried over (all field-verified): player and face animation under pause, the
FSMP drive (fingerprinted: Faster HDT-SMP 2.5.0, HDT-SMP Slot 32 Fix 1.1), the
CBPC drive, solo "void" declutter with FX-root culling, studio lighting with full
restore, camera collision bypass with a glide-free exit, the right-mouse purchase
guard, seamless menu switching, and quickload-safe restores.

Known limitations: see README (the orbit angle resets on a menu switch, which is
Show Player In Menus re-initialising per menu and out of scope; exterior solo
keeps terrain and sky by design).
