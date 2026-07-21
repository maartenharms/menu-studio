// Unit tests for the equip-sound mute ledger. No engine, no Skyrim: the
// bookkeeping takes opaque pointers so its invariants can be checked on the
// desk, like the weapon-preview gate.
//
// THE BUG THESE TESTS EXIST FOR (field report, 2026-07-21):
//
//   "If you open the inventory menu while the weapon-drawing animation is
//    playing, and then swap weapons within the inventory, the sound effects
//    for drawing or sheathing the weapon will be missing after exiting the
//    inventory menu."
//
// ScopedEquipSoundMute silenced BOTH hands by calling the same Take() twice.
// Dual-wield one weapon type and both hands hold the SAME base form, so the
// second call read back the nulls the first had just written, saved those as
// "the originals", and the restore loop put the real sounds back and then
// overwrote them with null. The form stayed silent for the rest of the
// session, because a base form is shared by every instance of that weapon.
#include "EquipSoundMute.h"

#include <cstdio>

using MTB::EquipSoundLedger;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace {
    // Stands in for a TESObjectWEAP: the two sound-descriptor fields the mute
    // nulls, and nothing else.
    struct FakeWeapon {
        void* equip;
        void* unequip;
    };

    // Real descriptors, distinct so a mix-up is visible.
    int  g_drawSound{};
    int  g_sheatheSound{};
    void* const kDraw    = &g_drawSound;
    void* const kSheathe = &g_sheatheSound;

    // The whole cycle exactly as ScopedEquipSoundMute runs it: save what the
    // form carries, null it, then hand it all back.
    void Mute(EquipSoundLedger& a_ledger, FakeWeapon* a_weapon) {
        if (!a_weapon || !a_ledger.Save(a_weapon, a_weapon->equip, a_weapon->unequip)) {
            return;  // already muted - MUST NOT touch the fields again
        }
        a_weapon->equip   = nullptr;
        a_weapon->unequip = nullptr;
    }

    void RestoreAll(const EquipSoundLedger& a_ledger) {
        a_ledger.RestoreAll([](void* a_weapon, void* a_equip, void* a_unequip) {
            auto* const w = static_cast<FakeWeapon*>(a_weapon);
            w->equip      = a_equip;
            w->unequip    = a_unequip;
        });
    }
}

int main() {
    // --- one weapon, one hand: the ordinary case --------------------------
    {
        FakeWeapon      sword{ kDraw, kSheathe };
        EquipSoundLedger ledger;
        Mute(ledger, &sword);
        CHECK(sword.equip == nullptr);
        CHECK(sword.unequip == nullptr);
        CHECK(ledger.Count() == 1);
        CHECK(ledger.AnySound());
        RestoreAll(ledger);
        CHECK(sword.equip == kDraw);
        CHECK(sword.unequip == kSheathe);
    }

    // --- two DIFFERENT weapons, one per hand ------------------------------
    {
        FakeWeapon      right{ kDraw, kSheathe };
        FakeWeapon      left{ kDraw, kSheathe };
        EquipSoundLedger ledger;
        Mute(ledger, &right);
        Mute(ledger, &left);
        CHECK(ledger.Count() == 2);
        CHECK(right.equip == nullptr && left.equip == nullptr);
        RestoreAll(ledger);
        CHECK(right.equip == kDraw && right.unequip == kSheathe);
        CHECK(left.equip == kDraw && left.unequip == kSheathe);
    }

    // --- THE REGRESSION: the SAME form in both hands ----------------------
    // Dual-wielding one weapon type. GetEquippedObject(false) and (true)
    // return the same base form, so the mute is asked to silence it twice.
    {
        FakeWeapon      dagger{ kDraw, kSheathe };
        EquipSoundLedger ledger;
        Mute(ledger, &dagger);
        Mute(ledger, &dagger);  // the second hand - same pointer
        CHECK(ledger.Count() == 1);  // saved ONCE, not twice
        CHECK(dagger.equip == nullptr);
        RestoreAll(ledger);
        // The whole bug in two lines: these were null before the fix.
        CHECK(dagger.equip == kDraw);
        CHECK(dagger.unequip == kSheathe);
    }

    // --- a weapon that carries no sounds at all ---------------------------
    // Silent forms are legitimate (the draw sound can live on the animation
    // as a sound ANNOTATION instead), and AnySound() is what the log line
    // uses to say "NOTHING TO MUTE" rather than claiming a mute that did
    // nothing.
    {
        FakeWeapon      silent{ nullptr, nullptr };
        EquipSoundLedger ledger;
        Mute(ledger, &silent);
        CHECK(ledger.Count() == 1);
        CHECK(!ledger.AnySound());
        RestoreAll(ledger);
        CHECK(silent.equip == nullptr);
    }

    // --- nulls and overflow are refused, not stored -----------------------
    {
        EquipSoundLedger ledger;
        CHECK(!ledger.Save(nullptr, kDraw, kSheathe));
        CHECK(ledger.Count() == 0);

        FakeWeapon a{}, b{}, c{};
        CHECK(ledger.Save(&a, kDraw, kSheathe));
        CHECK(ledger.Save(&b, kDraw, kSheathe));
        // Only two hands exist; a third would have nowhere to be restored
        // from, and silently dropping it is how a form stays muted.
        CHECK(!ledger.Save(&c, kDraw, kSheathe));
        CHECK(ledger.Count() == 2);
    }

    // --- restore is idempotent -------------------------------------------
    // Restore runs from a destructor, but a second pass must not be able to
    // write anything different: the ledger holds values, not live reads.
    {
        FakeWeapon      axe{ kDraw, kSheathe };
        EquipSoundLedger ledger;
        Mute(ledger, &axe);
        RestoreAll(ledger);
        RestoreAll(ledger);
        CHECK(axe.equip == kDraw && axe.unequip == kSheathe);
    }

    if (g_failures == 0) {
        std::printf("equip_sound_mute_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
