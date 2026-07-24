#include "CameraCollisionPolicy.h"

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
    using namespace MTB::CameraCollisionPolicy;

    // Only an enabled, active Menu Studio bubble may bypass the obstruction
    // result. Ordinary gameplay and disabled configurations remain vanilla.
    CHECK(Decide({ .enabled = true, .bubbleActive = true }) == Action::kBypass);
    CHECK(Decide({ .enabled = true, .bubbleActive = false }) == Action::kRunOriginal);
    CHECK(Decide({ .enabled = false, .bubbleActive = true }) == Action::kRunOriginal);
    CHECK(Decide({ .enabled = false, .bubbleActive = false }) == Action::kRunOriginal);

    if (g_failures == 0) {
        std::printf("all CameraCollisionPolicy tests passed\n");
    }
    return g_failures ? 1 : 0;
}
