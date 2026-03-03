#include "compositor.h"

namespace katux::graphics {

void Compositor::begin(int16_t width, int16_t height) {
    width_ = width;
    height_ = height;
    dirtyCount_ = 0;
}

void Compositor::invalidateAll() {
    dirty_[0] = {0, 0, width_, height_};
    dirtyCount_ = 1;
}

void Compositor::invalidate(const Rect& rect) {
    Rect c = rect;
    if (!normalize(c)) return;

    for (uint8_t i = 0; i < dirtyCount_; ++i) {
        Rect& d = dirty_[i];
        const bool overlap = !(c.x + c.w < d.x || d.x + d.w < c.x || c.y + c.h < d.y || d.y + d.h < c.y);
        if (!overlap) continue;
        const int16_t nx = c.x < d.x ? c.x : d.x;
        const int16_t ny = c.y < d.y ? c.y : d.y;
        const int16_t rx = (c.x + c.w) > (d.x + d.w) ? (c.x + c.w) : (d.x + d.w);
        const int16_t by = (c.y + c.h) > (d.y + d.h) ? (c.y + c.h) : (d.y + d.h);
        d = {nx, ny, static_cast<int16_t>(rx - nx), static_cast<int16_t>(by - ny)};
        normalize(d);
        return;
    }

    if (dirtyCount_ >= kMaxDirtyRects) {
        invalidateAll();
        return;
    }

    dirty_[dirtyCount_++] = c;
}

void Compositor::invalidatePair(const Rect& before, const Rect& after) {
    invalidate(before);
    invalidate(after);
}

uint8_t Compositor::consume(Rect* out, uint8_t maxItems) {
    if (!out || maxItems == 0 || dirtyCount_ == 0) return 0;
    const uint8_t n = dirtyCount_ < maxItems ? dirtyCount_ : maxItems;
    for (uint8_t i = 0; i < n; ++i) {
        out[i] = dirty_[i];
    }
    dirtyCount_ = 0;
    return n;
}

bool Compositor::hasDirty() const {
    return dirtyCount_ > 0;
}

bool Compositor::normalize(Rect& r) const {
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    if (r.x >= width_ || r.y >= height_) return false;
    if (r.x + r.w > width_) r.w = width_ - r.x;
    if (r.y + r.h > height_) r.h = height_ - r.y;
    return r.w > 0 && r.h > 0;
}

}
