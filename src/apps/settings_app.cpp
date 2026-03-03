#include "settings_app.h"

namespace katux::apps {

void SettingsApp::onStart() {
    selected_ = 0;
}

void SettingsApp::onEvent(const katux::core::Event& e) {
    if (e.type == katux::core::EventType::ButtonClick && e.source == katux::core::EventSource::ButtonB) {
        selected_ = static_cast<uint8_t>((selected_ + 1U) % 3U);
    }
}

void SettingsApp::render() {
    if (!renderer_) return;
    renderer_->fillRect({36, 30, 130, 78}, TFT_DARKGREY);
    renderer_->drawRect({36, 30, 130, 78}, TFT_WHITE);
    renderer_->drawText(42, 36, "Settings", TFT_WHITE, TFT_DARKGREY);

    const char* options[3] = {"Theme", "Brightness", "Back"};
    for (uint8_t i = 0; i < 3; ++i) {
        const uint16_t bg = (i == selected_) ? TFT_WHITE : TFT_DARKGREY;
        const uint16_t fg = (i == selected_) ? TFT_BLACK : TFT_WHITE;
        renderer_->fillRect({44, static_cast<int16_t>(54 + i * 16), 110, 14}, bg);
        renderer_->drawText(48, static_cast<int16_t>(56 + i * 16), options[i], fg, bg);
    }
}

void SettingsApp::onClose() {}

void SettingsApp::bindRenderer(katux::graphics::Renderer* renderer) {
    renderer_ = renderer;
}

}