#pragma once

namespace MTB::ActorTickProbe {

    // DIAGNOSTIC ONLY (2026-07-20, r24). Counts how many times the PLAYER's
    // Actor::Update (vfunc 0x0AD) runs, so the log can say whether it runs at
    // all while a menu holds the pause.
    //
    // WHY. r23 read the clip the graph actually plays and found it picks
    // 2HW_Equip for a dagger - the graph still believes the OLD weapon is
    // equipped. r22 found why it is never told: the engine's replace
    // transition does not run while paused, it runs ~111 ms AFTER the menu
    // closes.
    //
    // Something has to drain that. Bubble ticks the player's ANIMATION
    // (UpdateAnimation) every frame while paused, but never the ACTOR
    // (Actor::Update), and ActorEquipManager::EquipObject takes a queueEquip
    // flag - so a queue that only drains from the actor's own per-frame update
    // would explain every observation exactly.
    //
    // That is a hypothesis, not a finding. This measures it. If the count does
    // not move while a menu is armed, the actor genuinely stops updating and
    // the search has a specific target. If it DOES move, the hypothesis is
    // dead and we have spent one cheap hook instead of a redesign.
    //
    // Reads only. The original runs first and unconditionally.

    // Install the vfunc hook. Once per process; safe to call repeatedly.
    void Install();

    // Total calls seen since load.
    std::uint64_t Count();

    // Log the delta across a menu session. Call on arm and on disarm.
    void MarkArmed(bool a_armed);

}  // namespace MTB::ActorTickProbe
