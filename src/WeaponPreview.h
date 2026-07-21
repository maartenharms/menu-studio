#pragma once

namespace MTB::WeaponPreview {

    // F-26. While a bubbled menu is open, draw the player's equipped weapon so
    // it is visible in their hand, and sheathe it again on close.
    //
    // The design decision that makes this small: we drive weapon STATE and let
    // the engine's animation graph pick the drawn idle for the weapon class.
    // Nothing here knows about one-handers, greatswords or bows - the graph
    // does, it stays correct under animation replacers, and it handles modded
    // weapon classes we have never heard of. A bow is the one genuinely special
    // case (drawn, it parents to the Shield node rather than Weapon) and that
    // too is the engine's own table, not ours.
    //
    // MAIN THREAD ONLY - every call mutates engine state.

    // Evaluate the gate and act. Safe to call every tick: it is a no-op unless
    // the decision changes. Call at the bubble's arm edge and per tick.
    void Update(RE::PlayerCharacter* a_player, bool a_bubbleArmed, bool a_raceMenu);

    // Pay any outstanding restore immediately (bubble disarm).
    void Restore(RE::PlayerCharacter* a_player);

    // TRUE while this preview owes a sheathe. Bubble asks before deferring a
    // restore: the deferral has to be taken whenever a debt exists, NOT merely
    // when the bubble was armed. A disarm that never armed (the unpause race /
    // missing 3D) still has to carry the debt forward, or a menu switch whose
    // second menu fails to arm strands it and the player stays drawn.
    [[nodiscard]] bool HasDebt();

    // Drop all state without touching the engine - the 3D is already gone.
    // kPreLoadGame / kNewGame, alongside Bubble::ForceReset.
    void Reset();

    // DIAGNOSTIC ONLY (2026-07-20), reads nothing but the graph and the biped.
    // Call every frame while DISARMED. Logs the same observables the pump path
    // logs, on a live unpaused weapon change, so a menu swap can be diffed
    // against a known-good one instead of against an assumption. Owns state
    // separate from Update()'s and can never affect the restore debt.
    void ObserveUnarmed(RE::PlayerCharacter* a_player);

    // r26. Run the ENGINE's own equip clip to completion, inside this frame.
    //
    // Called from EquipNotifyGate right after Actor::OnItemEquipped returns,
    // which is the only moment this can work: the notification lands ~14 ms
    // AFTER our own swap handler has already been and gone, so nothing in
    // Update()'s path can carry it.
    //
    // Without this, the field saw the bow: r25 let the engine start Bow_Equip
    // correctly, nobody pumped it, and 1.16 s of unsheathe animation played out
    // over real frames. It was intermittent because NormalizeToTerminal only
    // pumps a weapon state caught mid-draw, and after an equip the state is
    // already terminal - so whether anything carried the clip depended on
    // timing.
    //
    // ⚠ ARMED ONLY. Pumping the graph while the world is live is the r8 shape -
    // that is what produced a walking loop in a frozen scene. The caller gates
    // on the bubble being armed and this asserts nothing about it, so keep the
    // gate at the call site where it is visible.
    void PumpEngineEquip(RE::PlayerCharacter* a_player);

    // TRUE while a draw or sheathe THIS feature asked for is still in flight.
    // Bubble's r36 freeze consults this so our own transition is allowed to
    // animate, while every other transition still holds its caught frame.
    //
    // CALLER ORDERING: Update() must run BEFORE the freeze reads this in the
    // same tick. The flag is only ever cleared inside Update(), so a freeze
    // evaluated first at an arm edge can still be holding a stale true from the
    // previous menu's disarm sheathe. Update() is also what starts the arm-edge
    // draw, so it has to come first anyway - reading before updating would
    // freeze the very transition this exists to exempt.
    [[nodiscard]] bool TransitionInFlight();

    // TRUE when the arm-edge normalize could not drive the player's OWN
    // draw/sheathe to a terminal state inside its budget - i.e. the clip is a
    // replaced one that runs longer than the pump was sized for.
    //
    // Bubble's r36 freeze consults this the same way it consults
    // TransitionInFlight(): a clip we cannot normalize is allowed to ANIMATE to
    // completion, because the alternative is what the field reported - the
    // transition held half-played for the whole menu and then leaking into live
    // gameplay on close. Clears itself once the state goes terminal.
    [[nodiscard]] bool NormalizeCapped();

    // Release the per-arm normalize latch. Bubble calls this at the arm edge,
    // BEFORE the first Update() of the session.
    void ArmEdgeReset();

}  // namespace MTB::WeaponPreview
