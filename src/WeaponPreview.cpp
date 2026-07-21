#include "PCH.h"

#include "WeaponPreview.h"

#include "ClipProbe.h"  // the equip pump budgets from the clip's real length
#include "EquipSoundMute.h"
#include "Settings.h"
#include "WeaponPreviewGate.h"

#include <cmath>

namespace MTB::WeaponPreview {

    namespace {
        // Do WE owe a sheathe? The single piece of state that matters: if this
        // is ever left true with nothing to pay it, the player walks around
        // permanently drawn.
        bool weDrew = false;
        // The weapon the preview is currently showing. A swap in the menu
        // changes this and re-runs the draw so the graph moves to the new
        // class's idle.
        RE::FormID shownWeapon = 0;
        // The LEFT-hand item, tracked for the same reason (r10). Equipping a
        // shield does not touch the right hand, so watching only shownWeapon
        // meant a shield equipped inside the menu was never noticed at all and
        // nothing ever ran the transition that attaches it - which is the whole
        // of "if i equip a shield it's also invisible until after i exit the
        // menu". A shield is a TESObjectARMO, so the FormID is compared here
        // rather than anything weapon-shaped.
        RE::FormID shownLeft = 0;
        // WEAPON_TYPE of the shown right-hand weapon, -1 for none (r13). The
        // swap path spends a full sheathe+redraw only when the CLASS changes,
        // since that is the only case the engine needs to re-select an idle for,
        // and a same-class swap already looks right.
        int shownType = -1;
        // Set while we are waiting for our own transition to finish, which is
        // what unlocks the r36 freeze for it.
        bool inFlight = false;
        // 0.7.1. The normalize is attempted ONCE per arm, and remembers whether
        // it failed.
        //
        // It used to be neither. PumpToState returns whether it arrived and
        // NormalizeToTerminal threw that away, and the only guard on the call
        // was `!inFlight` - which this path can never set, because inFlight is
        // written in Draw() alone. So a pump that could not finish re-ran on
        // EVERY armed tick: 96 UpdateAnimation steps and a fresh moveStop, per
        // tick, for the whole menu. That also broke this file's own rule at
        // SettleLocomotion ("Only at pump time, NEVER per tick").
        //
        // What made it reachable is mod animations. kPumpMaxSteps is commented
        // "far past any vanilla draw" and is sized for vanilla; a replaced draw
        // clip (Weapon Styles and friends) runs past the budget, so the pump
        // caps every time and the retry loop is permanent. Field report:
        // "opening menu while unsheathing anim is playing can make it loop ...
        // the character is lunging forward every 4 seconds".
        bool normalizeTried  = false;
        bool normalizeCapped = false;

        [[nodiscard]] RE::TESForm* RightHandWeapon(RE::PlayerCharacter* a_player) {
            auto* const form = a_player->GetEquippedObject(false);
            return (form && form->IsWeapon()) ? form : nullptr;
        }

        void Draw(RE::PlayerCharacter* a_player, bool a_draw) {
            // The engine's own draw path. It moves the weapon between the
            // sheath node and the hand node itself, and the graph selects the
            // matching idle - which is the entire reason this function is two
            // lines instead of a per-weapon-class table.
            a_player->DrawWeaponMagicHands(a_draw);
            inFlight = true;
        }

        // r10 - STOP FORGING THE STATE, RUN THE ENGINE'S OWN TRANSITION TO THE END.
        //
        // r3 through r9 all tried to reach the drawn pose by hand: re-parent the
        // node, write iRightHandEquipped, send an event that jumps the graph
        // into the equipped-out state. Each round fixed the symptom in front of
        // it and exposed a new one behind it, and r8's event finally produced a
        // walking character in a frozen scene (field: "then locomotion of a walk
        // can start playing and looping") because the replace subgraph has a
        // Locomotion branch as well as a Standing one and nothing on this side
        // can know which one the graph will latch. Six rounds of that is the
        // architecture reporting itself, not six unlucky bugs.
        //
        // The field report also carries the answer: "when I do exit I see the
        // player play the unsheathe animation even though we were already
        // unsheathed in the menu and then when that animation ends only then do
        // we see the shield". So the engine never believed the weapon was drawn.
        // It reconciled on exit, and when it did, EVERY behaviour the earlier
        // rounds were trying to forge appeared on its own: the hand attach, the
        // shield, the right idle for the weapon class. Nothing was missing
        // except the time for the transition to finish.
        //
        // We already own that time. The world is paused in a bubbled menu, and
        // UpdateAnimation (TESObjectREFR vfunc 0x7D) is how this mod steps the
        // behaviour graph - once per frame, from Bubble's tick. Calling it in a
        // bounded loop inside a SINGLE frame runs the whole transition before
        // anything is rendered: the clip plays out internally, its weaponDraw
        // annotation fires and attaches the weapon, the shield attaches with it,
        // the graph selects the drawn idle for the class itself, and the actor's
        // weapon state genuinely arrives at kDrawn - so there is nothing left
        // for the engine to replay on exit.
        //
        // That restores the premise in the header, which was right from the
        // start and which every hand-made shortcut since r3 had been eroding:
        // drive weapon STATE, let the graph pick the pose. Note what this
        // DELETES - the event constants, the iRightHandEquipped write, the
        // WEAPON_TYPE mapping. None of it was ever needed; it was all
        // compensation for a transition we were cutting short.
        //
        // HARD BOUND. This steps an engine system inside one frame, so it can
        // never be open-ended: the caps below stop it and the outcome is logged
        // either way. A capped pump is a bug report, never a hang.
        constexpr float kPumpStep     = 1.0f / 30.0f;
        constexpr int   kPumpMaxSteps = 60;  // 2.0s, far past any vanilla draw

        // ⚠ r11: REACHING THE STATE IS NOT THE END OF THE ANIMATION, and r10
        // stopped at the state. Field log:
        //
        //   pump (open) - ws 2->3 want=3 in 18 steps (0.60s) ARRIVED
        //
        // so the pump worked and the actor genuinely reached kDrawn - and the
        // user still watched an unsheathe. kDrawn is set by the weaponDraw
        // ANNOTATION, which fires when the weapon leaves the sheath, roughly
        // HALFWAY through a ~1.2s clip. Stopping on that edge left the rest of
        // the clip and the blend into the idle to play out over real frames,
        // which is exactly what was reported. The shield followed from the same
        // cut: it was already parented to the SHIELD node and merely culled
        // (`shield slot (armed) - has3D=true culled=true parent='SHIELD'`),
        // waiting on a part of the clip we never ran.
        //
        // So keep pumping past the edge. The settle budget is what makes the
        // transition invisible; the state edge only proves it happened.
        constexpr int kPumpSettleSteps = 36;  // 1.2s past the state edge
        // A swap holds the actor at kDrawn throughout, so there is no state edge
        // to wait on at all - only a budget. 24 steps (0.8s) was measurably too
        // short for the same reason.
        constexpr int kPumpSwapSteps = 45;  // 1.5s

        // ── 0.7.1: EVERY PUMP RUNS THROUGH HERE, AND IT CAN STOP EARLY. ───────
        //
        // Field log 2026-07-21 14:25, one weapon swap inside the inventory:
        //
        //   42.714  pump (engine-equip) BEGIN - 45 steps (0.00s of clip left)
        //   42.715    clip [MENU] 1HM_Idle.hkx  (node '1HM_PickIdle', 22.90s)
        //   42.715    clip [MENU] Dag_Equip.hkx (node 'Dag_Equip', 1.33s)
        //   42.716  pump (engine-equip) - 45 steps (1.50s)
        //   42.722  pump (engine-equip) BEGIN - ws 2, 43 steps (1.43s of clip left)
        //   42.722    clip [MENU] 1HM_Idle.hkx (node '1HM_PickIdle', 9.00s)
        //   42.723    clip [MENU] 1HM_Idle.hkx (node '1HM_PickIdle', 3.33s)
        //   42.724    clip [MENU] 1HM_Idle.hkx (node '1HM_Idle',     8.93s)
        //   42.724  pump (engine-equip) - 43 steps (1.43s)
        //   42.724  pump (swap) BEGIN - 45 steps budgeted
        //
        // THREE pumps for one swap - 4.4 s of graph time inside 12 ms of wall
        // time - and every idle activation in the whole menu fell inside one of
        // those windows. The 1.07 s draw ran in two milliseconds: the clip
        // activates at .722 and its halfway `weaponDraw` annotation fires at
        // .723. The FootScuff annotations - the steps the user sees as a lunge -
        // fire in there too.
        //
        // PumpEngineEquip already says, in its own comment, "Stop at the END of
        // the clip and no further. Running ON into the idle would pick the idle
        // under a frozen VM". It does not: the budget is
        // ceil(clipDuration / kPumpStep) taken microseconds after the clip
        // started, so it steps to exactly at-or-past the end - and the end IS
        // the transition. Then PumpSwap adds an unconditional 1.5 s on top.
        //
        // So ask the graph instead of a stopwatch. ClipProbe knows when a
        // draw/sheathe has started and not yet reached an idle; the moment that
        // stops being true, the transition this pump exists to drive is over
        // and every further step is graph time spent on nobody's behalf.
        //
        // The step budget stays a HARD ceiling - this only ever stops EARLIER,
        // never later, so a pump can still not run away.
        //
        // ⚠ Behind bPumpStopsAtIdle, DEFAULT OFF. The measurement that selects
        // this has not been taken on the actual repro yet. Run the repro with
        // it off, read the "idle session:" line, turn it on, run it again.
        // ⚠ r2 (field 2026-07-21 14:58). The first version of this guarded only
        // on "is the draw I am driving still finishing?", and the field showed
        // that predicate is blind to the pumps that actually cause the loop:
        //
        //   16.876  pump (engine-equip) BEGIN - ws 3, 45 steps (0.00s of clip left)
        //   16.876    clip [MENU] 1HM_Idle.hkx (node '1HM_Idle', 19.80s, pump:engine-equip)
        //   16.877  pump (engine-equip) - 45/45 steps (1.50s)      <- never bounded
        //   16.878    clip [MENU] Dag_Equip.hkx (..., frame)       <- equip lands AFTER
        //
        // A pump that fires BEFORE its equip clip exists has no transition to
        // be "still finishing", so the guard never armed and it fast-forwarded
        // 1.5 s through an already-settled idle - re-picking it. `pump:swap`
        // after the draw settled is the same hole. Those unbounded 45/45 runs
        // were most of the remaining 5.37 s and 7.03 s, and the lunge survived.
        //
        // That log also retires the reason the blind budget existed: the equip
        // clip arrives on a later FRAME regardless of how long this pump ran,
        // so the 1.5 s buys nothing at all.
        //
        // So there are two regimes, and only one of them is about a draw:
        //
        //   DRIVING  - a draw/sheathe is in flight. Run until it reaches its
        //              idle, then stop. (This half was right and is unchanged.)
        //   BLIND    - nothing in flight. Do not fast-forward a settled graph.
        //              Stop the instant a step re-picks the idle, and cap the
        //              whole thing at a couple of frames regardless.
        //
        // Idles are kModeSinglePlay (mode=0, measured), so stepping a settled
        // one far enough ENDS it and hands the state machine a re-pick. That is
        // the loop, and in the blind regime it is entirely self-inflicted.
        constexpr int kPumpBlindSteps = 2;  // 0.07s - lets a change land, ends nothing

        // Why the last pump stopped early, or null if it ran its whole budget.
        // Named rather than a bare "STOPPED AT IDLE": r1 stopped for exactly
        // one reason so a flag sufficed, and the log then could not say WHICH
        // pumps were still unbounded without cross-reading the step fraction.
        const char* g_lastPumpStopReason = nullptr;

        // Defined further down beside SettleLocomotion, which is where the
        // reasoning about which variables may be re-asserted lives.
        void HoldLocomotionZero(RE::PlayerCharacter* a_player);

        [[nodiscard]] int PumpGraph(RE::PlayerCharacter* a_player, int a_steps,
                                    const char* a_why) {
            const ClipProbe::PumpWindow window{ a_why };
            const bool bound = Settings::GetSingleton().pumpStopsAtIdle;
            // "Have we seen the transition at all yet?" A pump often starts
            // BEFORE its own equip clip activates, so an entry check alone
            // would stop instantly and never drive anything.
            bool             sawTransition = ClipProbe::EquipTransitionUnfinished();
            const auto       picksAtEntry  = ClipProbe::IdlePickCount();
            int              taken         = 0;
            const char*      why           = nullptr;
            for (; taken < a_steps; ++taken) {
                if (bound) {
                    if (sawTransition) {
                        if (!ClipProbe::EquipTransitionUnfinished()) {
                            why = "DRAW SETTLED";
                            break;
                        }
                    } else {
                        if (ClipProbe::IdlePickCount() != picksAtEntry) {
                            why = "RE-PICKED THE IDLE";
                            break;
                        }
                        if (taken >= kPumpBlindSteps) {
                            why = "NOTHING TO DRIVE";
                            break;
                        }
                    }
                }
                a_player->UpdateAnimation(kPumpStep);
                // Re-assert AFTER the step, because the step is what undoes it.
                HoldLocomotionZero(a_player);
                if (!sawTransition) {
                    sawTransition = ClipProbe::EquipTransitionUnfinished();
                }
            }
            // Say whether it actually held. A settle that silently fails is what
            // put a walk animation in a paused menu for three rounds.
            float speedNow = -1.0f;
            a_player->GetGraphVariableFloat("Speed", speedNow);
            if (speedNow > 1.0f) {
                spdlog::warn("weapon preview: pump ({}) - Speed is STILL {:.2f} after the "
                             "settle re-assert; something outside UpdateAnimation is driving "
                             "locomotion and the character will walk in the menu.",
                             a_why, speedNow);
            }
            ClipProbe::NotePumpSeconds(static_cast<float>(taken) * kPumpStep);
            g_lastPumpStopReason = why;
            return taken;
        }

        // ⚠ r12: THE PUMP STEPS THE WHOLE GRAPH, NOT JUST THE WEAPON.
        //
        // Field: "when i switch weapon we can see them walking idle when we were
        // walking when we opened the menu even though they were not walking when
        // i first entered the menu". Exactly that. If the player was moving when
        // the menu opened, the locomotion variables still say so, and every
        // frame the pump steps replays a walk. r8's synthetic event produced the
        // same visible bug by a different route, which is the clue that the
        // graph's movement state - not the lever used on it - is the thing that
        // has to be settled first.
        //
        // Bubble already has this exact settle at the arm edge and it is
        // field-proven (r19/r36), but it cannot be relied on from here for two
        // independent reasons:
        //
        //   1. It sits in an else-if chain BELOW two freeze branches, so if the
        //      draw trips bAnimationDriven the freeze branch wins and the settle
        //      never runs at all. The tracker predicted this exact interaction.
        //   2. It runs AFTER WeaponPreview::Update, so even when it does fire,
        //      the pump has already replayed the walk by then.
        //
        // And it only ever ran at the ARM edge, so the per-tick swap path - the
        // one the field reported - was never covered by it in any case.
        //
        // Doing it here, immediately before stepping, cannot be skipped by a
        // branch that fired earlier and cannot be out-ordered. Only at pump
        // time, NEVER per tick: r19 found that writing these every tick fought
        // the preview's own turn input all menu long ("cannot rotate the
        // player").
        // ── THE PHANTOM UNSHEATHE SOUND ────────────────────────────────────
        //
        // Field: "when i exit the weapon is already unsheathed, but i then hear
        // the unsheathe sound effect which is a little jarring".
        //
        // The event log names the culprit: `weaponDraw` fires 18 times inside
        // menus (9 swaps x 2 graphs). That annotation both moves the weapon
        // sheath->hand AND plays the draw sound. It sits early in the clip,
        // which is why changing the pump BUDGET made no difference at all -
        // every budget included it. The audio engine is paused, so the sound is
        // queued and lands the instant the menu closes, with the weapon already
        // in hand.
        //
        // We cannot skip the annotation: it is what tells the graph the weapon
        // changed, which is the whole reason r25/r26 exist. So silence just
        // that weapon's equip sounds for the duration of the pump and put them
        // straight back.
        //
        // ⚠ These are BASE FORM fields, shared by every instance of the weapon.
        // Editing them is only safe because of something this session measured
        // directly: `Actor::Update` runs ZERO times while the menu holds the
        // pause, so no NPC can be mid-equip and lose its sound in the window.
        // The window is one pumped frame, inside a paused world, and the
        // destructor restores on every path including an early return.
        // ⚠ ONE ENTRY PER WEAPON, NOT ONE PER HAND. The first version of this
        // saved and nulled once for each hand, and dual-wielding one weapon
        // type gives BOTH hands the same base form (see ReparentToHand's note
        // on the shared sheath node). The second save then read back the nulls
        // the first had just written, kept those as "the originals", and the
        // restore put the real sounds back and immediately overwrote them with
        // null - leaving that weapon permanently silent for the session.
        // Field: "the sound effects for drawing or sheathing the weapon will be
        // missing after exiting the inventory menu."
        // EquipSoundLedger owns that invariant and is desk-tested for it.
        struct ScopedEquipSoundMute {
            explicit ScopedEquipSoundMute(RE::PlayerCharacter* a_player) {
                Take(a_player->GetEquippedObject(false));
                Take(a_player->GetEquippedObject(true));
            }
            ~ScopedEquipSoundMute() {
                ledger_.RestoreAll([](void* a_weapon, void* a_equip, void* a_unequip) {
                    auto* const weap       = static_cast<RE::TESObjectWEAP*>(a_weapon);
                    weap->equipSound   = static_cast<RE::BGSSoundDescriptorForm*>(a_equip);
                    weap->unequipSound = static_cast<RE::BGSSoundDescriptorForm*>(a_unequip);
                });
            }
            ScopedEquipSoundMute(const ScopedEquipSoundMute&)            = delete;
            ScopedEquipSoundMute& operator=(const ScopedEquipSoundMute&) = delete;

            [[nodiscard]] bool MutedAnything() const { return ledger_.AnySound(); }

        private:
            EquipSoundLedger ledger_;

            void Take(RE::TESForm* a_form) {
                if (!a_form) {
                    return;
                }
                auto* const weap = a_form->As<RE::TESObjectWEAP>();
                if (!weap) {
                    return;
                }
                // Save FIRST and only null when the ledger accepted it - a
                // refusal means this weapon is already muted and its real
                // sounds are already recorded elsewhere.
                if (ledger_.Save(weap, weap->equipSound, weap->unequipSound)) {
                    weap->equipSound   = nullptr;
                    weap->unequipSound = nullptr;
                } else {
                    // The only way to get here with two hands: the other hand
                    // holds this same base form. Logged because it is the
                    // precondition of the bug this ledger fixes, so a field log
                    // says outright whether the reporter's loadout can reach it.
                    spdlog::debug("weapon preview: both hands hold '{}' - muted once, not twice "
                                  "(the double-null that silenced it permanently).",
                                  weap->GetName());
                }
            }
        };

        // ⚠ ZEROING ONCE DOES NOT HOLD. The engine's movement-to-graph sync
        // rewrites Speed from inside UpdateAnimation, so a single write before a
        // 40-step pump is undone by step one. The field log said so plainly and
        // it was read past twice:
        //
        //   weapon preview: pose (engine-equip) - Speed=203.91 Direction=0.00 ...
        //
        // That line is printed AFTER SettleLocomotion and AFTER the whole pump,
        // and Speed is still full walking pace - which is the user's "i can
        // still see the player walking when i switch weapon".
        //
        // Split in two so the fix does not break this file's own rule. The
        // VARIABLES are idempotent writes and safe to re-assert as often as
        // needed; the moveStop EVENT is not, and stays exactly where it was
        // ("Only at pump time, NEVER per tick" - re-firing it every tick is the
        // shape that produced a permanent re-pump loop).
        void SettleLocomotionVars(RE::PlayerCharacter* a_player) {
            a_player->SetGraphVariableFloat("Speed", 0.0f);
            a_player->SetGraphVariableFloat("Direction", 0.0f);
            a_player->SetGraphVariableFloat("TurnDelta", 0.0f);
        }

        // ⚠ SPEED AND DIRECTION ONLY - NOT TurnDelta. Bubble's arm-edge settle
        // carries the reason, field-proven at r19: writing these repeatedly
        // fought the preview's turn input and the report was "cannot rotate the
        // player". TurnDelta is the one the spin needs, so it is written once at
        // the arm edge and never again. Speed and Direction have no such
        // conflict - nothing the user does in a menu asks the character to walk.
        void HoldLocomotionZero(RE::PlayerCharacter* a_player) {
            a_player->SetGraphVariableFloat("Speed", 0.0f);
            a_player->SetGraphVariableFloat("Direction", 0.0f);
        }

        void SettleLocomotion(RE::PlayerCharacter* a_player) {
            SettleLocomotionVars(a_player);
            a_player->NotifyAnimationGraph("moveStop");
        }

        // What the graph actually holds once a pump has finished. The pose has
        // been wrong three rounds running and each round the reason was guessed;
        // this makes it a read. Speed says whether the settle above took,
        // iRightHandEquipped versus the weapon's own type says whether the graph
        // agrees about the CLASS, and animDriven names the freeze interaction.
        void LogGraphPose(RE::PlayerCharacter* a_player, const char* a_why) {
            float        speed = -1.0f;
            float        dir   = -1.0f;
            std::int32_t iRight = -1;
            bool         animDriven = false;
            a_player->GetGraphVariableFloat("Speed", speed);
            a_player->GetGraphVariableFloat("Direction", dir);
            a_player->GetGraphVariableInt("iRightHandEquipped", iRight);
            a_player->GetGraphVariableBool("bAnimationDriven", animDriven);
            auto* const form = RightHandWeapon(a_player);
            auto* const weap = form ? form->As<RE::TESObjectWEAP>() : nullptr;
            spdlog::debug("weapon preview: pose ({}) - Speed={:.2f} Direction={:.2f} "
                          "iRightHandEquipped={} weaponType={} animDriven={}",
                          a_why, speed, dir, iRight,
                          weap ? static_cast<int>(weap->GetWeaponType()) : -1, animDriven);
        }

        // ── DIAGNOSTIC ONLY (2026-07-20). Adds observation, changes nothing. ──
        //
        // FIELD, from the 12:18-12:20 log plus the user looking at the screen:
        //
        //   pump (swap-redraw) - ws 2->3 want=3 in 18 steps +36 settle ARRIVED
        //   pose (swap-redraw) - Speed=0.00 iRightHandEquipped=2 weaponType=2
        //
        // and the character was "posed in 2 handed unsheathed idle with a 1
        // handed dagger". Every instrument this feature owns said CORRECT.
        //
        // That refutes the premise r10-r15 all rest on. Both instruments read
        // things WE drive directly - the weapon state machine and a graph
        // variable - so their agreement proves only that the pump ran. Neither
        // can see where the weapon's 3D ended up, and neither can see which
        // idle the graph actually selected. We have been measuring our own
        // inputs and calling them results.
        //
        // Three observables we have never had:
        //
        //   nodes - the weapon's parent node and cull state. The ONLY evidence
        //           that separates the user's hypothesis (the engine defers the
        //           hip->hand attach to its task queue, and tasks do not run
        //           under a kPausesGame freeze, so nothing moves until unpause)
        //           from "the attach happened and the POSE is what is wrong".
        //           These want opposite fixes, so guessing is expensive.
        //   vars  - a wider graph-variable net, INI-extensible. See
        //           Settings::diagGraphVars for why the names are candidates.
        //   live  - the same dump on an UNPAUSED weapon change, where the idle
        //           is known to come out right. The difference between a live
        //           swap and a menu swap IS the cause. A control beats an
        //           assumption about what the graph "should" say, and this
        //           feature has now spent three rounds on such assumptions.
        //
        // Reads only. Nothing here writes engine state or touches the debt.
        [[nodiscard]] const char* ParentNodeName(RE::NiAVObject* a_obj) {
            if (!a_obj || !a_obj->parent) {
                return "<none>";
            }
            const char* const name = a_obj->parent->name.c_str();
            return (name && *name) ? name : "<unnamed>";
        }

        // EVERY weapon-class slot, not the one the current weapon maps to. If
        // the OLD class's 3D is still resident somewhere that is the answer on
        // its own, and a WEAPON_TYPE -> slot mapping would have hidden it. Slot
        // numbers are the biped table's own (33 = 1H sword, 37 = 2H melee,
        // 38 = bow, 41 = quiver, 9 = shield), so the raw index is more useful
        // here than a prettified name.
        void LogWeaponNodes(RE::PlayerCharacter* a_player, const char* a_why) {
            const auto& biped = a_player->GetBiped();
            if (!biped) {
                spdlog::debug("weapon diag: nodes ({}) - NO BIPED", a_why);
                return;
            }
            std::string out;
            const auto  append = [&biped, &out](int a_slot) {
                const auto& part = biped->objects[a_slot];
                if (!part.partClone) {
                    return;
                }
                out += " [";
                out += std::to_string(a_slot);
                out += ":'";
                out += (part.item && part.item->GetName()) ? part.item->GetName() : "-";
                out += "' on '";
                out += ParentNodeName(part.partClone.get());
                out += part.partClone->GetAppCulled() ? "' CULLED]" : "']";
            };
            for (int slot = RE::BIPED_OBJECTS::kHandToHandMelee;
                 slot <= RE::BIPED_OBJECTS::kQuiver; ++slot) {
                append(slot);
            }
            append(RE::BIPED_OBJECTS::kShield);
            spdlog::debug("weapon diag: nodes ({}) -{}", a_why,
                          out.empty() ? std::string(" <no weapon 3D at all>") : out);
        }

        // Typed probe in int/bool/float order. An int read can succeed on a
        // bool variable and print 0/1, which is harmless for a diff; what
        // matters is that an UNKNOWN name fails all three and prints nothing.
        void LogGraphVars(RE::PlayerCharacter* a_player, const char* a_why) {
            std::string out;
            for (const auto& var : Settings::GetSingleton().diagGraphVars) {
                std::int32_t i = 0;
                bool         b = false;
                float        f = 0.0f;
                if (a_player->GetGraphVariableInt(var.c_str(), i)) {
                    out += " " + var + "=" + std::to_string(i);
                } else if (a_player->GetGraphVariableBool(var.c_str(), b)) {
                    out += " " + var + "=" + (b ? "true" : "false");
                } else if (a_player->GetGraphVariableFloat(var.c_str(), f)) {
                    out += " " + var + "=" + std::to_string(f);
                }
            }
            spdlog::debug("weapon diag: vars ({}) -{}", a_why,
                          out.empty() ? std::string(" <none of the candidates exist>") : out);
        }

        void LogDiag(RE::PlayerCharacter* a_player, const char* a_why) {
            LogWeaponNodes(a_player, a_why);
            LogGraphVars(a_player, a_why);
        }

        // Step the graph until the actor reaches a_target, or the cap. Returns
        // whether it arrived.
        bool PumpToState(RE::PlayerCharacter* a_player, RE::WEAPON_STATE a_target,
                         const char* a_why) {
            auto* const state = a_player->AsActorState();
            if (!state) {
                return false;
            }
            SettleLocomotion(a_player);
            const auto before = state->GetWeaponState();
            // OPEN FENCE. The pump's own report prints AFTER it, so without
            // this the anim events it causes can only be attributed to it by
            // ordering. r21's diff is read event by event and the whole answer
            // turns on which pump raised what, so make the attribution exact
            // rather than inferred - two lines beats re-running a field test.
            spdlog::debug("weapon preview: pump ({}) BEGIN - ws {} want={}", a_why,
                          static_cast<int>(before), static_cast<int>(a_target));
            int steps = 0;
            {
                // Attribution only - the exit condition here is the weapon STATE
                // edge, which is a different contract from the settle below and
                // is left exactly as it was.
                const ClipProbe::PumpWindow window{ "pump:seek" };
                while (steps < kPumpMaxSteps && state->GetWeaponState() != a_target) {
                    a_player->UpdateAnimation(kPumpStep);
                    ++steps;
                }
                ClipProbe::NotePumpSeconds(static_cast<float>(steps) * kPumpStep);
            }
            const bool arrived = state->GetWeaponState() == a_target;
            // Past the edge and on to the end of the clip. Unconditional: a
            // CAPPED pump needs the settle at least as much as an arrived one.
            //
            // ⚠ The rest of that sentence used to read "and over-running only
            // advances an idle on a character who is standing still in a paused
            // menu" - i.e. it assumed over-running is free. The 2026-07-21 log
            // is what put that in doubt (see PumpGraph), so the settle now stops
            // at the idle too when bPumpStopsAtIdle is on.
            const int settle = PumpGraph(a_player, kPumpSettleSteps, "pump:settle");
            spdlog::debug("weapon preview: pump ({}) - ws {}->{} want={} in {} steps "
                          "+{}/{} settle ({:.2f}s total) {}",
                          a_why, static_cast<int>(before),
                          static_cast<int>(state->GetWeaponState()),
                          static_cast<int>(a_target), steps, settle, kPumpSettleSteps,
                          static_cast<float>(steps + settle) * kPumpStep,
                          fmt::format("{} (settle: {})", arrived ? "ARRIVED" : "CAPPED",
                                      g_lastPumpStopReason ? g_lastPumpStopReason
                                                           : "ran full budget"));
            LogGraphPose(a_player, a_why);
            LogDiag(a_player, a_why);
            return arrived;
        }

        // ── r20 EXPERIMENT: THE SLOW SWAP. INI bSlowSwapExperiment. ──────────
        //
        // Every observable is IDENTICAL between a menu swap that looked WRONG
        // and a live swap that looked RIGHT. Field 12:50:04 (menu) vs 12:50:05
        // (live), same weapon, seconds apart: same weapon state, same node
        // (`[37:'Champion's Cudgel' on 'WEAPON']` in both), and the same 13
        // graph variables to the value. So what differs is not a value we can
        // read - it is which clip the graph latched, and nothing exposes that.
        //
        // The one difference left is the SHAPE OF TIME. r13's swap runs the
        // whole sheathe+redraw inside a SINGLE frame by calling UpdateAnimation
        // 96 times in a row. That advances graph TIME but never crosses a frame
        // BOUNDARY, so anything that re-picks a clip once per engine frame
        // never gets its chance. The state machine still reports ARRIVED,
        // because the weapon-state annotations fire inside the graph tick
        // regardless - which is precisely why every instrument said correct.
        //
        // If that is right then r13 is self-defeating. Sheathe+redraw was
        // rejected twice for costing ~2s of visible animation; r13 reversed the
        // rejection by making it invisible; and the very mechanism that hides
        // it is what stops the pose being re-picked. We bought invisibility
        // with correctness without noticing the trade.
        //
        // So run the same transition across REAL frames. Nothing new is needed
        // to drive it: Bubble already ticks the graph once per frame while a
        // menu is open, and the r36 freeze already exempts our own transition
        // through TransitionInFlight(). All this adds is the patience the pump
        // refuses to have.
        //
        // It IS visible, roughly a second per swap. That is the price of the
        // test, not a proposal. If the pose comes out right, hiding it becomes
        // the next problem and a far better one to have than this.
        //
        // ⚠ This is an EXPERIMENT, not a design. Do not build on it until the
        // field says the pose is actually correct.
        int slowPhase  = 0;  // 0 idle, 1 awaiting sheathe, 2 awaiting redraw
        int slowFrames = 0;  // PER PHASE - see below

        // ⚠ r20a: THE FIRST VERSION OF THIS CAP INVALIDATED THE EXPERIMENT.
        //
        // The budget was cumulative across both phases and set to 180. Field
        // 13:15:29: the sheathe alone took 158 frames, so the redraw got 22
        // before capping out into r13's in-frame pump - and the redraw is the
        // ONLY half that re-picks the pose. The test reported a refuted
        // hypothesis when it had never run the thing being tested. The user saw
        // it directly ("they play the unsheathe animation, but the moment it
        // ends they instantly swap to the unsheathed idle") - that instant snap
        // WAS this cap firing.
        //
        // So: reset per phase, and size the cap for the actual machine. The
        // field runs at ~175 fps (158 frames of sheathe in 0.90 s), where the
        // old 180 was barely one second - shorter than a ~1.2 s draw clip, so
        // the redraw could never have finished inside it at any budget split.
        // A cap must be slack enough that hitting it means something is WRONG,
        // never merely that the machine is fast.
        constexpr int kSlowMaxFrames = 600;  // ~3.4s at 175fps, ~10s at 60fps

        // TRUE while a slow swap owns the tick, so Update() holds everything
        // else off until it finishes. Never strands the player: the frame cap
        // finishes the job with r13's in-frame pump and says so in the log.
        bool DriveSlowSwap(RE::PlayerCharacter* a_player, RE::ActorState* a_state) {
            if (slowPhase == 0) {
                return false;
            }
            const auto now = a_state->GetWeaponState();
            if (++slowFrames > kSlowMaxFrames) {
                spdlog::warn("weapon preview: slow swap CAPPED in phase {} after {} frames "
                             "(ws={}) - finishing with the in-frame pump.",
                             slowPhase, slowFrames, static_cast<int>(now));
                if (slowPhase == 1) {
                    PumpToState(a_player, RE::WEAPON_STATE::kSheathed, "slow-cap-sheathe");
                    Draw(a_player, true);
                }
                PumpToState(a_player, RE::WEAPON_STATE::kDrawn, "slow-cap-redraw");
                slowPhase = 0;
                return false;
            }
            if (slowPhase == 1 && now == RE::WEAPON_STATE::kSheathed) {
                Draw(a_player, true);
                slowPhase = 2;
                spdlog::debug("weapon preview: slow swap - sheathe finished in {} real frames, "
                              "redraw issued.", slowFrames);
                slowFrames = 0;  // r20a: PER-PHASE budget - see kSlowMaxFrames
            } else if (slowPhase == 2 && now == RE::WEAPON_STATE::kDrawn) {
                slowPhase = 0;
                spdlog::debug("weapon preview: slow swap - redraw finished in {} real frames. "
                              "THIS is the pose to judge.", slowFrames);
                LogGraphPose(a_player, "slow-done");
                LogDiag(a_player, "slow-done");
                return false;
            }
            return true;
        }

        // r15 - NORMALIZE, DO NOT SPECIAL-CASE. The root-cause fix for the whole
        // bug class, and the reason this file kept needing another round.
        //
        // User: "i really fucking hate the fact that sometimes when we are in
        // the later stage of an unsheathe animation and we enter the menu, then
        // the animation fully plays out the last part which causes visual issues
        // when we exit out the menu".
        //
        // Every bug this feature has produced traces to ONE thing: the animation
        // state when the menu opens is ARBITRARY, and each round answered a new
        // arbitrary case with a new special rule. Mid-draw got a rule (the gate
        // treats kDrawing as drawn so it does not take a debt for the player's
        // own draw). Walking got a rule (SettleLocomotion). A cross-class swap
        // got a rule. That is a bandaid factory by construction: every entry
        // state anyone can be in is another round, forever.
        //
        // So stop having entry states. A transition caught in flight is driven
        // to the terminal state it was already heading for, pumped, before the
        // gate is even consulted - and because the world is paused, none of it
        // is rendered. Downstream then only ever sees kSheathed or kDrawn, which
        // is what the gate's rules were quietly assuming all along.
        //
        // This is why it fixes the exit too: the world never resumes holding
        // half a clip, so there is no leftover to play out and nothing to
        // reconcile. The gate's mid-draw special case is left in place as a
        // fallback for a CAPPED normalize.
        //
        // ⚠ 0.7.1 - "IT SHOULD NOW BE UNREACHABLE" WAS WRONG, AND THAT IS THIS
        // BUG. r15 assumed the pump can always reach terminal inside its
        // budget. It cannot: kPumpMaxSteps is sized for vanilla ("far past any
        // vanilla draw"), and a replaced draw clip runs past it, so CAPPED is
        // not a rare fallback but the NORMAL outcome on a modded list. r15 was
        // also never field-tested before shipping - this report is its first
        // field data.
        //
        // The premise survives; the guarantee does not. So the capped case now
        // has a defined behaviour of its own instead of being treated as
        // impossible: normalize once, and if it does not arrive, let the clip
        // ANIMATE to completion rather than freezing it half-played. That
        // still ends at a terminal state, just over real frames instead of
        // inside one - which is the honest version of the same promise.
        // Returns TRUE when the actor is left in a terminal state - either it
        // already was one, or the pump got it there. FALSE means the pump
        // capped and the graph is still mid-clip; the caller must not retry,
        // and Bubble must let that clip finish instead of freezing it.
        [[nodiscard]] bool NormalizeToTerminal(RE::PlayerCharacter* a_player,
                                               RE::ActorState*      a_state) {
            switch (a_state->GetWeaponState()) {
                case RE::WEAPON_STATE::kWantToDraw:
                case RE::WEAPON_STATE::kDrawing:
                    return PumpToState(a_player, RE::WEAPON_STATE::kDrawn, "normalize-draw");
                case RE::WEAPON_STATE::kWantToSheathe:
                case RE::WEAPON_STATE::kSheathing:
                    return PumpToState(a_player, RE::WEAPON_STATE::kSheathed,
                                       "normalize-sheathe");
                default:
                    return true;  // already terminal - the common case, and free
            }
        }

        // The swap variant. There is no state edge here, so run a fixed budget
        // and let the engine's own replace animation finish inside the frame.
        void PumpSwap(RE::PlayerCharacter* a_player) {
            SettleLocomotion(a_player);
            spdlog::debug("weapon preview: pump (swap) BEGIN - {} steps budgeted",
                          kPumpSwapSteps);
            const int taken = PumpGraph(a_player, kPumpSwapSteps, "pump:swap");
            spdlog::debug("weapon preview: pump (swap) - {}/{} steps ({:.2f}s) {}", taken,
                          kPumpSwapSteps, static_cast<float>(taken) * kPumpStep,
                          g_lastPumpStopReason ? g_lastPumpStopReason : "ran full budget");
            LogGraphPose(a_player, "swap");
            LogDiag(a_player, "swap");
        }

        // Actor virtual 0xB4 - the hip<->hand re-parent, and the ONLY thing
        // that moves a weapon between its sheath node and the hand node.
        //
        // Needed because a weapon SWAP is not a draw. The engine's attach path
        // always parks the new weapon on its SHEATH node (it never reads weapon
        // state to decide placement), and the hip->hand move is a separate step
        // that normally only runs on a draw/sheathe transition. During the
        // preview the state machine is ALREADY kDrawn, so DrawWeaponMagicHands
        // has nothing to transition and is a no-op - which left the swapped-in
        // weapon sitting on the hip (field 2026-07-18). Same root cause and
        // same fix as Fitting Room OS-76's follow-up; see
        // [[skyrim-weapon-attach-change-detect]].
        //
        // Called as a VIRTUAL, never by address: the `Actor` implementation
        // only touches the 3rd-person root, and it is the `PlayerCharacter`
        // OVERRIDE that also does first person. Slot verified on both binaries
        // during the FR work - PlayerCharacter vtable 0xB4 is SE 1406A1BF0 /
        // AE 140736500, and the callee opens `movzx edi, r9b`, confirming arg4
        // is the bool. Pure AttachChild re-parent with NO animation, idempotent
        // with draw=true (the source node simply has no children if the weapon
        // is already in hand), and it self-defers through the engine's own task
        // queue. The engine uses it exactly this way to restore already-equipped
        // weapons without animating - which is what a paused world wants.
        // CommonLibSSE declares this slot only as Unk_B4(void).
        void ReparentToHand(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_weapon) {
            using func_t          = void (*)(RE::Actor*, RE::TESObjectWEAP*, bool, bool);
            auto** const vtbl     = *reinterpret_cast<void***>(a_player);
            const auto   reparent = reinterpret_cast<func_t>(vtbl[0xB4]);
            // RIGHT HAND ONLY (a_leftHand = false). A left-hand weapon and a
            // staff go straight to the hand node and only have kHidden set from
            // live state, so they are already correct. That restriction also
            // sidesteps this call's one sharp edge: it moves children[0] of the
            // SOURCE node, and dual-wielding one weapon type gives both hands
            // the same sheath node, so asking for the left can yank the right
            // hand's weapon.
            reparent(a_player, a_weapon, true, false);
        }

        // SHIELD EVIDENCE, not a fix (r9). The field says a shield does not
        // appear until the menu is exited, and this module has never touched
        // shields at all: RightHandWeapon() returns a TESObjectWEAP and a
        // shield is a TESObjectARMO, so one has never entered the pipeline.
        //
        // Read what the 3D is doing before building an attach path for it,
        // because the candidate causes want opposite fixes and the log tells
        // them apart in one run:
        //   no 3D at all           - never attached; the fix is an attach.
        //   parented to the back   - attached but nothing moved it; the fix is
        //                            a re-parent, the shield-shaped version of
        //                            what 0xB4 does for the right hand.
        //   parented to the arm
        //     but culled           - it moved and is merely hidden; the fix is
        //                            a visibility flag, not an attach at all.
        //
        // Logged on CHANGE rather than per tick, because the report is about
        // WHEN it appears ("not until I exit the menu"), so the moment it moves
        // is the whole datum and it should be one line, not buried in a dump.
        //
        // Biped slot 9 is also where a DRAWN BOW lives - the engine's own
        // table, and the reason the header calls a bow the one special case -
        // so this reads a bow's placement for free.
        // The shield's last step (r11). The field log settled which of the three
        // candidate causes it actually was:
        //
        //   shield slot (armed) - left='Iron Shield' has3D=true culled=true parent='SHIELD'
        //
        // has3D plus parent='SHIELD' means the engine had ALREADY attached it to
        // the drawn position. culled=true means it was only invisible. So this
        // attaches nothing and re-parents nothing - the placement is already
        // correct and forging it again is the mistake r10 exists to undo. It
        // clears a display flag on a node the engine placed itself, which the
        // engine would have cleared too had the clip been allowed to reach that
        // point.
        //
        // Gated on DRAWN, because a culled shield is CORRECT while sheathed.
        // Nothing has to restore it: our own sheathe is pumped to completion, so
        // the engine re-culls it exactly as it culled it in the first place.
        void ShowShieldIfHidden(RE::PlayerCharacter* a_player) {
            const auto& biped = a_player->GetBiped();
            if (!biped) {
                return;
            }
            const auto& part = biped->objects[RE::BIPED_OBJECTS::kShield];
            if (!part.partClone || !part.partClone->GetAppCulled()) {
                return;
            }
            part.partClone->SetAppCulled(false);
            spdlog::debug("weapon preview: shield was culled while drawn - un-culled "
                          "(parent='{}').",
                          part.partClone->parent && part.partClone->parent->name.c_str()
                              ? part.partClone->parent->name.c_str()
                              : "<none>");
        }

        void LogShieldState(RE::PlayerCharacter* a_player, const char* a_why) {
            auto* const  left  = a_player->GetEquippedObject(true);
            const auto&  biped = a_player->GetBiped();
            const char*  node  = "<none>";
            bool         has3D = false;
            bool         culled = false;
            if (biped) {
                const auto& part = biped->objects[RE::BIPED_OBJECTS::kShield];
                if (part.partClone) {
                    has3D  = true;
                    culled = part.partClone->GetAppCulled();
                    if (part.partClone->parent && part.partClone->parent->name.c_str()) {
                        node = part.partClone->parent->name.c_str();
                    }
                }
            }
            // a_why is part of the key on purpose: the arm/disarm edge is the
            // exact moment the report is about, so crossing it re-logs even
            // when the node and the flags have not moved.
            static std::string s_last;
            std::string        key = std::string(a_why) + "|" + node +
                              (has3D ? "|3d" : "|no3d") + (culled ? "|culled" : "|shown");
            if (key == s_last) {
                return;
            }
            s_last = key;
            spdlog::debug("weapon preview: shield slot ({}) - left='{}' has3D={} culled={} "
                          "parent='{}'",
                          a_why, left && left->GetName() ? left->GetName() : "-", has3D, culled,
                          node);
        }
    }

    bool TransitionInFlight() { return inFlight; }

    bool NormalizeCapped() { return normalizeCapped; }

    void ArmEdgeReset() {
        // The normalize is per-ARM, so its latch has to be released on the way
        // in. Doing it here rather than at disarm means a disarm that never
        // armed (the unpause race, missing 3D) cannot strand the latch and
        // silently disable the normalize for the next menu - the same class of
        // bug the restore debt already had twice.
        normalizeTried  = false;
        normalizeCapped = false;
    }

    void PumpEngineEquip(RE::PlayerCharacter* a_player) {
        if (!a_player) {
            return;
        }
        auto* const state = a_player->AsActorState();
        if (!state) {
            return;
        }
        // A fixed budget, exactly like PumpSwap and for the same reason: the
        // engine's equip holds the actor at kDrawn throughout, so there is no
        // state edge to wait on. This is why the existing normalize could never
        // catch it - it waits on an edge that never comes.
        //
        // shownType is deliberately NOT touched. This runs after the engine has
        // already applied the equip and after Update() has recorded the swap;
        // writing it here would make two owners for one value, which is the
        // shape of three bugs in this file already.
        //
        // ⚠ 0.7.1 - BUDGET FROM THE CLIP, NOT FROM A CONSTANT. kPumpSwapSteps
        // is 1.5 s and the field log carries a 2.00 s `1HM_Unequip`, so the
        // pump left ~0.5 s of clip unplayed. That tail then ran live after the
        // menu closed and fired its SOUND annotation - the field report:
        // "when i exit the weapon is already unsheathed, but i then hear the
        // unsheathe sound effect which is a little jarring". Same shape as
        // kPumpMaxSteps: a budget sized for vanilla, silently short for a
        // replaced animation.
        //
        // Stop at the END of the clip and no further. Running ON into the idle
        // would pick the idle under a frozen VM, which is the very thing the
        // draw/sheathe hold exists to prevent.
        // Silence the draw/sheathe sound for exactly this pump - see
        // ScopedEquipSoundMute. Declared before the pump and restored by its
        // destructor, so no return path can leave a weapon permanently silent.
        const ScopedEquipSoundMute soundMute{ a_player };
        SettleLocomotion(a_player);
        const float remaining = ClipProbe::EquipClipRemainingSeconds();
        const int   budget    = (remaining > 0.0f)
                                    ? static_cast<int>(std::ceil(remaining / kPumpStep))
                                    : kPumpSwapSteps;
        const int   steps     = (std::min)(budget, kPumpMaxSteps);
        spdlog::debug("weapon preview: pump (engine-equip) BEGIN - ws {}, {} steps ({:.2f}s "
                      "of clip left; default budget {}), equip sound {}",
                      static_cast<int>(state->GetWeaponState()), steps, remaining,
                      kPumpSwapSteps,
                      // If this says NOTHING TO MUTE, the draw sound is not on
                      // the weapon form at all - it is an animation SOUND
                      // ANNOTATION with its own descriptor, and silencing it
                      // needs a different lever. One log line decides that.
                      soundMute.MutedAnything() ? "MUTED for this pump"
                                                : "NOTHING TO MUTE (form carries none)");
        const int taken = PumpGraph(a_player, steps, "pump:engine-equip");
        spdlog::debug("weapon preview: pump (engine-equip) - {}/{} steps ({:.2f}s), ws now {} {}",
                      taken, steps, static_cast<float>(taken) * kPumpStep,
                      static_cast<int>(state->GetWeaponState()),
                      g_lastPumpStopReason ? g_lastPumpStopReason : "ran full budget");
        LogGraphPose(a_player, "engine-equip");
        LogDiag(a_player, "engine-equip");
    }

    bool HasDebt() { return weDrew; }

    void Reset() {
        weDrew          = false;
        shownWeapon     = 0;
        shownLeft       = 0;
        shownType       = -1;
        inFlight        = false;
        slowPhase       = 0;
        normalizeTried  = false;
        normalizeCapped = false;
    }

    // DIAGNOSTIC ONLY. The CONTROL arm: the same dump on a weapon change made
    // in the live, unpaused world, where the idle is known to come out right.
    // Without it we would be comparing the menu case against an assumption
    // about what the graph ought to say, which is the exact habit that cost
    // this feature three rounds.
    //
    // Owns state entirely SEPARATE from Update()'s shownWeapon/shownLeft. It
    // must not be able to perturb the debt bookkeeping in any way, and it runs
    // on a path Update() never sees (disarmed), so sharing state would be a
    // second writer to a field whose whole correctness argument is that one
    // place owns it.
    //
    // Samples TWICE: once at the change, and once ~90 frames later. The first
    // catches the graph mid-transition, which is not comparable with anything;
    // the SETTLED sample is the one to diff against a menu swap's settled pose.
    void ObserveUnarmed(RE::PlayerCharacter* a_player) {
        if (!a_player || !a_player->Is3DLoaded()) {
            return;
        }
        static RE::FormID s_lastRight   = 0;
        static RE::FormID s_lastLeft    = 0;
        static int        s_settleFrames = 0;

        if (s_settleFrames > 0 && --s_settleFrames == 0) {
            LogGraphPose(a_player, "live-settled");
            LogDiag(a_player, "live-settled");
        }

        auto* const      right  = RightHandWeapon(a_player);
        auto* const      leftFm = a_player->GetEquippedObject(true);
        const RE::FormID r      = right ? right->GetFormID() : 0;
        const RE::FormID l      = leftFm ? leftFm->GetFormID() : 0;
        if (r == s_lastRight && l == s_lastLeft) {
            return;
        }
        const bool first = s_lastRight == 0 && s_lastLeft == 0;
        s_lastRight      = r;
        s_lastLeft       = l;
        // First sight is the session adopting whatever is already equipped, not
        // a change worth a control sample.
        if (first) {
            return;
        }
        auto* const state = a_player->AsActorState();
        spdlog::debug("weapon diag: LIVE weapon change - right='{}' left='{}' ws={} drawn={}",
                      right && right->GetName() ? right->GetName() : "-",
                      leftFm && leftFm->GetName() ? leftFm->GetName() : "-",
                      state ? static_cast<int>(state->GetWeaponState()) : -1,
                      state ? state->IsWeaponDrawn() : false);
        LogGraphPose(a_player, "live-change");
        LogDiag(a_player, "live-change");
        s_settleFrames = 90;
    }

    void Restore(RE::PlayerCharacter* a_player) {
        // r20: a restore supersedes any slow swap still in flight. The sheathe
        // below is where the weapon is going regardless of which half of the
        // swap we caught, and leaving the phase set would drive a REDRAW after
        // the restore - handing the player a drawn weapon with the debt already
        // marked paid, which is the one unrecoverable outcome this file exists
        // to prevent.
        slowPhase = 0;
        if (!weDrew || !a_player || !a_player->Is3DLoaded()) {
            // inFlight is deliberately NOT cleared here, and tidying it back in
            // reintroduces a real bug. Reaching this branch usually means a
            // SECOND Restore() for a debt the first call already paid - and that
            // first sheathe may still be animating. Clearing the flag would hand
            // our own in-flight transition back to the r36 freeze, which would
            // then read it as any other draw/sheathe, latch airFrozenArm_ at the
            // next arm edge and hold the character mid-sheathe for that whole
            // menu (reachable on a menu SWITCH during the sheathe).
            // Leaving it set is safe: nothing reads TransitionInFlight() while
            // disarmed, and Update() clears it on the first armed tick where the
            // weapon state has settled. Reset() owns the hard clear, for the
            // case where the 3D is gone.
            //
            // A null player FORGIVES the debt rather than holding it: there is
            // nothing left to sheathe, and carrying an unpayable debt into the
            // next arm would sheathe a weapon we never drew. (Update() takes the
            // opposite line on null and returns without touching any state - it
            // is a per-tick observer, so it can afford to wait for a live
            // player; this is a teardown edge that may never come again.)
            //
            // An unloaded 3D forgives the same way: DrawWeaponMagicHands() needs
            // a live graph manager to move anything, so there is nothing safe to
            // sheathe and nothing worth holding the debt against. The next arm
            // evaluates fresh once a 3D is live again.
            // NOTE (F-26 r2): Disarm no longer calls this directly - it defers
            // past the menu-switch gap and Bubble::FireDeferredWeaponRestore
            // pays it. That fire site is gated on !armedLastFrame_ rather than
            // on the menu count precisely so Tick's !Is3DLoaded() disarm still
            // reaches this forgive branch promptly, with a menu still open.
            weDrew      = false;
            shownWeapon = 0;
            shownLeft   = 0;
            shownType   = -1;
            return;
        }
        Draw(a_player, false);
        // Run the sheathe out here too, for the same reason the draw is pumped.
        // The field saw the opposite of this: "when i do exit i see the player
        // play the unsheathe animation even though we were already unsheathed in
        // the menu" - the engine reconciling a state we had only forged. With a
        // genuine kDrawn established at open, there is nothing to reconcile, and
        // finishing the sheathe here means the player is not handed a leftover
        // transition on the way out either.
        PumpToState(a_player, RE::WEAPON_STATE::kSheathed, "restore");
        weDrew      = false;
        shownWeapon = 0;
        shownLeft   = 0;
        shownType   = -1;
        spdlog::debug("weapon preview: sheathing (restore).");
    }

    void Update(RE::PlayerCharacter* a_player, bool a_bubbleArmed, bool a_raceMenu) {
        if (!a_player) {
            return;
        }
        auto* const state = a_player->AsActorState();
        if (!state) {
            return;
        }

        // Our transition is finished once the state machine settles on a
        // non-transitional value. Clearing this re-arms the r36 freeze for
        // everything else.
        {
            const auto settling = state->GetWeaponState();
            if (inFlight && settling != RE::WEAPON_STATE::kWantToDraw &&
                settling != RE::WEAPON_STATE::kDrawing &&
                settling != RE::WEAPON_STATE::kWantToSheathe &&
                settling != RE::WEAPON_STATE::kSheathing) {
                inFlight = false;
            }
        }

        // r20: a slow swap owns the tick until it lands. Called unconditionally
        // rather than behind the setting, so flipping bSlowSwapExperiment off
        // mid-swap still finishes the one already in flight instead of
        // stranding the player between a sheathe and a redraw. Sits ABOVE the
        // normalize: the whole point is to NOT drive our own transition to its
        // terminal state inside one frame.
        if (DriveSlowSwap(a_player, state)) {
            return;
        }

        // r15: normalize BEFORE anything reads the weapon state, so every rule
        // below it sees a terminal value and none of them need a special case
        // for "caught mid-transition". Armed only, because a clean pose is a
        // studio requirement and there is no reason to touch the player's own
        // animation otherwise; and never over our own in-flight transition,
        // which is already pumped synchronously at the site that started it.
        //
        // 0.7.1: ONCE per arm, and gated on the feature's own switch. The
        // switch matters because this call sits ABOVE the gate that reads
        // in.enabled, so bWeaponPreviewInMenus=0 did not disable this path at
        // all - a user hitting the loop had no way to turn it off, and
        // STATUS.md's "restores prior behaviour exactly" was untrue here.
        //
        // ⚠ AND IT ANNOUNCES ITSELF, EVERY ARM, WHATEVER IT DECIDES. The first
        // field run could not answer whether the menu was even opened mid-draw,
        // because the only weapon line is the gate's and that prints ON CHANGE
        // against a key holding `weaponDrawn` but NOT `ws` - and `weaponDrawn`
        // is deliberately true for kWantToDraw/kDrawing. A mid-unsheathe arm
        // therefore produced a key byte-identical to an already-drawn arm and
        // logged NOTHING. The one fact the whole bug turns on was the one fact
        // no line carried.
        if (a_bubbleArmed && !normalizeTried) {
            normalizeTried         = true;
            const auto entryWs     = state->GetWeaponState();
            const bool terminal    = entryWs == RE::WEAPON_STATE::kSheathed ||
                                     entryWs == RE::WEAPON_STATE::kDrawn;
            const bool enabled     = Settings::GetSingleton().weaponPreviewInMenus;
            const char* const skip = !enabled  ? "bWeaponPreviewInMenus=0"
                                   : inFlight  ? "our own transition is in flight"
                                               : nullptr;
            spdlog::info("weapon preview: ARM EDGE - ws={} ({}){}", static_cast<int>(entryWs),
                         terminal ? "terminal" : "MID-TRANSITION",
                         skip ? fmt::format(" - normalize skipped, {}", skip)
                              : (terminal ? " - nothing to normalize"
                                          : " - normalizing now"));
            // NOT an early return. Everything below - swap detection, the
            // inFlight clear, the shield report - has to keep running whatever
            // the normalize decided; `inFlight` in particular is CLEARED below,
            // so returning here would strand it set forever.
            if (!skip) {
                normalizeCapped = !NormalizeToTerminal(a_player, state);
                if (normalizeCapped) {
                    // Not retried, and deliberately not fatal. Bubble reads
                    // this and declines to freeze the arm, so the clip the
                    // player started finishes on real frames instead of being
                    // held half-played and leaking past the menu.
                    spdlog::info("weapon preview: normalize CAPPED - the draw/sheathe clip is "
                                 "longer than the {:.1f}s pump budget (replaced animation). "
                                 "Letting it finish on its own instead of retrying.",
                                 static_cast<float>(kPumpMaxSteps) * kPumpStep);
                }
            }
        }

        // Read AFTER normalizing - this is the value the gate and the log below
        // both consume.
        const auto ws = state->GetWeaponState();

        // The capped clip reached its terminal state on its own, which is
        // exactly what letting it animate was for. Stop reporting it so the
        // freeze goes back to normal for the rest of the menu.
        if (normalizeCapped && (ws == RE::WEAPON_STATE::kSheathed ||
                                ws == RE::WEAPON_STATE::kDrawn)) {
            normalizeCapped = false;
            spdlog::info("weapon preview: the capped clip finished on its own (ws now {}) - "
                         "normal freeze resumes.", static_cast<int>(ws));
        }

        auto* const weapon = RightHandWeapon(a_player);

        WeaponPreviewGate::Input in;
        in.enabled        = Settings::GetSingleton().weaponPreviewInMenus;
        in.bubbleArmed    = a_bubbleArmed;
        in.raceMenu       = a_raceMenu;
        in.weaponEquipped = weapon != nullptr;
        // IsWeaponDrawn() is false during kWantToDraw/kDrawing, so a player who
        // opened the menu mid-draw would read as sheathed and we would take a
        // debt for THEIR draw - then sheathe it on close, which is exactly what
        // the gate's no-debt branch exists to prevent. Treat an in-progress draw
        // as drawn. The mirror case needs nothing: mid-sheathe already reads as
        // drawn, yields kNone, and settles on its own.
        in.weaponDrawn    = state->IsWeaponDrawn() ||
                            ws == RE::WEAPON_STATE::kWantToDraw ||
                            ws == RE::WEAPON_STATE::kDrawing;
        in.inCombat       = a_player->IsInCombat();
        in.mounted        = a_player->IsOnMount();
        in.autoDraw       = Settings::GetSingleton().autoDrawInMenus;

        const auto action = WeaponPreviewGate::Decide(in, weDrew);

        // Gate evidence. kNone is SILENT by design (it is the common case), and
        // that silence cost a field round: a log with no "weapon preview" line
        // at all proves only that we did not draw, not WHY. Logged on change so
        // it is one line per real transition rather than per tick.
        {
            static int s_lastKey = -1;
            // ⚠ `ws` IS IN THE KEY (0.7.1). It was printed but not keyed, and
            // `weaponDrawn` is true for kWantToDraw/kDrawing by design - so a
            // menu opened MID-UNSHEATHE produced a key identical to one opened
            // already-drawn and printed nothing at all. That is the exact fact
            // the loop bug turns on, and a whole field round could not answer
            // it. A printed value that is not in the change key is invisible
            // whenever it is the only thing that changed.
            const int  key       = (static_cast<int>(action) << 8) |
                             (in.enabled << 0) | (in.bubbleArmed << 1) | (in.raceMenu << 2) |
                             (in.weaponEquipped << 3) | (in.weaponDrawn << 4) |
                             (in.inCombat << 5) | (in.mounted << 6) | (weDrew << 7) |
                             (static_cast<int>(ws) << 12);
            if (key != s_lastKey) {
                s_lastKey = key;
                spdlog::debug(
                    "weapon preview gate: action={} weDrew={} | enabled={} armed={} race={} "
                    "equipped={} drawn={} combat={} mounted={} (ws={})",
                    static_cast<int>(action), weDrew, in.enabled, in.bubbleArmed, in.raceMenu,
                    in.weaponEquipped, in.weaponDrawn, in.inCombat, in.mounted,
                    static_cast<int>(ws));
            }
        }

        // Runs on every tick and prints only on change, so it costs a few
        // pointer derefs per tick and a line per real move. Deliberately OUTSIDE
        // the switch: the shield report is about a menu that is already open and
        // holding steady, which is the kNone case, and kNone is silent.
        LogShieldState(a_player, a_bubbleArmed ? "armed" : "disarmed");

        // PER TICK, not at the transition edges (r12). Edge-triggered un-culling
        // is exactly why the field had to unequip and re-equip a shield to make
        // it appear: the swap path fired it and worked, while the open path ran
        // BEFORE the engine had culled anything, so it found nothing to clear
        // and nothing ever revisited it. Whoever culls it and whenever, this
        // sees it on the next tick. Costs a couple of pointer derefs and is a
        // no-op the moment the flag is already clear.
        if (a_bubbleArmed && state->IsWeaponDrawn()) {
            ShowShieldIfHidden(a_player);
        }

        switch (action) {
            case WeaponPreviewGate::Action::kDraw: {
                // kDraw implies weaponEquipped implies a non-null weapon - but
                // that guarantee lives in another file, and the two dereferences
                // below are load-bearing. Keep the check next to what it
                // protects so a future reorder of the gate's guards cannot turn
                // this into a crash.
                if (!weapon) {
                    break;
                }
                // DrawWeaponMagicHands still runs: it owns the actor's weapon
                // STATE, and skipping it would leave the state machine saying
                // sheathed while the model sits in the hand - IsWeaponDrawn()
                // would lie to the gate and to the restore. What we drop is the
                // ANIMATION, by snapping straight to the drawn idle on the same
                // frame. The world is paused here, so the draw animation only
                // advances because we tick it; cutting to the pose before that
                // first tick means it is never seen.
                Draw(a_player, true);
                // Run the draw to completion inside this frame. Everything the
                // older rounds tried to forge by hand falls out of the engine's
                // own transition once it is allowed to finish: the weaponDraw
                // annotation attaches the weapon, the shield attaches with it,
                // and the graph picks the drawn idle for this weapon's class.
                // The world is paused, so none of it is ever rendered.
                const bool arrived = PumpToState(a_player, RE::WEAPON_STATE::kDrawn, "open");
                if (!arrived) {
                    // Capped, so the annotation may not have run. 0xB4 is
                    // idempotent, which makes this a free safety net rather
                    // than a second competing mechanism.
                    if (auto* const w = weapon->As<RE::TESObjectWEAP>()) {
                        ReparentToHand(a_player, w);
                    }
                }
                weDrew      = true;
                shownWeapon = weapon->GetFormID();
                {
                    auto* const drawn = weapon->As<RE::TESObjectWEAP>();
                    shownType = drawn ? static_cast<int>(drawn->GetWeaponType()) : -1;
                }
                spdlog::debug("weapon preview: drawing '{}'{}.",
                              weapon->GetName() ? weapon->GetName() : "?",
                              arrived ? "" : " (pump CAPPED - re-parent fallback sent)");
                break;
            }

            case WeaponPreviewGate::Action::kRestore:
                Restore(a_player);
                break;

            case WeaponPreviewGate::Action::kNone: {
                // A weapon is on screen and we are holding steady. Two ways to
                // get here, and BOTH need swap handling:
                //
                //   a) we drew it (weDrew) - the preview owns it;
                //   b) the player already had it out when the menu opened - the
                //      "mirror the real state" case, which deliberately takes NO
                //      debt so closing the menu cannot sheathe a weapon they
                //      chose to have drawn.
                //
                // r3 gated this whole branch on weDrew and so handled only (a).
                // In (b) a swap went completely unhandled: the engine parks the
                // newly equipped weapon on its SHEATH node, nothing moved it,
                // and it sat on the hip with the hands empty - and because the
                // gate returns kNone silently, the log showed nothing at all.
                // That is the field report. Track the shown weapon in both
                // cases; who drew it does not change where it has to go.
                //
                // r10 watches BOTH hands. Equipping a shield never touches the
                // right hand, so an id-only comparison returned early and the
                // shield equip was never handled at all - it stayed invisible
                // until the menu closed and the engine ran the transition for
                // itself. The left slot is compared by FormID because a shield
                // is a TESObjectARMO and nothing weapon-shaped will match it.
                const RE::FormID id     = weapon ? weapon->GetFormID() : 0;
                auto* const      leftFm = a_player->GetEquippedObject(true);
                const RE::FormID leftId = leftFm ? leftFm->GetFormID() : 0;
                if (inFlight || (id == shownWeapon && leftId == shownLeft)) {
                    break;
                }
                const bool hadOne     = shownWeapon != 0 || shownLeft != 0;
                const bool rightMoved = id != shownWeapon;
                shownWeapon           = id;
                shownLeft             = leftId;
                // First sight (menu just opened on an already-drawn loadout):
                // adopt it, but there is nothing to correct yet.
                if (!hadOne) {
                    auto* const firstWeap = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
                    shownType = firstWeap ? static_cast<int>(firstWeap->GetWeaponType()) : -1;
                    break;
                }
                {
                    // r5 (field): NO DRAW ON A SWAP, EVER.
                    //
                    // This used to re-issue Draw() when weDrew, for the one case
                    // where it did real work: the player sheathed by hand mid-menu
                    // while we owed a draw. On an already-drawn weapon it was a
                    // no-op, so it looked harmless. It was not - in exactly that
                    // sheathed case it fires DrawWeaponMagicHands(true) and the
                    // player watches a full equip animation play inside a paused
                    // menu (user: "when we have our weapon sheathed and then we
                    // change weapon the equip animation plays, this shouldn't
                    // happen, in this case it should just switch on the hip").
                    //
                    // Sheathed is a state the player chose, whether they started
                    // that way or sheathed by hand mid-menu, and a swap is not a
                    // reason to overrule it. Sheathed swaps now do nothing at all
                    // here: the engine has already parked the new weapon on its
                    // sheath node, which IS "switch on the hip", so the correct
                    // action is none. The re-parent below stays gated on drawnNow,
                    // so it only moves a weapon that is genuinely out.
                    //
                    // CROSS-CLASS POSE (user: "when i switch to a bow i can see
                    // them use a bow or 2 handed with 1 handed animation which
                    // is not right"). The engine ALREADY queues its own replace
                    // when the new weapon is equipped - the field sees that
                    // animation begin - and that path re-picks the idle for the
                    // new class by itself. It simply never got to finish in a
                    // paused menu, so the pose stayed on the old class.
                    //
                    // So this PUMPS rather than poses. r8 sent
                    // WeapOutRightReplaceForceEquip here instead, and that is
                    // what produced the looping walk: the subgraph behind it has
                    // a Locomotion branch as well as a Standing one, and
                    // choosing between them is the graph's job using state we do
                    // not own. Synthetic events are how this feature kept
                    // acquiring new bugs; the engine's own transition does not.
                    //
                    // r13 - ON A CROSS-CLASS SWAP, REPLAY THE ENGINE'S OWN DRAW.
                    //
                    // User: "we still have that annoying issue where we can have
                    // a 1 handed weapon show on a 2 handed weapon idle and it
                    // looks silly, the game is paused so it's difficult to get
                    // logic for it to be displayed correctly". Correct, so stop
                    // writing logic for it. The engine already picks the right
                    // idle for a weapon class perfectly on a DRAW - that is why
                    // the open path looks right and only swaps look silly.
                    //
                    // A sheathe+redraw was the obvious answer from the start and
                    // was rejected TWICE, for exactly one reason: it cost ~2s of
                    // visible animation per swap, the flicker r2 removed, and
                    // doubly wrong in a paused world. The pump removes that
                    // reason entirely. Pumped to completion inside a single
                    // frame, the whole cycle is never rendered, so what is left
                    // is the engine's own guaranteed idle selection at no
                    // visible cost. The objection was about the price, not the
                    // mechanism, and the price is now zero.
                    //
                    // Only when the CLASS changes. A same-class swap already
                    // looks right and the replace pump handles it, so there is
                    // no reason to spend two transitions on it - and this keeps
                    // the expensive path on the rare event.
                    auto* const weap     = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
                    const bool  drawnNow = state->IsWeaponDrawn();
                    const int   newType =
                        weap ? static_cast<int>(weap->GetWeaponType()) : -1;
                    const bool crossClass = drawnNow && weap && newType != shownType;
                    // Gated on DRAWN only, not on a weapon being present: a
                    // shield equipped over an empty right hand still has a
                    // transition to run, and requiring a TESObjectWEAP here is
                    // exactly what kept shields out of this path entirely.
                    if (crossClass && Settings::GetSingleton().slowSwapExperiment) {
                        // r20: same transition, real frames. DriveSlowSwap at
                        // the top of Update() carries it from here.
                        Draw(a_player, false);
                        slowPhase  = 1;
                        slowFrames = 0;
                        // The round marker is deliberate: r20's cap silently
                        // invalidated its own experiment, and nothing in the
                        // log said which build produced the result. A build
                        // that reports its own identity and its own budget
                        // costs one token and settles that in one grep.
                        spdlog::debug("weapon preview: slow swap r20a STARTED (cross-class "
                                      "{}->{}) - sheathe issued, real frames from here "
                                      "(per-phase cap {} frames).",
                                      shownType, newType, kSlowMaxFrames);
                    } else if (crossClass &&
                               !Settings::GetSingleton().crossClassSheatheRedraw) {
                        // r22 A/B ARM B. Skip the sheathe. Fall through to the
                        // same replace pump a same-class swap uses, which is
                        // the closest we can get to what the engine does live.
                        //
                        // r21 measured the live path and it NEVER sheathes:
                        // tailCombatIdle -> BeginWeaponDraw -> WeapEquip_Out,
                        // drawn the whole way. The sheathe is ours alone.
                        //
                        // The round marker is a real string literal, not a
                        // comment: r20a's marker was in a comment, the DLL did
                        // not contain it, and there was briefly no way to prove
                        // which build was deployed. A log line that names its
                        // own round settles that in one grep.
                        spdlog::debug("weapon preview: r22 ARM B - cross-class {}->{} "
                                      "WITHOUT the sheathe detour (replace pump only).",
                                      shownType, newType);
                        PumpSwap(a_player);
                        // Same idempotent guarantee the same-class path takes.
                        // Without the sheathe there is no redraw annotation to
                        // re-attach the weapon, so this is load-bearing here in
                        // a way it is not below. Left hand deliberately not
                        // touched - see ReparentToHand on why asking for the
                        // left can yank the right hand's weapon.
                        if (weap) {
                            ReparentToHand(a_player, weap);
                        }
                    } else if (crossClass) {
                        // Down and back up, both pumped. The redraw is what
                        // re-selects the idle, and its annotations re-attach the
                        // weapon and un-cull the shield on the way, so no hand
                        // placement is needed after it.
                        spdlog::debug("weapon preview: r22 ARM A - cross-class {}->{} "
                                      "WITH the sheathe detour (r13 behaviour).",
                                      shownType, newType);
                        Draw(a_player, false);
                        PumpToState(a_player, RE::WEAPON_STATE::kSheathed, "swap-sheathe");
                        Draw(a_player, true);
                        PumpToState(a_player, RE::WEAPON_STATE::kDrawn, "swap-redraw");
                    } else if (drawnNow) {
                        PumpSwap(a_player);
                        // Idempotent position guarantee for the right hand. The
                        // pump should have attached it through the clip's own
                        // annotation; if the replace ran longer than the budget
                        // this still puts it in the hand, and it is a no-op in
                        // every other case. The left hand is deliberately NOT
                        // re-parented by hand - see ReparentToHand's note on why
                        // asking for the left can yank the right hand's weapon.
                        if (weap) {
                            ReparentToHand(a_player, weap);
                        }
                    }
                    const int oldType = shownType;
                    shownType         = newType;
                    spdlog::debug("weapon preview: swap to '{}' - {} (moved={} type {}->{} "
                                  "drawn={}).",
                                  weapon && weapon->GetName() ? weapon->GetName() : "?",
                                  crossClass ? "CROSS-CLASS redraw"
                                             : (drawnNow ? "same-class pump" : "skipped"),
                                  rightMoved ? "right" : "left", oldType, newType, drawnNow);
                }
                break;
            }
        }
    }

}  // namespace MTB::WeaponPreview
