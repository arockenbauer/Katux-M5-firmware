#pragma once

#include <M5StickCPlus2.h>
#include <stdint.h>

namespace katux::graphics {

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

enum class CursorStyle : uint8_t {
    Idle = 0,
    Hover,
    Click,
    Drag,
    Input,
    Busy
};

class Renderer {
   public:
    static constexpr uint8_t kDirtyCapacity = 24;

    void begin();
    void clear(uint16_t color);
    void fillRect(const Rect& r, uint16_t color);
    void drawRect(const Rect& r, uint16_t color);
    void drawText(int16_t x, int16_t y, const char* text, uint16_t fg, uint16_t bg);
    void drawCursor(int16_t x, int16_t y, CursorStyle style = CursorStyle::Idle);
    void markDirty(const Rect& area);
    uint8_t consumeDirty(Rect* out, uint8_t maxItems);
    bool normalize(Rect& r) const;
    bool readPixels(const Rect& r, uint16_t* out);
    bool pushPixels(const Rect& r, const uint16_t* pixels);
    void present();
    int16_t width() const;
    int16_t height() const;

   private:
    Rect dirty_[kDirtyCapacity]{};
    uint8_t dirtyCount_ = 0;
};

}