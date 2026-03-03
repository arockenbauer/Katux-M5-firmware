#pragma once

#include <M5StickCPlus2.h>

#include "../core/event_manager.h"
#include "../graphics/cursor.h"

namespace katux::input {

class InputManager {
   public:
    void begin(katux::core::EventManager* events, katux::graphics::Cursor* cursor);
    void update();
    void resetState();

    void setCursorSpeed(uint8_t speed);
    uint8_t cursorSpeed() const;

   private:
    struct ButtonState {
        bool previousPressed = false;
        bool longDispatched = false;
        uint32_t downMs = 0;
        uint32_t lastReleaseMs = 0;
        uint8_t clickCount = 0;
    };

    katux::core::EventManager* events_ = nullptr;
    katux::graphics::Cursor* cursor_ = nullptr;

    ButtonState a_{};
    ButtonState b_{};
    ButtonState pwr_{};

    bool comboABLatched_ = false;

    uint8_t cursorSpeed_ = 2;

    uint8_t bRightStreak_ = 0;
    uint8_t bLeftStreak_ = 0;
    uint8_t pDownStreak_ = 0;
    uint8_t pUpStreak_ = 0;
    uint32_t lastBRightMs_ = 0;
    uint32_t lastBLeftMs_ = 0;
    uint32_t lastPDownMs_ = 0;
    uint32_t lastPUpMs_ = 0;

    static constexpr uint16_t kLongPressMs = 450;
    static constexpr uint16_t kMultiClickGapMs = 320;
    static constexpr uint16_t kStreakGapMs = 700;

    void updateButton(bool pressed, ButtonState& state, katux::core::EventSource source);
    void flushClicks(ButtonState& state, katux::core::EventSource source, uint32_t now);
    void emit(katux::core::EventType type, katux::core::EventSource source, int32_t d0 = 0, int32_t d1 = 0);
    int16_t nextStep(uint8_t& streak, uint32_t& lastMs, uint32_t now) const;
    void processHeldMotion(uint32_t now);
};

}