#include "power_manager.h"

namespace katux::system {

void PowerManager::begin() {
    lastActivityMs_ = millis();
    dimmed_ = false;
    sleeping_ = false;
    wakeRequest_ = false;
    StickCP2.Display.setBrightness(brightness_);
}

void PowerManager::touch() {
    lastActivityMs_ = millis();
    if (sleeping_) {
        sleeping_ = false;
        wakeRequest_ = true;
        StickCP2.Display.setBrightness(brightness_);
        dimmed_ = false;
        return;
    }
    if (dimmed_) {
        StickCP2.Display.setBrightness(brightness_);
        dimmed_ = false;
    }
}

void PowerManager::update(uint32_t nowMs) {
    if (sleeping_) {
        if (StickCP2.BtnA.isPressed() || StickCP2.BtnB.isPressed() || M5.BtnPWR.isPressed()) {
            sleeping_ = false;
            wakeRequest_ = true;
            lastActivityMs_ = nowMs;
            dimmed_ = false;
            StickCP2.Display.setBrightness(brightness_);
        }
        return;
    }

    const uint32_t idleMs = nowMs - lastActivityMs_;

    if (!dimmed_ && idleMs > dimTimeoutMs_) {
        const uint8_t dimLevel = brightness_ > 20 ? static_cast<uint8_t>(brightness_ / 2U) : 10;
        StickCP2.Display.setBrightness(dimLevel);
        dimmed_ = true;
    }

    if (idleMs > sleepTimeoutMs_) {
        sleeping_ = true;
        dimmed_ = false;
        StickCP2.Display.setBrightness(0);
    }
}

void PowerManager::setBrightness(uint8_t brightness) {
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    brightness_ = brightness;
    if (!dimmed_) {
        StickCP2.Display.setBrightness(brightness_);
    }
}

uint8_t PowerManager::brightness() const {
    return brightness_;
}

void PowerManager::setTimeouts(uint32_t dimTimeoutMs, uint32_t sleepTimeoutMs) {
    dimTimeoutMs_ = dimTimeoutMs;
    sleepTimeoutMs_ = sleepTimeoutMs;
    if (sleepTimeoutMs_ < dimTimeoutMs_ + 5000U) {
        sleepTimeoutMs_ = dimTimeoutMs_ + 5000U;
    }
}

uint32_t PowerManager::dimTimeoutMs() const {
    return dimTimeoutMs_;
}

uint32_t PowerManager::sleepTimeoutMs() const {
    return sleepTimeoutMs_;
}

bool PowerManager::isSleeping() const {
    return sleeping_;
}

bool PowerManager::consumeWakeRequest() {
    const bool wake = wakeRequest_;
    wakeRequest_ = false;
    return wake;
}

}