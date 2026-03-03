#include "src/core/kernel.h"

void setup() {
    katux::core::kernel().begin();
}

void loop() {
    katux::core::kernel().update();
}
