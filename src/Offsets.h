#pragma once

namespace MTB::Offsets {
    // §3.4 AE-readiness (docs/SPEC.md): every engine address the plugin uses,
    // in one table, as RELOCATION-style (SE, AE) pairs. AE values of 0 are
    // unresolved placeholders - plugin.cpp refuses every runtime except SE
    // 1.5.97 until the AE pass fills them, so a placeholder can never
    // resolve. AE anchors that ARE known (Main::Update, the dt globals) come
    // from FSMP's own support table and ship now so the pass starts warm.

    // Main::Update - hosts the frame-driver call site.
    inline constexpr REL::RelocationID MainUpdate{ 35565, 36564 };

    // Engine frame-dt globals (Main::UpdateTimers) - telemetry only; the
    // bubble runs on its own QPC clock.
    inline constexpr REL::RelocationID SlowDt{ 523660, 410199 };
    inline constexpr REL::RelocationID RealDt{ 523661, 410200 };
    inline constexpr REL::RelocationID DtVariant3{ 523662, 410201 };

    // BSFaceGenAnimationData::Update - the face tick (SE RVA 0x3C4030).
    inline constexpr REL::RelocationID FaceGenUpdate{ 25983, 0 };

    // Third-person camera position builder (SE RVA 0x850260) - hosts the
    // collision-smoother call CameraGate gates.
    inline constexpr REL::RelocationID CameraPositionBuilder{ 49975, 0 };

    // Studio rig lights - engine factories for formless render-side lights
    // (no LIGH forms, no placed refs, zero save surface; the approach
    // po3's Light Placer proved). Signatures verified by decompile
    // (Tools\re\research\mtb_studiorig.c): AddLight RTTI-checks the light
    // and branches on params.shadowLight/params.fov exactly per the
    // LIGHT_CREATE_PARAMS layout mirrored in StudioRig.cpp.
    inline constexpr REL::RelocationID NiPointLightCreate{ 69582, 0 };     // () -> NiPointLight*
    inline constexpr REL::RelocationID ShadowSceneAddLight{ 99692, 0 };    // (SSN*, NiLight*, params*) -> BSLight*
    inline constexpr REL::RelocationID ShadowSceneRemoveLight{ 99698, 0 }; // (SSN*, NiPointer<BSLight>&)

    // Sky's forced interior-lighting refresh (SE RVA 0x3B4030): when an
    // interior cell is current it re-pulls ambient/directional/fog/DALC
    // from the cell through the inherit-aware blend getters (family at
    // 0x262C00..0x263400), pushes them to the renderer + sun light, then
    // restores the sky mode and re-runs Sky::Update (25682; CalculateColors
    // = 25685). The engine otherwise only does this on cell ATTACH - the
    // reason live INTERIOR_DATA edits never rendered. Gated by a refresh
    // timer at Sky+0x1D4 (write big to force) and a TESWaterSystem flag
    // (irrelevant in menus). Decompile: Tools\re\research\mtb_skyrefresh.c.
    inline constexpr REL::RelocationID SkyForceInteriorRefresh{ 25690, 0 };

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
    // CommonLibSSE-NG's table (unverified here; the SE-only guard holds).
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
    inline constexpr REL::RelocationID FadeOutGame{ 51909, 0 };

    // UI's by-name menu pause setter (SE RVA 0xEBFC10): finds the menu in
    // the registered-menu map, no-ops when kPausesGame already matches,
    // otherwise sets/clears the flag, adjusts UI::numPausesGame and fires
    // the pause-edge event - the exact bookkeeping the UI message pump's
    // open (ID 79945) and close paths run. Raw counter helpers 79942/79943
    // exist but skip the flag, which the close path re-reads; the setter is
    // the symmetric choice. Decompiles: Tools\re\research\mtb_pausecounter.c.
    inline constexpr REL::RelocationID SetMenuPausesGame{ 79952, 0 };

    // Call-site offsets INSIDE the functions above. Both are byte-verified
    // E8 before install; refusal is loud but non-fatal (SPEC §4 invariant 4).
    inline std::ptrdiff_t DispatchCallOffset() {  // Main::Update -> player dispatch (ID 35578)
        return REL::Relocate(std::ptrdiff_t{ 0x28E }, std::ptrdiff_t{ 0 });
    }
    inline std::ptrdiff_t SmootherCallOffset() {  // position builder -> smoother (ID 49980)
        return REL::Relocate(std::ptrdiff_t{ 0x1C5 }, std::ptrdiff_t{ 0 });
    }
}
