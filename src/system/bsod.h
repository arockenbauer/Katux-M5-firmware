#pragma once

#include <stdint.h>

#include "../graphics/renderer.h"

namespace katux::system {

class Bsod {
   public:
    void trigger(const char* code);
    bool active() const;
    void clear();
    void render(katux::graphics::Renderer& renderer);

   private:
    bool active_ = false;
    char code_[28] = "UNSET";
};

}