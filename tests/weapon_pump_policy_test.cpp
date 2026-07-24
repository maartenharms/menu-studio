#include "WeaponPumpPolicy.h"

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
    using MTB::WeaponPumpPolicy::Action;
    using MTB::WeaponPumpPolicy::Decide;
    using MTB::WeaponPumpPolicy::Input;

    // A blind pump cannot make progress: field tracing proved the equip clip
    // arrives on a later engine frame regardless. Even one blind step can
    // finish a single-play idle and re-pick the lunge under the paused VM.
    CHECK(Decide(Input{
              .bounded = true,
              .sawTransition = false,
              .transitionUnfinished = false,
              .idlePickChanged = false,
              .stepsTaken = 0,
              .blindStepLimit = 0,
          }) == Action::kStopNothingToDrive);

    // Never take the step that crosses the equip -> idle boundary. The final
    // step must happen after close, when Papyrus can update stance markers.
    CHECK(Decide(Input{
              .bounded = true,
              .sawTransition = true,
              .transitionUnfinished = true,
              .transitionRemainingSeconds = 1.0f / 30.0f,
              .stepSeconds = 1.0f / 30.0f,
          }) == Action::kHoldBeforeIdle);

    CHECK(Decide(Input{
              .bounded = true,
              .sawTransition = true,
              .transitionUnfinished = true,
              .transitionRemainingSeconds = 0.5f,
              .stepSeconds = 1.0f / 30.0f,
          }) == Action::kContinue);

    CHECK(Decide(Input{
              .bounded = true,
              .sawTransition = true,
              .transitionUnfinished = false,
          }) == Action::kStopTransitionSettled);

    // The old off mode remains an explicit diagnostic control.
    CHECK(Decide(Input{
              .bounded = false,
              .sawTransition = false,
              .stepsTaken = 99,
              .blindStepLimit = 0,
          }) == Action::kContinue);

    if (g_failures == 0) {
        std::printf("weapon pump policy tests passed\n");
    }
    return g_failures ? 1 : 0;
}
