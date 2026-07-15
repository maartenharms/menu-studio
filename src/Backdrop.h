#pragma once

namespace MTB::Backdrop {
    void Apply();
    void Tick();
    void PushFade();  // F-12: fade-only refresh for the teardown grace window
    void Remove();

    // Preheat the configured meshes into BSModelDB so the first arm's Apply
    // pays no cold demand-load - the shell then reaches the screen a frame
    // sooner, closing the "blue void" gap (the world is culled before the
    // shell draws). Cheap + idempotent (a config-signature guard skips repeats).
    void Warm();

    // PERSISTENT GAP OCCLUDER - the definitive "blue void" fix (RE Option P).
    // A freshly-attached shell can't occlude until the NEXT frame (one-frame
    // publish latency of new geometry into the batch renderer), so on open the
    // world is culled a frame before the shell draws → ENB blue / real world
    // shows. But un-culling an ALREADY-RESIDENT node is render-side and
    // immediate (proven: the mod's own world-feeder culls). So keep a plain
    // opaque sphere resident (AppCulled while idle) and just UN-CULL it in the
    // menu-open event, paired with the world cull - it blocks the gap the SAME
    // frame. It sits just OUTSIDE the real shell so the real (tinted) shell wins
    // once it draws; between menus it is AppCulled (invisible, cast-shadows-off).
    // OccluderShow returns true when it un-culled a WARM resident node (gap-free);
    // false when it had to (re)build cold this open (first open / cell change) -
    // the caller then keeps the no-fader interim (defer the cull) for that open.
    bool OccluderShow();
    void OccluderHide();  // AppCull, keep resident + warm (menu close / mode<2)
    void OccluderDrop();  // detach + release (load / disable - parent dies)
}
