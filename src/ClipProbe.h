#pragma once

namespace MTB::ClipProbe {

    // DIAGNOSTIC ONLY (2026-07-20, r23). Logs the NAME OF THE ANIMATION FILE
    // the player's behaviour graph actually starts playing, tagged live/MENU.
    //
    // WHY THIS EXISTS. Every measurement this bug has produced so far reads an
    // INPUT to the stance choice, and every one of them has now come back
    // matching the case that works:
    //
    //   - the class variable (iRightHandEquipped) - correct after r22 ARM A
    //   - the weapon's skeleton node - correct, per-slot biped dump
    //   - the animation EVENT stream - identical to a live swap (r21, r22)
    //   - real frames instead of an in-frame pump - no change (r20b)
    //   - Open Animation Replacer - was never loaded (r21)
    //
    // Nothing that feeds the choice is wrong, and the pose is still wrong. So
    // stop reading inputs. This reads the OUTPUT: which clip is on screen.
    //
    // If it says 2hm_idle while a dagger is equipped, the graph genuinely
    // picked the wrong clip and the search moves to why. If it says the RIGHT
    // clip while the body still looks wrong, the bug is not clip selection at
    // all and the whole line of enquiry relocates - which is worth as much.
    //
    // HOW. write_vfunc on hkbClipGenerator::Activate (vfunc 0x04). The clip
    // carries its own animationName at +0x048, so the name is read from the
    // object that is about to play it - no address arithmetic on an opaque
    // hkRefVariant, and vfunc hooks are immune to the AE inlined-call-site
    // trap that has bitten this project before.
    //
    // Reads only. Calls the original FIRST and then logs, so ordering is
    // untouched. Never sends an event - the r8 prohibition is on INJECTING
    // synthetic events, and this only watches.

    // Install the vfunc hook. Once per process; safe to call repeatedly.
    void Install();

    // Cache the player's hkbCharacter pointers so the hook can ignore every
    // other actor in the cell. Call wherever the player's graphs are walked.
    void TrackPlayerGraphs();

    // Drop the cached pointers. MUST run on load / new game: the graph is
    // rebuilt and the old hkbCharacter allocations can be recycled, so a stale
    // pointer could start matching somebody else's clips.
    void Reset();

    // ── NOT DIAGNOSTIC. This one is load-bearing, and it is why the hook above
    // is now installed unconditionally. ────────────────────────────────────────
    //
    // TRUE while a draw/sheathe (equip/unequip) CLIP is still playing on the
    // player.
    //
    // THE STATE LIES; THE CLIP DOES NOT. r11 established that kDrawn is set by
    // the `weaponDraw` ANNOTATION, roughly HALFWAY through the clip - so for the
    // whole back half of an unsheathe the weapon state already reads terminal
    // while the animation is visibly still running. Every attempt to catch
    // "caught mid-draw" by reading the weapon state therefore missed: 21 field
    // arm edges, every one reporting a terminal state, while the user was
    // deliberately opening the menu mid-unsheathe.
    //
    // WHY IT MATTERS (the 0.7.1 field bug). If we tick the animation graph while
    // that clip finishes INSIDE a paused menu, the graph runs on to pick its
    // idle - and the idle is picked with the Papyrus VM frozen, so mods that
    // publish their state through magic-effect markers (Smooth Moveset, and the
    // Nolvus OAR stance framework that keys 420 of its 662 configs on them) are
    // evaluated against stale markers. The higher-priority "Idle Loop" submod
    // fails its HasMagicEffect condition, the lower-priority "Idle Start" wins,
    // a hkbManualSelectorGenerator LATCHES it, and a ~4 s step-into-stance clip
    // loops forever: "the character is lunging forward every 4 seconds".
    //
    // Vanilla pause menus never hit this because they do not tick the graph -
    // the clip stays frozen and completes after unpause, with live conditions.
    // Holding the caught pose restores exactly that ordering.
    //
    // Measured, not guessed: the clip's own duration is read at Activate time
    // from binding->animation->duration and scaled by playbackSpeed, so a long
    // replaced draw (Weapon Styles and friends) is covered without a magic
    // number, and a short vanilla one does not over-freeze.
    [[nodiscard]] bool EquipClipInFlight();

    // TRUE while the graph has started ANY clip very recently - i.e. it is
    // still moving between poses rather than sitting in a settled idle.
    //
    // The draw/sheathe hold above is specific to equips, but the underlying
    // hazard is not: ANY transition whose progression depends on script or
    // effect state gets mis-resolved while a menu freezes the Papyrus VM. Field
    // report on Movement Behavior Overhaul: come to a stop, open the inventory,
    // and the stop animation loops - the same defect, entered through
    // locomotion instead of an unsheathe.
    //
    // ⚠ ARM EDGE ONLY. As a per-frame gate this would stutter (freeze, expire,
    // tick one frame, a clip activates, freeze again). As an arm-edge latch it
    // reads "this menu opened mid-transition, so hold the caught pose", which
    // is what every other freeze reason already does.
    [[nodiscard]] bool GraphSettling();

    // Seconds left of the equip clip currently playing, or 0. The equip pump
    // uses this instead of a fixed step budget: the shipped budget is 1.5 s and
    // the field log has a 2.00 s 1HM_Unequip, so the tail was left to play
    // after the menu closed - which is why the unsheathe SOUND fires on exit
    // with the weapon already drawn.
    //
    // ⚠ WALL CLOCK, NOT GRAPH TIME. Inside a pump the wall clock advances a
    // couple of milliseconds while the graph advances a second and a half, so
    // this is only meaningful ACROSS frames. Do not poll it inside a pump loop;
    // EquipTransitionUnfinished() is the in-pump question.
    [[nodiscard]] float EquipClipRemainingSeconds();

    // TRUE while some player graph has started a draw/sheathe clip and has NOT
    // yet reached an idle. Unlike EquipClipInFlight() this carries no settle
    // window and retires nothing, so it can be polled step-by-step inside a
    // pump: it answers "is the transition I am driving still running?".
    [[nodiscard]] bool EquipTransitionUnfinished();

    // Remaining GRAPH time on the closest active equip clip, unlike the
    // wall-clock value above. A pump checks this before every synthetic step
    // and holds when one step would cross into idle selection. Negative means
    // no active transition; zero means an unfinished transition whose boundary
    // could not be read and must fail closed.
    [[nodiscard]] float EquipTransitionRemainingGraphSeconds();

    // Monotonic count of idle clip activations on the player's graphs. Sample
    // it, step the graph, sample again: a change means the step you just took
    // re-picked the idle.
    //
    // ⚠ This is the signal EquipTransitionUnfinished() cannot give. A pump with
    // no draw in flight has nothing to "still be finishing", so a guard built
    // only on that predicate never engages for it - which is exactly how the
    // first bound left `45/45 steps` running on the pump that fires BEFORE the
    // equip clip exists, and on the swap pump that fires after it settled.
    // Those two were doing most of the damage (field 2026-07-21 14:58).
    [[nodiscard]] std::uint64_t IdlePickCount();

    // TRUE while any tracked player graph's most recently started clip is a
    // LOCOMOTION clip (walk, run, sprint, strafe, turn).
    //
    // ⚠ THIS EXISTS BECAUSE IdlePickCount() IS THE WRONG QUESTION for "did the
    // graph settle". That counts idle ACTIVATIONS, and a graph blending back
    // into an idle generator that is already active never calls Activate again -
    // so a settle that WORKED looked exactly like one that failed. The
    // locomotion settle read "NO IDLE ACTIVATED" on every single arm and froze
    // the character every time, which is the user's "even when we are
    // performing regular movement and we open the menu the character is
    // frozen".
    //
    // What the graph is PLAYING answers it directly, and the Activate hook
    // already sees every clip, so this costs one string classification per
    // activation and no new machinery.
    [[nodiscard]] bool InLocomotionClip();

    // ── MEASUREMENT (2026-07-21). Why the clip line grew three fields. ────────
    //
    // The 0.7.1 handoff's pass/fail test was "a 3.33 s idle in a menu is a
    // wrong pick, 9-20 s is a right one". The next field log refuted it: a
    // 3.33 s idle activates LIVE too (14:25:38.832 and 14:25:43.868), so the
    // duration alone convicts nothing. The same log also showed a SECOND idle
    // node the handoff never mentions - `1HM_PickIdle` alongside `1HM_Idle` -
    // and it is the one that churns.
    //
    // What the line could not say, and now must:
    //
    //  - WHICH GRAPH. The player has two and their clip lengths differ by
    //    hundredths, so a re-pick on one reads exactly like a first pick on the
    //    other. Every earlier trace is ambiguous for this reason alone.
    //  - WHICH PUMP, or none. Every in-menu activation in that log fell inside
    //    a `weapon preview: pump` window by timestamp - one swap runs three
    //    pumps and burns 4.4 s of graph time in 12 ms of wall time. Whether a
    //    re-pick happens inside our own fast-forward or on a real ticked frame
    //    is the whole mechanism question, and a timestamp diff against a
    //    separate log line is not evidence anyone should have to reconstruct.
    //  - THE PLAYBACK MODE. A looping clip wraps at its end; a single-play clip
    //    ENDS and hands control back to the state machine, which is what a
    //    re-pick is. Whether the idles loop decides whether pumping past one
    //    can cause a re-pick at all, and nothing has ever measured it.
    void PumpWindowBegin(const char* a_why);
    void PumpWindowEnd();

    // RAII for the above - a pump has early returns and an un-ended window
    // would mislabel every later clip line as pumped.
    struct PumpWindow {
        explicit PumpWindow(const char* a_why) { PumpWindowBegin(a_why); }
        ~PumpWindow() { PumpWindowEnd(); }
        PumpWindow(const PumpWindow&)            = delete;
        PumpWindow& operator=(const PumpWindow&) = delete;
    };

    // Per-armed-session counters, reported at the disarm edge. A defect you
    // cannot count is a defect you cannot close - the blink took three rounds
    // of "hard to tell" and closed on a blink COUNT.
    void ArmedSessionBegin();
    void ArmedSessionReport();

    // TRUE after any equip/unequip clip activates on a tracked player graph
    // while this menu is armed. Reset at ArmedSessionBegin. Unlike
    // EquipClipInFlight this is deliberately a session latch: an expired
    // wall-clock hold must not reopen body-graph ticking under the pause.
    [[nodiscard]] bool EquipOccurredThisSession();

    // Graph seconds a pump has just advanced, for the pump's own log line.
    void NotePumpSeconds(float a_seconds);

    // ── THE STANCE MARKERS. Read off the OAR configs on disk, 2026-07-21. ─────
    //
    // The lunge is a STABLE ATTRACTOR, not churn. Field trace, one 3.33 s cycle
    // repeating for eight seconds with ZERO clip activations in the window:
    //
    //   FootScuffRight -> FootScuffLeft -> Smooth_loop -> PickNewIdle
    //                  -> Smooth_NonCombat -> FootScuffRight -> ...
    //
    // `PickNewIdle` fires every cycle and gets the same answer every cycle,
    // because the thing that decides it cannot change while a menu is open.
    // From `Sword Non Combat Idle Loop/config.json` (priority ...071):
    //
    //     HasMagicEffect  Smooth Moveset.esp | 0x802
    //     HasMagicEffect  Smooth Moveset.esp | 0x804   NEGATED
    //
    // `Sword Non Combat Idle Start` (priority ...070) carries no such gate, so
    // it is the fallback that wins whenever Loop is unsatisfiable. 239 configs
    // in the Nolvus stance framework key on 0x802 and 314 on 0x804 - the whole
    // framework is built on those two markers.
    //
    // ⚠ WHICH marker is wrong while armed is NOT yet measured, and the fix
    // depends entirely on the answer, so this logs it rather than assuming it.
    // Two conclusions have already been reasoned past the evidence this session
    // (the "3.33 s means a wrong pick" rule, and my own "the pumps are the
    // driver"), and both cost a field round.
    //
    // Reads only. Never adds, removes or dispels anything.
    void LogStanceMarkers(bool a_armed);

    // ── THE FIX. Measured, then built - in that order, for once. ─────────────
    //
    // Field 2026-07-21 16:34, the user's own repro (open the menu at the end of
    // an unsheathe, then swap):
    //
    //   16:34:51 -> 16:35:08  [MENU]  loop=true combat=true   FAILS
    //   16:35:09              [live]  loop=true combat=false  PASSES
    //
    // The installed ESP settles the meaning that the OAR configs alone could
    // not: 0x802 is `mag_loop on`; 0x804 is the permanent `mag_combat on`
    // ability. It is not an "idle starting" timer. The same ESP supplies the
    // paired `mag_combat off` effect and `sp_combat off` spell.
    //
    // In a non-combat menu, cast that paired off spell once for each continuous
    // appearance of 0x804. This uses Smooth Moveset's own state transition and
    // leaves a legitimate combat stance alone.
    //
    // ⚠ r2 - DISPEL WAS THE WRONG LEVER, and the field said so:
    //
    //   16:43:50  starting=false PASSES     <- dispel appeared to work
    //   16:43:52  starting=true  FAILS      <- and then it was back, still paused
    //
    // Dispel FLAGS an effect for a sweep that runs in the actor's update, and
    // that update does not run while paused (measured: 0 calls across 403
    // frames). So the flag stuck, the effect stayed in the list, HasMagicEffect
    // kept counting it, and r1's own "skip anything already dispelled" guard
    // meant it never even tried again. Three mistakes, one line.
    //
    // The retired implementation called ActiveEffect::Update every menu frame.
    // Field logging then showed duration=0 and an ever-growing elapsed clock:
    // a permanent ability can never expire that way, and the intervention
    // leaked the bad state into gameplay. This path must never tick 0x804.
    //
    // No-op when Smooth Moveset.esp is absent and armed-only.
}  // namespace MTB::ClipProbe
