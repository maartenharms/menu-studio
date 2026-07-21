#pragma once

namespace MTB::EquipNotifyGate {

    // r25. THE WRONG-IDLE BUG, AND IT IS OUR OWN PAUSE DOING IT.
    //
    // Chain, every link measured or read off the AE 1.6.1170 binary:
    //
    //  1. Swapping a weapon in a menu is NOT queued. The equip dispatcher
    //     checks UI::numPausesGame (+0x160) and UI::numItemMenus (+0x164) and
    //     takes the IMMEDIATE path precisely BECAUSE a menu is open. The item
    //     really is equipped inline, and Actor::UpdateWeaponAbility runs.
    //
    //  2. What does NOT run is the notification. PlayerCharacter::OnItemEquipped
    //     (vfunc 0x0B2) sets bit 0x20 at player+0xBE0 when numPausesGame != 0;
    //     Actor::OnItemEquipped then calls Unk_B3 (vfunc 0x0B3, which on the
    //     PLAYER returns !(byte_BE0 & 0x20)) and RETURNS IMMEDIATELY on it.
    //     Skipped: the weapon re-parent (vfunc 0x0B4), the per-hand graph
    //     notifications, and the tail AIProcess call.
    //
    //  3. So the animation graph is never told the weapon changed. r23's clip
    //     probe caught the consequence directly: a forced draw plays
    //     Animations\2HW_Equip.hkx -> 2HW_Idle.hkx with an Iron Dagger in hand,
    //     where the same swap live plays Dag_Equip.hkx -> 1HM_Idle.hkx.
    //
    //  4. On menu close numPausesGame drops, the notification runs, and it
    //     corrects itself ~112 ms later. Which is exactly what the field saw.
    //
    // ⚠ THE COUNTER IS OURS. numPausesGame counts any menu with kPausesGame,
    // and r19's MTB_StudioPause is one. Vanilla suppresses this too - it is why
    // Skyrim never animates an equip in a paused inventory - but Skyrim Souls
    // UNPAUSES the menu, which would let it run, and we re-pause and
    // re-suppress it. The bug is self-inflicted by the pause architecture.
    //
    // ⚠ TWO DEAD EXPLANATIONS, BOTH MINE, RECORDED SO NOBODY RE-RUNS THEM:
    //   - "the engine queues its replace and it never gets to finish" (r13's
    //     premise). It is not queued and it does not start. REFUTED by r22.
    //   - "the equip is queued and the queue drains from Actor::Update, which
    //     does not run while paused" (r24). Actor::Update really does not run -
    //     measured, 0 calls across 403 frames - but that is NOT the gate. The
    //     real queue is a "3D not loaded yet" deferral drained from 3D-attach
    //     and game-load paths. A correct measurement of the wrong link.
    //   - and iRightHandEquipped is NOT what selects the clip. It read 0 when
    //     the draw was issued and 2 a millisecond later while the graph played
    //     the two-handed clip throughout. Five rounds were spent confirming a
    //     variable that was never connected to the bug.
    //
    // WHAT THIS DOES. Nothing is forced, sent, or called. The equip path
    // already runs inline; only the notification short-circuits. This makes
    // Unk_B3 answer "do not bail" while the bubble is armed, and the engine
    // does the rest itself - including picking the right clip.
    //
    // Deliberately hooks the DECISION, not the bit: anything else that reads
    // player+0xBE0 keeps seeing the truth. And it is a vfunc hook, so no raw
    // address and no exposure to the AE inlined-call-site trap.
    //
    // ⚠ We know of exactly ONE caller of Unk_B3 (inside Actor::OnItemEquipped).
    // That is why this is scoped to the armed window and gated OFF by default
    // rather than installed unconditionally.

    // Install the vfunc hook. Once per process; safe to call repeatedly.
    void Install();

    // TRUE while a bubbled menu is armed. The override only applies inside
    // this window AND only when the INI/checkbox enables it.
    void SetArmed(bool a_armed);

}  // namespace MTB::EquipNotifyGate
