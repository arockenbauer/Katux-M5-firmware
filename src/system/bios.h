#pragma once

#include <stdint.h>

#include "../graphics/renderer.h"
#include "../core/event_manager.h"

namespace katux::system {

class Bios {
   public:
    void begin();
    void onEvent(const katux::core::Event& event);
    void render(katux::graphics::Renderer& renderer);

    bool shouldExit() const;
    bool darkThemeEnabled() const;
    void setDarkTheme(bool enabled);

    bool debugOverlayEnabled() const;
    void setDebugOverlay(bool enabled);

    bool takeResetRequest();

    void setDebugStats(uint8_t fps, uint8_t queueDepth, uint32_t freeHeap, bool fsReady, bool safeMode);

   private:
    uint8_t selected_ = 0;
    bool exitRequested_ = false;
    bool darkTheme_ = true;
    bool debugOverlay_ = false;
    bool resetRequested_ = false;

    uint8_t fps_ = 0;
    uint8_t queueDepth_ = 0;
    uint32_t freeHeap_ = 0;
    bool fsReady_ = false;
    bool safeMode_ = false;
};

}