#pragma once

#include <stdint.h>

namespace katux::graphics {

class Cursor {
   public:
    void begin(int16_t width, int16_t height);
    void move(int16_t dx, int16_t dy, bool fast);
    void setPosition(int16_t x, int16_t y);
    int16_t x() const;
    int16_t y() const;

   private:
    int16_t x_ = 0;
    int16_t y_ = 0;
    int16_t maxX_ = 0;
    int16_t maxY_ = 0;
    void clamp();
};

}