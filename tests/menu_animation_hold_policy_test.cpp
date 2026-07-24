#include "MenuAnimationHoldPolicy.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

int main() {
    using MTB::MenuAnimationHoldPolicy::ShouldHold;

    // Existing arm-edge and live-transition holds remain intact.
    CHECK(ShouldHold({
        .armEdgeHeld = true,
    }));
    CHECK(ShouldHold({
        .freezeDrawSheathe = true,
        .equipClipInFlight = true,
    }));

    // Regression: once a paused-menu equip has occurred, do not resume the
    // graph later in the same menu and let it select an idle against stale
    // framework state. Selection belongs to the first live gameplay frame.
    CHECK(ShouldHold({
        .freezeDrawSheathe = true,
        .equipOccurredThisSession = true,
    }));

    CHECK(!ShouldHold({
        .freezeDrawSheathe = false,
        .equipOccurredThisSession = true,
    }));

    CHECK(!ShouldHold({}));

    if (g_failures == 0) {
        std::printf("menu animation hold policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}
