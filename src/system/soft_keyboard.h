#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../graphics/renderer.h"

namespace katux::system {

class SoftKeyboard {
   public:
    enum class Layout : uint8_t {
        Qwerty = 0,
        Azerty,
        Special,
        Numeric,
        Macros
    };

    void begin();
    void open(const char* initial = "", bool browserMode = false);
    void close();
    bool isOpen() const;

    const char* value() const;
    bool consumeAccepted();

    bool onClick(int16_t x, int16_t y, const katux::graphics::Rect& area);
    void render(katux::graphics::Renderer& renderer, const katux::graphics::Rect& area, bool darkTheme, int16_t cursorX, int16_t cursorY);

   private:
    int16_t hitKeyId(int16_t x, int16_t y, const katux::graphics::Rect& area) const;
    void setCursorFromClick(int16_t x, const katux::graphics::Rect& valueBox, uint16_t visibleStart, uint16_t maxVisible);
    void ensureCursorVisible(uint16_t maxVisible);
    void insertText(const char* text);
    bool open_ = false;
    bool accepted_ = false;
    bool uppercase_ = false;
    bool browserMode_ = false;
    Layout layout_ = Layout::Azerty;
    // text entry buffer; 256 characters is still plenty for most fields
    char value_[256] = "";
    uint16_t cursorPos_ = 0;
    uint16_t viewOffset_ = 0;
    uint8_t macroPage_ = 0;
    uint8_t specialPage_ = 0;
    int16_t pressedKeyId_ = -1;
    int16_t hoverKeyId_ = -1;
    uint32_t pressedUntilMs_ = 0;

    void append(char c);
    void backspace();
};

}
