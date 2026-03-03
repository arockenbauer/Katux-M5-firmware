#include <Arduino.h>

namespace katux::core {

class Scheduler {
   public:
    static uint32_t now() {
        return millis();
    }
};

}