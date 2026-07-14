#include "PCH.h"

#include "FaceNeutral.h"

#include <cstring>
#include <vector>

namespace {
    bool g_saved = false;
    bool g_exprOverrideWas = false;
    std::vector<float> g_expression;
    std::vector<float> g_modifier;
    std::vector<float> g_phoneme;

    // MFG modifier channels (facegen keyframe indices).
    constexpr std::uint32_t kBlinkLeft = 0, kBlinkRight = 1;
    constexpr std::uint32_t kLookDown = 8, kLookLeft = 9, kLookRight = 10, kLookUp = 11;

    float ModifierValue(const RE::BSFaceGenKeyframeMultiple& a_kf, std::uint32_t a_i) {
        return (a_kf.values && a_kf.count > a_i) ? a_kf.values[a_i] : -1.0f;
    }

    RE::BSFaceGenAnimationData* PlayerFace() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? player->GetFaceGenAnimationData() : nullptr;
    }

    void CopyOut(const RE::BSFaceGenKeyframeMultiple& a_kf, std::vector<float>& a_out) {
        if (a_kf.values && a_kf.count > 0) {
            a_out.assign(a_kf.values, a_kf.values + a_kf.count);
        } else {
            a_out.clear();
        }
    }

    void CopyIn(RE::BSFaceGenKeyframeMultiple& a_kf, const std::vector<float>& a_in) {
        if (a_kf.values && a_kf.count == a_in.size() && !a_in.empty()) {
            std::memcpy(a_kf.values, a_in.data(), a_in.size() * sizeof(float));
            a_kf.isUpdated = true;
        }
    }
}

namespace MTB::FaceNeutral {
    void Apply() {
        auto* face = PlayerFace();
        if (!face) {
            return;
        }
        CopyOut(face->expressionKeyFrame, g_expression);
        CopyOut(face->modifierKeyFrame, g_modifier);
        CopyOut(face->phenomeKeyFrame, g_phoneme);
        g_saved = true;
        // F-13 lead 1 (r30 field: "our eyes can still get stuck"): MFG-style
        // mods set exprOverride (+0x21E), which blocks the engine's
        // expression processing - with it up, the Reset ramp below is INERT
        // and whatever face the arm caught holds all menu. Save + clear it
        // BEFORE the reset; Restore() puts it back for the mod that owns it.
        g_exprOverrideWas = face->exprOverride;
        face->ClearExpressionOverride();
        // …and the channel discriminator: one line convicts eyelids vs gaze
        // vs a held expression when the user reports a stuck face. Gaze
        // modifiers (8-11) track the HEADTRACK target - if these sit high
        // with level eyes expected, the pin target (or its old +120 z) is
        // the writer, not the expression system.
        {
            const auto& expr = face->expressionKeyFrame;
            std::uint32_t strongest = 0;
            float strongestVal = 0.0f;
            for (std::uint32_t i = 0; expr.values && i < expr.count; ++i) {
                if (expr.values[i] > strongestVal) {
                    strongestVal = expr.values[i];
                    strongest = i;
                }
            }
            const auto& mod = face->modifierKeyFrame;
            spdlog::info(
                "face neutral: exprOverride={} (cleared) | blink L/R={:.2f}/{:.2f} | "
                "gaze down/left/right/up={:.2f}/{:.2f}/{:.2f}/{:.2f} | expr[{}]={:.2f}",
                g_exprOverrideWas, ModifierValue(mod, kBlinkLeft),
                ModifierValue(mod, kBlinkRight), ModifierValue(mod, kLookDown),
                ModifierValue(mod, kLookLeft), ModifierValue(mod, kLookRight),
                ModifierValue(mod, kLookUp), strongest, strongestVal);
        }
        // The engine's own facegen reset (ID 25977): expression +
        // modifiers + phonemes ramp to neutral over the timer; custom
        // (RaceMenu) morphs untouched, eyes left open. Our face tick
        // advances the ramp - the mid-blink squint dissolves instead of
        // holding all menu.
        face->Reset(0.25f, true, true, false, false);
        // The dissolve starts FROM the caught frame, so a half-closed
        // blink stays visible for its first beats (r28 field: "can still
        // be blinking when we open"). Eyelids snap open instead - MFG
        // modifiers 0/1 are BlinkLeft/BlinkRight; everything else keeps
        // the gentle ramp.
        if (auto& mod = face->modifierKeyFrame; mod.values && mod.count > 1) {
            mod.SetValue(0, 0.0f);
            mod.SetValue(1, 0.0f);
        }
        // …and hold the AMBIENT blink generator off for a beat (r29 field:
        // "they can still be blinking" - the engine's own blink machine
        // kept cycling right at open, re-closing the snapped eyelids). A
        // fresh delay means the menu opens on calm, open eyes; natural
        // blinking resumes after.
        face->blinkDelay = 3.0f;
        spdlog::debug("face neutral: expression saved ({} expr / {} mod / {} phon), "
                      "ramping to neutral.",
                      g_expression.size(), g_modifier.size(), g_phoneme.size());
    }

    void Restore() {
        if (!g_saved) {
            return;
        }
        g_saved = false;
        auto* face = PlayerFace();
        if (!face) {
            return;
        }
        CopyIn(face->expressionKeyFrame, g_expression);
        CopyIn(face->modifierKeyFrame, g_modifier);
        CopyIn(face->phenomeKeyFrame, g_phoneme);
        // Give exprOverride back to whichever mod raised it - clearing was
        // only ever for OUR menu-time reset ramp.
        face->exprOverride = g_exprOverrideWas;
        spdlog::debug("face neutral: expression restored (exprOverride={}).",
                      g_exprOverrideWas);
    }

    void Drop() {
        g_saved = false;
    }
}
