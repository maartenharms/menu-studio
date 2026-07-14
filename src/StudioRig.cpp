#include "PCH.h"

#include "StudioRig.h"

#include "Offsets.h"
#include "Settings.h"
#include "Transition.h"

#include <array>
#include <cmath>
#include <cstring>

namespace {
    // po3-fork layout, byte-verified against the 1.5.97 AddLight decompile
    // (shadow branch reads +0x01 and +0x08; Tools\re\research\mtb_studiorig.c).
    // dynamic=true: static BSLights added mid-session never re-enter the
    // per-geometry light lists (round-5 field: registered fine, lit
    // nothing); dynamic lights are re-evaluated every frame - the same
    // path a torch equipped in a paused menu uses.
    struct LightCreateParams {
        bool                dynamic{ true };        // 00
        bool                shadowLight{ false };   // 01
        bool                portalStrict{ false };  // 02
        bool                affectLand{ true };     // 03
        bool                affectWater{ false };   // 04
        bool                neverFades{ true };     // 05
        float               fov{ 0.0f };            // 08
        float               falloff{ 1.0f };        // 0C
        float               nearDistance{ 5.0f };   // 10
        float               depthBias{ 1.0f };      // 14
        std::uint32_t       sceneGraphIndex{ 0 };   // 18
        RE::NiAVObject*     restrictedNode{ nullptr };  // 20
        void*               lensFlareData{ nullptr };   // 28
    };
    static_assert(sizeof(LightCreateParams) == 0x30);

    using CreatePointLight_t = RE::NiPointLight* (*)();
    using AddLight_t = RE::BSLight* (*)(RE::ShadowSceneNode*, RE::NiLight*,
                                        const LightCreateParams*);
    using RemoveLight_t = void (*)(RE::ShadowSceneNode*, RE::NiPointer<RE::BSLight>&);

    REL::Relocation<CreatePointLight_t> g_createPointLight{ MTB::Offsets::NiPointLightCreate };
    REL::Relocation<AddLight_t>         g_addLight{ MTB::Offsets::ShadowSceneAddLight };
    REL::Relocation<RemoveLight_t>      g_removeLight{ MTB::Offsets::ShadowSceneRemoveLight };

    struct RigSpec {
        const char* name;
        float       angleOffset;  // radians from player heading (0 = facing dir)
        float       distance;
        float       zOffset;
        RE::NiColor color;
        float       radius;
        float       fade;
    };
    // Player faces the camera under SPIM, so heading+0 is between character
    // and camera: key front-left-high, fill front-right-low, rim behind-high.
    constexpr std::array<RigSpec, 3> kRig{ {
        { "MTB_RigKey", 0.66f, 150.0f, 115.0f, { 1.00f, 0.95f, 0.87f }, 380.0f, 2.0f },
        { "MTB_RigFill", -0.84f, 175.0f, 55.0f, { 0.72f, 0.78f, 0.90f }, 420.0f, 1.0f },
        { "MTB_RigRim", 3.27f, 130.0f, 150.0f, { 1.00f, 1.00f, 1.00f }, 320.0f, 1.6f },
    } };

    struct ActiveLight {
        RE::NiPointer<RE::NiPointLight> node;
        RE::NiPointer<RE::BSLight>      bsLight;
    };
    std::array<ActiveLight, kRig.size()> g_active{};
    bool g_rigUp = false;

    // r39 (field: "the 3 point light system sometimes can appear even when
    // the menu is closed"): Remove() re-derived the shadow scene node from
    // the player's 3D at TEARDOWN time - when that walk failed, the code
    // dropped our BSLight references WITHOUT deregistering them, and the
    // SSN kept the rig lighting the character in normal gameplay until the
    // next load. Hold the scene node the lights were REGISTERED with from
    // Apply to Remove instead (as NiAVObject: ShadowSceneNode's NiNode
    // base is private in NG - the r5 lesson - so the public-base pointer
    // carries the refcount and we reinterpret at the call).
    RE::NiPointer<RE::NiAVObject> g_ssnHold;

    // Live values: colors/intensities/enables flow from the effective look
    // (manual or auto time-of-day) every armed tick, so slider drags and
    // the game clock both read back in real time. The transition scalar
    // (F-12) rides the same write: the rig ramps in at arm and dissolves
    // through the teardown grace.
    void PushConfig(std::size_t a_index, RE::NiPointLight* a_light,
                    const MTB::Settings::LookValues& a_look) {
        const auto& cfg = MTB::Settings::GetSingleton();
        const auto& lc = a_index == 0 ? a_look.key
                       : a_index == 1 ? a_look.fillLight
                                      : a_look.rim;
        auto& data = a_light->GetLightRuntimeData();
        data.diffuse = RE::NiColor{ lc.color.red / 255.0f, lc.color.green / 255.0f,
                                    lc.color.blue / 255.0f };
        data.fade = lc.enabled
                        ? kRig[a_index].fade * lc.intensity * cfg.rigBrightness *
                              MTB::Transition::Value()
                        : 0.0f;
    }

    RE::ShadowSceneNode* FindShadowSceneNode(RE::NiAVObject* a_from) {
        for (auto* node = a_from ? a_from->parent : nullptr; node; node = node->parent) {
            // NG declares the NiNode base private, which blocks
            // netimmerse_cast's static_cast - match the NiRTTI by name and
            // reinterpret (single inheritance, base at offset 0).
            if (const auto* rtti = node->GetRTTI();
                rtti && rtti->name && std::strcmp(rtti->name, "ShadowSceneNode") == 0) {
                return reinterpret_cast<RE::ShadowSceneNode*>(node);
            }
        }
        return nullptr;
    }
}

namespace MTB::StudioRig {
    void Apply() {
        const auto& cfg = Settings::GetSingleton();
        // The rig lights the framed character in the Void and the Dressing room
        // (mode >= 2). Only Off / Scene view skip it.
        if (g_rigUp || !cfg.studioRig || cfg.declutterMode < 2) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerRoot = player ? player->Get3D(false) : nullptr;
        auto* parent = playerRoot ? playerRoot->parent : nullptr;
        auto* ssn = FindShadowSceneNode(playerRoot);
        if (!parent || !ssn) {
            spdlog::warn("studio rig: no scene parent/shadow scene node - rig skipped.");
            return;
        }
        g_ssnHold = RE::NiPointer<RE::NiAVObject>{ reinterpret_cast<RE::NiAVObject*>(ssn) };

        const auto pos = player->GetPosition();
        const float heading = player->data.angle.z;

        // Parent-local placement: cell roots usually carry identity
        // transforms, but never assume - invert properly.
        const auto& pw = parent->world;
        const auto pwRotInv = pw.rotate.Transpose();
        const float invScale = pw.scale != 0.0f ? 1.0f / pw.scale : 1.0f;

        int up = 0;
        for (std::size_t i = 0; i < kRig.size(); ++i) {
            const auto& spec = kRig[i];
            auto* light = g_createPointLight();
            if (!light) {
                continue;
            }
            light->name = spec.name;
            auto& data = light->GetLightRuntimeData();
            data.ambient = RE::NiColor{ 0.0f, 0.0f, 0.0f };
            data.radius = RE::NiPoint3{ spec.radius, spec.radius, spec.radius };
            PushConfig(i, light, cfg.CurrentLook());  // diffuse + fade, live look

            const float a = heading + spec.angleOffset;
            const RE::NiPoint3 world{ pos.x + std::sin(a) * spec.distance,
                                      pos.y + std::cos(a) * spec.distance,
                                      pos.z + spec.zOffset };
            parent->AttachChild(light, true);
            light->local.translate = (pwRotInv * (world - pw.translate)) * invScale;
            light->world.rotate = RE::NiMatrix3();
            light->world.translate = world;
            light->world.scale = 1.0f;
            // A bare-created light has a ZERO world bound - light gathering
            // intersects the light's bound against geometry, so a zero
            // bound lights nothing regardless of registration (round-6
            // field: dynamic lights up, zero photons). Give it the real
            // sphere.
            light->worldBound.center = world;
            light->worldBound.radius = spec.radius;
            spdlog::debug("studio rig: '{}' at ({:.0f},{:.0f},{:.0f}) r={:.0f}.",
                          spec.name, world.x, world.y, world.z, spec.radius);

            LightCreateParams params{};
            auto* bsLight = g_addLight(ssn, light, &params);
            if (!bsLight) {
                spdlog::warn("studio rig: AddLight refused '{}'.", spec.name);
                if (auto* p = light->parent) {
                    p->DetachChild2(light);
                }
                continue;
            }
            g_active[i].node = RE::NiPointer<RE::NiPointLight>{ light };
            g_active[i].bsLight = RE::NiPointer<RE::BSLight>{ bsLight };
            ++up;
        }
        g_rigUp = up > 0;
        if (g_rigUp) {
            spdlog::debug("studio rig: {} light(s) up (brightness {:.2f}).",
                          up, cfg.rigBrightness);
        }
    }

    void Tick() {
        const auto& cfg = MTB::Settings::GetSingleton();
        // Live master toggle / view-mode change: spawn/remove mid-arm.
        if (g_rigUp && (!cfg.studioRig || cfg.declutterMode < 2)) {
            MTB::StudioRig::Remove();
            return;
        }
        if (!g_rigUp && cfg.studioRig && cfg.declutterMode >= 2) {
            MTB::StudioRig::Apply();
        }
        if (!g_rigUp) {
            return;
        }
        // Cheap defense: some engine passes recompute world data from local
        // or reset bounds; keep the rig's world state exactly as placed -
        // and un-culled (the declutter sweep now exempts MTB_ nodes, but a
        // pre-fix sweep entry restored on close could re-flag mid-session).
        // PushConfig makes panel color/intensity edits land the same frame.
        const auto look = cfg.CurrentLook();
        for (std::size_t i = 0; i < g_active.size(); ++i) {
            if (auto* light = g_active[i].node.get()) {
                light->worldBound.center = light->world.translate;
                light->worldBound.radius = light->GetLightRuntimeData().radius.x;
                if (light->GetAppCulled()) {
                    light->SetAppCulled(false);
                }
                PushConfig(i, light, look);
            }
        }
    }

    void PushFade() {
        // Grace-window refresh (F-12): only the fade values move - no
        // Apply/Remove decisions, no transform writes; the world is live
        // again out there and this must stay a pure visual dissolve.
        if (!g_rigUp) {
            return;
        }
        const auto look = MTB::Settings::GetSingleton().CurrentLook();
        for (std::size_t i = 0; i < g_active.size(); ++i) {
            if (auto* light = g_active[i].node.get()) {
                PushConfig(i, light, look);
            }
        }
    }

    void Remove() {
        if (!g_rigUp) {
            g_ssnHold.reset();
            return;
        }
        // Deregister from the node the lights were REGISTERED with (held
        // since Apply) - never from a fresh player walk, which can fail
        // exactly when the teardown races a 3D change and used to leak the
        // rig into gameplay. The walk stays as a legacy fallback only.
        auto* ssn = g_ssnHold
            ? reinterpret_cast<RE::ShadowSceneNode*>(g_ssnHold.get())
            : FindShadowSceneNode(
                  RE::PlayerCharacter::GetSingleton()
                      ? RE::PlayerCharacter::GetSingleton()->Get3D(false)
                      : nullptr);
        int down = 0;
        int leaked = 0;
        for (auto& active : g_active) {
            if (active.bsLight) {
                if (ssn) {
                    g_removeLight(ssn, active.bsLight);
                    ++down;
                } else {
                    ++leaked;
                }
            }
            active.bsLight.reset();
            if (active.node) {
                if (auto* p = active.node->parent) {
                    p->DetachChild2(active.node.get());
                }
                active.node.reset();
            }
        }
        g_rigUp = false;
        g_ssnHold.reset();
        if (leaked > 0) {
            spdlog::warn("studio rig: {} light(s) could NOT be deregistered "
                         "(no scene node at teardown) - rig-leak tripwire; "
                         "report this line with what preceded it.", leaked);
        } else {
            spdlog::debug("studio rig: {} light(s) removed.", down);
        }
    }
}
