#include "cursor.h"

namespace katux::graphics {

void Cursor::begin(int16_t width, int16_t height) {
    maxX_ = width > 0 ? static_cast<int16_t>(width - 1) : 0;
    maxY_ = height > 0 ? static_cast<int16_t>(height - 1) : 0;
    x_ = width / 2;
    y_ = height / 2;
    clamp();
}

void Cursor::move(int16_t dx, int16_t dy, bool fast) {
    const int16_t factor = fast ? 3 : 1;
    x_ += dx * factor;
    y_ += dy * factor;
    clamp();
}

void Cursor::setPosition(int16_t x, int16_t y) {
    x_ = x;
    y_ = y;
    clamp();
}

int16_t Cursor::x() const {
    return x_;
}

int16_t Cursor::y() const {
    return y_;
}

void Cursor::clamp() {
    if (x_ < 0) x_ = 0;
    if (y_ < 0) y_ = 0;
    if (x_ > maxX_) x_ = maxX_;
    if (y_ > maxY_) y_ = maxY_;
}

}