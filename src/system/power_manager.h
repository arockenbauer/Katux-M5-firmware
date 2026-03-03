#pragma once

#include <M5StickCPlus2.h>
#include <stdint.h>

namespace katux::system {

class PowerManager {
   public:
    void begin();
    void touch();
    void update(uint32_t nowMs);

    void setBrightness(uint8_t brightness);
    uint8_t brightness() const;

    void setTimeouts(uint32_t dimTimeoutMs, uint32_t sleepTimeoutMs);
    uint32_t dimTimeoutMs() const;
    uint32_t sleepTimeoutMs() const;

    bool isSleeping() const;
    bool consumeWakeRequest();

   private:
    uint32_t lastActivityMs_ = 0;
    bool dimmed_ = false;
    bool sleeping_ = false;
    bool wakeRequest_ = false;
    uint8_t brightness_ = 90;
    uint32_t dimTimeoutMs_ = 25000;
    uint32_t sleepTimeoutMs_ = 90000;
};

}