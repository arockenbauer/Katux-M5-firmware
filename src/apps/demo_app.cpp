#include "demo_app.h"

namespace katux::apps {

void DemoApp::onStart() {
    color_ = TFT_GREEN;
}

void DemoApp::onEvent(const katux::core::Event& e) {
    if (e.type == katux::core::EventType::ButtonClick && e.source == katux::core::EventSource::ButtonA) {
        color_ = (color_ == TFT_GREEN) ? TFT_ORANGE : TFT_GREEN;
    }
}

void DemoApp::render() {
    if (!renderer_) return;
    renderer_->fillRect({160, 30, 70, 70}, color_);
    renderer_->drawRect({160, 30, 70, 70}, TFT_WHITE);
    renderer_->drawText(168, 58, "Demo", TFT_BLACK, color_);
}

void DemoApp::onClose() {}

void DemoApp::bindRenderer(katux::graphics::Renderer* renderer) {
    renderer_ = renderer;
}

}