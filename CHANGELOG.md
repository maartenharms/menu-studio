# Changelog - Menu Studio

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
