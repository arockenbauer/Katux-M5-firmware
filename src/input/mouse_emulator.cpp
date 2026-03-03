#include <stdint.h>

namespace katux::input {

class MouseEmulator {
   public:
    int16_t x = 67;
    int16_t y = 120;

    void step(int8_t dx, int8_t dy) {
        x += dx;
        y += dy;
    }
};

}  // namespace katux::input
