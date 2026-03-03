#include "input_manager.h"

namespace katux::input {

void InputManager::begin(katux::core::EventManager* events, katux::graphics::Cursor* cursor) {
    events_ = events;
    cursor_ = cursor;
    a_ = {};
    b_ = {};
    pwr_ = {};
    comboABLatched_ = false;

    bRightStreak_ = 0;
    bLeftStreak_ = 0;
    pDownStreak_ = 0;
    pUpStreak_ = 0;
    lastBRightMs_ = 0;
    lastBLeftMs_ = 0;
    lastPDownMs_ = 0;
    lastPUpMs_ = 0;
}

void InputManager::update() {
    if (!events_ || !cursor_) return;

    const uint32_t now = millis();
    const bool aPressed = StickCP2.BtnA.isPressed();
    const bool bPressed = StickCP2.BtnB.isPressed();

    updateButton(aPressed, a_, katux::core::EventSource::ButtonA);
    updateButton(bPressed, b_, katux::core::EventSource::ButtonB);
    updateButton(M5.BtnPWR.isPressed(), pwr_, katux::core::EventSource::ButtonPower);

    if (aPressed && bPressed && !comboABLatched_) {
        comboABLatched_ = true;
        emit(katux::core::EventType::ShortcutSafeMode, katux::core::EventSource::ButtonA);
    } else if (!aPressed || !bPressed) {
        comboABLatched_ = false;
    }

    processHeldMotion(now);
    flushClicks(a_, katux::core::EventSource::ButtonA, now);
}

void InputManager::updateButton(bool pressed, ButtonState& state, katux::core::EventSource source) {
    const uint32_t now = millis();

    if (pressed && !state.previousPressed) {
        state.previousPressed = true;
        state.longDispatched = false;
        state.downMs = now;
        emit(katux::core::EventType::ButtonDown, source);
    }

    if (pressed && state.previousPressed && !state.longDispatched && now - state.downMs >= kLongPressMs) {
        state.longDispatched = true;
        emit(katux::core::EventType::ButtonLong, source);
    }

    if (!pressed && state.previousPressed) {
        state.previousPressed = false;
        state.lastReleaseMs = now;
        emit(katux::core::EventType::ButtonUp, source);

        if (source == katux::core::EventSource::ButtonA) {
            if (!state.longDispatched) {
                state.clickCount = 1;
            }
            return;
        }

        if (source == katux::core::EventSource::ButtonB) {
            if (!state.longDispatched) {
                const int16_t step = nextStep(bRightStreak_, lastBRightMs_, now);
                bLeftStreak_ = 0;
                cursor_->move(step, 0, false);
            } else {
                bRightStreak_ = 0;
                bLeftStreak_ = 0;
            }
            return;
        }

        if (source == katux::core::EventSource::ButtonPower) {
            if (!state.longDispatched) {
                const int16_t step = nextStep(pDownStreak_, lastPDownMs_, now);
                pUpStreak_ = 0;
                cursor_->move(0, step, false);
            } else {
                pDownStreak_ = 0;
                pUpStreak_ = 0;
            }
            return;
        }
    }
}

void InputManager::flushClicks(ButtonState& state, katux::core::EventSource source, uint32_t now) {
    if (source != katux::core::EventSource::ButtonA) return;
    if (state.clickCount == 0) return;
    if (now - state.lastReleaseMs < kMultiClickGapMs) return;

    emit(katux::core::EventType::ButtonClick, source);
    state.clickCount = 0;
}

void InputManager::emit(katux::core::EventType type, katux::core::EventSource source, int32_t d0, int32_t d1) {
    katux::core::Event e;
    e.type = type;
    e.source = source;
    e.timestampMs = millis();
    e.x = cursor_ ? cursor_->x() : 0;
    e.y = cursor_ ? cursor_->y() : 0;
    e.data0 = d0;
    e.data1 = d1;
    events_->push(e);
}

int16_t InputManager::nextStep(uint8_t& streak, uint32_t& lastMs, uint32_t now) const {
    if (now - lastMs <= kStreakGapMs) {
        if (streak < 7) ++streak;
    } else {
        streak = 0;
    }

    lastMs = now;

    const int16_t base = static_cast<int16_t>(1 + (streak * 2));
    const int16_t scaled = static_cast<int16_t>(base * cursorSpeed_);
    const int16_t maxStep = static_cast<int16_t>(10 + (cursorSpeed_ * 2));
    return scaled > maxStep ? maxStep : scaled;
}

void InputManager::processHeldMotion(uint32_t now) {
    if (!cursor_) return;

    if (b_.previousPressed && b_.longDispatched) {
        const uint32_t holdMs = now - b_.downMs;
        const uint32_t accel = holdMs / 22U;
        const uint32_t interval = (85U > accel + 22U) ? (85U - accel) : 22U;

        bRightStreak_ = 0;
        if (now - lastBLeftMs_ >= interval) {
            const int16_t step = nextStep(bLeftStreak_, lastBLeftMs_, now);
            cursor_->move(-step, 0, false);
        }
    }

    if (pwr_.previousPressed && pwr_.longDispatched) {
        const uint32_t holdMs = now - pwr_.downMs;
        const uint32_t accel = holdMs / 22U;
        const uint32_t interval = (85U > accel + 22U) ? (85U - accel) : 22U;

        pDownStreak_ = 0;
        if (now - lastPUpMs_ >= interval) {
            const int16_t step = nextStep(pUpStreak_, lastPUpMs_, now);
            cursor_->move(0, -step, false);
        }
    }
}

void InputManager::resetState() {
    a_ = {};
    b_ = {};
    pwr_ = {};
    comboABLatched_ = false;

    bRightStreak_ = 0;
    bLeftStreak_ = 0;
    pDownStreak_ = 0;
    pUpStreak_ = 0;
    lastBRightMs_ = 0;
    lastBLeftMs_ = 0;
    lastPDownMs_ = 0;
    lastPUpMs_ = 0;
}

void InputManager::setCursorSpeed(uint8_t speed) {
    if (speed < 1) speed = 1;
    if (speed > 5) speed = 5;
    cursorSpeed_ = speed;
}

uint8_t InputManager::cursorSpeed() const {
    return cursorSpeed_;
}

}