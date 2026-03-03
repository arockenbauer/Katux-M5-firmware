#include "renderer.h"

namespace katux::graphics {

void Renderer::begin() {
    dirtyCount_ = 0;
}

void Renderer::clear(uint16_t color) {
    StickCP2.Display.fillRect(0, 0, width(), height(), color);
    dirtyCount_ = 0;
}

void Renderer::fillRect(const Rect& r, uint16_t color) {
    Rect c = r;
    if (!normalize(c)) return;
    StickCP2.Display.fillRect(c.x, c.y, c.w, c.h, color);
}

void Renderer::drawRect(const Rect& r, uint16_t color) {
    Rect c = r;
    if (!normalize(c)) return;
    StickCP2.Display.drawRect(c.x, c.y, c.w, c.h, color);
}

void Renderer::drawText(int16_t x, int16_t y, const char* text, uint16_t fg, uint16_t bg) {
    StickCP2.Display.setTextColor(fg, bg);
    StickCP2.Display.setCursor(x, y);
    StickCP2.Display.print(text);
}

void Renderer::drawCursor(int16_t x, int16_t y, CursorStyle style) {
    uint16_t fill = 0x0000;
    const uint16_t outline = 0xFFFF;
    if (style == CursorStyle::Busy) {
        fill = 0x4208;
    } else if (style == CursorStyle::Hover) {
        fill = 0x1082;
    }

    StickCP2.Display.fillTriangle(x, y, x, y + 11, x + 8, y + 5, fill);
    StickCP2.Display.drawLine(x, y, x, y + 11, outline);
    StickCP2.Display.drawLine(x, y, x + 8, y + 5, outline);
    StickCP2.Display.drawLine(x, y + 11, x + 8, y + 5, outline);

    if (style == CursorStyle::Click) {
        StickCP2.Display.fillRect(x + 6, y + 7, 4, 4, 0xF800);
        StickCP2.Display.drawRect(x + 6, y + 7, 4, 4, outline);
    } else if (style == CursorStyle::Drag) {
        StickCP2.Display.fillRect(x + 6, y + 6, 6, 3, 0xFFE0);
        StickCP2.Display.drawRect(x + 6, y + 6, 6, 3, outline);
    } else if (style == CursorStyle::Input) {
        StickCP2.Display.drawLine(x + 6, y + 2, x + 6, y + 12, 0x07E0);
        StickCP2.Display.drawLine(x + 7, y + 2, x + 7, y + 12, outline);
    } else if (style == CursorStyle::Hover) {
        StickCP2.Display.fillRect(x + 6, y + 6, 4, 2, 0x07FF);
        StickCP2.Display.drawRect(x + 6, y + 6, 4, 2, outline);
    } else if (style == CursorStyle::Busy) {
        const uint16_t spin = (millis() / 120U) % 2U == 0 ? 0xFFE0 : 0xFD20;
        StickCP2.Display.drawRect(x + 6, y + 5, 4, 4, spin);
        StickCP2.Display.fillRect(x + 7, y + 6, 2, 2, spin);
    }
}
void Renderer::markDirty(const Rect& area) {
    Rect c = area;
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

    if (dirtyCount_ >= kDirtyCapacity) {
        dirty_[0] = {0, 0, width(), height()};
        dirtyCount_ = 1;
        return;
    }

    dirty_[dirtyCount_++] = c;
}

uint8_t Renderer::consumeDirty(Rect* out, uint8_t maxItems) {
    if (!out || maxItems == 0 || dirtyCount_ == 0) return 0;
    const uint8_t n = dirtyCount_ < maxItems ? dirtyCount_ : maxItems;
    for (uint8_t i = 0; i < n; ++i) {
        out[i] = dirty_[i];
    }
    dirtyCount_ = 0;
    return n;
}

bool Renderer::readPixels(const Rect& r, uint16_t* out) {
    if (!out) return false;
    Rect c = r;
    if (!normalize(c)) return false;
    StickCP2.Display.readRect(c.x, c.y, c.w, c.h, out);
    return true;
}

bool Renderer::pushPixels(const Rect& r, const uint16_t* pixels) {
    if (!pixels) return false;
    Rect c = r;
    if (!normalize(c)) return false;
    StickCP2.Display.pushImage(c.x, c.y, c.w, c.h, pixels);
    return true;
}

void Renderer::present() {
    dirtyCount_ = 0;
}

int16_t Renderer::width() const {
    return StickCP2.Display.width();
}

int16_t Renderer::height() const {
    return StickCP2.Display.height();
}

bool Renderer::normalize(Rect& r) const {
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    const int16_t w = width();
    const int16_t h = height();
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    return r.w > 0 && r.h > 0;
}

}