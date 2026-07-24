#include "MovementArmPolicy.h"

#include <cstdio>
#include <iostream>

static int g_failures = 0;
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

int main() {
    using MTB::MovementArmPolicy::Choose;
    using MTB::MovementArmPolicy::Input;

    // Rapid direction changes can leave any combination of the four movement
    // controls active at the menu edge. None may select the old synthetic
    // stop/settle/restart transaction.
    for (unsigned directions = 0; directions < 16; ++directions) {
        const auto plan = Choose(Input{
            .locomoting = true,
            .grounded = true,
            .swimming = false,
            .directionMask = directions,
        });
        CHECK(plan.freezeCaughtPose);
        CHECK(!plan.runSyntheticSettle);
        CHECK(!plan.createExitRestartDebt);
        // Preserving the movement graph also requires preserving the actor's
        // direction inputs. Clearing them directly while the menu is paused
        // consumes no normal input-release edge, so the graph can remain in
        // locomotion after close and the next weapon idle loops its start clip.
        CHECK(plan.preserveDirectionBits);
    }

    // A standing character remains live so breathing/idles can keep playing.
    const auto standing = Choose(Input{
        .locomoting = false,
        .grounded = true,
        .swimming = false,
        .directionMask = 0,
    });
    CHECK(!standing.freezeCaughtPose);
    CHECK(!standing.runSyntheticSettle);
    CHECK(!standing.createExitRestartDebt);
    CHECK(!standing.preserveDirectionBits);

    if (g_failures == 0) {
        std::cout << "movement arm policy tests passed\n";
    }
    return g_failures ? 1 : 0;
}
