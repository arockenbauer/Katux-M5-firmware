#include "core/kernel.h"

namespace katux {

void setupMain() {
    katux::core::kernel().begin();
}

void loopMain() {
    katux::core::kernel().update();
}

}  // namespace katux
