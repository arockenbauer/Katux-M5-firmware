#include "bios.h"

#include <stdio.h>

namespace katux::system {

void Bios::begin() {
    selected_ = 0;
    exitRequested_ = false;
    resetRequested_ = false;
    rebootRequested_ = false;
    factoryResetRequested_ = false;
    factoryResetWipeStorage_ = false;
}

void Bios::onEvent(const katux::core::Event& event) {
    if (event.type == katux::core::EventType::ButtonUp && event.source == katux::core::EventSource::ButtonB) {
        selected_ = static_cast<uint8_t>((selected_ + 1U) % 4U);
        return;
    }

    if (event.type == katux::core::EventType::ButtonClick && event.source == katux::core::EventSource::ButtonA) {
        if (rescueMode_) {
            if (selected_ == 0) {
                rebootRequested_ = true;
            } else if (selected_ == 1) {
                factoryResetRequested_ = true;
                factoryResetWipeStorage_ = false;
            } else if (selected_ == 2) {
                factoryResetRequested_ = true;
                factoryResetWipeStorage_ = true;
            } else if (selected_ == 3) {
                exitRequested_ = true;
            }
            return;
        }

        if (selected_ == 0) {
            darkTheme_ = !darkTheme_;
        } else if (selected_ == 1) {
            debugOverlay_ = !debugOverlay_;
        } else if (selected_ == 2) {
            resetRequested_ = true;
        } else if (selected_ == 3) {
            exitRequested_ = true;
        }
    }
}

void Bios::render(katux::graphics::Renderer& renderer) {
    if (rescueMode_) {
        const uint16_t rescueBg = 0x7800;
        renderer.clear(rescueBg);
        renderer.drawText(8, 8, "KATUX RESCUE", TFT_WHITE, rescueBg);

        const char* rows[4] = {"Reboot", "Reset params", "Wipe + reset", "Exit rescue"};
        for (uint8_t i = 0; i < 4; ++i) {
            const uint16_t bg = (i == selected_) ? TFT_WHITE : rescueBg;
            const uint16_t fg = (i == selected_) ? rescueBg : TFT_WHITE;
            renderer.fillRect({12, static_cast<int16_t>(30 + i * 20), 160, 16}, bg);
            renderer.drawText(16, static_cast<int16_t>(34 + i * 20), rows[i], fg, bg);
        }

        renderer.drawText(8, 114, "BtnA run / BtnB next", TFT_YELLOW, rescueBg);
        return;
    }

    renderer.clear(TFT_BLUE);
    renderer.drawText(8, 8, "KATUX BIOS", TFT_WHITE, TFT_BLUE);

    const char* rows[4] = {"Theme", "Debug", "Reset cfg", "Exit"};
    for (uint8_t i = 0; i < 4; ++i) {
        const uint16_t bg = (i == selected_) ? TFT_WHITE : TFT_BLUE;
        const uint16_t fg = (i == selected_) ? TFT_BLUE : TFT_WHITE;
        renderer.fillRect({12, static_cast<int16_t>(30 + i * 20), 126, 16}, bg);
        renderer.drawText(16, static_cast<int16_t>(34 + i * 20), rows[i], fg, bg);
    }

    renderer.drawText(150, 34, darkTheme_ ? "Dark" : "Light", TFT_YELLOW, TFT_BLUE);
    renderer.drawText(150, 54, debugOverlay_ ? "ON" : "OFF", TFT_YELLOW, TFT_BLUE);

    char line[26] = "";
    snprintf(line, sizeof(line), "FPS:%u Q:%u", fps_, queueDepth_);
    renderer.drawText(8, 114, line, TFT_CYAN, TFT_BLUE);

    snprintf(line, sizeof(line), "Heap:%luk", static_cast<unsigned long>(freeHeap_ / 1024UL));
    renderer.drawText(88, 114, line, TFT_CYAN, TFT_BLUE);

    renderer.drawText(176, 114, fsReady_ ? "FS" : "NOFS", fsReady_ ? TFT_GREEN : TFT_RED, TFT_BLUE);
    if (safeMode_) {
        renderer.drawText(204, 114, "SAFE", TFT_ORANGE, TFT_BLUE);
    }
}

bool Bios::shouldExit() const {
    return exitRequested_;
}

bool Bios::darkThemeEnabled() const {
    return darkTheme_;
}

void Bios::setDarkTheme(bool enabled) {
    darkTheme_ = enabled;
}

bool Bios::debugOverlayEnabled() const {
    return debugOverlay_;
}

void Bios::setDebugOverlay(bool enabled) {
    debugOverlay_ = enabled;
}

bool Bios::takeResetRequest() {
    const bool pending = resetRequested_;
    resetRequested_ = false;
    return pending;
}

bool Bios::takeRebootRequest() {
    const bool pending = rebootRequested_;
    rebootRequested_ = false;
    return pending;
}

bool Bios::takeFactoryResetRequest(bool& wipeStorage) {
    const bool pending = factoryResetRequested_;
    wipeStorage = factoryResetWipeStorage_;
    factoryResetRequested_ = false;
    factoryResetWipeStorage_ = false;
    return pending;
}

void Bios::setRescueMode(bool enabled) {
    rescueMode_ = enabled;
    selected_ = 0;
    exitRequested_ = false;
}

bool Bios::rescueMode() const {
    return rescueMode_;
}

void Bios::setDebugStats(uint8_t fps, uint8_t queueDepth, uint32_t freeHeap, bool fsReady, bool safeMode) {
    fps_ = fps;
    queueDepth_ = queueDepth;
    freeHeap_ = freeHeap;
    fsReady_ = fsReady;
    safeMode_ = safeMode;
}

}