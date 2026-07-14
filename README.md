# Menu Studio

Open a menu in Skyrim and the world freezes around you: whatever clutter, NPCs and harsh light you happened to be standing in, held in place, with your character's hair and cloth seizing up because the game is paused. Menu Studio pauses the world the moment a menu opens but keeps your character alive and moving, then stages the scene around them the way you want, from a clean void to a lit dressing room, so you can actually look at your character.

SKSE plugin for Skyrim Special Edition 1.5.97. It covers the inventory, barter, container and magic menus by default.

## Why

With a "show player in menus" mod you can rotate your character in the inventory and barter menus, but two things get in the way. Under Skyrim Souls (unpaused menus) the dialogue auto-facing keeps re-asserting and your rotation snaps back. In a normal paused menu, HDT/SMP hair and cloth pin in place and explode as you turn. Menu Studio pauses the world so nothing re-asserts your facing, and it keeps the physics ticking so hair and cloth settle instead of exploding, then gives you a clean stage to look at.

## Features

- Pauses the world when a covered menu opens, but keeps your character live: animation, blinks and expression, HDT-SMP hair and cloth, and CBPC body physics all keep running on a real clock.
- Pick how the scene looks: leave the world as it is, keep just the room, clear everything into a clean void, or build a full dressing room around you.
- Backdrops for the void and dressing room: a plain tintable colour, a starfield constellation, or your own image (drop a 2:1 texture in as `textures\mtb\voidshell_g.dds`). An optional lock keeps a custom image framed the same way no matter which direction you were facing when the menu opened.
- A three-point studio light rig so your character reads clearly, with per-light colour and intensity, and moods that can follow the in-game time of day and season.
- An optional colour filter that grades the whole menu scene like a photo filter, in any view. Off by default.
- Frames your character in its own camera, even in first person, and lets you spin to inspect your gear from any angle.
- Works on foot and mounted. Configured from an in-game panel, or the INI.
- No .esp, no scripts, nothing written to your save. Safe to add or remove at any time.

## Requirements

Hard (the plugin checks and refuses to load otherwise):
- Skyrim SE **1.5.97** and [SKSE64](https://skse.silverlock.org/). Anniversary Edition (1.6.x) and VR are not supported yet.
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).

To have a character to frame (pick one; Menu Studio does not add the in-menu view itself, it stages the one these provide):
- [Show Player In Menus](https://www.nexusmods.com/skyrimspecialedition/mods/81291) (all four menus), or
- [Show Player In Inventory](https://www.nexusmods.com/skyrimspecialedition/mods/178689) (inventory and magic).

Recommended:
- [Faster HDT-SMP](https://www.nexusmods.com/skyrimspecialedition/mods/57339) or CBPC, so hair, cloth and body keep simulating while the menu is paused. Without one your character is still shown and posed, just without live physics.
- [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) for the in-game settings panel. Everything is configurable through the INI without it.

Skyrim Souls RE is supported but not required. Menu Studio re-pauses its own covered menus at open time, so you do not need to edit `SkyrimSoulsRE.ini`, and every other menu keeps its Skyrim Souls behaviour.

## Settings

With FLICK installed, open its panel and pick **Menu Studio**; changes apply right away. Without it, edit `Data/SKSE/Plugins/MenuStudio.ini` (created on first run; under MO2 it lands in the overwrite folder). Every feature has a switch: the view mode, the backgrounds, the studio rig and its moods, the colour filter, force-pause, the camera framing and spin, the covered-menu list, and the physics drives. The INI is re-read on every save load, so edits apply without relaunching.

## Compatibility

- **Show Player In Menus / Show Player In Inventory**: the intended companions. Menu Studio frames the character they present and hands back cleanly on close.
- **Faster HDT-SMP and CBPC**: driven under pause so physics keeps running. Both are detected and matched by build; a drive stays dormant, with a log line, if a build is not recognised, and the rest of the mod still works.
- **Inverse Kinematics - Feet of Skyrim**: its foot IK is stood down while the void view is active so the feet keep a neutral pose instead of reaching for the hidden ground, then restored on close. No effect without it.
- **Skyrim Souls RE**: supported. Menu Studio re-pauses its own menus and leaves the rest to Skyrim Souls.
- **Apparel Preview** and **Fitting Room**: built to pair with these; no shared hooks.
- No scripts, no .esp, no save changes, so it cannot break gear, quests or saves, and removing it leaves no trace.

## Known limitations

See [KNOWN-ISSUES.md](KNOWN-ISSUES.md). In short: the optional colour filter tints your character too; AE and VR are not supported yet; live physics needs Faster HDT-SMP or CBPC; the camera angle can reset when you switch straight from one menu to another; and outdoors keeps the terrain and sky by design.

## How it works

When a covered menu opens, Menu Studio pauses the world through the engine's own pause bookkeeping, then, depending on the view you pick, clears the scene around you, wraps it in a backdrop, and lights it with a small three-point rig. Your character keeps simulating because the mod drives the animation, face and physics systems that normally stop when the game pauses, each on its own real clock. On close, every change is reversed in a single frame, so gameplay continues exactly where it left off. Nothing is equipped, no inventory or save data is touched, and every engine address is resolved through the Address Library.

## Credits

- **Fuzzles**: [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603), the in-game UI framework the settings panel is built on.
- **hydrogensaysHDT, Karonar1 and the Faster HDT-SMP maintainers**, and **Shizof** (CBPC): the physics engines Menu Studio keeps running under pause.
- **derickso** and **ItzIvy05**: Show Player In Menus and Show Player In Inventory, the companion mods this frames and coexists with.
- **powerof3 and the CommonLibSSE-NG contributors**: [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG), and the Light Placer technique the studio rig follows.
- **meh321**: [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).
- **The SKSE team**: the Skyrim Script Extender.

## Building

CMake and vcpkg (`commonlibsse-ng`, `xbyak`, `simpleini`, `imgui`). Run `tools\build.bat` from PowerShell; it configures, builds and deploys. The release FOMOD installer is assembled by `tools/make_fomod.sh`, with the installer metadata in `fomod/`.

## License

GPL-3.0; see [LICENSE](LICENSE). The vendored `extern/FUCK_API.h` (FLICK) is GPL-3.0. Attributions and the licenses of all build dependencies are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
