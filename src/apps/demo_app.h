#pragma once

#include "app_base.h"
#include "../graphics/renderer.h"

namespace katux::apps {

class DemoApp : public AppBase {
   public:
    void onStart() override;
    void onEvent(const katux::core::Event& e) override;
    void render() override;
    void onClose() override;
    void bindRenderer(katux::graphics::Renderer* renderer);

   private:
    katux::graphics::Renderer* renderer_ = nullptr;
    uint16_t color_ = TFT_GREEN;
};

}