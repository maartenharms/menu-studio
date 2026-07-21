// Unit tests for the weapon-preview decision. No engine, no Skyrim: the gate
// takes plain booleans so its invariants can be checked on the desk.
#include "WeaponPreviewGate.h"

#include <cstdio>

using namespace MTB::WeaponPreviewGate;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace {
    // A player standing in a bubbled menu with a sheathed weapon equipped:
    // the case the whole feature exists for.
    Input Ready() {
        Input in;
        in.enabled        = true;
        in.bubbleArmed    = true;
        in.raceMenu       = false;
        in.weaponEquipped = true;
        in.weaponDrawn    = false;
        in.inCombat       = false;
        in.mounted        = false;
        in.autoDraw       = true;  // r20b: opt-in now; Ready() is the drawing case
        return in;
    }
}

int main() {
    // --- drawing ---------------------------------------------------------
    CHECK(Decide(Ready(), false) == Action::kDraw);

    // --- r20b: mirror-only (the new DEFAULT) -------------------------------
    {   // a sheathed weapon is left alone, and NO debt is taken - which is the
        // whole point: no debt means nothing can outlive the menu and end up
        // in a save.
        Input in = Ready(); in.autoDraw = false;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {   // already drawn: unchanged, still mirrored, still no debt.
        Input in = Ready(); in.autoDraw = false; in.weaponDrawn = true;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {   // THE INVARIANT THAT MATTERS. A debt already taken is still paid when
        // the setting is flipped off mid-menu. autoDraw gates the START of a
        // preview only; if it could also swallow the restore, flipping this
        // setting would strand the player permanently drawn - the exact class
        // of bug r19c was about.
        Input in = Ready(); in.autoDraw = false;
        CHECK(Decide(in, true) == Action::kNone);      // still previewing, hold
        in.bubbleArmed = false;
        CHECK(Decide(in, true) == Action::kRestore);   // menu gone, PAY IT
    }

    {   // every gate that must decline
        Input in = Ready(); in.enabled = false;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {
        Input in = Ready(); in.bubbleArmed = false;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {
        Input in = Ready(); in.raceMenu = true;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {
        Input in = Ready(); in.weaponEquipped = false;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {
        Input in = Ready(); in.inCombat = true;
        CHECK(Decide(in, false) == Action::kNone);
    }
    {
        Input in = Ready(); in.mounted = true;
        CHECK(Decide(in, false) == Action::kNone);
    }

    {   // ALREADY drawn in the world: mirror it, and never take on a debt -
        // closing the menu must not sheathe a weapon the player had out.
        Input in = Ready(); in.weaponDrawn = true;
        CHECK(Decide(in, false) == Action::kNone);
    }

    // --- holding ---------------------------------------------------------
    {   // we drew, conditions still hold: keep it drawn, do not re-draw
        Input in = Ready(); in.weaponDrawn = true;
        CHECK(Decide(in, true) == Action::kNone);
    }
    {   // we drew, then the player sheathed by hand: hold the debt, do not re-draw
        CHECK(Decide(Ready(), true) == Action::kNone);
    }

    // --- restoring -------------------------------------------------------
    // A debt is always paid. Each of these would leave the player permanently
    // drawn if the gate let the condition swallow the restore.
    {
        Input in = Ready(); in.weaponDrawn = true; in.bubbleArmed = false;
        CHECK(Decide(in, true) == Action::kRestore);
    }
    {
        Input in = Ready(); in.weaponDrawn = true; in.enabled = false;
        CHECK(Decide(in, true) == Action::kRestore);
    }
    {
        Input in = Ready(); in.weaponDrawn = true; in.mounted = true;
        CHECK(Decide(in, true) == Action::kRestore);
    }
    {
        Input in = Ready(); in.weaponDrawn = true; in.inCombat = true;
        CHECK(Decide(in, true) == Action::kRestore);
    }
    {   // the weapon was unequipped while we owed a restore
        Input in = Ready(); in.weaponDrawn = true; in.weaponEquipped = false;
        CHECK(Decide(in, true) == Action::kRestore);
    }
    {
        Input in = Ready(); in.weaponDrawn = true; in.raceMenu = true;
        CHECK(Decide(in, true) == Action::kRestore);
    }

    if (g_failures == 0) {
        std::printf("all WeaponPreviewGate tests passed\n");
        return 0;
    }
    std::printf("%d WeaponPreviewGate test(s) FAILED\n", g_failures);
    return 1;
}
