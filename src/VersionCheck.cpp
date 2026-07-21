#include "PCH.h"

#include "VersionCheck.h"

#include "Offsets.h"

#include <unordered_set>

// WHY THIS FILE EXISTS
//
// Menu Studio used to refuse every runtime that was not 1.5.97 or 1.6.1130+,
// which is what users on the mid 1.6 builds (1.6.640 and friends) saw as
// SKSE's "reported as incompatible during load". That gate was written when
// the AE half of the Offsets.h table had been verified against exactly one
// binary, and "verified on one build" was turned into "refuse every other
// build".
//
// Widening it is only safe with evidence, and we do not have a mid-version
// machine. So the plugin gathers the evidence itself, on the user's machine,
// and writes it to the log: one line per engine address, resolved or not,
// plus the bytes actually sitting there. A remote tester's report is unusable
// without it.
//
// ⚠ THE TWO THINGS THIS HAD TO GET RIGHT, BOTH OF WHICH LOOK FINE AND ARE NOT:
//
// 1. "The id resolved" is NOT the same as "the id exists". CommonLib's
//    IDDatabase::id2offset does a lower_bound over the id table and, on
//    SE/AE, only reports a failure when the id is past the END of the
//    database. An id that is simply ABSENT from this version's database lands
//    on its NEIGHBOUR and returns that function's offset, silently. Every
//    address in the table is mid-range, so absence NEVER throws - it just
//    hands back the wrong function. IdOk() does its own exact-match lookup
//    for that reason.
//
// 2. "The address resolved" is NOT the same as "the hook will fire". That is
//    [[ae-inlined-hook-sites-pitfall]]: addrlib resolves, the E8 byte check
//    passes, the hook installs, and it never fires because the runtime
//    inlined the callee and the surviving call site is dead code. Which is
//    also why the call-site offsets are the part most likely to be wrong on a
//    mid version: they are hand-measured BYTE OFFSETS INSIDE a function
//    (Main::Update+0x2A7), not Address Library ids, and nothing makes a byte
//    offset survive a recompile. LocateCall therefore verifies the CALL
//    TARGET rather than trusting the offset, and scans for the site when the
//    hand-measured one does not hold.

namespace {
    // How far into a containing function to look for a call site, and how
    // many int3 bytes in a row end the search. Padding between functions is
    // 0xCC, so a run of them means we have walked off the end.
    constexpr std::size_t kScanWindow = 0x1000;
    constexpr std::size_t kPadRun     = 4;

    bool           g_ran{ false };
    bool           g_criticalOk{ false };
    std::ptrdiff_t g_dispatchCall{ 0 };
    std::ptrdiff_t g_smootherCall{ 0 };

    // Every id in this version's Address Library, for exact-membership tests.
    //
    // Built from IDDatabase::Offset2ID because that is the only PUBLIC way to
    // reach the id table - the raw span is private. It copies and sorts the
    // whole database (~380k entries on AE, tens of milliseconds) and we throw
    // the sort away, which is wasteful but happens once at startup and is the
    // price of not reaching into CommonLib's internals.
    const std::unordered_set<std::uint64_t>& IdSet() {
        static const std::unordered_set<std::uint64_t> set = [] {
            std::unordered_set<std::uint64_t> out;
            const REL::IDDatabase::Offset2ID  db{};
            out.reserve(db.size());
            for (const auto& entry : db) {
                out.insert(entry.id);
            }
            return out;
        }();
        return set;
    }

    // The .text bounds, so a resolved address can be sanity-checked against
    // the segment it is supposed to live in and a scan cannot read past the
    // end of the image.
    std::pair<std::uintptr_t, std::uintptr_t> TextRange() {
        const auto seg = REL::Module::get().segment(REL::Segment::textx);
        return { seg.address(), seg.address() + seg.size() };
    }

    // Target of the `E8 rel32` at a_site. Caller has already checked the byte.
    std::uintptr_t CallTarget(std::uintptr_t a_site) {
        const auto rel = *reinterpret_cast<const std::int32_t*>(a_site + 1);
        return static_cast<std::uintptr_t>(a_site + 5 + rel);
    }

    struct Located {
        std::ptrdiff_t offset{ 0 };  // relative to a_start; 0 = not found
        const char*    how{ "absent" };
    };

    // TRUE on the two runtimes whose call-site offsets were measured by hand
    // against the real binary. On those the hint is KNOWN correct, which is
    // what lets LocateCall fall back to it - see the chaining case there.
    bool HintIsHandMeasured() {
        const auto v = REL::Module::get().version();
        return v == REL::Version(1, 5, 97, 0) || v >= REL::Version(1, 6, 1130, 0);
    }

    // Find the call to a_callee inside the function at a_start.
    //
    // The hand-measured offset is tried first and, when it holds, nothing is
    // scanned - so on the two verified builds this is exactly the old
    // behaviour plus one comparison. When it does not hold we scan, and
    // require a UNIQUE match on the call target. Matching the target rather
    // than just the E8 byte is what makes this safe: a stray 0xE8 that is
    // really some other instruction's operand would have to encode a
    // displacement landing exactly on the callee to fool it, and a second
    // genuine caller inside the window is caught by the uniqueness rule
    // instead of being silently picked.
    Located LocateCall(std::uintptr_t a_start, std::uintptr_t a_callee, std::ptrdiff_t a_hint) {
        if (a_start == 0 || a_callee == 0) {
            return {};
        }

        const auto [textLo, textHi] = TextRange();
        if (a_start < textLo || a_start >= textHi) {
            return {};
        }

        if (a_hint != 0 && a_start + static_cast<std::uintptr_t>(a_hint) + 5 < textHi) {
            const auto site = a_start + static_cast<std::uintptr_t>(a_hint);
            if (*reinterpret_cast<const std::uint8_t*>(site) == 0xE8 &&
                CallTarget(site) == a_callee) {
                return { a_hint, "hand-measured offset" };
            }
        }

        const auto*    code = reinterpret_cast<const std::uint8_t*>(a_start);
        const auto     room = static_cast<std::size_t>(textHi - a_start);
        const auto     span = (std::min)(kScanWindow, room > 5 ? room - 5 : 0);
        std::ptrdiff_t found = 0;
        int            hits = 0;
        std::size_t    pad = 0;

        for (std::size_t i = 0; i < span; ++i) {
            if (code[i] == 0xCC) {
                if (++pad >= kPadRun) {
                    break;  // walked off the end of the function
                }
                continue;
            }
            pad = 0;
            if (code[i] == 0xE8 && CallTarget(a_start + i) == a_callee) {
                found = static_cast<std::ptrdiff_t>(i);
                ++hits;
            }
        }

        if (hits == 1) {
            return { found, "located by scan" };
        }
        if (hits > 1) {
            spdlog::error("  call site: {} calls to the target inside the function - ambiguous, "
                          "refusing to guess.", hits);
            return {};
        }

        // ⚠ NO TARGET MATCH IS NOT THE SAME AS NO CALL SITE, AND TREATING IT
        // AS ONE WAS A REGRESSION AGAINST 0.7.1.
        //
        // When ANOTHER MOD has already hooked this site, its E8 points at that
        // mod's trampoline instead of the engine function, so every check
        // above fails and the scan finds nothing - the site is right there and
        // simply no longer names the callee. 0.7.1 byte-checked E8 and wrote,
        // which chains onto the other mod correctly and is the behaviour every
        // SKSE plugin relies on. Refusing here would mean Menu Studio declines
        // to load in exactly the load orders where it used to work.
        //
        // Caught in the field on the very first run, by Fitting Room's twin of
        // this function: "input block: NOT FOUND inside the containing
        // function" on a site the clean 1.6.1170 binary shows as a plain E8 to
        // the expected callee. Something else in that load order got there
        // first.
        //
        // Gated on the runtime, because that is what makes it safe: on 1.5.97
        // and 1.6.1130+ the hint was measured against the real binary, so an
        // E8 there IS the site. On the mid versions the hint is a guess, and a
        // guess that happens to land on an E8 is exactly how you write five
        // bytes into the middle of an unrelated instruction - so there we keep
        // requiring the target to match and decline if it does not.
        if (a_hint != 0 && HintIsHandMeasured() &&
            a_start + static_cast<std::uintptr_t>(a_hint) + 5 < textHi &&
            *reinterpret_cast<const std::uint8_t*>(a_start + a_hint) == 0xE8) {
            return { a_hint, "hand-measured offset, ALREADY HOOKED by another mod - chaining" };
        }
        return {};
    }

    enum class Kind {
        kCode,  // must live in .text; first bytes are worth dumping
        kData   // a global, not a function - no .text check
    };

    struct Entry {
        const char*               name;
        const REL::RelocationID&  id;
        Kind                      kind;
    };

    // Every engine address the plugin uses, in Offsets.h order. Anything added
    // to that table belongs here too - an address absent from this report is
    // an address nobody will check on the machine that actually has the
    // problem.
    const Entry kTable[] = {
        { "Main::Update",                  MTB::Offsets::MainUpdate,             Kind::kCode },
        { "Main::Update->playerDispatch",  MTB::Offsets::MainUpdatePlayerDispatch, Kind::kCode },
        { "g_slowDt",                      MTB::Offsets::SlowDt,                 Kind::kData },
        { "g_realDt",                      MTB::Offsets::RealDt,                 Kind::kData },
        { "g_dtVariant3",                  MTB::Offsets::DtVariant3,             Kind::kData },
        { "BSFaceGenAnimationData::Update", MTB::Offsets::FaceGenUpdate,         Kind::kCode },
        { "FaceGenApplyMorphs",            MTB::Offsets::FaceGenApplyMorphs,     Kind::kCode },
        { "3rd-person position builder",   MTB::Offsets::CameraPositionBuilder,  Kind::kCode },
        { "camera collision smoother",     MTB::Offsets::CameraCollisionSmoother, Kind::kCode },
        { "NiPointLight create",           MTB::Offsets::NiPointLightCreate,     Kind::kCode },
        { "ShadowSceneNode::AddLight",     MTB::Offsets::ShadowSceneAddLight,    Kind::kCode },
        { "ShadowSceneNode::RemoveLight",  MTB::Offsets::ShadowSceneRemoveLight, Kind::kCode },
        { "Sky interior-light refresh",    MTB::Offsets::SkyForceInteriorRefresh, Kind::kCode },
        { "TESWaterSystem singleton",      MTB::Offsets::TESWaterSystemSingleton, Kind::kData },
        { "BSModelDB::Demand",             MTB::Offsets::BSModelDBDemand,        Kind::kCode },
        { "NiAVObject::Clone",             MTB::Offsets::NiAVObjectClone,        Kind::kCode },
        { "FadeOutGame",                   MTB::Offsets::FadeOutGame,            Kind::kCode },
        { "UI::SetMenuPausesGame",         MTB::Offsets::SetMenuPausesGame,      Kind::kCode },
    };

    std::string HexBytes(std::uintptr_t a_addr, std::size_t a_count) {
        const auto [textLo, textHi] = TextRange();
        std::string out;
        for (std::size_t i = 0; i < a_count; ++i) {
            const auto at = a_addr + i;
            if (at < textLo || at >= textHi) {
                break;  // only .text is guaranteed readable here
            }
            out += fmt::format("{:02X} ", *reinterpret_cast<const std::uint8_t*>(at));
        }
        return out.empty() ? "??" : out;
    }

    // One report line per address. Returns whether it looks usable.
    bool ReportEntry(const Entry& a_entry) {
        const auto id = a_entry.id.id();
        if (id == 0) {
            // Deliberate: the AE half of CameraCollisionSmoother is 0 because
            // AE inlined the function. Not a failure.
            spdlog::info("  {:<32} n/a on this runtime.", a_entry.name);
            return false;
        }
        if (!MTB::VersionCheck::IdOk(id)) {
            spdlog::error("  {:<32} id {} ABSENT from this build's Address Library - the feature "
                          "using it is OFF (resolving it anyway would hand back a neighbouring "
                          "function).", a_entry.name, id);
            return false;
        }

        const auto addr = a_entry.id.address();
        const auto off  = a_entry.id.offset();
        if (a_entry.kind == Kind::kCode) {
            const auto [textLo, textHi] = TextRange();
            if (addr < textLo || addr >= textHi) {
                spdlog::error("  {:<32} id {} resolved to 0x{:X}, which is OUTSIDE .text.",
                              a_entry.name, id, off);
                return false;
            }
            spdlog::info("  {:<32} id {:<6} rva 0x{:06X}  [{}]", a_entry.name, id, off,
                         HexBytes(addr, 8));
        } else {
            spdlog::info("  {:<32} id {:<6} rva 0x{:06X}", a_entry.name, id, off);
        }
        return true;
    }
}

namespace MTB::VersionCheck {
    bool IdOk(std::uint64_t a_id) {
        return a_id != 0 && IdSet().contains(a_id);
    }

    bool IdOk(const REL::RelocationID& a_id) {
        return IdOk(a_id.id());
    }

    bool CriticalOk() {
        return g_criticalOk;
    }

    std::ptrdiff_t DispatchCallOffset() {
        return g_dispatchCall;
    }

    std::ptrdiff_t SmootherCallOffset() {
        return g_smootherCall;
    }

    void Run() {
        if (g_ran) {
            return;
        }
        g_ran = true;

        const auto ver = REL::Module::get().version();
        spdlog::info("--- address self-check, runtime {} ---", ver.string());

        // Do this BEFORE anything else touches the database: if the user has
        // no Address Library for their build, CommonLib fails hard here, and
        // the log should show which step died.
        spdlog::info("  reading Address Library...");
        const auto ids = IdSet().size();
        spdlog::info("  Address Library: {} ids.", ids);

        for (const auto& entry : kTable) {
            ReportEntry(entry);
        }

        // The frame driver. Everything Menu Studio does hangs off this one
        // call, so it is the only address whose failure means "do not load".
        //
        // ⚠ address() is only called on ids that passed IdOk. CommonLib's
        // id2offset report_and_fail()s - a hard message box, not an exception -
        // when an id is past the end of the database, and a self-check that
        // can kill the process is not a self-check.
        const auto dispatch =
            (IdOk(Offsets::MainUpdate) && IdOk(Offsets::MainUpdatePlayerDispatch))
                ? LocateCall(Offsets::MainUpdate.address(),
                             Offsets::MainUpdatePlayerDispatch.address(),
                             Offsets::DispatchCallOffsetHint())
                : Located{};
        g_dispatchCall = dispatch.offset;
        if (g_dispatchCall != 0) {
            spdlog::info("  frame driver: Main::Update+0x{:X} ({}).", g_dispatchCall, dispatch.how);
        } else {
            spdlog::error("  frame driver: NO call to the player dispatch found inside "
                          "Main::Update. Menu Studio cannot work on this runtime.");
        }
        g_criticalOk = g_dispatchCall != 0;

        // The camera-collision bypass. Optional: AE inlined the smoother, so
        // absence here is the documented AE limitation, not an error.
        const auto smoother =
            (IdOk(Offsets::CameraPositionBuilder) && IdOk(Offsets::CameraCollisionSmoother))
                ? LocateCall(Offsets::CameraPositionBuilder.address(),
                             Offsets::CameraCollisionSmoother.address(),
                             Offsets::SmootherCallOffsetHint())
                : Located{};
        g_smootherCall = smoother.offset;
        if (g_smootherCall != 0) {
            spdlog::info("  camera collision: position-builder+0x{:X} ({}).", g_smootherCall,
                         smoother.how);
        } else {
            spdlog::info("  camera collision: no standalone smoother call on this runtime "
                         "(AE inlines it); bypass unavailable, everything else works.");
        }

        spdlog::info("--- self-check {} ---", g_criticalOk ? "PASSED" : "FAILED");
    }
}
