#pragma once

namespace MTB::WeaponPreviewGate {

    // Everything the decision needs, read by the caller. Deliberately plain
    // bools and no engine types: this is the half of the feature that can be
    // tested without Skyrim running, and the half where a mistake is invisible
    // in the field (a missed restore leaves the player permanently drawn).
    struct Input {
        bool enabled        = false;  // bWeaponPreviewInMenus
        bool bubbleArmed    = false;  // a bubbled menu is open and the bubble is armed
        bool raceMenu       = false;  // RaceSexMenu owns the character
        bool weaponEquipped = false;  // the right hand holds a WEAP
        bool weaponDrawn    = false;  // AsActorState()->IsWeaponDrawn()
        bool inCombat       = false;
        bool mounted        = false;
        bool autoDraw       = false;  // bAutoDrawInMenus - draw a SHEATHED weapon
    };

    enum class Action {
        kNone,     // leave the character alone
        kDraw,     // draw the equipped weapon for the preview
        kRestore,  // sheathe again - only ever reachable when WE drew
    };

    // a_weDrew: this preview performed the draw and still owes a sheathe.
    [[nodiscard]] constexpr Action Decide(const Input& a_in, bool a_weDrew) {
        // A DEBT IS ALWAYS PAID, and it is checked before anything else. If any
        // of the conditions below could swallow the restore - settings flipped
        // mid-menu, the player mounted, combat started, the weapon was
        // unequipped - the character would be left drawn for the rest of the
        // session with nothing to put it back.
        if (a_weDrew) {
            const bool stillPreviewing = a_in.enabled && a_in.bubbleArmed && !a_in.raceMenu &&
                                         a_in.weaponEquipped && !a_in.inCombat && !a_in.mounted;
            return stillPreviewing ? Action::kNone : Action::kRestore;
        }

        if (!a_in.enabled || !a_in.bubbleArmed || a_in.raceMenu) {
            return Action::kNone;
        }
        // Declining in combat and while mounted mirrors what SPII already does
        // for its own arms; a draw is a gameplay action and those are the two
        // states where forcing one is most likely to be noticed.
        if (a_in.inCombat || a_in.mounted) {
            return Action::kNone;
        }
        if (!a_in.weaponEquipped) {
            return Action::kNone;
        }
        // Already drawn in the world - the "mirror the real state" case. Do
        // nothing AND take on no debt, so closing the menu does not sheathe a
        // weapon the player deliberately had out.
        if (a_in.weaponDrawn) {
            return Action::kNone;
        }
        // r20b: MIRROR-ONLY BY DEFAULT. Drawing a sheathed weapon is the ONLY
        // way this feature can come to owe a sheathe, and an unpaid sheathe is
        // what wrote a broken weapon state into the user's SAVE (field
        // 2026-07-20: reload showed empty hands with the weapon on the hip,
        // because the save recorded a drawn state we had manufactured and the
        // rebuilt graph put the model back on its sheath node). No draw, no
        // debt, nothing that can outlive the menu.
        //
        // Deliberately NOT folded into `stillPreviewing` above: this gates only
        // the START of a preview, never the end. A debt already taken must be
        // paid even if the setting flips mid-menu. That is exactly the r19c
        // lesson - a predicate gating BOTH ends of a lifetime must be stable -
        // so this one is allowed to gate precisely one end.
        if (!a_in.autoDraw) {
            return Action::kNone;
        }
        return Action::kDraw;
    }

}  // namespace MTB::WeaponPreviewGate
