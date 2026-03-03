#pragma once

#include <stdint.h>

namespace katux::graphics {

struct Theme {
    uint16_t desktopBg = 0x0000;
    uint16_t taskbarBg = 0x18C3;
    uint16_t windowBg = 0x2104;
    uint16_t windowBorder = 0xFFFF;
    uint16_t titleActive = 0x001F;
    uint16_t titleInactive = 0x39E7;
    uint16_t text = 0xFFFF;
    uint16_t accent = 0x07E0;
    uint16_t danger = 0xF800;
};

Theme& theme();

}