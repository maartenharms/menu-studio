#pragma once

namespace MTB::Offsets {
    // §3.4 AE-readiness (docs/SPEC.md): every engine address the plugin uses,
    // in one table, as RELOCATION-style (SE, AE) pairs. The AE (1.6.1170) half
    // was reverse-engineered against the real binary and each function
    // decompile-matched to its SE twin (Ghidra AE project SkyrimAE_1170), then
    // checked so every id resolves in the 1.6.1170 Address Library and the
    // DispatchCallOffset byte is E8 on both builds. Universal DLL: gate in
    // plugin.cpp accepts SE 1.5.97 || AE 1.6.1130+.

    // Main::Update - hosts the frame-driver call site.
    inline constexpr REL::RelocationID MainUpdate{ 35565, 36564 };

    // Engine frame-dt globals (Main::UpdateTimers) - telemetry only; the
    // bubble runs on its own QPC clock.
    inline constexpr REL::RelocationID SlowDt{ 523660, 410199 };
    inline constexpr REL::RelocationID RealDt{ 523661, 410200 };
    inline constexpr REL::RelocationID DtVariant3{ 523662, 410201 };

    // BSFaceGenAnimationData::Update - the face tick (SE RVA 0x3C4030).
    // AE 26598 (FUN_14041d750): exact decompile-match - same (this,float,char)
    // sig, +0x220 lock, +0x217 return flag, +0x200==4 reset gate, +0x224 dance,
    // and the sub-controller cascade on +0x20..+0x180. Sibling of Reset
    // (25977/26586) + SetExpressionOverride (25980/26594) in CommonLib.
    inline constexpr REL::RelocationID FaceGenUpdate{ 25983, 26598 };

    // Third-person camera position builder (SE RVA 0x850260) - hosts the
    // collision-smoother call CameraGate gates. AE 50911 (FUN_1408e8640):
    // decompile-matched via its caller (49960->50896). NOTE: AE INLINED the
    // collision smoother (SE 49980) into this builder - there is no standalone
    // call, so SmootherCallOffset has no AE E8 site (see below).
    inline constexpr REL::RelocationID CameraPositionBuilder{ 49975, 50911 };

    // Studio rig lights - engine factories for formless render-side lights
    // (no LIGH forms, no placed refs, zero save surface; the approach
    // po3's Light Placer proved). Signatures verified by decompile
    // (Tools\re\research\mtb_studiorig.c): AddLight RTTI-checks the light
    // and branches on params.shadowLight/params.fov exactly per the
    // LIGHT_CREATE_PARAMS layout mirrored in StudioRig.cpp.
    // AE: NiPointLightCreate 70966 (FUN_140d399d0, the plain factory that
    // writes the NiPointLight vtable + allocs 0x150; disambiguated from an
    // identical sibling by reinit-adjacency). AddLight 106326 (FUN_1414a0940,
    // exact match: two-descriptor RTTI check + shadowLight/fov 0x140/0x560/0x578
    // branch). RemoveLight 106332 (FUN_1414a1250, same *param_2 && +0x300 guard
    // + the +6-id adjacency to AddLight).
    inline constexpr REL::RelocationID NiPointLightCreate{ 69582, 70966 };     // () -> NiPointLight*
    inline constexpr REL::RelocationID ShadowSceneAddLight{ 99692, 106326 };   // (SSN*, NiLight*, params*) -> BSLight*
    inline constexpr REL::RelocationID ShadowSceneRemoveLight{ 99698, 106332 };// (SSN*, NiPointer<BSLight>&)

    // Sky's forced interior-lighting refresh (SE RVA 0x3B4030): when an
    // interior cell is current it re-pulls ambient/directional/fog/DALC
    // from the cell through the inherit-aware blend getters (family at
    // 0x262C00..0x263400), pushes them to the renderer + sun light, then
    // restores the sky mode and re-runs Sky::Update (25682; CalculateColors
    // = 25685). The engine otherwise only does this on cell ATTACH - the
    // reason live INTERIOR_DATA edits never rendered. Gated by a refresh
    // timer at Sky+0x1D4 (write big to force) and a TESWaterSystem flag
    // (irrelevant in menus). Decompile: Tools\re\research\mtb_skyrefresh.c.
    // AE 26237 (FUN_14040d780): exact decompile-match - TLS tag 0x5d, timer
    // +0x1d4, mode +0x1bc set-to-3/restore, +0xB8 gate, interior flags +0xf4,
    // two-phase calculate -> Sky::Update, +0x70/+0x88 light-list vfunc+0x18.
    inline constexpr REL::RelocationID SkyForceInteriorRefresh{ 25690, 26237 };

    // TESWaterSystem singleton (AE id from CommonLib) - the refresh wrapper
    // above silently no-ops while its +0xB8 byte is set; StudioLight clears
    // it around the call and restores.
    inline constexpr REL::RelocationID TESWaterSystemSingleton{ 514290, 400450 };

    // Backdrop meshes - BSModelDB::Demand (SE RVA 0xD2F220): resolves
    // "meshes\\<path>" through the model DB and hands back the CACHED
    // template root (entry+0x28, refcount-bumped), shared with every placed
    // ref of that model - clone it, never attach it. NiAVObject deep clone
    // (SE RVA 0xC52750): stack NiCloningProcess wired to the engine's own
    // copyType/appendChar globals, CreateClone + ProcessClone - the same
    // instantiate-a-template path placed refs use. Decompiles:
    // Tools\re\research\mtb_modeldb.c + mtb_niclone.c. AE ids are from
    // CommonLibSSE-NG's table; both resolve in the 1.6.1170 addrlib.
    inline constexpr REL::RelocationID BSModelDBDemand{ 74040, 75782 };
    inline constexpr REL::RelocationID NiAVObjectClone{ 68835, 70187 };

    // The engine's screen fade - the function behind the Papyrus native
    // Game.FadeOutGame (SE RVA 0x8D5530): looks up the FADER MENU via
    // InterfaceStrings+0x08, writes direction (+0x24, stored inverted),
    // black flag (+0x25), duration (+0x20), then starts the fade. It is a
    // MENU - its Scaleform movie advances on UI time, so the fade animates
    // while the game is paused (why RaceMenu's and the skills menu's dips
    // work in paused contexts - the exact look F-12's arm dip borrows).
    // ABI byte-verified from the native wrapper (0x9724F0): RCX=fadingOut,
    // RDX=blackFade, XMM2=duration, R9=unk(0), stack=secsBeforeFade.
    // Decompiles: Tools\re\research\mtb_fadeout*.c.
    // AE 52847 (FUN_1409768e0): exact decompile-match - TLS tag 0x46,
    // InterfaceStrings menu lookup, the five field writes +0x24(inverted
    // dir)/+0x25/+0x26/+0x20(duration)/+0x1c, single tail call to start the fade.
    inline constexpr REL::RelocationID FadeOutGame{ 51909, 52847 };

    // UI's by-name menu pause setter (SE RVA 0xEBFC10): finds the menu in
    // the registered-menu map, no-ops when kPausesGame already matches,
    // otherwise sets/clears the flag, adjusts UI::numPausesGame and fires
    // the pause-edge event - the exact bookkeeping the UI message pump's
    // open (ID 79945) and close paths run. Raw counter helpers 79942/79943
    // exist but skip the flag, which the close path re-reads; the setter is
    // the symmetric choice. Decompiles: Tools\re\research\mtb_pausecounter.c.
    // AE 82089 (FUN_140fa5870): exact decompile-match - +0x1c flag bit&1 =
    // kPausesGame, +0x160 numPausesGame ++/--, the (bit != arg) no-op guard, the
    // pause-edge event fired on this+0x60. (The sibling 82090 uses bit 0x8000 /
    // counter +0x174 - a different setter.) Matches the menu-pump (79945/82082)
    // +7 adjacency.
    inline constexpr REL::RelocationID SetMenuPausesGame{ 79952, 82089 };

    // Call-site offsets INSIDE the functions above. Both are byte-verified
    // E8 before install; refusal is loud but non-fatal (SPEC §4 invariant 4).
    // Dispatch: Main::Update -> player dispatch. SE +0x28E, AE +0x2A7 (MOVED;
    // byte E8 confirmed on both builds, targets 35578 / 36581).
    inline std::ptrdiff_t DispatchCallOffset() {  // Main::Update -> player dispatch (ID 35578 / AE 36581)
        return REL::Relocate(std::ptrdiff_t{ 0x28E }, std::ptrdiff_t{ 0x2A7 });
    }
    // Smoother: SE position builder calls the collision smoother (ID 49980) at
    // +0x1C5. On AE the engine INLINED the smoother into the builder (50911) -
    // there is no standalone call, so no E8 site exists. AE offset stays 0: the
    // builder-relative byte there is not E8, so CameraGate's byte-check declines
    // and the camera-collision bypass is simply OFF on AE (documented
    // limitation; no crash). The rest of Menu Studio, incl. physics, is AE-live.
    inline std::ptrdiff_t SmootherCallOffset() {  // position builder -> smoother (SE ID 49980; AE inlined)
        return REL::Relocate(std::ptrdiff_t{ 0x1C5 }, std::ptrdiff_t{ 0 });
    }
}
