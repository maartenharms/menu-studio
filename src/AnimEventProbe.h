#pragma once

namespace MTB::AnimEventProbe {

    // DIAGNOSTIC ONLY (2026-07-20). Logs every animation event the PLAYER's
    // behaviour graph raises, tagged LIVE or MENU.
    //
    // WHY THIS AND NOT ANOTHER FIX. Four explanations for the wrong idle after
    // a cross-class swap died in one day: the class variable was correct, the
    // weapon's node was correct, running the transition over real frames
    // changed nothing, and Open Animation Replacer turned out not to be loaded
    // at all. Every one of those was a change made in the hope it would help.
    //
    // Nobody has yet WATCHED THE CASE THAT WORKS. A weapon swap in the live
    // world produces the right pose every time, and we have never recorded what
    // the graph does during it. This records both, so the working sequence and
    // the broken one can be put side by side and the difference read rather
    // than guessed.
    //
    // Graph states raise events on entry, so the event stream also names which
    // states the graph actually entered - which is the thing no variable we can
    // read has been able to tell us.
    //
    // Reads only. Never writes engine state, never sends an event. The r8
    // prohibition on SYNTHETIC events is about injecting them; this only
    // listens.

    // Attach the sink to the player's graphs. Idempotent and cheap to retry:
    // the graph does not exist before the 3D loads and is rebuilt across a
    // load, so this is called per tick until it takes and re-armed on reset.
    void Install();

    // Drop the installed flag so Install() re-attaches to a rebuilt graph.
    // kPreLoadGame / kNewGame, alongside the other resets.
    void Reset();

    // Tag subsequent events. TRUE while a bubbled menu is armed, so the log
    // separates the broken case from the working one without a timestamp diff.
    void SetArmed(bool a_armed);

    // The same flag, for ClipProbe. Deliberately shared rather than a second
    // copy: two flags set from the same call sites are two flags that can
    // drift, and a clip line tagged [live] next to an event line tagged [MENU]
    // would discredit both logs at the moment they matter most.
    bool IsArmed();

}  // namespace MTB::AnimEventProbe
