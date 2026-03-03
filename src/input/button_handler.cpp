#include <M5StickCPlus2.h>

namespace katux::input {

class ButtonHandler {
   public:
    void update() {
        StickCP2.update();
    }
};

}  // namespace katux::input
