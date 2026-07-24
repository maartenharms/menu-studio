#include "BackdropAnchorPolicy.h"

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
    using namespace MTB::BackdropAnchorPolicy;

    // The transparent sphere must remain player-centred for stable depth
    // sorting, but it needs a few post-arm refits after the scene parent settles.
    CHECK(Choose(true) == Anchor::kPlayer);
    CHECK(ShouldRefitAfterArm(true, 4));
    CHECK(ShouldRefitAfterArm(true, 1));
    CHECK(!ShouldRefitAfterArm(true, 0));

    // Floors, stages and decorative pieces belong around the actor's feet.
    CHECK(Choose(false) == Anchor::kPlayer);
    CHECK(!ShouldRefitAfterArm(false, 4));

    if (g_failures == 0) {
        std::printf("all BackdropAnchorPolicy tests passed\n");
    }
    return g_failures ? 1 : 0;
}
