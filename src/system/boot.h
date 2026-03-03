#pragma once

#include <stdint.h>

#include "../graphics/renderer.h"

namespace katux::system {

class BootManager {
   public:
    void begin();
    void setTargetProgress(uint8_t progress);
    bool update();
    void render(katux::graphics::Renderer& renderer);
    uint8_t progress() const;

   private:
    uint8_t progress_ = 0;
    uint8_t targetProgress_ = 0;
    uint8_t spinnerFrame_ = 0;
    uint32_t lastAnimMs_ = 0;
    bool backdropDrawn_ = false;
    uint8_t lastRenderedProgress_ = 255;
    uint8_t lastRenderedSpinner_ = 255;
    uint8_t lastRenderedStage_ = 255;
};

}