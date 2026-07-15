#pragma once

#include "Settings.h"

// Colour FILTER (bColorFilter, off by default): an optional uniform colour grade
// over the whole menu scene, available in ANY view. Delivered through the
// ImageSpaceManager base override, NOT by editing imagespace forms - a form edit
// does nothing while a menu is paused (the manager caches the base each render
// and only re-reads forms on a weather/cell change, which never runs paused), but
// the manager's base POINTER is consulted on every rendered frame, so pointing it
// at our own tinted buffer paints the whole screen live. It grades everything in
// frame, the character INCLUDED - a true world-not-character filter needs a
// stencil / two-layer compositor pass the plugin can't do (deferred). Guarded so
// Restore is a safe no-op when inactive.
namespace MTB::SceneTint {
    // Install the tint: save the manager's real base pointer once, then repoint
    // it at our tinted buffer. Re-applying only rewrites; it does not re-save.
    void Apply(const Settings::TintValues& a_tint);

    // Rewrite the tint from the given values (live slider drags). No-op when not
    // applied.
    void LiveRefresh();

    // Restore the manager's own base pointer. Idempotent, guarded, safe on any
    // exit path.
    void Restore();

    // One entry point for the arm / live-settings paths: enabled -> apply (or
    // refresh if already on), disabled -> restore. Keeps the filter's on/off in
    // one place so a mid-menu toggle just works.
    void Sync(bool a_enabled, const Settings::TintValues& a_tint);
}
