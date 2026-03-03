#pragma once

#include <M5StickCPlus2.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <time.h>

#include "event_manager.h"
#include "../graphics/renderer.h"
#include "../graphics/cursor.h"
#include "../graphics/desktop.h"
#include "../input/input_manager.h"
#include "../system/boot.h"
#include "../system/bios.h"
#include "../system/bsod.h"
#include "../system/power_manager.h"
#include "../apps/settings_app.h"
#include "../apps/demo_app.h"

namespace katux::core {

class Kernel {
   public:
    bool begin();
    void update();

   private:
    enum class Mode : uint8_t {
        Boot,
        Lock,
        Bios,
        Desktop,
        Bsod
    };

    struct Config {
        bool darkTheme = true;
        uint8_t brightness = 90;
        uint8_t cursorSpeed = 2;
        uint8_t performanceProfile = 1;
        bool debugOverlay = false;
        bool animationsEnabled = true;
        bool autoTime = true;
        int8_t timezoneOffset = 0;
        uint16_t manualYear = 2026;
        uint8_t manualMonth = 1;
        uint8_t manualDay = 1;
        uint8_t manualHour = 0;
        uint8_t manualMinute = 0;
    };

    EventManager events_;
    katux::graphics::Renderer renderer_;
    katux::graphics::Cursor cursor_;
    katux::graphics::Desktop desktop_;
    katux::input::InputManager input_;
    katux::system::BootManager boot_;
    katux::system::Bios bios_;
    katux::system::Bsod bsod_;
    katux::system::PowerManager power_;
    katux::apps::SettingsApp settingsApp_;
    katux::apps::DemoApp demoApp_;
    Preferences prefs_;

    Mode mode_ = Mode::Boot;
    Config config_{};
    bool initialized_ = false;
    bool fsReady_ = false;
    bool prefsReady_ = false;
    bool bootCompleteDispatched_ = false;
    bool desktopFullRedrawRequested_ = true;
    bool hasCursorSnapshot_ = false;
    static constexpr int16_t kCursorWidth = 10;
    static constexpr int16_t kCursorHeight = 14;
    uint16_t cursorUnderlay_[kCursorWidth * kCursorHeight]{};
    katux::graphics::Rect lastCursorRect_{0, 0, 0, 0};
    bool safeMode_ = false;
    bool bootToBios_ = false;

    int16_t lastCursorX_ = 0;
    int16_t lastCursorY_ = 0;
    uint8_t lastBootProgress_ = 255;
    uint8_t lastLockSecond_ = 255;
    uint32_t lastBootRenderMs_ = 0;
    uint32_t lastTickMs_ = 0;
    uint32_t lastLockFallbackRedrawMs_ = 0;

    uint8_t fps_ = 0;
    uint32_t fpsWindowStartMs_ = 0;
    uint8_t framesInWindow_ = 0;
    bool timeSynced_ = false;
    uint32_t lastTimeSyncAttemptMs_ = 0;
    bool lockUnlockAnimating_ = false;
    bool lockAnimDesktopReady_ = false;
    int16_t lastLockAnimOffset_ = 0;
    uint32_t lockWakeGuardUntilMs_ = 0;
    uint32_t lockUnlockAnimStartMs_ = 0;
    uint8_t desktopWarmupFrames_ = 0;

    void dispatchEvents();
    void handleEvent(const Event& event);
    void render();
    void renderLockScreen(int16_t yOffset, bool hasTime, const struct tm* timeInfo, bool clearFirst);

    void loadConfig();
    void saveConfig();
    void resetConfig();

    void applyTheme(bool dark);
    void applyPerformanceProfile(uint8_t profile);
    void applyConfig();
    void syncDesktopState();
    void applyManualTime();
    void trySyncTime(uint32_t now);
};

Kernel& kernel();

}