#include "bsod.h"

#include <string.h>

namespace katux::system {

void Bsod::trigger(const char* code) {
    active_ = true;
    if (code && code[0] != '\0') {
        strlcpy(code_, code, sizeof(code_));
    }
}

bool Bsod::active() const {
    return active_;
}

void Bsod::clear() {
    active_ = false;
    code_[0] = '\0';
}

void Bsod::render(katux::graphics::Renderer& renderer) {
    renderer.clear(TFT_BLUE);
    renderer.drawText(6, 8, "KATUX STOP", TFT_WHITE, TFT_BLUE);
    renderer.drawText(6, 28, code_, TFT_WHITE, TFT_BLUE);
    renderer.drawText(6, 54, "A: reboot", TFT_WHITE, TFT_BLUE);
    renderer.drawText(6, 70, "B: diagnostics", TFT_WHITE, TFT_BLUE);
}

}