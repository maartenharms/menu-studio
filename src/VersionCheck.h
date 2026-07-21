#pragma once

// Startup address self-check - the whole diagnosis for a runtime we do not
// have. See VersionCheck.cpp for why every part of it exists.
namespace MTB::VersionCheck {
    // Run once from SKSEPluginLoad, BEFORE any hook installs. Logs one line
    // per engine address and decides whether the critical set is usable.
    void Run();

    // TRUE when the frame driver's call site was located. Everything Menu
    // Studio does hangs off it, so a false here means "do not load".
    bool CriticalOk();

    // Exact-membership test against the running build's Address Library.
    // ⚠ NOT the same question as "REL::Relocation gave me an address".
    // CommonLib's id2offset does a lower_bound and, on SE/AE, only fails when
    // the id is past the END of the database - an id that is simply ABSENT
    // resolves to its NEIGHBOUR's offset with no error at all. That silent
    // wrong answer is the exact failure mode that makes a widened version gate
    // dangerous, so every address we CALL is checked through here first.
    bool IdOk(std::uint64_t a_id);
    bool IdOk(const REL::RelocationID& a_id);

    // Located call sites, relative to their containing function. 0 = absent,
    // which callers must treat as "do not install this hook".
    std::ptrdiff_t DispatchCallOffset();
    std::ptrdiff_t SmootherCallOffset();
}
