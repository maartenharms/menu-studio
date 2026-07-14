# Third-party notices

Menu Studio is licensed GPL-3.0 (see [LICENSE](LICENSE)). It builds on the following third-party components; each remains under its own license.

## Vendored headers (in `extern/`)

| File | Origin | License |
|---|---|---|
| `FUCK_API.h` | [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) by Fuzzles | GPL-3.0 (project license; the header itself carries no separate notice) |

`FUCK_API.h` declares a C-ABI that binds at runtime against `FUCK.dll` through `GetModuleHandle`/`RequestFUCK`; no third-party object code is linked into this plugin. It references [Dear ImGui](https://github.com/ocornut/imgui) types (MIT), pulled in as a build dependency below. FLICK is a soft dependency: without it the mod is fully configurable through its INI.

## Build dependencies (via vcpkg)

| Component | License |
|---|---|
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | MIT |
| [xbyak](https://github.com/herumi/xbyak) | BSD-3-Clause |
| [simpleini](https://github.com/brofield/simpleini) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [fmt](https://github.com/fmtlib/fmt) | MIT, with binary-embedding exception |
| [spdlog](https://github.com/gabime/spdlog) | MIT |

## Reverse-engineered integrations and techniques (documentation, no code copied)

Menu Studio keeps your character's physics and animation running while the menu holds the world paused. It does that by driving a handful of other plugins at runtime, resolved through their shipped debug symbols and public reverse-engineering, never by linking their object code:

- **Faster HDT-SMP** and **HDT-SMP "Slot 32 Fix"** (hydrogensaysHDT, Karonar1, and maintainers; GPL-3.0): the paused-menu SMP drive (suspend then step the simulation on a real clock) was implemented from FSMP's published GPL-3.0 source and its shipped symbols. Per-build fingerprints gate it, so an unrecognized build stays dormant.
- **CBPC - CBP Physics with Collisions** (Shizof): the paused-simulation flag comes from CBPC's shipped PDB; it is set at runtime while a menu is open, then restored.
- **Inverse Kinematics - Feet of Skyrim**: the foot-IK master flag was located in the mod's shipped PDB and is toggled while the void view is active so the feet keep a neutral pose; restored on close.
- **Show Player In Inventory** (ItzIvy05, MIT) and **Show Player In Menus** (derickso): Menu Studio frames the character these mods present. The menu-coverage check follows Show Player In Inventory's own decline conditions, reimplemented from its source.
- The studio light rig uses the formless point-light approach documented by **powerof3's Light Placer**, reimplemented from public reverse-engineering.

The void staging, camera handling, imagespace and lighting work are implemented against CommonLibSSE-NG and public reverse-engineering of the Skyrim SE 1.5.97 runtime. No source code from the projects above is included in this plugin.
