#pragma once

#include <stdint.h>

#include "renderer.h"

namespace katux::graphics {

class Compositor {
   public:
    static constexpr uint8_t kMaxDirtyRects = 40;

    void begin(int16_t width, int16_t height);
    void invalidateAll();
    void invalidate(const Rect& rect);
    void invalidatePair(const Rect& before, const Rect& after);
    uint8_t consume(Rect* out, uint8_t maxItems);
    bool hasDirty() const;

   private:
    Rect dirty_[kMaxDirtyRects]{};
    uint8_t dirtyCount_ = 0;
    int16_t width_ = 240;
    int16_t height_ = 135;

    bool normalize(Rect& r) const;
};

}
