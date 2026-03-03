#include "boot.h"

#include <Arduino.h>

#include "../graphics/theme.h"

namespace katux::system {

void BootManager::begin() {
    progress_ = 0;
    targetProgress_ = 0;
    spinnerFrame_ = 0;
    lastAnimMs_ = millis();
    backdropDrawn_ = false;
    lastRenderedProgress_ = 255;
    lastRenderedSpinner_ = 255;
    lastRenderedStage_ = 255;
}

void BootManager::setTargetProgress(uint8_t progress) {
    targetProgress_ = (progress > 100U) ? 100U : progress;
}

bool BootManager::update() {
    if (progress_ < targetProgress_) {
        ++progress_;
    }

    const uint32_t now = millis();
    if (now - lastAnimMs_ >= 70U) {
        spinnerFrame_ = static_cast<uint8_t>((spinnerFrame_ + 1U) % 8U);
        lastAnimMs_ = now;
    }

    return progress_ >= 100U;
}

void BootManager::render(katux::graphics::Renderer& renderer) {
    const auto& t = katux::graphics::theme();

    if (!backdropDrawn_) {
        renderer.clear(0x0000);
        renderer.drawText(70, 22, "KATUX", 0xFFFF, 0x0000);
        renderer.drawRect({44, 110, 152, 8}, 0x6B4D);
        backdropDrawn_ = true;
    }

    const int16_t cx = 120;
    const int16_t cy = 72;
    const int8_t dx[8] = {0, 5, 8, 5, 0, -5, -8, -5};
    const int8_t dy[8] = {-8, -5, 0, 5, 8, 5, 0, -5};

    if (lastRenderedSpinner_ != spinnerFrame_) {
        renderer.fillRect({110, 62, 20, 20}, 0x0000);
        for (uint8_t i = 0; i < 8; ++i) {
            const uint8_t phase = static_cast<uint8_t>((i + 8U - spinnerFrame_) % 8U);
            uint16_t c = 0x39E7;
            if (phase == 0) c = 0xFFFF;
            else if (phase == 1) c = 0xC618;
            else if (phase == 2) c = 0x8C71;
            renderer.fillRect({static_cast<int16_t>(cx + dx[i]), static_cast<int16_t>(cy + dy[i]), 3, 3}, c);
        }
        lastRenderedSpinner_ = spinnerFrame_;
    }

    uint8_t stage = 0;
    const char* msg = "Please wait...";
    if (progress_ > 82) {
        stage = 3;
        msg = "Starting desktop...";
    } else if (progress_ > 58) {
        stage = 2;
        msg = "Loading services...";
    } else if (progress_ > 26) {
        stage = 1;
        msg = "Initializing kernel...";
    }

    if (lastRenderedStage_ != stage) {
        renderer.fillRect({56, 94, 130, 10}, 0x0000);
        renderer.drawText(72, 96, msg, t.text, 0x0000);
        lastRenderedStage_ = stage;
    }

    if (lastRenderedProgress_ != progress_) {
        renderer.fillRect({46, 112, 148, 4}, 0x0000);
        renderer.fillRect({46, 112, static_cast<int16_t>((progress_ * 148) / 100), 4}, 0x39E7);
        lastRenderedProgress_ = progress_;
    }
}

uint8_t BootManager::progress() const {
    return progress_;
}

}