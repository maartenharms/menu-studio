#include "PCH.h"

#include "Declutter.h"
#include "Settings.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
    // Refs WE culled (only those whose 3D was visible before), to restore
    // exactly and never un-hide something another mod hid. Handles, not
    // pointers: a quickload invalidates them and resolution just fails.
    std::vector<RE::ObjectRefHandle> g_hidden;

    // Interior cell-root children we culled (solo mode). NiPointer keeps the
    // nodes alive until RestoreAll, which always runs before any unload
    // (menu close / disarm / ForceReset at kPreLoadGame).
    std::vector<RE::NiPointer<RE::NiAVObject>> g_hiddenNodes;

    // r33: cell light SOURCES cut this arm (see CutCellLightSources) - the
    // walk is once per arm, not per periodic sweep; RestoreAll re-arms it.
    bool g_cellLightsCut = false;

    // r37 F-20 VOID ENGINE: world feeders cut this arm (grass root, land
    // quadrants, LOD roots, precipitation geometry) - once per arm, the
    // paused world can't grow new ones; RestoreAll re-arms it.
    bool g_worldFeedersCut = false;

    // r37 F-22: active imagespace-modifier instances we zeroed (combat blur
    // frozen mid-ramp over the studio). RAW pointers by necessity -
    // ImageSpaceModifierInstance privately inherits NiObject in NG (the
    // ShadowSceneNode lesson again), so NiPointer won't compile. Safe
    // anyway: restore only touches instances still present in the engine's
    // own live list, so a pointer that died with a quickload is never
    // dereferenced.
    struct ImodSave {
        RE::ImageSpaceModifierInstance* inst;
        float strength;
    };
    std::vector<ImodSave> g_imodSaves;

    // r53: the player's mount stays visible in every cull path (r52 only
    // caught the actor sweep; the ref sweep re-hid the horse → "saddling
    // empty space"). One helper, used by both.
    RE::Actor* PlayerMount(RE::PlayerCharacter* a_player) {
        RE::ActorPtr mount;
        if (a_player && a_player->GetMount(mount)) {
            return mount.get();
        }
        return nullptr;
    }

    bool HideRef(RE::TESObjectREFR* a_ref) {
        auto* root = a_ref ? a_ref->Get3D() : nullptr;
        if (!root || root->GetAppCulled()) {
            return false;
        }
        root->SetAppCulled(true);
        g_hidden.push_back(a_ref->GetHandle());
        return true;
    }

    // AE-safe stand-in for RE::TES::ForEachReferenceInRange. CommonLib-NG's
    // version finishes by walking worldSpace->GetSkyCell() through a TES member
    // whose AE offset it flags as uncertain ("worldSpace // 140 - actual offset
    // change is somewhere near showLandBorder"). On AE exteriors that read comes
    // back as garbage (float/coordinate bytes) and GetSkyCell dereferences it,
    // so the game CTDs the instant the inventory opens outdoors (SE's layout is
    // correct, which is why SE never hit it). We reproduce the interior and
    // grid-cell sweep using only members that are stable across SE and AE
    // (interiorCell 0xC0, gridCells 0x78) and drop the sky-cell pass, which only
    // ever holds distant sky-dome art, never the local clutter this is about
    // (and outdoors the void keeps terrain and sky by design anyway).
    template <class Fn>
    void ForEachRefInRangeSafe(RE::TES* a_tes, RE::PlayerCharacter* a_origin,
                               float a_radius, Fn a_fn) {
        if (!a_tes || !a_origin || a_radius <= 0.0f) {
            return;
        }
        const auto originPos = a_origin->GetPosition();
        auto visit = [&](RE::TESObjectREFR& a_ref) -> RE::BSContainer::ForEachResult {
            return a_fn(a_ref);
        };
        if (a_tes->interiorCell) {
            a_tes->interiorCell->ForEachReferenceInRange(originPos, a_radius, visit);
            return;
        }
        auto* grid = a_tes->gridCells;
        const std::uint32_t gridLength = grid ? grid->length : 0;
        if (gridLength == 0) {
            return;
        }
        const float xPlus = originPos.x + a_radius;
        const float xMinus = originPos.x - a_radius;
        const float yPlus = originPos.y + a_radius;
        const float yMinus = originPos.y - a_radius;
        for (std::uint32_t x = 0; x < gridLength; ++x) {
            for (std::uint32_t y = 0; y < gridLength; ++y) {
                auto* cell = grid->GetCell(x, y);
                if (!cell || !cell->IsAttached()) {
                    continue;
                }
                const auto* coords = cell->GetCoordinates();
                if (!coords) {
                    continue;
                }
                const float wx = coords->worldX;
                const float wy = coords->worldY;
                if (wx < xPlus && (wx + 4096.0f) > xMinus &&
                    wy < yPlus && (wy + 4096.0f) > yMinus) {
                    cell->ForEachReferenceInRange(originPos, a_radius, visit);
                }
            }
        }
    }

    void HideNearbyActors(RE::PlayerCharacter* a_player, float a_radius) {
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return;
        }
        const auto playerPos = a_player->GetPosition();
        RE::Actor* mountPtr = PlayerMount(a_player);  // r52/r53: keep the horse
        int hidden = 0;
        auto sweep = [&](RE::BSTArray<RE::ActorHandle>& a_handles) {
            for (auto& handle : a_handles) {
                auto actor = handle.get();
                if (!actor || actor.get() == a_player || actor.get() == mountPtr) {
                    continue;
                }
                if (actor->GetPosition().GetDistance(playerPos) > a_radius) {
                    continue;
                }
                if (HideRef(actor.get())) {
                    ++hidden;
                }
            }
        };
        sweep(lists->highActorHandles);
        sweep(lists->middleHighActorHandles);
        if (hidden > 0) {
            spdlog::debug("declutter: hid {} actor(s) within {:.0f} units.", hidden, a_radius);
        }
    }

    // Interiors, solo mode: walk from the player's 3D root up to the cell 3D
    // root, culling every SIBLING at every level. Nothing in the cell tree
    // renders except the exact node chain leading to the player - including
    // art that belongs to no reference and shares the player's top-level
    // branch (addon-node candle flames, smoke, sparks: the field survivors
    // of both the per-ref sweep and the branch-level wholesale cull).
    bool HideInteriorPathSiblings(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        if (!cell || !cell->IsInteriorCell()) {
            return false;
        }
        auto* loaded = cell->GetRuntimeData().loadedData;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        auto* playerRoot = a_player->Get3D();
        if (!cell3D || !playerRoot) {
            return false;
        }

        // The player must actually live under this cell root.
        bool underCell = false;
        for (auto* n = playerRoot->parent; n; n = n->parent) {
            if (n == cell3D) {
                underCell = true;
                break;
            }
        }
        if (!underCell) {
            return false;
        }

        int hidden = 0;
        for (RE::NiAVObject* node = playerRoot; node && node != cell3D; node = node->parent) {
            auto* parent = node->parent;
            if (!parent) {
                break;
            }
            for (auto& childPtr : parent->GetChildren()) {
                auto* child = childPtr.get();
                if (!child || child == node || child->GetAppCulled()) {
                    continue;
                }
                // Our own studio-rig lights live as siblings on this exact
                // path - a light whose OWN node is culled is skipped by the
                // light gathering (field-proven: the sweep executed the rig
                // 15 ticks after every arm, log r7). Never cull MTB_ nodes.
                if (const char* nm = child->name.c_str();
                    nm && std::strncmp(nm, "MTB_", 4) == 0) {
                    continue;
                }
                child->SetAppCulled(true);
                g_hiddenNodes.emplace_back(child);
                ++hidden;
            }
            if (parent == cell3D) {
                break;
            }
        }
        // Above the cell: the 2026-07-11 scene dump showed the survivors'
        // homes - UNNAMED siblings of cell3D under 'ObjectLODRoot' and two
        // more unnamed roots under 'shadow scene node' (global particle/FX
        // containers). Cull unnamed siblings for two levels above the cell;
        // named roots (Sky, Weather, LODRoot) stay, and the camera lives a
        // level higher still - never reached.
        RE::NiAVObject* up = cell3D;
        for (int lvl = 0; lvl < 2 && up->parent; ++lvl) {
            auto* parent = up->parent;
            for (auto& sibPtr : parent->GetChildren()) {
                auto* sib = sibPtr.get();
                if (!sib || sib == up || sib->GetAppCulled()) {
                    continue;
                }
                if (const char* nm = sib->name.c_str(); nm && *nm) {
                    continue;  // named engine roots untouched
                }
                sib->SetAppCulled(true);
                g_hiddenNodes.emplace_back(sib);
                ++hidden;
            }
            up = parent;
        }

        if (hidden > 0) {
            spdlog::debug("declutter[solo]: culled {} node(s) (path siblings + "
                          "unnamed FX roots above the cell).", hidden);
        }

        // One-shot structure dump kept for future diagnosis (now also shows
        // which of the culprits are marked culled).
        static bool dumpedOnce = false;
        if (!dumpedOnce) {
            dumpedOnce = true;
            int level = 0;
            for (RE::NiNode* up = cell3D->parent; up && level < 4; up = up->parent, ++level) {
                std::string line;
                for (auto& sib : up->GetChildren()) {
                    if (auto* s = sib.get()) {
                        line += line.empty() ? "" : ", ";
                        const char* nm = s->name.c_str();
                        line += (nm && *nm) ? nm : "<unnamed>";
                        line += s->GetAppCulled() ? "(culled)" : "";
                        if (s == (level == 0 ? static_cast<RE::NiAVObject*>(cell3D) : nullptr)) {
                            line += "[cell3D]";
                        }
                    }
                }
                spdlog::debug("scene above cell3D, level {} '{}': [{}]", level,
                              up->name.c_str() ? up->name.c_str() : "<unnamed>", line);
            }
        }
        return true;
    }

    // r38 (field: "particles like fire embers" in the exterior void): the
    // unnamed FX containers live at the same levels above EXTERIOR cell
    // roots as the interior ones did (r13 scene dump lineage) - but the
    // above-the-cell cull only ever ran for interiors. Same rules: unnamed
    // siblings only (named engine roots - Sky, LODRoot, camera parents -
    // untouched), two levels, shared restore list. Plus the same one-shot
    // structure dump so the next leak names its own home.
    void HideExteriorAboveCell(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        if (!cell || cell->IsInteriorCell()) {
            return;
        }
        auto* loaded = cell->GetRuntimeData().loadedData;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        if (!cell3D) {
            return;
        }
        int hidden = 0;
        RE::NiAVObject* up = cell3D;
        for (int lvl = 0; lvl < 2 && up->parent; ++lvl) {
            auto* parent = up->parent;
            for (auto& sibPtr : parent->GetChildren()) {
                auto* sib = sibPtr.get();
                if (!sib || sib == up || sib->GetAppCulled()) {
                    continue;
                }
                if (const char* nm = sib->name.c_str(); nm && *nm) {
                    continue;  // named engine roots untouched
                }
                sib->SetAppCulled(true);
                g_hiddenNodes.emplace_back(sib);
                ++hidden;
            }
            up = parent;
        }
        if (hidden > 0) {
            spdlog::debug("declutter[solo]: culled {} unnamed root(s) above "
                          "the exterior cell.", hidden);
        }
        static bool dumpedOnceExt = false;
        if (!dumpedOnceExt) {
            dumpedOnceExt = true;
            int level = 0;
            for (RE::NiNode* walk = cell3D->parent; walk && level < 4;
                 walk = walk->parent, ++level) {
                std::string line;
                for (auto& sib : walk->GetChildren()) {
                    if (auto* s = sib.get()) {
                        line += line.empty() ? "" : ", ";
                        const char* nm = s->name.c_str();
                        line += (nm && *nm) ? nm : "<unnamed>";
                        line += s->GetAppCulled() ? "(culled)" : "";
                    }
                }
                spdlog::debug("scene above EXTERIOR cell3D, level {} '{}': [{}]",
                              level,
                              walk->name.c_str() ? walk->name.c_str() : "<unnamed>",
                              line);
            }
        }
    }

    // Solo mode: hide every loaded reference around the player except the
    // player, light sources (culling them would change how the player is
    // lit) and the furniture the player occupies (inventory can open while
    // sitting - the chair must not vanish under them).
    void HideEverythingElse(RE::PlayerCharacter* a_player, float a_radius) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        const auto occupied = a_player->GetOccupiedFurniture();
        RE::Actor* mountPtr = PlayerMount(a_player);  // r53: the horse stays
        // Hoisted out of the lambda: this is read once per REF otherwise, and
        // the sweep visits every ref in the loaded grid.
        const bool hideLights = MTB::Settings::GetSingleton().hideLightRefs;
        int hidden = 0;
        ForEachRefInRangeSafe(
            tes, a_player, a_radius, [&](RE::TESObjectREFR& a_ref) {
                // ⚠ ALREADY-CULLED CHECK FIRST - it is the cheapest test here
                // and, after the first sweep, the one that answers for almost
                // every ref. Refresh() re-runs this walk ~4x/second for the
                // whole time a menu is open, and on a default install the
                // radius is 16384 (bVoidEngine), i.e. the entire loaded
                // exterior grid. The order used to be handle -> disabled ->
                // base-object -> formtype -> HideRef, so sweeps 2..N paid a
                // GetHandle() (a refhandle lookup) and three virtual calls per
                // ref only to discover inside HideRef that the node was culled
                // on sweep 1. Steady-state cost is now one Get3D + one flag
                // read per ref. Perf audit 2026-07-22.
                auto* const root = a_ref.Get3D();
                if (!root || root->GetAppCulled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (&a_ref == a_player || &a_ref == mountPtr ||
                    a_ref.GetHandle() == occupied || a_ref.IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* base = a_ref.GetBaseObject();
                if (base && base->GetFormType() == RE::FormType::Light &&
                    !hideLights) {
                    // Light refs carry the flame/smoke/spark art. Hiding the
                    // mesh usually keeps the ILLUMINATION (the BSLight lives
                    // in the scene light list, not the culled geometry) -
                    // bHideLightRefs=0 restores the old skip if a setup
                    // disagrees and the player goes dark.
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (HideRef(&a_ref)) {
                    ++hidden;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        if (hidden > 0) {
            spdlog::debug("declutter[solo]: hid {} ref(s) within {:.0f} units.", hidden, a_radius);
        }
    }

    // r33 (user root-caused it in the field: "the extra lighting comes from
    // the other sources of light produced by the cell"): every sweep above
    // culls PARENT branches, and a parent-culled light keeps illuminating -
    // only a light whose OWN node is culled is skipped by the light
    // gathering (r8, proven both directions by the studio rig). So the void
    // was still lit by braziers/candles the player couldn't even see. Walk
    // the scene and self-cull every NiLight that isn't ours (MTB_ rig) or
    // on the player's branch (equipped torch/candlelight keeps lighting the
    // character); the shared restore list un-culls them on every exit.
    int CutLightsUnder(RE::NiAVObject* a_obj, RE::NiAVObject* a_playerRoot) {
        if (!a_obj || a_obj == a_playerRoot) {
            return 0;
        }
        if (const char* nm = a_obj->name.c_str();
            nm && std::strncmp(nm, "MTB_", 4) == 0) {
            return 0;
        }
        if (auto* light = netimmerse_cast<RE::NiLight*>(a_obj)) {
            if (!light->GetAppCulled()) {
                light->SetAppCulled(true);
                g_hiddenNodes.emplace_back(light);
                return 1;
            }
            return 0;
        }
        int cut = 0;
        if (auto* node = a_obj->AsNode()) {
            for (auto& child : node->GetChildren()) {
                cut += CutLightsUnder(child.get(), a_playerRoot);
            }
        }
        return cut;
    }

    void CutCellLightSources(RE::PlayerCharacter* a_player) {
        auto* cell = a_player->GetParentCell();
        auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
        auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
        auto* playerRoot = a_player->Get3D();
        if (!cell3D || !playerRoot) {
            return;
        }
        // Interiors: start two levels above the cell - the unnamed FX roots
        // up there (scene-dump finding) can hold light emitters too.
        // Exteriors: the player's cell root only (the sun/sky directional is
        // not a scene NiLight; StudioLight owns the ambient/directional).
        RE::NiAVObject* root = cell3D;
        if (cell->IsInteriorCell()) {
            for (int i = 0; i < 2 && root->parent; ++i) {
                root = root->parent;
            }
        }
        if (const int cut = CutLightsUnder(root, playerRoot); cut > 0) {
            spdlog::debug("declutter[solo]: self-culled {} cell light source(s) - "
                          "illumination off; the rig + studio light own the "
                          "character now.", cut);
        }
    }

    // ------------------------------------------------------------------
    // r37 F-20 VOID ENGINE. The RE verdict first (mtb_renderscenes.c /
    // mtb_menureplace.c / mtb_statsmenu2.c): the skills menu blanks the
    // world through IMenu flag kFreezeFrameBackground (1<<5, carried by the
    // TweenMenu under StatsMenu) - the render dispatcher (0x1405B1020)
    // renders the world ONCE more into a held frame, then only re-presents
    // that frozen image while the flag menu is stacked; the perk dome draws
    // through a separate menu-3D pipeline. That mechanism is binary: a
    // frozen world cannot contain OUR live, animating character, so the
    // flag is unusable here. What we adopt instead are the engine's own
    // WHOLESALE per-subsystem switches - each leak the field found gets
    // killed at its engine root, not per-ref:
    //   grass  = the grass scene ROOT node (BGSGrassManager+0x68); culling
    //            it is literally the engine's ToggleGrass ("tg") console
    //            implementation (decompile mtb_togglegrass2.c).
    //   land   = every attached cell's land quadrant meshes
    //            (cellLand->loadedData->mesh[0..3]) - terrain is not a
    //            reference, no ref sweep can reach it.
    //   LOD    = TES::lodLandRoot + TES::objLODWaterRoot (distant terrain /
    //            water impostors; the shell occludes most, these kill the
    //            rest).
    //   precip = Sky::precip current/last geometries - rain/snow already
    //            mid-air at arm stays frozen INSIDE the shell otherwise.
    // Everything lands in g_hiddenNodes, so the existing three-exit
    // restore covers it (SPEC §4 invariant 1).
    int CullNode(RE::NiAVObject* a_obj) {
        if (a_obj && !a_obj->GetAppCulled()) {
            a_obj->SetAppCulled(true);
            g_hiddenNodes.emplace_back(a_obj);
            return 1;
        }
        return 0;
    }

    RE::NiAVObject* GrassSceneRoot() {
        auto* mgr = RE::BGSGrassManager::GetSingleton();
        if (!mgr) {
            return nullptr;
        }
        // +0x68 = the grass scene root. Engine evidence: the ToggleGrass
        // handler (1.5.97 @ 0x140313600) flips exactly this node's AppCull
        // bit. Not modeled in CommonLib - raw offset, SE-only like the rest
        // of Offsets.h.
        return *reinterpret_cast<RE::NiAVObject**>(
            reinterpret_cast<std::uintptr_t>(mgr) + 0x68);
    }

    void CullLandOfCell(RE::TESObjectCELL* a_cell, int& a_quads) {
        if (!a_cell) {
            return;
        }
        auto* land = a_cell->GetRuntimeData().cellLand;
        auto* data = land ? land->loadedData : nullptr;
        if (!data) {
            return;
        }
        for (auto* mesh : data->mesh) {
            a_quads += CullNode(mesh);
        }
    }

    void CullWorldFeeders(RE::PlayerCharacter* a_player) {
        const int grass = CullNode(GrassSceneRoot());
        int quads = 0;
        int lod = 0;
        if (auto* tes = RE::TES::GetSingleton()) {
            CullLandOfCell(a_player->GetParentCell(), quads);
            if (auto* grid = tes->gridCells) {
                for (std::uint32_t x = 0; x < grid->length; ++x) {
                    for (std::uint32_t y = 0; y < grid->length; ++y) {
                        CullLandOfCell(grid->GetCell(x, y), quads);
                    }
                }
            }
            lod += CullNode(tes->lodLandRoot);
            lod += CullNode(tes->objLODWaterRoot);
            // r41 - the r38 exterior dump settled the mountain question:
            // TES::objRoot is the node NAMED 'ObjectLODRoot' and it is the
            // GRID CELL CONTAINER (the player's ancestor on every arm -
            // the r38 guard refused it 38/38 times, misleading CommonLib
            // field name and all). The distant-LOD content - object LOD
            // (the mountain silhouettes) and tree LOD - lives under the
            // NAMED 'LODRoot' sibling beneath the shadow scene node, which
            // the unnamed-sibling cull deliberately skips.
            //
            // r42 - 'Sky' and 'Weather' join the named culls, and THIS is
            // the real void switch (field: clouds + sky visible in the
            // armed void with ENB off): Sky::mode is consumed by
            // Sky::Update, WHICH NEVER RUNS WHILE THE MENU IS PAUSED - the
            // mode park was visually inert in every armed menu, and each
            // dark night sky spent five rounds masquerading as our void.
            // Culling the sky's scene BRANCH is render-side and immediate,
            // pause or not; the mode park stays for its real consumers
            // (fog ingest, and the r40 close-edge audio flow is untouched).
            for (RE::NiNode* up = a_player->Get3D() ? a_player->Get3D()->parent : nullptr;
                 up; up = up->parent) {
                for (auto& childPtr : up->GetChildren()) {
                    auto* child = childPtr.get();
                    if (child && (child->name == "LODRoot" ||
                                  child->name == "Sky" ||
                                  child->name == "Weather")) {
                        lod += CullNode(child);
                    }
                }
            }
        }
        // r45 (field: "some circle at our feet … where the voidsphere and
        // floor interact?" - exactly right): a WATER PLANE slicing the
        // shell sphere is a circle at foot height. Water was the one
        // feeder r37 consciously skipped; the live water meshes hang off
        // TESWaterSystem's object list (LOD water was already culled).
        int water = 0;
        if (auto* ws = RE::TESWaterSystem::GetSingleton()) {
            for (auto& obj : ws->waterObjects) {
                if (obj) {
                    water += CullNode(obj->shape.get());
                }
            }
        }
        if (water > 0) {
            spdlog::debug("void engine: {} water shape(s) culled.", water);
        }
        int precip = 0;
        if (auto* sky = RE::Sky::GetSingleton(); sky && sky->precip) {
            precip += CullNode(sky->precip->currentPrecip.get());
            precip += CullNode(sky->precip->lastPrecip.get());
        }
        spdlog::info("void engine: grass root {} | {} land quad(s) | {} LOD "
                     "root(s) | {} precip geom(s) culled.",
                     grass ? "culled" : "absent/off", quads, lod, precip);
    }

    // r37 F-22: a combat arm can catch a magic-projectile imagespace
    // modifier mid-ramp - the pause freezes its interpolators and the blur
    // sits over the studio for the whole menu. The engine keeps the live
    // instances in TES::activeImageSpaceModifiers; zeroing each instance's
    // strength removes its contribution, the saved value returns on exit.
    // If the field still shows blur with this log line present, the applied
    // state is baked elsewhere (ImageSpaceManager accumulation) and the
    // next lever is forcing a manager rebuild - evidence first.
    void NeutralizeActiveImods() {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        int n = 0;
        int seen = 0;
        for (auto& inst : tes->activeImageSpaceModifiers) {
            auto* raw = inst.get();
            if (!raw) {
                continue;
            }
            ++seen;
            // The note above called this exactly: "if the field still shows
            // blur with this log line present, the applied state is baked
            // elsewhere". It does (2026-07-18, Community Shaders, blur on
            // ContainerMenu) - so name every live instance before deciding
            // anything, because a COUNT cannot tell us which one survives.
            // Two things separate the candidates. The concrete type: a DOF
            // or Temp instance is not form-backed (IsForm() is null on
            // those), so a depth-of-field blur shows up here with no form id
            // at all, which is itself the answer to "vanilla imod or
            // something else". And the source form id when there is one:
            // 000434BB is the vanilla menu blur that OwnView already parks,
            // so seeing it here would mean that park is not holding.
            // No RTTI name here: this CommonLib build inherits NiObject
            // PRIVATELY on ImageSpaceModifierInstance, so GetRTTI() will not
            // compile through it. IsForm() is the public discriminator and it
            // carries the signal that matters anyway - it returns null for a
            // NON form-backed instance (DOF or Temp), and a depth-of-field
            // instance is exactly the shape a "blur" report points at.
            RE::FormID srcID = 0;
            bool       formBacked = false;
            if (auto* asForm = raw->IsForm()) {
                formBacked = true;
                if (asForm->imod) {
                    srcID = asForm->imod->GetFormID();
                }
            }
            const bool live = raw->strength != 0.0f;
            // debug, not info: the blur this was cut for turned out to be a
            // vanilla effect (field 2026-07-18), so this is now standing
            // instrumentation for the NEXT imod question rather than an open
            // investigation, and it must not cost every user two log lines per
            // menu open. bVerboseLog brings it back.
            spdlog::debug("void engine: imod #{} {} imod={:08X} "
                          "strength={:.3f} age={:.2f} -> {}",
                          seen, formBacked ? "form-backed" : "NON-form (DOF/Temp)",
                          srcID, raw->strength, raw->age,
                          live ? "ZEROED" : "left (already 0)");
            if (live) {
                g_imodSaves.push_back({ raw, raw->strength });
                raw->strength = 0.0f;
                ++n;
            }
        }
        if (seen > 0) {
            spdlog::info("void engine: zeroed {} of {} active imod instance(s) "
                         "(F-22 blur) - strengths restore on exit.", n, seen);
        }
    }

    // Scene view (mode 1): the room stays; clear out furniture so the player
    // reads against the environment. Occupied furniture stays (menus open
    // while sitting), light refs stay (the cell keeps its own look).
    void HideSceneFurniture(RE::PlayerCharacter* a_player, float a_radius) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) {
            return;
        }
        const auto occupied = a_player->GetOccupiedFurniture();
        int hidden = 0;
        ForEachRefInRangeSafe(
            tes, a_player, a_radius, [&](RE::TESObjectREFR& a_ref) {
                if (&a_ref == a_player || a_ref.GetHandle() == occupied || a_ref.IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* base = a_ref.GetBaseObject();
                if (base && base->GetFormType() == RE::FormType::Furniture && HideRef(&a_ref)) {
                    ++hidden;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        if (hidden > 0) {
            spdlog::debug("declutter[scene]: hid {} furniture within {:.0f} units.",
                          hidden, a_radius);
        }
    }

}

namespace MTB::Declutter {
    void Refresh() {
        const auto& cfg = Settings::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || cfg.declutterMode == 0) {
            return;
        }
        if (cfg.IsVoidFamily()) {
            // r37: with the void engine on, the ref sweeps reach further -
            // the 'Editor Smoke Test Cell' arm (r33 field) had architecture
            // beyond the 4096-unit radius that no cull could touch. 16384
            // covers a full loaded exterior grid quadrant; one-shot cost,
            // paused world.
            const float radius = cfg.voidEngine
                ? std::max(cfg.soloHideRadius, 16384.0f)
                : cfg.soloHideRadius;
            // Void (2) and dressing room (3) share the solo sweep.
            // Actor sweep first: process lists cover actors whose 3D lives
            // outside the ref-in-range walk (mounts, summons mid-transition).
            HideNearbyActors(player, radius);
            // Interiors: path-sibling cull - only the player's node chain
            // renders, so ref-less emitters can't survive anywhere in the
            // cell tree. The ref sweep still runs as a safety net (and is
            // what exteriors rely on).
            HideInteriorPathSiblings(player);
            // r38: exteriors get the above-the-cell FX-root cull too (the
            // ember particles the interior version killed in r13).
            if (cfg.voidEngine) {
                HideExteriorAboveCell(player);
            }
            HideEverythingElse(player, radius);
            // r33: cut the cell's light SOURCES (illumination, not art) so
            // the character is lit by the studio alone. Once per arm - the
            // paused world can't spawn new lights mid-menu.
            if (cfg.cutCellLights && !g_cellLightsCut) {
                g_cellLightsCut = true;
                CutCellLightSources(player);
            }
            // r37 F-20/F-22: the world's non-ref feeders (grass, land, LOD,
            // precipitation) + frozen imod blur - once per arm.
            if (cfg.voidEngine && !g_worldFeedersCut) {
                g_worldFeedersCut = true;
                CullWorldFeeders(player);
                NeutralizeActiveImods();
            }
            return;
        }
        // Scene view (mode 1): environment visible, no lighting override -
        // only actors and furniture leave the set. (The old corridor-only
        // mode retired in r16 - "less is more".)
        HideNearbyActors(player, cfg.soloHideRadius);
        HideSceneFurniture(player, cfg.soloHideRadius);
    }

    void ReassertWorldFeederCulls() {
        // The armed menu is paused, so the culls hold by themselves; this
        // exists for the UNPAUSED window between a close and the at-black
        // teardown (switch gaps, exit hold + dip): Sky::Update ticks there
        // with the true mode (r40) and un-culls its own branch each frame -
        // the skybox flashed over the studio mid-transition (field r43).
        // Same named walk as the arm-time cull; CullNode skips anything
        // still culled, so steady-state cost is a handful of name checks.
        if (!g_worldFeedersCut || !Settings::GetSingleton().voidEngine) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player ? player->Get3D() : nullptr;
        if (!root) {
            return;
        }
        for (RE::NiNode* up = root->parent; up; up = up->parent) {
            for (auto& childPtr : up->GetChildren()) {
                auto* child = childPtr.get();
                if (child && (child->name == "LODRoot" ||
                              child->name == "Sky" ||
                              child->name == "Weather")) {
                    CullNode(child);
                }
            }
        }
    }

    void RestoreAll() {
        // r37 F-22: give the imod instances their strengths back first -
        // independent of the cull lists, cheap, and a stray zero must never
        // outlive the menu. Raw saved pointers are only dereferenced when
        // the instance is STILL in the engine's live list (quickload-safe).
        if (!g_imodSaves.empty()) {
            int back = 0;
            if (auto* tes = RE::TES::GetSingleton()) {
                for (auto& live : tes->activeImageSpaceModifiers) {
                    auto* raw = live.get();
                    if (!raw) {
                        continue;
                    }
                    for (auto& save : g_imodSaves) {
                        if (save.inst == raw) {
                            raw->strength = save.strength;
                            ++back;
                            break;
                        }
                    }
                }
            }
            spdlog::debug("void engine: restored {}/{} imod strength(s).",
                          back, g_imodSaves.size());
            g_imodSaves.clear();
        }
        g_worldFeedersCut = false;
        if (g_hidden.empty() && g_hiddenNodes.empty()) {
            return;
        }
        int restored = 0;
        for (auto& handle : g_hidden) {
            auto ref = handle.get();
            auto* root = ref ? ref->Get3D() : nullptr;
            if (root && root->GetAppCulled()) {
                root->SetAppCulled(false);
                ++restored;
            }
        }
        int nodesRestored = 0;
        for (auto& node : g_hiddenNodes) {
            if (node && node->GetAppCulled()) {
                node->SetAppCulled(false);
                ++nodesRestored;
            }
        }
        spdlog::info("declutter: restored {}/{} ref(s) + {}/{} cell branch(es).",
                     restored, g_hidden.size(), nodesRestored, g_hiddenNodes.size());
        g_hidden.clear();
        g_hiddenNodes.clear();
        g_cellLightsCut = false;
    }
}
