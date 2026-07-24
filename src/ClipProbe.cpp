#include "PCH.h"

#include "ClipProbe.h"

#include "AnimEventProbe.h"
#include "Settings.h"  // diagnosticProbes gates the LOGGING only; the hook is always on

#include <chrono>
#include <limits>

namespace MTB::ClipProbe {

    namespace {
        // The player has two graphs (third person / first person), and the
        // hook fires from the animation thread while the main thread may be
        // re-tracking across a load. Atomics rather than a lock: the hook is on
        // a hot path shared with every actor in the cell, and it must never be
        // the reason a frame stalls.
        constexpr std::size_t kMaxTracked = 4;

        std::atomic<const RE::hkbCharacter*> g_playerChars[kMaxTracked]{};
        std::atomic<bool>                    g_tracking{ false };
        bool                                 g_installed = false;

        // ⚠ SELF-VERIFICATION. A probe that logs nothing is indistinguishable
        // from a probe that never installed, never tracked, or whose filter
        // never matched - and this project has now been burned by exactly that
        // twice (ActorTickProbe printed "Actor::Update ran 0 time(s)" for runs
        // in which its hook was never installed at all, and that vacuous zero
        // is cited in the docs as a measurement). So count both sides of the
        // filter and say so out loud: "seen" proves the hook fires, "matched"
        // proves the player-graph identity is right. Zero clips with a healthy
        // seen count is real evidence (a looping idle re-activates nothing);
        // zero SEEN means the probe itself is broken.
        std::atomic<std::uint64_t> g_seen{ 0 };
        std::atomic<std::uint64_t> g_matched{ 0 };

        // When the player's graph last started ANY clip. A settled character
        // starts nothing - a looping idle activates once and then just loops -
        // so recent activity means the graph is still moving between poses.
        //
        // Used ONLY at the arm edge, deliberately. As a per-frame gate it would
        // stutter: freeze, expire, tick one frame, a clip activates, freeze
        // again. As an arm-edge latch it simply says "this menu opened while
        // the character was mid-transition, so hold the pose".
        std::atomic<std::int64_t> g_lastActivityUs{ 0 };

        // End of the equip clip that is currently running (clip start +
        // duration), or 0. Lets the equip pump run the clip's REAL length
        // instead of a fixed budget sized for vanilla.
        std::atomic<std::int64_t> g_equipClipEndUs{ 0 };

        // ── Pump window. A pump calls Actor::UpdateAnimation in a tight loop on
        // THIS thread, and hkbClipGenerator::Activate runs synchronously inside
        // it, so a plain counter is enough - no cross-thread ordering to think
        // about. Depth rather than a bool because a pump can nest (PumpSwap
        // calls SettleLocomotion, which steps the graph itself).
        std::atomic<int>          g_pumpDepth{ 0 };
        std::atomic<const char*>  g_pumpWhy{ nullptr };
        std::atomic<float>        g_pumpSeconds{ 0.0f };  // this armed session

        // Per-armed-session counters. Reported at the disarm edge, reset at the
        // arm edge - the same shape as the face counters, and for the same
        // reason: an impression is not a measurement.
        std::atomic<std::uint32_t> g_sessClips{ 0 };
        std::atomic<std::uint32_t> g_sessIdles{ 0 };
        std::atomic<std::uint32_t> g_sessIdlesPumped{ 0 };
        std::atomic<std::uint32_t> g_sessIdlesAfterEquip{ 0 };
        std::atomic<std::uint32_t> g_sessEquips{ 0 };

        // Every idle activation on a player graph, armed or not. Never reset -
        // a pump samples deltas, so wrapping is the only thing that could hurt
        // and a 64-bit counter at a few hundred a minute will not.
        std::atomic<std::uint64_t> g_idlePicks{ 0 };

        // Once-per-menu latch for the clear. Cleared on the DISARM side, not the
        // arm side: the arm path runs every frame, so clearing there would
        // re-open it every frame and log per call. Fifth time this shape has
        // come up in this codebase - latch at the edge, never re-derive.
        std::atomic<bool> g_clearLoggedThisSession{ false };
        std::atomic<bool> g_settledLoggedThisSession{ false };
        bool              g_combatClearAttemptedForPresence = false;

        // The ability WE added, if any, so the disarm edge can take it back.
        // Main thread only - the drive and the disarm both run from the bubble
        // tick, so no atomic is needed and a plain pointer reads honestly.
        RE::SpellItem* g_addedSettledSpell = nullptr;

        // The two Nolvus stance framework markers, resolved once. LookupForm
        // walks the load order and both callers below run every frame.
        //
        // Lives here rather than in its own file because the marker IS the idle
        // story: `Idle Loop` negates 0x804, and this file is where every other
        // piece of that story already lives.
        struct StanceForms {
            RE::EffectSetting* settled;    // 0x802 - Idle Loop REQUIRES it
            RE::EffectSetting* combat;     // 0x804 - non-combat Idle Loop FORBIDS it
            RE::EffectSetting* combatOff;  // 0x808 - paired dispel effect
        };

        // The spell that CARRIES the settled marker. Found by search rather than
        // hardcoded: the OAR configs name the EFFECT (0x802), never the spell,
        // and a form ID guessed from the effect's neighbours would be a silent
        // wrong answer. Iterating the spell array once at startup and matching
        // on the effect pointer is exact, and it logs what it found.
        [[nodiscard]] RE::SpellItem* FindSpellCarrying(RE::TESDataHandler* a_data,
                                                       RE::EffectSetting*  a_effect) {
            if (!a_effect) {
                return nullptr;
            }
            for (auto* const spell : a_data->GetFormArray<RE::SpellItem>()) {
                if (!spell) {
                    continue;
                }
                for (const auto* const eff : spell->effects) {
                    if (eff && eff->baseEffect == a_effect) {
                        return spell;
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] StanceForms StanceMarkerForms(RE::TESDataHandler* a_data) {
            static bool        s_looked = false;
            static StanceForms s_forms{ nullptr, nullptr, nullptr };
            if (!s_looked) {
                s_looked = true;
                s_forms.settled =
                    a_data->LookupForm<RE::EffectSetting>(0x802, "Smooth Moveset.esp");
                s_forms.combat =
                    a_data->LookupForm<RE::EffectSetting>(0x804, "Smooth Moveset.esp");
                s_forms.combatOff =
                    a_data->LookupForm<RE::EffectSetting>(0x808, "Smooth Moveset.esp");
                // ⚠ Say so out loud when the plugin is absent. A probe that logs
                // nothing is indistinguishable from one that never installed,
                // and this project has been burned by exactly that twice.
                spdlog::info("stance markers: Smooth Moveset.esp lookup - 0x802 loop={}, "
                             "0x804 combat={}, 0x808 combat-off={}{}",
                             static_cast<const void*>(s_forms.settled),
                             static_cast<const void*>(s_forms.combat),
                             static_cast<const void*>(s_forms.combatOff),
                             (!s_forms.settled && !s_forms.combat)
                                 ? " - PLUGIN NOT LOADED, the stance marker probe and the "
                                   "combat-marker clear are both inert from here"
                                 : "");
            }
            return s_forms;
        }

        // ⚠ THE TRANSITION ENDS AT THE IDLE PICK, NOT AT THE END OF THE CLIP.
        //
        // The first version of this stamped an end time from the clip's own
        // duration, and the field found the hole immediately: "if I open the
        // menu early it pauses, but when it's later the anim completes and then
        // it loops again". Measured on that run - equip clip started 38.171 and
        // ran 1.23 s, so it ended ~39.40, but the graph did not pick its idle
        // until 40.593. That 1.2 s gap is a window where the clip is over, the
        // idle is not chosen yet, and ticking runs the SELECTION under a frozen
        // Papyrus VM - which is the entire defect (see ClipProbe.h).
        //
        // So hold from the equip clip starting until an IDLE clip actually
        // activates on the same graph. That is the graph telling us the
        // transition is complete, rather than us predicting when it should be.
        //
        // The graph pointer is stored for IDENTITY COMPARISON ONLY and is never
        // dereferenced, so a rebuilt graph cannot turn it into a bad deref - it
        // just stops matching. Reset() clears it regardless.
        // ⚠ PER GRAPH, NOT ONE GLOBAL. The player has TWO graphs (first person
        // and third person) and an equip fires on BOTH - the field log shows
        // the pair every time, with different clip lengths (1.23 s and 1.33 s
        // for the same Axe_Equip). A single slot let the second activation
        // overwrite the first, so whichever graph reached its idle FIRST
        // released the hold while the other was still transitioning. The tell
        // was in the log all along: equip lines always came in pairs, but every
        // "transition COMPLETE" line was a single.
        //
        // That residue is exactly what the field then reported: "much better,
        // but now it happens in an even smaller window towards when the
        // animation is finished" - the window having shrunk to the gap between
        // the two graphs finishing.
        [[nodiscard]] std::int64_t NowUs() {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        struct PendingGraph {
            std::atomic<const RE::hkbCharacter*> graph{ nullptr };
            // Valid while settleUntilUs is zero: the active equip generator
            // whose localTime lets a pump stop before it selects an idle.
            std::atomic<RE::hkbClipGenerator*>    equipClip{ nullptr };
            std::atomic<std::int64_t>            stampUs{ 0 };
            // 0 while the equip clip itself is still running; once the graph
            // picks its idle this becomes the moment the SETTLE ends.
            std::atomic<std::int64_t> settleUntilUs{ 0 };
        };
        PendingGraph g_pending[kMaxTracked];

        // ⚠ THE IDLE PICK IS NOT THE END OF THE TRANSITION EITHER.
        //
        // Third field lesson on the same bug, and the same shape each time: the
        // signal I trusted turned out to be one step short of the truth. The
        // weapon STATE lies (kDrawn fires at the halfway annotation), the clip
        // END lies (the idle is not picked for another ~1.2 s), and now the
        // idle PICK lies too - because what follows it is a start-phase idle
        // that has to progress to a settled one.
        //
        // Field events, captured inside a menu that armed with nothing in
        // flight, 0.65 s after the transition completed:
        //
        //     Smooth_NonCombat / FootScuffLeft / FootScuffRight / Smooth_loop
        //     / PickNewIdle / Smooth_NonCombat / FootScuffLeft ...
        //
        // FootScuff* are footstep annotations: that cycle IS the visible lunge.
        // Smooth Moveset drives start -> settled through script-maintained
        // state, and a paused menu freezes the Papyrus VM, so the progression
        // can never complete while a menu is open. Tick the graph in that state
        // and the stepping start clip simply loops.
        //
        // So hold a little past the pick and let the settle happen live. This
        // is a TUNING VALUE, not a mechanism - it trades a static pose for
        // menus opened immediately after a draw against the loop. Field-chosen.
        constexpr std::int64_t kIdleSettleUs = 1'000'000;  // 1.0 s

        // Mark this graph as mid-transition. Reuses its existing slot so a
        // re-triggered equip re-stamps rather than consuming a second slot.
        void MarkEquipPending(const RE::hkbCharacter* a_graph,
                              RE::hkbClipGenerator*    a_clip) {
            for (auto& slot : g_pending) {
                if (slot.graph.load(std::memory_order_acquire) == a_graph) {
                    slot.equipClip.store(a_clip, std::memory_order_release);
                    slot.stampUs.store(NowUs(), std::memory_order_release);
                    slot.settleUntilUs.store(0, std::memory_order_release);  // clip running again
                    return;
                }
            }
            for (auto& slot : g_pending) {
                const RE::hkbCharacter* expected = nullptr;
                if (slot.graph.compare_exchange_strong(expected, a_graph,
                                                       std::memory_order_acq_rel)) {
                    slot.equipClip.store(a_clip, std::memory_order_release);
                    slot.stampUs.store(NowUs(), std::memory_order_release);
                    slot.settleUntilUs.store(0, std::memory_order_release);
                    return;
                }
            }
            // Every slot busy: drop it rather than evict something still
            // pending. Failing OPEN (no hold) matches the old behaviour and can
            // never wedge the freeze on.
        }

        // The graph reached its idle. Start the settle rather than releasing
        // outright - see kIdleSettleUs. Returns true if this graph was pending
        // and has not already begun settling (so the log prints once).
        bool BeginIdleSettle(const RE::hkbCharacter* a_graph) {
            for (auto& slot : g_pending) {
                if (slot.graph.load(std::memory_order_acquire) == a_graph &&
                    slot.settleUntilUs.load(std::memory_order_acquire) == 0) {
                    slot.equipClip.store(nullptr, std::memory_order_release);
                    slot.settleUntilUs.store(NowUs() + kIdleSettleUs,
                                             std::memory_order_release);
                    return true;
                }
            }
            return false;
        }

        // Safety net only. If the graph never activates an idle (the player
        // goes straight into an attack, a killmove, furniture), the hold must
        // still expire - a stuck freeze would look exactly like the bug it
        // fixes. Generous enough for any replaced draw seen in the field.
        constexpr std::int64_t kEquipHoldCapUs = 10'000'000;

        // Draw and sheathe clips both carry "equip" in the file name across
        // vanilla and every replacer seen in the field: 1HM_Equip, Dag_Equip,
        // 2HW_Equip, Bow_Equip, Dag_Unequip, and the OAR-replaced
        // "...\Sword Non Combat Equipping\1hm_equip.hkx". Substring, case
        // insensitive, so "Unequip" and "Equipping" are both covered.
        [[nodiscard]] bool ContainsNoCase(const char* a_hay, const char* a_needleLower) {
            for (const char* p = a_hay; *p; ++p) {
                const char* h = p;
                const char* n = a_needleLower;
                while (*n && *h) {
                    const char lower = (*h >= 'A' && *h <= 'Z')
                                           ? static_cast<char>(*h - 'A' + 'a')
                                           : *h;
                    if (lower != *n) {
                        break;
                    }
                    ++h;
                    ++n;
                }
                if (!*n) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool IsEquipClipName(const char* a_name) {
            return ContainsNoCase(a_name, "equip");  // covers Equip / Unequip / Equipping
        }

        // What ENDS the hold. The graph reaching an idle is the observable that
        // says the draw/sheathe transition is finished and a pose has been
        // chosen: 1HM_Idle, MT_Idle, male\MT_Idle, and the OAR submod files
        // under "...Idle Start\1hm_idle.hkx" all carry it.
        [[nodiscard]] bool IsIdleClipName(const char* a_name) {
            return ContainsNoCase(a_name, "idle");
        }

        // Is this clip the character MOVING? Covers the vanilla and replaced
        // names seen in the field: 1HM_WalkForward, 1HM_RunForward,
        // 2HM_WalkBackward, Bow_1stP_Run, 1HM_TurnRight60, 1HM_TurnRightSlow,
        // *_WalkArmBlend, *_RunArmBlend, sprint and strafe variants.
        //
        // ⚠ TURN COUNTS AS LOCOMOTION, BY DECISION, NOT BY ANALYSIS.
        //
        // It went in, came out, and went back in within three builds. In: a
        // camera rotation held the graph in 1HM_TurnRight60 / 1HM_TurnRight180
        // and the character kept moving. Out: with the actor's walking state
        // cleared, the turn classification made ANY rotation report "still
        // moving" and freeze the character. Back in, on the user's call
        // (2026-07-21): "rotation is something i don't think will work well
        // until we fully understand, let's revert and make rotation freeze".
        //
        // So this is a deliberate trade, not a conclusion: a rotating arm gets a
        // frozen pose, which is predictable and never wrong-looking, instead of
        // a live one that sometimes walks. Whoever picks this up next - the open
        // question is why the graph will not leave a turn state under a paused
        // menu, and that is unanswered, not settled.
        [[nodiscard]] bool IsLocomotionClipName(const char* a_name) {
            return ContainsNoCase(a_name, "walk") || ContainsNoCase(a_name, "run") ||
                   ContainsNoCase(a_name, "sprint") || ContainsNoCase(a_name, "strafe") ||
                   ContainsNoCase(a_name, "turn");
        }

        // Per graph, whether its most recent clip was locomotion. Indexed the
        // same way as g_playerChars so a rebuilt graph cannot leave a stale
        // answer behind - TrackPlayerGraphs clears both together.
        std::atomic<bool> g_graphLocomoting[kMaxTracked]{};

        // How long this clip will run, in seconds, or 0 if it cannot be read.
        // Every deref here is valid: we are inside the clip's own Activate.
        [[nodiscard]] float ClipDurationSeconds(RE::hkbClipGenerator* a_clip) {
            if (!a_clip->binding || !a_clip->binding->animation) {
                return 0.0f;
            }
            const float duration = a_clip->binding->animation->duration;
            if (!(duration > 0.0f)) {
                return 0.0f;
            }
            // A clip played at 2x speed is over in half the time. Guard the
            // divide: a zero or negative speed would otherwise produce an
            // infinite or negative freeze.
            const float speed = a_clip->playbackSpeed;
            return (speed > 0.01f) ? (duration / speed) : duration;
        }

        bool IsPlayer(const RE::hkbCharacter* a_character) {
            if (!a_character || !g_tracking.load(std::memory_order_acquire)) {
                return false;
            }
            for (auto& slot : g_playerChars) {
                if (slot.load(std::memory_order_relaxed) == a_character) {
                    return true;
                }
            }
            return false;
        }

        struct ClipActivateHook {
            static void thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context) {
                // ORIGINAL FIRST, unconditionally, before any branch of ours.
                // A diagnostic that can change when the engine's own activation
                // runs is not a diagnostic, and an early return that skipped it
                // would break the graph for every actor in the cell.
                func(a_this, a_context);

                // Heartbeat BEFORE the filter, so silence can be attributed.
                const auto seen = g_seen.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool verbose = Settings::GetSingleton().diagnosticProbes;
                if (verbose && (seen & 0x3FF) == 0) {
                    spdlog::debug("clip probe: {} activations seen, {} were the player's "
                                  "(tracking={}).", seen,
                                  g_matched.load(std::memory_order_relaxed),
                                  g_tracking.load(std::memory_order_relaxed));
                }
                if (!a_this || !IsPlayer(a_context.character)) {
                    return;  // every NPC in the cell lands here - keep it cheap
                }
                g_matched.fetch_add(1, std::memory_order_relaxed);
                const char* const anim = a_this->animationName.c_str();
                if (!anim || !*anim) {
                    return;
                }
                // ── The load-bearing part. Stamp when this clip will finish so
                // the arm edge can ask whether a draw/sheathe is still running,
                // instead of asking the weapon state, which has already lied by
                // then (r11: kDrawn fires at the halfway annotation).
                g_lastActivityUs.store(NowUs(), std::memory_order_release);
                // What THIS graph is now playing. Written before anything else
                // so the settle below can read it the instant a step returns.
                for (std::size_t i = 0; i < kMaxTracked; ++i) {
                    if (g_playerChars[i].load(std::memory_order_relaxed) == a_context.character) {
                        g_graphLocomoting[i].store(IsLocomotionClipName(anim),
                                                   std::memory_order_release);
                        break;
                    }
                }
                const bool armed  = AnimEventProbe::IsArmed();
                const bool pumped = g_pumpDepth.load(std::memory_order_relaxed) > 0;
                if (armed) {
                    g_sessClips.fetch_add(1, std::memory_order_relaxed);
                }
                if (IsEquipClipName(anim)) {
                    if (armed) {
                        g_sessEquips.fetch_add(1, std::memory_order_relaxed);
                    }
                    MarkEquipPending(a_context.character, a_this);
                    if (const float secs = ClipDurationSeconds(a_this); secs > 0.0f) {
                        const float capped = (secs > 10.0f) ? 10.0f : secs;
                        g_equipClipEndUs.store(
                            NowUs() + static_cast<std::int64_t>(capped * 1e6f),
                            std::memory_order_release);
                    }
                    if (verbose) {
                        spdlog::debug("clip probe: equip clip '{}' started ({:.2f}s, mode={}) on "
                                      "graph {} - IN FLIGHT until THIS graph picks an idle.",
                                      anim, ClipDurationSeconds(a_this),
                                      static_cast<int>(a_this->mode.get()),
                                      static_cast<const void*>(a_context.character));
                    }
                } else if (IsIdleClipName(anim)) {
                    g_idlePicks.fetch_add(1, std::memory_order_release);
                    if (armed) {
                        g_sessIdles.fetch_add(1, std::memory_order_relaxed);
                        if (g_sessEquips.load(std::memory_order_relaxed) != 0) {
                            g_sessIdlesAfterEquip.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (pumped) {
                            g_sessIdlesPumped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (BeginIdleSettle(a_context.character) && verbose) {
                        spdlog::debug("clip probe: idle '{}' picked on graph {} - holding "
                                      "{:.1f}s more so the idle SETTLES live (start-phase "
                                      "clips step, and the VM cannot advance them in a menu).",
                                      anim, static_cast<const void*>(a_context.character),
                                      static_cast<float>(kIdleSettleUs) / 1e6f);
                    }
                }
                if (!verbose) {
                    return;
                }
                // The node's own name too. The clip says WHICH FILE, the node
                // says which slot in the graph asked for it, and the pair is
                // what makes a wrong pick readable rather than merely visible.
                //
                // ⚠ AND THE DURATION, because the FILE NAME CANNOT TELL THE TWO
                // IDLES APART. The lunge is a stance framework picking `Idle
                // Start` over `Idle Loop`, and both are served from the same
                // path (`1hm_idle.hkx`, merged into one folder by the MO2 VFS) -
                // so every line in the last field log read identically whether
                // the pick was right or wrong. They differ in LENGTH: the
                // stepping start clip is 30,912 bytes against the loop's
                // 106,608. Printing seconds is what makes a wrong pick
                // countable instead of merely suspected.
                //
                // ⚠ AND THE GRAPH, THE PUMP AND THE MODE - see ClipProbe.h. The
                // duration alone convicted the wrong suspect once already.
                const char* const node = a_this->name.c_str();
                const char* const why  = g_pumpWhy.load(std::memory_order_relaxed);
                spdlog::debug("clip [{}] {} (node '{}', {:.2f}s, mode={}, graph {}, {})",
                              armed ? "MENU" : "live", anim,
                              (node && *node) ? node : "?", ClipDurationSeconds(a_this),
                              static_cast<int>(a_this->mode.get()),
                              static_cast<const void*>(a_context.character),
                              pumped ? (why ? why : "pump") : "frame");
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void Reset() {
        g_tracking.store(false, std::memory_order_release);
        for (auto& slot : g_playerChars) {
            slot.store(nullptr, std::memory_order_relaxed);
        }
        for (auto& slot : g_pending) {
            slot.equipClip.store(nullptr, std::memory_order_release);
            slot.graph.store(nullptr, std::memory_order_release);
            slot.stampUs.store(0, std::memory_order_release);
            slot.settleUntilUs.store(0, std::memory_order_release);
        }
        g_lastActivityUs.store(0, std::memory_order_release);
        g_equipClipEndUs.store(0, std::memory_order_release);
        // Not the pump depth: Reset() runs on a load, never inside a pump, and
        // zeroing a depth some live PumpWindow still owns would leave the next
        // PumpWindowEnd to underflow it.
        g_pumpWhy.store(nullptr, std::memory_order_relaxed);
    }

    bool GraphSettling() {
        const auto last = g_lastActivityUs.load(std::memory_order_acquire);
        return last != 0 && (NowUs() - last) < kIdleSettleUs;
    }

    float EquipClipRemainingSeconds() {
        const auto end = g_equipClipEndUs.load(std::memory_order_acquire);
        if (end == 0) {
            return 0.0f;
        }
        const auto left = end - NowUs();
        return (left > 0) ? static_cast<float>(left) / 1e6f : 0.0f;
    }

    bool EquipTransitionUnfinished() {
        // A pure read - it retires nothing and grants no settle window, so the
        // answer is exactly "some graph started a draw and has not reached an
        // idle". The hold cap is still honoured: a slot that never picks an
        // idle (attack, killmove, furniture) must not be able to pin a pump.
        //
        // ⚠ The cap is WALL CLOCK and a pump is two milliseconds long, so it
        // can never trip inside one. The pump's own step budget is the real
        // bound, which is why that budget stays a hard ceiling below.
        const auto now = NowUs();
        for (auto& slot : g_pending) {
            if (!slot.graph.load(std::memory_order_acquire)) {
                continue;
            }
            if ((now - slot.stampUs.load(std::memory_order_acquire)) >= kEquipHoldCapUs) {
                continue;
            }
            if (slot.settleUntilUs.load(std::memory_order_acquire) == 0) {
                return true;
            }
        }
        return false;
    }

    float EquipTransitionRemainingGraphSeconds() {
        float minimum = (std::numeric_limits<float>::max)();
        bool  found = false;
        for (auto& slot : g_pending) {
            if (!slot.graph.load(std::memory_order_acquire) ||
                slot.settleUntilUs.load(std::memory_order_acquire) != 0) {
                continue;
            }
            auto* const clip = slot.equipClip.load(std::memory_order_acquire);
            // An unfinished transition without a readable active generator is
            // not safe to advance: fail closed and hold before the idle.
            if (!clip || !clip->binding || !clip->binding->animation) {
                return 0.0f;
            }
            const float duration = clip->binding->animation->duration;
            const float speed = clip->playbackSpeed;
            if (!(duration > 0.0f) || !(speed > 0.01f)) {
                return 0.0f;
            }
            const float clipEnd = duration - clip->cropEndAmountLocalTime;
            const float remaining = (clipEnd - clip->localTime) / speed;
            minimum = (std::min)(minimum, (std::max)(0.0f, remaining));
            found = true;
        }
        return found ? minimum : -1.0f;
    }

    std::uint64_t IdlePickCount() { return g_idlePicks.load(std::memory_order_acquire); }

    bool InLocomotionClip() {
        if (!g_tracking.load(std::memory_order_acquire)) {
            return false;  // nothing tracked - do not claim the player is moving
        }
        for (std::size_t i = 0; i < kMaxTracked; ++i) {
            if (g_playerChars[i].load(std::memory_order_relaxed) &&
                g_graphLocomoting[i].load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void LogStanceMarkers(bool a_armed) {
        // Once a second. The loop's period is 3.33 s, so this samples it three
        // times a cycle - enough to see a marker flip, cheap enough to leave on.
        static std::int64_t s_lastUs = 0;
        const auto          now      = NowUs();
        if (s_lastUs != 0 && (now - s_lastUs) < 1'000'000) {
            return;
        }
        s_lastUs = now;

        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const data   = RE::TESDataHandler::GetSingleton();
        if (!player || !data) {
            return;
        }
        const auto forms = StanceMarkerForms(data);
        if (!forms.settled && !forms.combat) {
            return;
        }
        auto* const target = player->AsMagicTarget();
        if (!target) {
            return;
        }
        auto* const list = target->GetActiveEffectList();
        if (!list) {
            return;
        }
        bool  hasSettled      = false;
        bool  hasCombat       = false;
        float combatElapsed   = 0.0f;
        float combatDuration  = 0.0f;
        bool  combatInactive  = false;
        bool  combatDispelled = false;
        for (auto* const active : *list) {
            if (!active) {
                continue;
            }
            const auto* const base = active->GetBaseObject();
            if (!base) {
                continue;
            }
            if (base == forms.settled) {
                hasSettled = true;
            } else if (base == forms.combat) {
                hasCombat = true;
                // The clock and the flags, because "present" was not enough to
                // tell a dispelled-but-unswept effect from a live one, and that
                // ambiguity cost a whole field round.
                combatElapsed   = active->elapsedSeconds;
                combatDuration  = active->duration;
                combatInactive  = active->flags.any(RE::ActiveEffect::Flag::kInactive);
                combatDispelled = active->flags.any(RE::ActiveEffect::Flag::kDispelled);
            }
        }
        // The non-combat Loop submod passes only when loop-on is present and
        // combat-on is absent.
        // Printing the verdict, not just the inputs, is what makes this
        // readable in one grep against the PickNewIdle cadence.
        spdlog::debug("stance markers [{}]: 0x802 loop={} 0x804 combat={} "
                      "(clock {:.2f}/{:.2f}s, inactive={}, dispelled={}) -> "
                      "'Non-combat Idle Loop' condition {}",
                      a_armed ? "MENU" : "live", hasSettled, hasCombat,
                      combatElapsed, combatDuration, combatInactive, combatDispelled,
                      (hasSettled && !hasCombat) ? "PASSES"
                                                : "FAILS (falls back to Idle Start)");
    }

#if 0
    void DriveStanceMarkers(bool a_armed, float a_delta) {
        // Live, the marker is Smooth Moveset's to own. Only repair the stale
        // non-combat state while the menu is armed.
        if (!a_armed) {
            g_clearLoggedThisSession.store(false, std::memory_order_relaxed);
            g_settledLoggedThisSession.store(false, std::memory_order_relaxed);
            g_combatClearAttemptedForPresence = false;
            // Give back exactly what we took. Unconditional on the disarm edge
            // rather than keyed on any predicate: acquire and release must key
            // on the same state, and the INI can be edited mid-session.
            if (g_addedSettledSpell) {
                if (auto* const pc = RE::PlayerCharacter::GetSingleton()) {
                    pc->RemoveSpell(g_addedSettledSpell);
                    spdlog::info("stance marker drive: removed the '{}' ability we added - "
                                 "the player leaves the menu carrying exactly what they "
                                 "brought in.", g_addedSettledSpell->GetName());
                }
                g_addedSettledSpell = nullptr;
            }
            return;
        }
        (void)a_delta;
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const data   = RE::TESDataHandler::GetSingleton();
        if (!player || !data) {
            return;
        }
        const auto forms = StanceMarkerForms(data);
        if (!forms.combat) {
            return;  // plugin absent, or this list does not use the framework
        }
        auto* const target = player->AsMagicTarget();
        if (!target) {
            return;
        }
        auto* const list = target->GetActiveEffectList();
        if (!list) {
            return;
        }
        // ── HALF TWO: the SETTLED marker may be ABSENT, not stuck. ───────────
        //
        // Field 2026-07-21 17:01, walking into the menu then swapping:
        //
        //   17:01:28  settled=false starting=false -> FAILS
        //
        // Standing still the problem is `starting` stuck ON; walking it is
        // `settled` MISSING, because the player was moving so the script had
        // removed it and nothing can put it back while the VM is frozen. Two
        // halves of one broken condition needing OPPOSITE operations - ageing
        // an effect out cannot conjure one into existence.
        //
        // So cast the spell that carries it. Engine-side, no VM involved.
        bool settledPresent = false;
        for (auto* const active : *list) {
            if (active && active->GetBaseObject() == forms.settled) {
                settledPresent = true;
                break;
            }
        }
        if (!settledPresent && forms.settled && Settings::GetSingleton().applySettledMarker) {
            static bool            s_looked = false;
            static RE::SpellItem*  s_spell  = nullptr;
            if (!s_looked) {
                s_looked = true;
                s_spell  = FindSpellCarrying(data, forms.settled);
                spdlog::info("stance marker drive: spell carrying the SETTLED marker (0x802) "
                             "= {}{}",
                             s_spell ? s_spell->GetName() : "NOT FOUND",
                             s_spell ? fmt::format(" [{:08X}], type {}", s_spell->GetFormID(),
                                                   static_cast<int>(s_spell->GetSpellType()))
                                     : " - the walking case cannot be fixed this way");
            }
            if (s_spell) {
                // Abilities live on the actor; everything else is cast. Getting
                // this backwards either does nothing or leaves a permanent
                // buff, so branch on what the form actually says it is.
                if (s_spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility) {
                    // ⚠ AN ABILITY IS PERMANENT. r1 added it and never took it
                    // back, which leaves the player carrying 'loop on' forever
                    // - a mod that silently edits your character sheet is worse
                    // than the bug it was fixing. Remembered so the disarm edge
                    // can undo exactly what we did and nothing else.
                    if (player->AddSpell(s_spell)) {
                        g_addedSettledSpell = s_spell;
                    }
                } else if (auto* const caster = player->GetMagicCaster(
                               RE::MagicSystem::CastingSource::kInstant)) {
                    caster->CastSpellImmediate(s_spell, false, player, 1.0f, false, 0.0f,
                                               player);
                }
                if (!g_settledLoggedThisSession.exchange(true, std::memory_order_relaxed)) {
                    spdlog::info("stance marker drive: SETTLED marker was absent (you walked "
                                 "into the menu) - applied '{}' so 'Idle Loop' has its "
                                 "required half. Watch 'stance markers' for settled=true.",
                                 s_spell->GetName());
                }
            }
        }

        // The installed ESP is authoritative: 0x804 is `mag_combat on`, a
        // permanent ability with duration 0. Smooth Moveset supplies its exact
        // inverse as `mag_combat off` (0x808).
        bool combatPresent = false;
        for (auto* const active : *list) {
            // ⚠ THE STARTING MARKER ONLY. 0x802 is the SETTLED marker and the
            // Loop condition REQUIRES it - ageing that one out would break the
            // very condition this is trying to satisfy.
            if (!active || active->GetBaseObject() != forms.combat) {
                continue;
            }
            // Its own clock, its own expiry path. No dispel: r1 proved a
            // dispelled effect just sits in the list until a sweep that a
            // paused game never runs.
            combatPresent = true;
            break;
        }
        if (ticked > 0 &&
            !g_clearLoggedThisSession.exchange(true, std::memory_order_relaxed)) {
            // Once per menu, and it prints the CLOCK - if elapsed never climbs,
            // Update is not advancing it and this lever is dead too. An
            // untimed effect (duration 0) can never expire and would need a
            // different approach entirely, so say that out loud rather than
            // letting it look like it is working.
            spdlog::info("stance marker drive: ticking {} 'idle starting' effect(s) "
                         "(Smooth Moveset 0x804) on their own clock - elapsed {:.2f}s of "
                         "{:.2f}s.{}", ticked, elapsed, total,
                         (total > 0.0f)
                             ? " Watch 'stance markers' for starting=false."
                             : " ⚠ DURATION IS 0 - this effect never expires on a timer, so "
                               "ticking it cannot clear it and a different lever is needed.");
        }
    }

#endif

#if 0
    void DriveStanceMarkers(bool a_armed, float a_delta) {
        (void)a_delta;

        if (!a_armed) {
            g_clearLoggedThisSession.store(false, std::memory_order_relaxed);
            g_settledLoggedThisSession.store(false, std::memory_order_relaxed);
            g_combatClearAttemptedForPresence = false;
            if (g_addedSettledSpell) {
                if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                    player->RemoveSpell(g_addedSettledSpell);
                    spdlog::info("stance marker drive: removed the '{}' ability we added - "
                                 "the player leaves the menu carrying exactly what they "
                                 "brought in.", g_addedSettledSpell->GetName());
                }
                g_addedSettledSpell = nullptr;
            }
            return;
        }

        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const data   = RE::TESDataHandler::GetSingleton();
        if (!player || !data) {
            return;
        }
        const auto forms = StanceMarkerForms(data);
        if (!forms.combat) {
            return;
        }
        auto* const target = player->AsMagicTarget();
        auto* const list   = target ? target->GetActiveEffectList() : nullptr;
        if (!list) {
            return;
        }

        // Preserve the optional walking-case repair unchanged.
        bool settledPresent = false;
        for (auto* const active : *list) {
            if (active && active->GetBaseObject() == forms.settled) {
                settledPresent = true;
                break;
            }
        }
        if (!settledPresent && forms.settled && Settings::GetSingleton().applySettledMarker) {
            static bool           s_settledLooked = false;
            static RE::SpellItem* s_settledSpell  = nullptr;
            if (!s_settledLooked) {
                s_settledLooked = true;
                s_settledSpell  = FindSpellCarrying(data, forms.settled);
            }
            if (s_settledSpell &&
                s_settledSpell->GetSpellType() == RE::MagicSystem::SpellType::kAbility &&
                player->AddSpell(s_settledSpell)) {
                g_addedSettledSpell = s_settledSpell;
                if (!g_settledLoggedThisSession.exchange(true, std::memory_order_relaxed)) {
                    spdlog::info("stance marker drive: applied missing loop marker ability '{}'.",
                                 s_settledSpell->GetName());
                }
            }
        }

        // Installed Smooth Moveset.esp record names:
        //   0x804 = mag_combat on (permanent, duration 0)
        //   0x808 = mag_combat off (the paired dispel effect)
        // Never call ActiveEffect::Update on 0x804. Find and cast the framework's
        // own off spell instead.
        bool combatPresent = false;
        for (auto* const active : *list) {
            if (active && active->GetBaseObject() == forms.combat) {
                combatPresent = true;
                break;
            }
        }
        if (!combatPresent) {
            g_combatClearAttemptedForPresence = false;
        }

        static bool           s_clearLooked = false;
        static RE::SpellItem* s_clearSpell  = nullptr;
        if (!s_clearLooked) {
            s_clearLooked = true;
            s_clearSpell  = FindSpellCarrying(data, forms.combatOff);
            spdlog::info("stance marker drive: Smooth Moveset paired COMBAT-OFF spell = {}{}",
                         s_clearSpell ? s_clearSpell->GetName() : "NOT FOUND",
                         s_clearSpell
                             ? fmt::format(" [{:08X}], type {}", s_clearSpell->GetFormID(),
                                           static_cast<int>(s_clearSpell->GetSpellType()))
                             : " - 0x804 will be left entirely to Smooth Moveset");
        }

        const auto action = CombatMarkerPolicy::Decide({
            .armed = a_armed,
            .inCombat = player->IsInCombat(),
            .combatMarkerPresent = combatPresent,
            .clearSpellAvailable = s_clearSpell != nullptr,
            .attemptedForCurrentPresence = g_combatClearAttemptedForPresence,
        });
        if (action == CombatMarkerPolicy::Action::kCastPairedOffSpell) {
            g_combatClearAttemptedForPresence = true;
            if (auto* const caster =
                    player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                caster->CastSpellImmediate(s_clearSpell, false, player, 1.0f, false, 0.0f,
                                           player);
                if (!g_clearLoggedThisSession.exchange(true, std::memory_order_relaxed)) {
                    spdlog::info(
                        "stance marker drive: 0x804 is Smooth Moveset's permanent "
                        "'combat on' ability, not a timed idle transition. Cast paired '{}' "
                        "instead of advancing ActiveEffect::Update; combat marker present "
                        "immediately after cast={}.",
                        s_clearSpell->GetName(), target->HasMagicEffect(forms.combat));
                }
            }
        }
    }
#endif

    void PumpWindowBegin(const char* a_why) {
        g_pumpWhy.store(a_why, std::memory_order_relaxed);
        g_pumpDepth.fetch_add(1, std::memory_order_relaxed);
    }

    void PumpWindowEnd() {
        if (g_pumpDepth.fetch_sub(1, std::memory_order_relaxed) <= 1) {
            g_pumpDepth.store(0, std::memory_order_relaxed);
            g_pumpWhy.store(nullptr, std::memory_order_relaxed);
        }
    }

    void NotePumpSeconds(float a_seconds) {
        g_pumpSeconds.store(g_pumpSeconds.load(std::memory_order_relaxed) + a_seconds,
                            std::memory_order_relaxed);
    }

    void ArmedSessionBegin() {
        g_sessClips.store(0, std::memory_order_relaxed);
        g_sessIdles.store(0, std::memory_order_relaxed);
        g_sessIdlesPumped.store(0, std::memory_order_relaxed);
        g_sessIdlesAfterEquip.store(0, std::memory_order_relaxed);
        g_sessEquips.store(0, std::memory_order_relaxed);
        g_pumpSeconds.store(0.0f, std::memory_order_relaxed);
    }

    void ArmedSessionReport() {
        if (!Settings::GetSingleton().diagnosticProbes) {
            return;
        }
        const auto idles  = g_sessIdles.load(std::memory_order_relaxed);
        const auto pumped = g_sessIdlesPumped.load(std::memory_order_relaxed);
        // Always, not just under bDiagnosticProbes. This is the pass/fail
        // number for the idle re-pick and it costs one line per menu.
        spdlog::info("idle session: {} clip activations while armed - {} idle picks ({} inside "
                     "pumps, {} on ticked frames, {} after the first equip), {} equip clips; "
                     "pumps advanced {:.2f}s of graph time.",
                     g_sessClips.load(std::memory_order_relaxed), idles, pumped,
                     idles - pumped,
                     g_sessIdlesAfterEquip.load(std::memory_order_relaxed),
                     g_sessEquips.load(std::memory_order_relaxed),
                     g_pumpSeconds.load(std::memory_order_relaxed));
    }

    bool EquipOccurredThisSession() {
        return g_sessEquips.load(std::memory_order_acquire) != 0;
    }

    bool EquipClipInFlight() {
        // ANY graph still mid-transition holds the freeze. The third-person
        // graph is the one on screen, but which of the two settles last varies
        // with the clip lengths, so both have to clear before ticking resumes.
        const auto now      = NowUs();
        bool       inFlight = false;
        for (auto& slot : g_pending) {
            if (!slot.graph.load(std::memory_order_acquire)) {
                continue;
            }
            // Overall cap: never let a graph hold the freeze forever.
            if ((now - slot.stampUs.load(std::memory_order_acquire)) >= kEquipHoldCapUs) {
                slot.equipClip.store(nullptr, std::memory_order_release);
                slot.graph.store(nullptr, std::memory_order_release);
                continue;
            }
            const auto settle = slot.settleUntilUs.load(std::memory_order_acquire);
            if (settle == 0) {
                inFlight = true;  // the equip clip itself is still running
            } else if (now < settle) {
                inFlight = true;  // idle picked, still settling
            } else {
                // Settled. Retire the slot so it is free for the next draw.
                slot.equipClip.store(nullptr, std::memory_order_release);
                slot.graph.store(nullptr, std::memory_order_release);
            }
        }
        return inFlight;
    }

    void TrackPlayerGraphs() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) {
            return;
        }
        RE::BSAnimationGraphManagerPtr manager;
        if (!player->GetAnimationGraphManager(manager) || !manager) {
            return;
        }
        // ⚠ RE-TRACK WHEN THE GRAPH IS REBUILT, not just on load. This used to
        // early-out on a "tracking" flag that only Reset() cleared, and Reset()
        // only runs on kPreLoadGame / kNewGame. RaceMenu rebuilds the player's
        // graph with neither message, so after `showracemenu` the cached
        // hkbCharacter pointers were stale forever: the player filter stopped
        // matching, no clip was ever attributed to the player, and every feature
        // built on this - the equip hold above all - went silently dead. Field
        // 2026-07-21: "i switched gender using showracemenu ... and when i did
        // the test again it failed", with zero clip lines in the whole log.
        //
        // Comparing pointers is two loads per frame and cannot go stale. The
        // flag is now derived from the comparison rather than latched.
        const RE::hkbCharacter* current[kMaxTracked]{};
        std::size_t             found = 0;
        for (auto& graph : manager->graphs) {
            if (graph && found < kMaxTracked) {
                current[found++] = &graph->characterInstance;
            }
        }
        if (found == 0) {
            return;
        }
        bool same = g_tracking.load(std::memory_order_acquire);
        if (same) {
            for (std::size_t i = 0; i < kMaxTracked; ++i) {
                if (g_playerChars[i].load(std::memory_order_relaxed) !=
                    (i < found ? current[i] : nullptr)) {
                    same = false;
                    break;
                }
            }
        }
        if (same) {
            return;  // unchanged - the common case, and it costs two compares
        }
        // Stop matching on the old table before publishing the new one: a
        // half-updated table is worse than none.
        g_tracking.store(false, std::memory_order_release);
        // The pending slots key on the OLD graph pointers, so a rebuild strands
        // them: the graph they wait on can never pick an idle again, and the
        // slot would hold the equip freeze until its 10 s cap. Clear them with
        // the table they belong to.
        for (auto& slot : g_pending) {
            slot.graph.store(nullptr, std::memory_order_release);
            slot.stampUs.store(0, std::memory_order_release);
            slot.settleUntilUs.store(0, std::memory_order_release);
        }
        g_equipClipEndUs.store(0, std::memory_order_release);
        for (auto& loco : g_graphLocomoting) {
            loco.store(false, std::memory_order_release);
        }
        std::size_t n = 0;
        for (auto& graph : manager->graphs) {
            if (graph && n < kMaxTracked) {
                // characterInstance is EMBEDDED in the graph (+0x0C0), not a
                // pointer to one, so its address is the identity the hook's
                // context will carry. Taking the address of a member is why
                // this needs no vtable arithmetic and cannot crash.
                g_playerChars[n].store(&graph->characterInstance, std::memory_order_relaxed);
                ++n;
            }
        }
        if (n == 0) {
            return;
        }
        for (std::size_t i = n; i < kMaxTracked; ++i) {
            g_playerChars[i].store(nullptr, std::memory_order_relaxed);
        }
        // Released LAST so the hook never sees a half-filled table.
        g_tracking.store(true, std::memory_order_release);
        // Say when the table is armed. Without this, "no clip lines" could mean
        // the filter never had anything to match against, and there would be no
        // way to tell that from the graph genuinely starting no new clips.
        spdlog::info("clip probe: now tracking {} player graph(s) - clip lines from here on "
                     "are the player's. If none appear but 'activations seen' keeps rising, "
                     "the graph is re-using clips, not stalling.", n);
    }

    void Install() {
        if (g_installed) {
            return;
        }
        // write_vfunc, not write_branch: on a non-branch entry write_branch<5>
        // hands back a garbage "original" and the crash lands somewhere else
        // entirely. A vtable slot is also immune to the AE inlined-call-site
        // trap - there is no wrapper for AE to have inlined.
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_hkbClipGenerator[0] };
        ClipActivateHook::func = vtbl.write_vfunc(0x04, ClipActivateHook::thunk);
        g_installed            = true;
        spdlog::info("clip probe: hooked hkbClipGenerator::Activate. Logs the animation FILE "
                     "the player's graph starts, tagged [live] or [MENU] - the first look at "
                     "the OUTPUT of the stance choice rather than an input to it.");
    }

}  // namespace MTB::ClipProbe
