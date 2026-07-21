#pragma once

#include <cstddef>

namespace MTB {
    // Bookkeeping for ScopedEquipSoundMute (WeaponPreview.cpp): remember what
    // each weapon's equip/unequip sound descriptors were before we nulled
    // them, so they can be put back exactly as they were.
    //
    // Engine-free on purpose. It holds opaque pointers and never dereferences
    // them, so the one invariant that actually bit us can be checked on the
    // desk - tests/equip_sound_mute_test.cpp.
    //
    // ⚠ THE INVARIANT: SAVE EACH WEAPON ONCE. The mute silences both hands,
    // and the first version simply ran the same save-and-null twice:
    //
    //     Take(GetEquippedObject(false), slot_[0]);
    //     Take(GetEquippedObject(true),  slot_[1]);
    //
    // Dual-wield one weapon type and BOTH hands hold the same base form (this
    // file's own neighbour says so: "dual-wielding one weapon type gives both
    // hands the same sheath node"). The second call then read back the nulls
    // the first had just written, stored THOSE as the originals, and the
    // restore loop put the real sounds back and immediately overwrote them
    // with null. The form stayed silent for the rest of the session, because
    // equipSound is a BASE FORM field shared by every instance of that weapon.
    //
    // Field report that found it: "swap weapons within the inventory, the
    // sound effects for drawing or sheathing the weapon will be missing after
    // exiting the inventory menu."
    //
    // The general shape is worth naming, because it is not really about
    // sounds: SAVE/RESTORE OF SHARED STATE MUST BE KEYED ON THE OBJECT, NOT
    // ON THE SLOT THAT REACHED IT. Two slots can name one object.
    class EquipSoundLedger {
    public:
        // One entry per hand. A third weapon cannot exist, and accepting one
        // would mean an entry with nowhere to be restored from.
        static constexpr int kMaxSlots = 2;

        // Record a weapon and the descriptors it carried BEFORE anything was
        // nulled. Returns TRUE when the caller should now null the weapon's
        // fields, FALSE when it must leave them alone - null weapon, ledger
        // full, or, the case this exists for, a weapon already saved.
        bool Save(void* a_weapon, void* a_equip, void* a_unequip) noexcept {
            if (!a_weapon || count_ >= kMaxSlots) {
                return false;
            }
            for (int i = 0; i < count_; ++i) {
                if (saved_[i].weapon == a_weapon) {
                    return false;  // the other hand holds this same form
                }
            }
            saved_[count_++] = { a_weapon, a_equip, a_unequip };
            return true;
        }

        // Hand every saved weapon back its own descriptors, once each.
        // a_restore is called as (weapon, equip, unequip).
        template <class Fn>
        void RestoreAll(Fn&& a_restore) const {
            for (int i = 0; i < count_; ++i) {
                a_restore(saved_[i].weapon, saved_[i].equip, saved_[i].unequip);
            }
        }

        [[nodiscard]] int Count() const noexcept { return count_; }

        // Whether anything actually had a sound to silence. A weapon can
        // legitimately carry none - the draw sound may live on the animation
        // as a sound annotation instead - and the log line says so rather
        // than claiming a mute that silenced nothing.
        [[nodiscard]] bool AnySound() const noexcept {
            for (int i = 0; i < count_; ++i) {
                if (saved_[i].equip || saved_[i].unequip) {
                    return true;
                }
            }
            return false;
        }

    private:
        struct Saved {
            void* weapon{ nullptr };
            void* equip{ nullptr };
            void* unequip{ nullptr };
        };
        Saved saved_[kMaxSlots]{};
        int   count_{ 0 };
    };
}
