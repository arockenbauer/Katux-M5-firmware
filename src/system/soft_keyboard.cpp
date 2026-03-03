#include "soft_keyboard.h"

#include <ctype.h>
#include <string.h>

namespace katux::system {

namespace {

static bool contains(const katux::graphics::Rect& r, int16_t x, int16_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static const char kRow1[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
static const char kRow1Shift[10] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
static const char kQwertyRow2[10] = {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'};
static const char kQwertyRow3[9] = {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'};
static const char kQwertyRow4[7] = {'z', 'x', 'c', 'v', 'b', 'n', 'm'};
static const char kAzertyRow2[10] = {'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'};
static const char kAzertyRow3[9] = {'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'};
static const char kAzertyRow4[7] = {'w', 'x', 'c', 'v', 'b', 'n', 'm'};
static const char* kSpecialChars[] = {
    "!", "@", "#", "$", "%", "^", "&", "*",
    "(", ")", "-", "_", "=", "+", "[", "]",
    "{", "}", "\\", "|", ";", ":", "'", "\"",
    ",", ".", "<", ">", "/", "?", "`", "~",
    "€", "£", "¥", "§", "°", "±", "×", "÷",
    "©", "®", "™", "µ", "¶", "•", "…", "¬"
};
static constexpr uint8_t kSpecialCount = sizeof(kSpecialChars) / sizeof(kSpecialChars[0]);

static const char kNumRows[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', '-'};
static const char* kMacros[] = {
    ".com", ".fr", ".net", ".org", ".io", ".co", ".tv", ".uk",
    ".de", ".ru", ".ch", ".se", ".jp", ".cn", ".au", ".ca",
    ".br", ".mx", ".es", ".it", ".nl", ".be", ".at", ".gr",
    ".pt", ".ie", ".nz", ".za", ".in", ".hk", ".sg", ".tw",
    ".dev", ".app", ".xyz", ".tech", ".me", ".ai", ".shop", ".blog",
    ".", "-", "_", "@", "#", "$", ":", "/", "?", "&", "=", "%", "~", "+", "!", "*"
};
static constexpr uint8_t kMacroCount = sizeof(kMacros) / sizeof(kMacros[0]);

static const char* modeLabel(SoftKeyboard::Layout layout) {
    if (layout == SoftKeyboard::Layout::Qwerty) return "QWE";
    if (layout == SoftKeyboard::Layout::Azerty) return "AZE";
    if (layout == SoftKeyboard::Layout::Special) return "SPC";
    if (layout == SoftKeyboard::Layout::Numeric) return "NUM";
    return "TLD";
}

}

void SoftKeyboard::begin() {
    open_ = false;
    accepted_ = false;
    uppercase_ = false;
    browserMode_ = false;
    layout_ = Layout::Azerty;
    value_[0] = '\0';
    cursorPos_ = 0;
    viewOffset_ = 0;
    macroPage_ = 0;
    specialPage_ = 0;
    pressedKeyId_ = -1;
    hoverKeyId_ = -1;
    pressedUntilMs_ = 0;
}

void SoftKeyboard::open(const char* initial, bool browserMode) {
    open_ = true;
    accepted_ = false;
    uppercase_ = false;
    browserMode_ = browserMode;
    layout_ = Layout::Azerty;
    macroPage_ = 0;
    specialPage_ = 0;
    pressedKeyId_ = -1;
    hoverKeyId_ = -1;
    pressedUntilMs_ = 0;
    if (initial) {
        strlcpy(value_, initial, sizeof(value_));
    } else {
        value_[0] = '\0';
    }
    cursorPos_ = static_cast<uint16_t>(strlen(value_));
    viewOffset_ = 0;
}

void SoftKeyboard::close() {
    open_ = false;
    browserMode_ = false;
    pressedKeyId_ = -1;
    hoverKeyId_ = -1;
    pressedUntilMs_ = 0;
}

bool SoftKeyboard::isOpen() const {
    return open_;
}

const char* SoftKeyboard::value() const {
    return value_;
}

bool SoftKeyboard::consumeAccepted() {
    const bool accepted = accepted_;
    accepted_ = false;
    return accepted;
}

void SoftKeyboard::append(char c) {
    const size_t n = strlen(value_);
    if (n + 1 >= sizeof(value_)) return;
    if (cursorPos_ > n) cursorPos_ = static_cast<uint16_t>(n);
    for (size_t i = n + 1; i > cursorPos_; --i) {
        value_[i] = value_[i - 1];
    }
    value_[cursorPos_] = c;
    if (cursorPos_ < sizeof(value_) - 1) {
        ++cursorPos_;
    }
}

void SoftKeyboard::insertText(const char* text) {
    if (!text || !text[0]) return;
    for (size_t i = 0; text[i]; ++i) {
        append(text[i]);
    }
}

void SoftKeyboard::backspace() {
    const size_t n = strlen(value_);
    if (n == 0 || cursorPos_ == 0) return;
    if (cursorPos_ > n) cursorPos_ = static_cast<uint16_t>(n);
    for (size_t i = cursorPos_ - 1; i < n; ++i) {
        value_[i] = value_[i + 1];
    }
    --cursorPos_;
}

void SoftKeyboard::ensureCursorVisible(uint16_t maxVisible) {
    if (maxVisible == 0) maxVisible = 1;
    const size_t n = strlen(value_);
    if (cursorPos_ > n) cursorPos_ = static_cast<uint16_t>(n);
    if (viewOffset_ > n) viewOffset_ = static_cast<uint16_t>(n);

    if (cursorPos_ < viewOffset_) {
        viewOffset_ = cursorPos_;
    } else if (cursorPos_ > static_cast<uint16_t>(viewOffset_ + maxVisible)) {
        viewOffset_ = static_cast<uint16_t>(cursorPos_ - maxVisible);
    } else if (cursorPos_ == static_cast<uint16_t>(viewOffset_ + maxVisible)) {
        if (cursorPos_ > 0) viewOffset_ = static_cast<uint16_t>(cursorPos_ - 1);
    }

    if (n < viewOffset_) viewOffset_ = static_cast<uint16_t>(n);
}

void SoftKeyboard::setCursorFromClick(int16_t x, const katux::graphics::Rect& valueBox, uint16_t visibleStart, uint16_t maxVisible) {
    if (x <= valueBox.x + 2) {
        cursorPos_ = visibleStart;
        return;
    }
    const size_t n = strlen(value_);
    const int16_t px = static_cast<int16_t>(x - (valueBox.x + 2));
    int16_t col = static_cast<int16_t>(px / 6);
    if (col < 0) col = 0;
    if (col > static_cast<int16_t>(maxVisible)) col = static_cast<int16_t>(maxVisible);
    int32_t pos = static_cast<int32_t>(visibleStart) + static_cast<int32_t>(col);
    if (pos > static_cast<int32_t>(n)) pos = static_cast<int32_t>(n);
    cursorPos_ = static_cast<uint16_t>(pos);
}

int16_t SoftKeyboard::hitKeyId(int16_t x, int16_t y, const katux::graphics::Rect& area) const {
    if (!open_) return -1;
    if (!contains(area, x, y)) return -1;

    const katux::graphics::Rect leftBtn{area.x + 2, area.y + 1, 18, 11};
    const katux::graphics::Rect modeBtn{area.x + 22, area.y + 1, 22, 11};
    const katux::graphics::Rect pageLeftBtn{area.x + 46, area.y + 1, 10, 11};
    const katux::graphics::Rect pageRightBtn{area.x + 58, area.y + 1, 10, 11};

    const int16_t closeW = 18;
    const int16_t okW = 18;
    const int16_t quick3W = browserMode_ ? 10 : 0;
    const int16_t quick2W = browserMode_ ? 18 : 0;
    const int16_t quick1W = browserMode_ ? 18 : 0;

    int16_t closeX = static_cast<int16_t>(area.x + area.w - 2 - closeW);
    int16_t okX = static_cast<int16_t>(closeX - 2 - okW);
    int16_t quick3X = static_cast<int16_t>(okX - (browserMode_ ? (2 + quick3W) : 0));
    int16_t quick2X = static_cast<int16_t>(quick3X - (browserMode_ ? (2 + quick2W) : 0));
    int16_t quick1X = static_cast<int16_t>(quick2X - (browserMode_ ? (2 + quick1W) : 0));
    int16_t valueX = static_cast<int16_t>(pageRightBtn.x + pageRightBtn.w + 2);
    int16_t valueW = static_cast<int16_t>((browserMode_ ? quick1X : okX) - valueX - 2);
    if (valueW < 16) valueW = 16;

    const katux::graphics::Rect valueBox{valueX, area.y + 1, valueW, 11};
    const katux::graphics::Rect okBtn{okX, area.y + 1, okW, 11};
    const katux::graphics::Rect closeBtn{closeX, area.y + 1, closeW, 11};
    const katux::graphics::Rect quick1Btn{quick1X, area.y + 1, quick1W, 11};
    const katux::graphics::Rect quick2Btn{quick2X, area.y + 1, quick2W, 11};
    const katux::graphics::Rect quick3Btn{quick3X, area.y + 1, quick3W, 11};

    if (contains(leftBtn, x, y)) {
        return (layout_ == Layout::Qwerty || layout_ == Layout::Azerty) ? 200 : 90;
    }
    if (contains(modeBtn, x, y)) return 203;
    if (contains(pageLeftBtn, x, y)) return 204;
    if (contains(pageRightBtn, x, y)) return 205;
    if (contains(okBtn, x, y)) return 92;
    if (contains(closeBtn, x, y)) return 202;
    if (browserMode_ && contains(quick1Btn, x, y)) return 210;
    if (browserMode_ && contains(quick2Btn, x, y)) return 211;
    if (browserMode_ && contains(quick3Btn, x, y)) return 212;
    if (contains(valueBox, x, y)) return 201;

    const int16_t x0 = static_cast<int16_t>(area.x + 2);
    const int16_t w = static_cast<int16_t>(area.w - 4);

    if (layout_ == Layout::Qwerty || layout_ == Layout::Azerty) {
        if (y >= area.y + 12 && y < area.y + 22) {
            const int16_t kw = static_cast<int16_t>(w / 10);
            if (kw <= 0) return -1;
            const int16_t idx = static_cast<int16_t>((x - x0) / kw);
            if (idx >= 0 && idx < 10) return idx;
        }

        if (y >= area.y + 24 && y < area.y + 34) {
            const int16_t kw = static_cast<int16_t>(w / 10);
            if (kw <= 0) return -1;
            const int16_t idx = static_cast<int16_t>((x - x0) / kw);
            if (idx >= 0 && idx < 10) return static_cast<int16_t>(20 + idx);
        }

        if (y >= area.y + 36 && y < area.y + 46) {
            const int16_t kw = static_cast<int16_t>(w / 9);
            if (kw <= 0) return -1;
            const int16_t idx = static_cast<int16_t>((x - x0) / kw);
            if (idx >= 0 && idx < 9) return static_cast<int16_t>(40 + idx);
        }

        if (y >= area.y + 48 && y < area.y + 58) {
            const katux::graphics::Rect backBtn{area.x + 2, area.y + 48, 24, 10};
            const katux::graphics::Rect spaceBtn{static_cast<int16_t>(area.x + area.w - 46), area.y + 48, 44, 10};
            if (contains(backBtn, x, y)) return 90;
            if (contains(spaceBtn, x, y)) return 91;

            const int16_t lettersStart = static_cast<int16_t>(backBtn.x + backBtn.w + 2);
            const int16_t lettersWidth = static_cast<int16_t>(spaceBtn.x - lettersStart - 2);
            if (lettersWidth > 0 && x >= lettersStart && x < lettersStart + lettersWidth) {
                const int16_t kw = static_cast<int16_t>(lettersWidth / 7);
                if (kw <= 0) return -1;
                const int16_t idx = static_cast<int16_t>((x - lettersStart) / kw);
                if (idx >= 0 && idx < 7) return static_cast<int16_t>(60 + idx);
            }
        }
        return -1;
    }

    if (layout_ == Layout::Macros) {
        if (y < area.y + 12 || y >= area.y + 56) return -1;
        const int16_t colCount = 8;
        const int16_t pageSize = 32;
        const int16_t kw = static_cast<int16_t>(w / colCount);
        const int16_t kh = 11;
        if (kw <= 0) return -1;
        const int16_t col = static_cast<int16_t>((x - x0) / kw);
        const int16_t row = static_cast<int16_t>((y - area.y - 12) / kh);
        const int16_t localIdx = static_cast<int16_t>(row * colCount + col);
        const int16_t idx = static_cast<int16_t>(macroPage_ * pageSize + localIdx);
        if (localIdx >= 0 && localIdx < pageSize && idx >= 0 && idx < kMacroCount) return static_cast<int16_t>(500 + idx);
        return -1;
    }

    if (layout_ == Layout::Special) {
        if (y < area.y + 12 || y >= area.y + 56) return -1;
        const int16_t colCount = 8;
        const int16_t pageSize = 32;
        const int16_t kw = static_cast<int16_t>(w / colCount);
        const int16_t kh = 11;
        if (kw <= 0) return -1;
        const int16_t col = static_cast<int16_t>((x - x0) / kw);
        const int16_t row = static_cast<int16_t>((y - (area.y + 12)) / kh);
        const int16_t localIdx = static_cast<int16_t>(row * colCount + col);
        const int16_t idx = static_cast<int16_t>(specialPage_ * pageSize + localIdx);
        if (localIdx >= 0 && localIdx < pageSize && idx >= 0 && idx < kSpecialCount) {
            return static_cast<int16_t>(300 + idx);
        }
        return -1;
    }

    if (y < area.y + 12 || y >= area.y + 56) return -1;
    const int16_t kw = static_cast<int16_t>(w / 3);
    const int16_t kh = 11;
    if (kw <= 0) return -1;
    const int16_t col = static_cast<int16_t>((x - x0) / kw);
    const int16_t row = static_cast<int16_t>((y - (area.y + 12)) / kh);
    if (col >= 0 && col < 3 && row >= 0 && row < 4) {
        return static_cast<int16_t>(400 + row * 3 + col);
    }

    return -1;
}

bool SoftKeyboard::onClick(int16_t x, int16_t y, const katux::graphics::Rect& area) {
    if (!open_) return false;

    const int16_t keyId = hitKeyId(x, y, area);
    if (keyId < 0) return false;

    pressedKeyId_ = keyId;
    pressedUntilMs_ = millis() + 140U;

    if (keyId == 203) {
        if (layout_ == Layout::Azerty) {
            layout_ = Layout::Qwerty;
            uppercase_ = false;
        } else if (layout_ == Layout::Qwerty) {
            layout_ = Layout::Special;
            uppercase_ = false;
        } else if (layout_ == Layout::Special) {
            layout_ = Layout::Numeric;
            uppercase_ = false;
        } else if (layout_ == Layout::Numeric) {
            layout_ = Layout::Macros;
            uppercase_ = false;
        } else {
            layout_ = Layout::Azerty;
            uppercase_ = false;
        }
        return true;
    }

    if (keyId == 200) {
        if (layout_ == Layout::Qwerty || layout_ == Layout::Azerty) {
            uppercase_ = !uppercase_;
        }
        return true;
    }

    const int16_t closeW = 18;
    const int16_t okW = 18;
    const int16_t quick3W = browserMode_ ? 10 : 0;
    const int16_t quick2W = browserMode_ ? 18 : 0;
    const int16_t quick1W = browserMode_ ? 18 : 0;
    int16_t closeX = static_cast<int16_t>(area.x + area.w - 2 - closeW);
    int16_t okX = static_cast<int16_t>(closeX - 2 - okW);
    int16_t quick3X = static_cast<int16_t>(okX - (browserMode_ ? (2 + quick3W) : 0));
    int16_t quick2X = static_cast<int16_t>(quick3X - (browserMode_ ? (2 + quick2W) : 0));
    int16_t quick1X = static_cast<int16_t>(quick2X - (browserMode_ ? (2 + quick1W) : 0));
    int16_t valueX = static_cast<int16_t>(area.x + 70);
    int16_t valueW = static_cast<int16_t>((browserMode_ ? quick1X : okX) - valueX - 2);
    if (valueW < 16) valueW = 16;
    const katux::graphics::Rect valueBox{valueX, area.y + 1, valueW, 11};

    uint16_t maxVisible = static_cast<uint16_t>((valueBox.w > 4) ? ((valueBox.w - 4) / 6) : 1);
    if (maxVisible == 0) maxVisible = 1;
    ensureCursorVisible(maxVisible);

    if (keyId == 201) {
        setCursorFromClick(x, valueBox, viewOffset_, maxVisible);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 204) {
        if (layout_ == Layout::Macros) {
            if (macroPage_ > 0) --macroPage_;
            return true;
        }
        if (layout_ == Layout::Special) {
            if (specialPage_ > 0) --specialPage_;
            return true;
        }
        uint16_t step = static_cast<uint16_t>(maxVisible > 3 ? (maxVisible - 2) : 1);
        viewOffset_ = (viewOffset_ > step) ? static_cast<uint16_t>(viewOffset_ - step) : 0;
        return true;
    }

    if (keyId == 205) {
        if (layout_ == Layout::Macros) {
            const uint8_t maxPage = static_cast<uint8_t>((kMacroCount - 1) / 32);
            if (macroPage_ < maxPage) ++macroPage_;
            return true;
        }
        if (layout_ == Layout::Special) {
            const uint8_t maxPage = static_cast<uint8_t>((kSpecialCount - 1) / 32);
            if (specialPage_ < maxPage) ++specialPage_;
            return true;
        }
        const uint16_t n = static_cast<uint16_t>(strlen(value_));
        uint16_t step = static_cast<uint16_t>(maxVisible > 3 ? (maxVisible - 2) : 1);
        uint16_t maxOffset = n > maxVisible ? static_cast<uint16_t>(n - maxVisible) : 0;
        uint32_t next = static_cast<uint32_t>(viewOffset_) + static_cast<uint32_t>(step);
        viewOffset_ = next > maxOffset ? maxOffset : static_cast<uint16_t>(next);
        return true;
    }

    if (keyId == 202) {
        close();
        return true;
    }

    if (keyId == 210) {
        insertText("http://");
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 211) {
        insertText("https://");
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 212) {
        append('/');
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 500 && keyId < 500 + kMacroCount) {
        const uint8_t macroIdx = static_cast<uint8_t>(keyId - 500);
        insertText(kMacros[macroIdx]);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 90) {
        backspace();
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 91) {
        append(' ');
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId == 92) {
        accepted_ = true;
        close();
        return true;
    }

    if (keyId >= 0 && keyId < 10) {
        append(uppercase_ ? kRow1Shift[keyId] : kRow1[keyId]);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 20 && keyId < 30) {
        const char* row2 = layout_ == Layout::Azerty ? kAzertyRow2 : kQwertyRow2;
        char c = row2[keyId - 20];
        append(uppercase_ ? static_cast<char>(toupper(c)) : c);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 40 && keyId < 49) {
        const char* row3 = layout_ == Layout::Azerty ? kAzertyRow3 : kQwertyRow3;
        char c = row3[keyId - 40];
        append(uppercase_ ? static_cast<char>(toupper(c)) : c);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 60 && keyId < 67) {
        const char* row4 = layout_ == Layout::Azerty ? kAzertyRow4 : kQwertyRow4;
        char c = row4[keyId - 60];
        append(uppercase_ ? static_cast<char>(toupper(c)) : c);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 300 && keyId < 300 + kSpecialCount) {
        const uint8_t idx = static_cast<uint8_t>(keyId - 300);
        insertText(kSpecialChars[idx]);
        ensureCursorVisible(maxVisible);
        return true;
    }

    if (keyId >= 400 && keyId < 412) {
        append(kNumRows[keyId - 400]);
        ensureCursorVisible(maxVisible);
        return true;
    }

    return true;
}

void SoftKeyboard::render(katux::graphics::Renderer& renderer, const katux::graphics::Rect& area, bool darkTheme, int16_t cursorX, int16_t cursorY) {
    if (!open_) return;

    hoverKeyId_ = hitKeyId(cursorX, cursorY, area);

    const bool flash = pressedKeyId_ >= 0 && millis() <= pressedUntilMs_;
    const uint16_t bg = darkTheme ? 0x1082 : 0xC638;
    const uint16_t fg = darkTheme ? 0xFFFF : 0x0000;
    const uint16_t keyBg = darkTheme ? 0x2104 : 0xE71C;
    const uint16_t keyHover = darkTheme ? 0x3A69 : 0xB65C;
    const uint16_t keyHit = darkTheme ? 0x03EF : 0x7DFF;

    renderer.fillRect(area, bg);
    renderer.drawRect(area, fg);

    auto keyColor = [&](int16_t id, uint16_t baseColor) -> uint16_t {
        if (flash && pressedKeyId_ == id) return keyHit;
        if (hoverKeyId_ == id) return keyHover;
        return baseColor;
    };

    const katux::graphics::Rect leftBtn{area.x + 2, area.y + 1, 18, 11};
    const katux::graphics::Rect modeBtn{area.x + 22, area.y + 1, 22, 11};
    const katux::graphics::Rect pageLeftBtn{area.x + 46, area.y + 1, 10, 11};
    const katux::graphics::Rect pageRightBtn{area.x + 58, area.y + 1, 10, 11};

    const int16_t closeW = 18;
    const int16_t okW = 18;
    const int16_t quick3W = browserMode_ ? 10 : 0;
    const int16_t quick2W = browserMode_ ? 18 : 0;
    const int16_t quick1W = browserMode_ ? 18 : 0;

    int16_t closeX = static_cast<int16_t>(area.x + area.w - 2 - closeW);
    int16_t okX = static_cast<int16_t>(closeX - 2 - okW);
    int16_t quick3X = static_cast<int16_t>(okX - (browserMode_ ? (2 + quick3W) : 0));
    int16_t quick2X = static_cast<int16_t>(quick3X - (browserMode_ ? (2 + quick2W) : 0));
    int16_t quick1X = static_cast<int16_t>(quick2X - (browserMode_ ? (2 + quick1W) : 0));
    int16_t valueX = static_cast<int16_t>(pageRightBtn.x + pageRightBtn.w + 2);
    int16_t valueW = static_cast<int16_t>((browserMode_ ? quick1X : okX) - valueX - 2);
    if (valueW < 16) valueW = 16;

    const katux::graphics::Rect valueBox{valueX, area.y + 1, valueW, 11};
    const katux::graphics::Rect okBtn{okX, area.y + 1, okW, 11};
    const katux::graphics::Rect closeBtn{closeX, area.y + 1, closeW, 11};
    const katux::graphics::Rect quick1Btn{quick1X, area.y + 1, quick1W, 11};
    const katux::graphics::Rect quick2Btn{quick2X, area.y + 1, quick2W, 11};
    const katux::graphics::Rect quick3Btn{quick3X, area.y + 1, quick3W, 11};

    const bool alphaLayout = layout_ == Layout::Qwerty || layout_ == Layout::Azerty;
    const uint16_t leftBg = keyColor(alphaLayout ? 200 : 90, alphaLayout ? (uppercase_ ? 0x07E0 : keyBg) : (darkTheme ? 0x8410 : 0xA514));
    renderer.fillRect(leftBtn, leftBg);
    renderer.drawText(leftBtn.x + 3, leftBtn.y + 2, alphaLayout ? "Aa" : "BK", alphaLayout ? fg : 0xFFFF, leftBg);

    const uint16_t modeBg = keyColor(203, 0x5AEB);
    renderer.fillRect(modeBtn, modeBg);
    renderer.drawText(modeBtn.x + 1, modeBtn.y + 2, modeLabel(layout_), 0xFFFF, modeBg);

    const uint16_t pageBgL = keyColor(204, darkTheme ? 0x4A49 : 0xC618);
    const uint16_t pageBgR = keyColor(205, darkTheme ? 0x4A49 : 0xC618);
    renderer.fillRect(pageLeftBtn, pageBgL);
    renderer.fillRect(pageRightBtn, pageBgR);
    renderer.drawText(pageLeftBtn.x + 2, pageLeftBtn.y + 2, "<", fg, pageBgL);
    renderer.drawText(pageRightBtn.x + 2, pageRightBtn.y + 2, ">", fg, pageBgR);

    const uint16_t valueBg = keyColor(201, keyBg);
    renderer.fillRect(valueBox, valueBg);

    size_t n = strlen(value_);
    if (cursorPos_ > n) cursorPos_ = static_cast<uint16_t>(n);
    uint16_t maxVisible = static_cast<uint16_t>((valueBox.w > 4) ? ((valueBox.w - 4) / 6) : 1);
    if (maxVisible == 0) maxVisible = 1;
    ensureCursorVisible(maxVisible);

    char visible[65] = "";
    uint8_t write = 0;
    for (uint16_t i = viewOffset_; i < n && write < maxVisible && write + 1 < sizeof(visible); ++i) {
        visible[write++] = value_[i];
    }
    visible[write] = '\0';
    renderer.drawText(valueBox.x + 2, valueBox.y + 2, visible, fg, valueBg);

    const int16_t caretCol = static_cast<int16_t>(cursorPos_ >= viewOffset_ ? (cursorPos_ - viewOffset_) : 0);
    const int16_t caretX = static_cast<int16_t>(valueBox.x + 2 + caretCol * 6);
    if (caretX >= valueBox.x + 1 && caretX < valueBox.x + valueBox.w - 1) {
        renderer.fillRect({caretX, static_cast<int16_t>(valueBox.y + 2), 1, 7}, fg);
    }

    if (browserMode_) {
        const uint16_t q1Bg = keyColor(210, 0xC638);
        const uint16_t q2Bg = keyColor(211, 0xB596);
        const uint16_t q3Bg = keyColor(212, 0x9CF3);
        renderer.fillRect(quick1Btn, q1Bg);
        renderer.fillRect(quick2Btn, q2Bg);
        renderer.fillRect(quick3Btn, q3Bg);
        renderer.drawText(quick1Btn.x + 1, quick1Btn.y + 2, "H", 0x0000, q1Bg);
        renderer.drawText(quick2Btn.x + 1, quick2Btn.y + 2, "S", 0x0000, q2Bg);
        renderer.drawText(quick3Btn.x + 3, quick3Btn.y + 2, "/", 0x0000, q3Bg);
    }

    const uint16_t okBg = keyColor(92, 0x07E0);
    renderer.fillRect(okBtn, okBg);
    renderer.drawText(okBtn.x + 3, okBtn.y + 2, "OK", 0x0000, okBg);

    const uint16_t closeBg = keyColor(202, 0xF800);
    renderer.fillRect(closeBtn, closeBg);
    renderer.drawText(closeBtn.x + 6, closeBtn.y + 2, "X", 0xFFFF, closeBg);

    const int16_t x0 = static_cast<int16_t>(area.x + 2);
    const int16_t w = static_cast<int16_t>(area.w - 4);

    if (layout_ == Layout::Qwerty || layout_ == Layout::Azerty) {
        const char* row2 = layout_ == Layout::Azerty ? kAzertyRow2 : kQwertyRow2;
        const char* row3 = layout_ == Layout::Azerty ? kAzertyRow3 : kQwertyRow3;
        const char* row4 = layout_ == Layout::Azerty ? kAzertyRow4 : kQwertyRow4;

        const int16_t keyW10 = static_cast<int16_t>(w / 10);
        for (uint8_t i = 0; i < 10; ++i) {
            const katux::graphics::Rect r{static_cast<int16_t>(x0 + i * keyW10), static_cast<int16_t>(area.y + 12), static_cast<int16_t>(keyW10 - 1), 10};
            const uint16_t c = keyColor(i, keyBg);
            renderer.fillRect(r, c);
            char s[2] = {uppercase_ ? kRow1Shift[i] : kRow1[i], '\0'};
            renderer.drawText(static_cast<int16_t>(r.x + 4), static_cast<int16_t>(r.y + 1), s, fg, c);
        }

        for (uint8_t i = 0; i < 10; ++i) {
            const katux::graphics::Rect r{static_cast<int16_t>(x0 + i * keyW10), static_cast<int16_t>(area.y + 24), static_cast<int16_t>(keyW10 - 1), 10};
            const uint16_t c = keyColor(static_cast<int16_t>(20 + i), keyBg);
            renderer.fillRect(r, c);
            char ch = row2[i];
            char s[2] = {uppercase_ ? static_cast<char>(toupper(ch)) : ch, '\0'};
            renderer.drawText(static_cast<int16_t>(r.x + 4), static_cast<int16_t>(r.y + 1), s, fg, c);
        }

        const int16_t keyW9 = static_cast<int16_t>(w / 9);
        for (uint8_t i = 0; i < 9; ++i) {
            const katux::graphics::Rect r{static_cast<int16_t>(x0 + i * keyW9), static_cast<int16_t>(area.y + 36), static_cast<int16_t>(keyW9 - 1), 10};
            const uint16_t c = keyColor(static_cast<int16_t>(40 + i), keyBg);
            renderer.fillRect(r, c);
            char ch = row3[i];
            char s[2] = {uppercase_ ? static_cast<char>(toupper(ch)) : ch, '\0'};
            renderer.drawText(static_cast<int16_t>(r.x + 4), static_cast<int16_t>(r.y + 1), s, fg, c);
        }

        const katux::graphics::Rect backBtn{area.x + 2, area.y + 48, 24, 10};
        const katux::graphics::Rect spaceBtn{static_cast<int16_t>(area.x + area.w - 46), area.y + 48, 44, 10};

        const uint16_t backBg = keyColor(90, darkTheme ? 0x8410 : 0xA514);
        renderer.fillRect(backBtn, backBg);
        renderer.drawText(backBtn.x + 4, backBtn.y + 1, "BK", 0xFFFF, backBg);

        const uint16_t spaceBg = keyColor(91, 0x5ACB);
        renderer.fillRect(spaceBtn, spaceBg);
        renderer.drawText(spaceBtn.x + 6, spaceBtn.y + 1, "SPACE", 0xFFFF, spaceBg);

        const int16_t lettersStart = static_cast<int16_t>(backBtn.x + backBtn.w + 2);
        const int16_t lettersWidth = static_cast<int16_t>(spaceBtn.x - lettersStart - 2);
        if (lettersWidth > 0) {
            const int16_t keyW7 = static_cast<int16_t>(lettersWidth / 7);
            for (uint8_t i = 0; i < 7; ++i) {
                const katux::graphics::Rect r{static_cast<int16_t>(lettersStart + i * keyW7), static_cast<int16_t>(area.y + 48), static_cast<int16_t>(keyW7 - 1), 10};
                const uint16_t c = keyColor(static_cast<int16_t>(60 + i), keyBg);
                renderer.fillRect(r, c);
                char ch = row4[i];
                char s[2] = {uppercase_ ? static_cast<char>(toupper(ch)) : ch, '\0'};
                renderer.drawText(static_cast<int16_t>(r.x + 3), static_cast<int16_t>(r.y + 1), s, fg, c);
            }
        }
        return;
    }

    if (layout_ == Layout::Macros) {
        const int16_t colCount = 8;
        const int16_t pageSize = 32;
        const int16_t pageBase = static_cast<int16_t>(macroPage_ * pageSize);
        const int16_t kw = static_cast<int16_t>(w / colCount);
        const int16_t kh = 11;
        for (uint8_t local = 0; local < pageSize; ++local) {
            const int16_t idx = static_cast<int16_t>(pageBase + local);
            if (idx >= kMacroCount) break;
            const uint8_t row = static_cast<uint8_t>(local / colCount);
            const uint8_t col = static_cast<uint8_t>(local % colCount);
            const katux::graphics::Rect r{static_cast<int16_t>(x0 + col * kw), static_cast<int16_t>(area.y + 12 + row * kh), static_cast<int16_t>(kw - 1), 10};
            const uint16_t c = keyColor(static_cast<int16_t>(500 + idx), keyBg);
            renderer.fillRect(r, c);
            renderer.drawText(static_cast<int16_t>(r.x + 2), static_cast<int16_t>(r.y + 1), kMacros[idx], fg, c);
        }
        return;
    }

    if (layout_ == Layout::Special) {
        const int16_t colCount = 8;
        const int16_t pageSize = 32;
        const int16_t pageBase = static_cast<int16_t>(specialPage_ * pageSize);
        const int16_t kw = static_cast<int16_t>(w / colCount);
        const int16_t kh = 11;
        for (uint8_t local = 0; local < pageSize; ++local) {
            const int16_t idx = static_cast<int16_t>(pageBase + local);
            if (idx >= kSpecialCount) break;
            const uint8_t row = static_cast<uint8_t>(local / colCount);
            const uint8_t col = static_cast<uint8_t>(local % colCount);
            const katux::graphics::Rect r{static_cast<int16_t>(x0 + col * kw), static_cast<int16_t>(area.y + 12 + row * kh), static_cast<int16_t>(kw - 1), 10};
            const uint16_t c = keyColor(static_cast<int16_t>(300 + idx), keyBg);
            renderer.fillRect(r, c);
            renderer.drawText(static_cast<int16_t>(r.x + 3), static_cast<int16_t>(r.y + 1), kSpecialChars[idx], fg, c);
        }
        return;
    }

    const int16_t kw = static_cast<int16_t>(w / 3);
    const int16_t kh = 11;
    for (uint8_t i = 0; i < 12; ++i) {
        const uint8_t row = static_cast<uint8_t>(i / 3);
        const uint8_t col = static_cast<uint8_t>(i % 3);
        const katux::graphics::Rect r{static_cast<int16_t>(x0 + col * kw), static_cast<int16_t>(area.y + 12 + row * kh), static_cast<int16_t>(kw - 1), 10};
        const uint16_t c = keyColor(static_cast<int16_t>(400 + i), keyBg);
        renderer.fillRect(r, c);
        char s[2] = {kNumRows[i], '\0'};
        renderer.drawText(static_cast<int16_t>(r.x + 6), static_cast<int16_t>(r.y + 1), s, fg, c);
    }
}

}
