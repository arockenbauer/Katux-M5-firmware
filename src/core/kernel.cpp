#include "kernel.h"

#include <FS.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "../graphics/theme.h"

namespace katux::core {

static const char* kConfigPath = "/katux.cfg";
static const char* kBootFlagsKey = "boot_flags";
static constexpr uint8_t kBootFlagSafeMode = 0x01;
static constexpr uint8_t kBootFlagBios = 0x02;
static constexpr uint8_t kBootFlagRescue = 0x04;
static constexpr uint16_t kLockUnlockAnimDurationMs = 280;
static constexpr uint32_t kRescueHoldMs = 7000U;
RTC_DATA_ATTR static uint8_t gBootFlags = 0;

static void clearPreferenceNamespace(const char* name) {
    if (!name || !name[0]) return;
    Preferences prefs;
    if (!prefs.begin(name, false)) return;
    prefs.clear();
    prefs.end();
}

Kernel& kernel() {
    static Kernel instance;
    return instance;
}

bool Kernel::begin() {
    Serial.begin(115200);
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
    StickCP2.begin(cfg);

    StickCP2.Display.setRotation(1);
    StickCP2.Display.setBrightness(90);

    StickCP2.update();

    events_.begin();
    renderer_.begin();
    cursor_.begin(StickCP2.Display.width(), StickCP2.Display.height());
    input_.begin(&events_, &cursor_);
    power_.begin();
    boot_.begin();
    boot_.setTargetProgress(20);
    bios_.begin();

    settingsApp_.bindRenderer(&renderer_);
    demoApp_.bindRenderer(&renderer_);
    settingsApp_.onStart();
    demoApp_.onStart();
    boot_.setTargetProgress(45);

    prefsReady_ = prefs_.begin("katux", false);

    uint8_t bootFlags = gBootFlags;
    gBootFlags = 0;
    if (prefsReady_ && prefs_.isKey(kBootFlagsKey)) {
        bootFlags = static_cast<uint8_t>(bootFlags | prefs_.getUChar(kBootFlagsKey, 0));
        prefs_.remove(kBootFlagsKey);
    }

    safeMode_ = (bootFlags & kBootFlagSafeMode) != 0;
    bootToBios_ = (bootFlags & kBootFlagBios) != 0;
    bootToRescueBios_ = (bootFlags & kBootFlagRescue) != 0;
    if (!safeMode_) {
        safeMode_ = StickCP2.BtnA.isPressed() && StickCP2.BtnB.isPressed();
    }

    const esp_partition_t* spiffsPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (spiffsPartition) {
        SPIFFS.end();
        fsReady_ = SPIFFS.begin(false, "/spiffs", 8, spiffsPartition->label);
        if (!fsReady_) {
            SPIFFS.format();
            fsReady_ = SPIFFS.begin(false, "/spiffs", 8, spiffsPartition->label);
        }
    } else {
        fsReady_ = false;
    }

    loadConfig();
    boot_.setTargetProgress(85);

    Event e;
    e.source = EventSource::Kernel;
    e.timestampMs = millis();
    e.type = fsReady_ ? EventType::FsMounted : EventType::FsMountFailed;
    events_.push(e);

    if (bootToRescueBios_ || bootToBios_) {
        bios_.setRescueMode(bootToRescueBios_);
        bios_.begin();
        bootToBios_ = false;
        bootToRescueBios_ = false;
        mode_ = Mode::Bios;
    } else {
        mode_ = Mode::Boot;
    }
    desktopFullRedrawRequested_ = true;
    hasCursorSnapshot_ = false;
    lastBootProgress_ = 255;
    lastLockSecond_ = 255;
    lastBootRenderMs_ = millis();
    lastTickMs_ = millis();
    fpsWindowStartMs_ = lastTickMs_;
    framesInWindow_ = 0;
    fps_ = 0;
    lockUnlockAnimating_ = false;
    lockAnimDesktopReady_ = false;
    lastLockAnimOffset_ = 0;
    lockWakeGuardUntilMs_ = 0;
    lockUnlockAnimStartMs_ = 0;
    btnBHoldStartMs_ = 0;
    rescueShortcutLatched_ = false;
    rescueHoldHintVisible_ = false;
    rescueHoldHintRemainingSec_ = 255;
    modeBeforeBios_ = Mode::Desktop;
    initialized_ = true;
    return true;
}

void Kernel::update() {
    if (!initialized_) return;

    StickCP2.update();

    const uint32_t now = millis();
    updateRescueShortcut(now);
    if (!power_.isSleeping()) {
        input_.update();

        const uint32_t deltaMs = now - lastTickMs_;
        if (deltaMs >= 16) {
            Event tick;
            tick.type = EventType::SystemTick;
            tick.source = EventSource::Kernel;
            tick.timestampMs = now;
            tick.data0 = static_cast<int32_t>(deltaMs);
            events_.push(tick);
            lastTickMs_ = now;
        }

        if (mode_ == Mode::Boot) {
            boot_.setTargetProgress(100);
            if (boot_.update() && !bootCompleteDispatched_) {
                bootCompleteDispatched_ = true;
                Event done;
                done.type = EventType::BootComplete;
                done.source = EventSource::Boot;
                done.timestampMs = now;
                events_.push(done);
            }
        }

        bios_.setDebugStats(fps_, events_.size(), esp_get_free_heap_size(), fsReady_, safeMode_);
        desktop_.setRuntimeStats(fps_, esp_get_free_heap_size(), now, events_.size());
        trySyncTime(now);

        if (mode_ == Mode::Desktop && desktop_.needsFrameTick()) {
            power_.touch();
        }

        dispatchEvents();
        render();
    }

    power_.update(now);

    if (power_.consumeWakeRequest()) {
        lockUnlockAnimating_ = false;
        lockAnimDesktopReady_ = false;
        lastLockAnimOffset_ = 0;
        lockWakeGuardUntilMs_ = 0;
        lockUnlockAnimStartMs_ = 0;
        lastLockSecond_ = 255;
        lastLockFallbackRedrawMs_ = 0;
        desktopWarmupFrames_ = 0;
        hasCursorSnapshot_ = false;
        desktopFullRedrawRequested_ = true;
        btnBHoldStartMs_ = 0;
        rescueShortcutLatched_ = false;
        rescueHoldHintVisible_ = false;
        rescueHoldHintRemainingSec_ = 255;
        events_.clear();
        input_.resetState();
    }

    renderer_.present();

    ++framesInWindow_;
    const uint32_t fpsElapsed = now - fpsWindowStartMs_;
    if (fpsElapsed >= 1000U) {
        fps_ = static_cast<uint8_t>((framesInWindow_ * 1000U) / fpsElapsed);
        fpsWindowStartMs_ = now;
        framesInWindow_ = 0;
    }

    delay(1);
}

void Kernel::dispatchEvents() {
    Event event;
    uint8_t processed = 0;
    while (events_.pop(event)) {
        handleEvent(event);
        ++processed;
        if ((processed & 0x0F) == 0) {
            yield();
        }
        if (processed >= 64) {
            break;
        }
    }
}

void Kernel::handleEvent(const Event& event) {
    if (event.type != EventType::SystemTick) {
        power_.touch();
    }

    if (event.type == EventType::FsMountFailed) {
        return;
    }

    if (event.type == EventType::ShutdownRequest) {
        const int32_t mode = event.data0;
        if (mode == 1) {
            rebootWithLoading(0);
        } else if (mode == 2) {
            esp_sleep_enable_timer_wakeup(20ULL * 1000000ULL);
            esp_light_sleep_start();
        } else if (mode == 3) {
            esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
            esp_deep_sleep_start();
        } else if (mode == 4) {
            rebootWithLoading(kBootFlagBios);
        } else if (mode == 5) {
            rebootWithLoading(kBootFlagRescue);
        } else {
            StickCP2.Power.powerOff();
        }
        return;
    }

    if (event.type == EventType::ShortcutSafeMode) {
        rebootWithLoading(kBootFlagSafeMode);
        return;
    }

    if (event.type == EventType::OpenBios) {
        rebootWithLoading(kBootFlagBios);
        return;
    }

    if (mode_ == Mode::Bsod) {
        if (event.type == EventType::ButtonClick && event.source == EventSource::ButtonA) {
            esp_restart();
        }
        return;
    }

    if (mode_ == Mode::Boot) {
        if (event.type == EventType::BootComplete) {
            if (bootToRescueBios_ || bootToBios_) {
                bios_.setRescueMode(bootToRescueBios_);
                bios_.begin();
                bootToBios_ = false;
                bootToRescueBios_ = false;
                mode_ = Mode::Bios;
                desktopFullRedrawRequested_ = true;
                hasCursorSnapshot_ = false;
            } else {
                desktop_.begin(&events_, safeMode_);
                syncDesktopState();
                mode_ = Mode::Lock;
                lastLockSecond_ = 255;
                lastLockFallbackRedrawMs_ = 0;
                lockUnlockAnimating_ = false;
                lockAnimDesktopReady_ = false;
                lastLockAnimOffset_ = 0;
                lockWakeGuardUntilMs_ = 0;
                lockUnlockAnimStartMs_ = 0;
                desktopFullRedrawRequested_ = true;
                hasCursorSnapshot_ = false;
            }
        }
        return;
    }

    if (mode_ == Mode::Lock) {
        if (event.type == EventType::ButtonClick && event.source == EventSource::ButtonA) {
            if (event.timestampMs < lockWakeGuardUntilMs_) {
                return;
            }
            if (lockUnlockAnimating_) {
                return;
            }
            if (config_.animationsEnabled) {
                lockUnlockAnimating_ = true;
                lockAnimDesktopReady_ = false;
                lastLockAnimOffset_ = 0;
                lockUnlockAnimStartMs_ = event.timestampMs;
                desktopFullRedrawRequested_ = true;
            } else {
                enterDesktopFromLock();
            }
            return;
        }
        if (event.type == EventType::SystemTick) {
            if (lockUnlockAnimating_) {
                desktopFullRedrawRequested_ = true;
                if (event.timestampMs - lockUnlockAnimStartMs_ >= kLockUnlockAnimDurationMs) {
                    lockUnlockAnimating_ = false;
                    lockAnimDesktopReady_ = false;
                    lastLockAnimOffset_ = 0;
                    enterDesktopFromLock();
                }
                return;
            }
            time_t nowTs = time(nullptr);
            struct tm tmv;
            if (nowTs > 1000 && localtime_r(&nowTs, &tmv)) {
                const uint8_t sec = static_cast<uint8_t>(tmv.tm_sec);
                if (sec != lastLockSecond_) {
                    lastLockSecond_ = sec;
                    desktopFullRedrawRequested_ = true;
                }
            } else {
                if (event.timestampMs - lastLockFallbackRedrawMs_ >= 1000U) {
                    lastLockFallbackRedrawMs_ = event.timestampMs;
                    desktopFullRedrawRequested_ = true;
                }
            }
        }
        return;
    }

    if (mode_ == Mode::Bios) {
        if (event.type != EventType::SystemTick) {
            desktopFullRedrawRequested_ = true;
        }

        bios_.onEvent(event);

        if (bios_.takeRebootRequest()) {
            rebootWithLoading(0);
            return;
        }

        if (!bios_.rescueMode() && bios_.takeRescueRebootRequest()) {
            rebootWithLoading(kBootFlagRescue);
            return;
        }

        bool wipeStorage = false;
        if (bios_.takeFactoryResetRequest(wipeStorage)) {
            if (wipeStorage) {
                boot_.begin();
                boot_.setTargetProgress(30);
                uint32_t phaseStart = millis();
                while (millis() - phaseStart < 180U) {
                    boot_.update();
                    boot_.render(renderer_);
                    renderer_.present();
                    delay(1);
                }
            }

            resetConfig();

            if (wipeStorage) {
                boot_.setTargetProgress(70);
                uint32_t phaseStart = millis();
                while (millis() - phaseStart < 180U) {
                    boot_.update();
                    boot_.render(renderer_);
                    renderer_.present();
                    delay(1);
                }

                wipePersistentStorage();

                boot_.setTargetProgress(95);
                phaseStart = millis();
                while (millis() - phaseStart < 120U) {
                    boot_.update();
                    boot_.render(renderer_);
                    renderer_.present();
                    delay(1);
                }
            }

            rebootWithLoading(0);
            return;
        }

        if (!bios_.rescueMode() && bios_.takeResetRequest()) {
            resetConfig();
            rebootWithLoading(0);
            return;
        }

        if (bios_.shouldExit()) {
            if (!bios_.rescueMode()) {
                config_.darkTheme = bios_.darkThemeEnabled();
                config_.debugOverlay = bios_.debugOverlayEnabled();
                applyConfig();
                saveConfig();
            }
            rebootWithLoading(0);
            return;
        }
        return;
    }

    if (mode_ == Mode::Desktop) {
        if (event.type == EventType::SettingChanged) {
            const SettingKey key = static_cast<SettingKey>(event.data0);
            if (key == SettingKey::ThemeDark) {
                config_.darkTheme = event.data1 != 0;
            } else if (key == SettingKey::Brightness) {
                config_.brightness = static_cast<uint8_t>(event.data1);
            } else if (key == SettingKey::CursorSpeed) {
                config_.cursorSpeed = static_cast<uint8_t>(event.data1);
            } else if (key == SettingKey::PerformanceProfile) {
                config_.performanceProfile = static_cast<uint8_t>(event.data1 % 3);
            } else if (key == SettingKey::DebugOverlay) {
                config_.debugOverlay = event.data1 != 0;
            } else if (key == SettingKey::AnimationsEnabled) {
                config_.animationsEnabled = event.data1 != 0;
            } else if (key == SettingKey::AutoTime) {
                config_.autoTime = event.data1 != 0;
                timeSynced_ = false;
                lastTimeSyncAttemptMs_ = 0;
            } else if (key == SettingKey::TimezoneOffset) {
                int32_t tz = event.data1;
                if (tz < -12) tz = -12;
                if (tz > 14) tz = 14;
                config_.timezoneOffset = static_cast<int8_t>(tz);
                timeSynced_ = false;
                lastTimeSyncAttemptMs_ = 0;
            } else if (key == SettingKey::ManualYear) {
                int32_t v = event.data1;
                if (v < 2024) v = 2024;
                if (v > 2099) v = 2099;
                config_.manualYear = static_cast<uint16_t>(v);
            } else if (key == SettingKey::ManualMonth) {
                int32_t v = event.data1;
                if (v < 1) v = 1;
                if (v > 12) v = 12;
                config_.manualMonth = static_cast<uint8_t>(v);
            } else if (key == SettingKey::ManualDay) {
                int32_t v = event.data1;
                if (v < 1) v = 1;
                if (v > 31) v = 31;
                config_.manualDay = static_cast<uint8_t>(v);
            } else if (key == SettingKey::ManualHour) {
                int32_t v = event.data1;
                if (v < 0) v = 0;
                if (v > 23) v = 23;
                config_.manualHour = static_cast<uint8_t>(v);
            } else if (key == SettingKey::ManualMinute) {
                int32_t v = event.data1;
                if (v < 0) v = 0;
                if (v > 59) v = 59;
                config_.manualMinute = static_cast<uint8_t>(v);
            } else if (key == SettingKey::ClearSavedWifi) {
            }
            applyConfig();
            saveConfig();
            desktopFullRedrawRequested_ = true;
            return;
        }

        if (event.type == EventType::ResetConfigRequest) {
            resetConfig();
            desktopFullRedrawRequested_ = true;
            return;
        }

        const bool changed = desktop_.onEvent(event, cursor_.x(), cursor_.y());
        if (changed) {
            desktop_.invalidateAll();
        }
    }
}

void Kernel::render() {
    if (mode_ == Mode::Boot) {
        const uint8_t p = boot_.progress();
        const uint32_t nowMs = millis();
        if (p != lastBootProgress_ || (nowMs - lastBootRenderMs_) >= 70U) {
            boot_.render(renderer_);
            lastBootProgress_ = p;
            lastBootRenderMs_ = nowMs;
        }
        return;
    }

    if (mode_ == Mode::Lock) {
        if (desktopFullRedrawRequested_) {
            time_t nowTs = time(nullptr);
            struct tm tmv;
            bool hasTime = nowTs > 1000 && localtime_r(&nowTs, &tmv);

            if (lockUnlockAnimating_) {
                uint32_t elapsed = millis() - lockUnlockAnimStartMs_;
                if (elapsed > kLockUnlockAnimDurationMs) elapsed = kLockUnlockAnimDurationMs;
                const int16_t lockOffset = static_cast<int16_t>((static_cast<uint32_t>(renderer_.height()) * elapsed) / kLockUnlockAnimDurationMs);

                if (!lockAnimDesktopReady_) {
                    desktop_.invalidateAll();
                    desktop_.render(renderer_, true, cursor_.x(), cursor_.y());
                    lockAnimDesktopReady_ = true;
                    lastLockAnimOffset_ = 0;
                }

                if (lockOffset > lastLockAnimOffset_) {
                    const int16_t delta = static_cast<int16_t>(lockOffset - lastLockAnimOffset_);
                    const int16_t stripY = static_cast<int16_t>(renderer_.height() - lockOffset);
                    if (delta > 0 && stripY < renderer_.height()) {
                        desktop_.renderClip(renderer_, {0, stripY, renderer_.width(), delta});
                    }
                }

                renderLockScreen(static_cast<int16_t>(-lockOffset), hasTime, hasTime ? &tmv : nullptr, false);
                lastLockAnimOffset_ = lockOffset;
            } else {
                lockAnimDesktopReady_ = false;
                lastLockAnimOffset_ = 0;
                renderLockScreen(0, hasTime, hasTime ? &tmv : nullptr, true);
            }
            desktopFullRedrawRequested_ = false;
        }
        return;
    }

    if (mode_ == Mode::Bios) {
        if (desktopFullRedrawRequested_) {
            bios_.render(renderer_);
            desktopFullRedrawRequested_ = false;
        }
        return;
    }

    if (mode_ == Mode::Bsod) {
        if (desktopFullRedrawRequested_) {
            bsod_.render(renderer_);
            desktopFullRedrawRequested_ = false;
        }
        return;
    }

    const int16_t cx = cursor_.x();
    const int16_t cy = cursor_.y();
    const bool cursorMoved = hasCursorSnapshot_ && (cx != lastCursorX_ || cy != lastCursorY_);

    const bool forceWarmup = desktopWarmupFrames_ > 0;
    const bool fullDesktopRedraw = desktopFullRedrawRequested_ || !hasCursorSnapshot_ || forceWarmup;

    if (cursorMoved && hasCursorSnapshot_) {
        renderer_.pushPixels(lastCursorRect_, cursorUnderlay_);
    }

    bool cursorAreaTouched = desktop_.render(renderer_, fullDesktopRedraw, cx, cy);
    if (desktopWarmupFrames_ > 0) {
        --desktopWarmupFrames_;
    }

    if (config_.debugOverlay) {
        char line[24] = "";
        renderer_.fillRect({0, 0, 90, 12}, 0x0000);
        snprintf(line, sizeof(line), "FPS:%u Q:%u", fps_, events_.size());
        renderer_.drawText(2, 2, line, 0xFFFF, 0x0000);
        const katux::graphics::Rect overlayRect{0, 0, 90, 12};
        if (!(overlayRect.x + overlayRect.w <= cx || cx + kCursorWidth <= overlayRect.x || overlayRect.y + overlayRect.h <= cy || cy + kCursorHeight <= overlayRect.y)) {
            cursorAreaTouched = true;
        }
    }

    bool showRescueHint = false;
    uint8_t hintRemainingSec = 0;
    if (StickCP2.BtnB.isPressed() && btnBHoldStartMs_ > 0 && !rescueShortcutLatched_) {
        const uint32_t holdMs = millis() - btnBHoldStartMs_;
        if (holdMs >= 4000U && holdMs < kRescueHoldMs) {
            showRescueHint = true;
            hintRemainingSec = static_cast<uint8_t>((kRescueHoldMs - holdMs + 999U) / 1000U);
            if (hintRemainingSec == 0) hintRemainingSec = 1;
        }
    }

    if (rescueHoldHintVisible_ && (!showRescueHint || hintRemainingSec != rescueHoldHintRemainingSec_)) {
        desktop_.renderClip(renderer_, rescueHoldHintRect_);
        if (!(rescueHoldHintRect_.x + rescueHoldHintRect_.w <= cx || cx + kCursorWidth <= rescueHoldHintRect_.x ||
              rescueHoldHintRect_.y + rescueHoldHintRect_.h <= cy || cy + kCursorHeight <= rescueHoldHintRect_.y)) {
            cursorAreaTouched = true;
        }
        rescueHoldHintVisible_ = false;
        rescueHoldHintRemainingSec_ = 255;
    }

    if (showRescueHint) {
        char hint[56] = "";
        snprintf(hint, sizeof(hint), "Press %u more seconds to reboot...", static_cast<unsigned>(hintRemainingSec));
        renderer_.fillRect(rescueHoldHintRect_, 0x18C3);
        renderer_.drawRect(rescueHoldHintRect_, 0xFFFF);
        renderer_.drawText(static_cast<int16_t>(rescueHoldHintRect_.x + 4), static_cast<int16_t>(rescueHoldHintRect_.y + 2), hint, 0xFFFF, 0x18C3);
        if (!(rescueHoldHintRect_.x + rescueHoldHintRect_.w <= cx || cx + kCursorWidth <= rescueHoldHintRect_.x ||
              rescueHoldHintRect_.y + rescueHoldHintRect_.h <= cy || cy + kCursorHeight <= rescueHoldHintRect_.y)) {
            cursorAreaTouched = true;
        }
        rescueHoldHintVisible_ = true;
        rescueHoldHintRemainingSec_ = hintRemainingSec;
    }

    const bool redrawCursor = !hasCursorSnapshot_ || cursorMoved || cursorAreaTouched;
    katux::graphics::Rect cursorRect{cx, cy, kCursorWidth, kCursorHeight};
    const bool cursorVisible = renderer_.normalize(cursorRect);
    if (redrawCursor && cursorVisible) {
        renderer_.readPixels(cursorRect, cursorUnderlay_);
        renderer_.drawCursor(cx, cy, desktop_.cursorStyle());
        lastCursorRect_ = cursorRect;
        hasCursorSnapshot_ = true;
    } else if (!cursorVisible) {
        hasCursorSnapshot_ = false;
    }

    desktopFullRedrawRequested_ = false;
    lastCursorX_ = cx;
    lastCursorY_ = cy;
}

void Kernel::renderLockScreen(int16_t yOffset, bool hasTime, const struct tm* timeInfo, bool clearFirst) {
    (void)clearFirst;

    const int16_t width = renderer_.width();
    const int16_t height = renderer_.height();

    for (int16_t y = 0; y < 78; ++y) {
        const int16_t screenY = static_cast<int16_t>(y + yOffset);
        if (screenY < 0 || screenY >= height) continue;
        const uint16_t g = static_cast<uint16_t>((y * 24) / 78);
        const uint16_t sky = static_cast<uint16_t>(0x03DF + (g << 5));
        renderer_.fillRect({0, screenY, width, 1}, sky);
    }

    for (int16_t y = 78; y < 104; ++y) {
        const int16_t screenY = static_cast<int16_t>(y + yOffset);
        if (screenY < 0 || screenY >= height) continue;
        const uint16_t g = static_cast<uint16_t>(((y - 78) * 14) / 26);
        const uint16_t sea = static_cast<uint16_t>(0x043F + (g << 5));
        renderer_.fillRect({0, screenY, width, 1}, sea);
    }

    for (int16_t y = 104; y < height; ++y) {
        const int16_t screenY = static_cast<int16_t>(y + yOffset);
        if (screenY < 0 || screenY >= height) continue;
        const uint16_t g = static_cast<uint16_t>(((y - 104) * 12) / (height - 104));
        const uint16_t sand = static_cast<uint16_t>(0xD5B7 + (g << 6));
        renderer_.fillRect({0, screenY, width, 1}, sand);
    }

    renderer_.fillRect({30, static_cast<int16_t>(12 + yOffset), 76, 10}, 0xFFFF);
    renderer_.fillRect({34, static_cast<int16_t>(8 + yOffset), 34, 6}, 0xFFFF);
    renderer_.fillRect({120, static_cast<int16_t>(18 + yOffset), 58, 8}, 0xFFFF);
    renderer_.fillRect({126, static_cast<int16_t>(14 + yOffset), 26, 5}, 0xFFFF);

    renderer_.fillRect({160, static_cast<int16_t>(20 + yOffset), 26, 26}, 0xFFE0);
    renderer_.fillRect({166, static_cast<int16_t>(26 + yOffset), 14, 14}, 0xFFC0);

    for (int16_t y = 82; y < 104; ++y) {
        const int16_t screenY = static_cast<int16_t>(y + yOffset);
        if (screenY < 0 || screenY >= height) continue;
        const int16_t w = static_cast<int16_t>(38 + ((y - 82) * 3));
        renderer_.fillRect({static_cast<int16_t>(126 - w / 2), screenY, w, 1}, 0x86FF);
    }

    renderer_.fillRect({0, static_cast<int16_t>(102 + yOffset), width, 2}, 0xFFFF);

    char hourLine[16] = "--:--";
    char dateLine[32] = "Waiting network time";
    if (hasTime && timeInfo) {
        strftime(hourLine, sizeof(hourLine), "%H:%M", timeInfo);
        strftime(dateLine, sizeof(dateLine), "%A, %B %d", timeInfo);
    }

    const uint16_t lockPanelBg = 0x10C3;
    const katux::graphics::Rect lockTextPanel{10, static_cast<int16_t>(18 + yOffset), 116, 40};
    renderer_.fillRect(lockTextPanel, lockPanelBg);
    renderer_.drawRect(lockTextPanel, 0x7BEF);

    const int16_t hourY = static_cast<int16_t>(26 + yOffset);
    const int16_t dateY = static_cast<int16_t>(42 + yOffset);
    if (hourY >= 0 && hourY <= static_cast<int16_t>(height - 8)) {
        renderer_.drawText(16, hourY, hourLine, 0xFFFF, lockPanelBg);
    }
    if (dateY >= 0 && dateY <= static_cast<int16_t>(height - 8)) {
        renderer_.drawText(16, dateY, dateLine, 0xBDF7, lockPanelBg);
    }

    const katux::graphics::Rect batteryOutline{219, static_cast<int16_t>(120 + yOffset), 12, 10};
    const katux::graphics::Rect batteryPin{223, static_cast<int16_t>(124 + yOffset), 4, 2};
    renderer_.drawRect(batteryOutline, 0xBDF7);
    renderer_.fillRect(batteryPin, 0xBDF7);

    const int16_t hintY = static_cast<int16_t>(118 + yOffset);
    if (hintY >= 0 && hintY <= static_cast<int16_t>(height - 8)) {
        renderer_.drawText(10, hintY, "BtnA unlock", 0xBDF7, 0xD5B7);
    }
}

void Kernel::updateRescueShortcut(uint32_t now) {
    if (mode_ == Mode::Boot || mode_ == Mode::Bios || mode_ == Mode::Bsod) return;

    if (StickCP2.BtnB.isPressed()) {
        if (btnBHoldStartMs_ == 0) {
            btnBHoldStartMs_ = now;
        }
        if (!rescueShortcutLatched_ && now - btnBHoldStartMs_ >= kRescueHoldMs) {
            rescueShortcutLatched_ = true;
            rebootWithLoading(kBootFlagRescue);
        }
    } else {
        btnBHoldStartMs_ = 0;
        rescueShortcutLatched_ = false;
    }
}

void Kernel::rebootWithLoading(uint8_t bootFlags) {
    gBootFlags = bootFlags;
    if (prefsReady_) {
        if (bootFlags == 0) {
            prefs_.remove(kBootFlagsKey);
        } else {
            prefs_.putUChar(kBootFlagsKey, bootFlags);
        }
    }

    boot_.begin();
    boot_.setTargetProgress(100);
    const uint32_t start = millis();
    while (millis() - start < 500U) {
        boot_.update();
        boot_.render(renderer_);
        renderer_.present();
        delay(1);
    }

    esp_restart();
}

void Kernel::enterDesktopFromLock() {
    mode_ = Mode::Desktop;
    desktop_.invalidateAll();
    desktopFullRedrawRequested_ = true;
    hasCursorSnapshot_ = false;
    desktopWarmupFrames_ = 12;
    input_.resetState();
}

void Kernel::wipePersistentStorage() {
    if (prefsReady_) {
        prefs_.clear();
    }
    clearPreferenceNamespace("katuxwifi");
    clearPreferenceNamespace("katuxbrowser");
    clearPreferenceNamespace("katuxdesk");
    if (fsReady_) {
        SPIFFS.format();
    }
}

void Kernel::loadConfig() {
    uint8_t raw[13] = {'D', 90, 2, 1, 0, 1, 1, 0, 26, 1, 1, 0, 0};
    bool loaded = false;
    size_t loadedBytes = 0;

    if (fsReady_ && SPIFFS.exists(kConfigPath)) {
        File f = SPIFFS.open(kConfigPath, FILE_READ);
        if (f) {
            loadedBytes = f.read(raw, sizeof(raw));
            f.close();
            loaded = loadedBytes >= 5;
        }
    }

    if (!loaded && prefsReady_ && prefs_.isKey("cfg")) {
        loadedBytes = prefs_.getBytes("cfg", raw, sizeof(raw));
        loaded = loadedBytes >= 5;
    }

    if (!loaded) {
        config_ = {};
        applyConfig();
        return;
    }

    config_.darkTheme = raw[0] != 'L';
    config_.brightness = raw[1];
    config_.cursorSpeed = raw[2];
    config_.performanceProfile = static_cast<uint8_t>(raw[3] % 3U);
    config_.debugOverlay = raw[4] != 0;
    if (loadedBytes >= 13) {
        config_.animationsEnabled = raw[5] != 0;
        config_.autoTime = raw[6] != 0;
        config_.timezoneOffset = static_cast<int8_t>(raw[7]);
        config_.manualYear = static_cast<uint16_t>(2000 + raw[8]);
        config_.manualMonth = raw[9] == 0 ? 1 : raw[9];
        config_.manualDay = raw[10] == 0 ? 1 : raw[10];
        config_.manualHour = raw[11] % 24;
        config_.manualMinute = raw[12] % 60;
    } else {
        config_.animationsEnabled = true;
        config_.autoTime = raw[5] != 0;
        config_.timezoneOffset = static_cast<int8_t>(raw[6]);
        config_.manualYear = static_cast<uint16_t>(2000 + raw[7]);
        config_.manualMonth = raw[8] == 0 ? 1 : raw[8];
        config_.manualDay = raw[9] == 0 ? 1 : raw[9];
        config_.manualHour = raw[10] % 24;
        config_.manualMinute = raw[11] % 60;
    }
    applyConfig();
}

void Kernel::saveConfig() {
    uint8_t raw[13];
    raw[0] = config_.darkTheme ? 'D' : 'L';
    raw[1] = config_.brightness;
    raw[2] = config_.cursorSpeed;
    raw[3] = config_.performanceProfile;
    raw[4] = config_.debugOverlay ? 1 : 0;
    raw[5] = config_.animationsEnabled ? 1 : 0;
    raw[6] = config_.autoTime ? 1 : 0;
    raw[7] = static_cast<uint8_t>(config_.timezoneOffset);
    raw[8] = static_cast<uint8_t>(config_.manualYear > 2000 ? (config_.manualYear - 2000) : 26);
    raw[9] = config_.manualMonth;
    raw[10] = config_.manualDay;
    raw[11] = config_.manualHour;
    raw[12] = config_.manualMinute;

    if (fsReady_) {
        File f = SPIFFS.open(kConfigPath, FILE_WRITE);
        if (f) {
            f.write(raw, sizeof(raw));
            f.close();
        }
    }

    if (prefsReady_) {
        prefs_.putBytes("cfg", raw, sizeof(raw));
    }
}

void Kernel::resetConfig() {
    config_ = {};
    applyConfig();
    if (fsReady_ && SPIFFS.exists(kConfigPath)) {
        SPIFFS.remove(kConfigPath);
    }
    if (prefsReady_) {
        prefs_.remove("cfg");
    }
}

void Kernel::applyTheme(bool dark) {
    auto& t = katux::graphics::theme();
    if (dark) {
        t.desktopBg = 0x0000;
        t.taskbarBg = 0x1082;
        t.windowBg = 0xDEFB;
        t.windowBorder = 0x39E7;
        t.titleActive = 0x1A5F;
        t.titleInactive = 0x7BEF;
        t.text = 0xFFFF;
        t.accent = 0x07FF;
        t.danger = 0xF800;
    } else {
        t.desktopBg = 0x02D9;
        t.taskbarBg = 0x1082;
        t.windowBg = 0xF7BE;
        t.windowBorder = 0xA554;
        t.titleActive = 0x1A5F;
        t.titleInactive = 0x94B2;
        t.text = 0x0000;
        t.accent = 0x05BF;
        t.danger = 0xF800;
    }
}

void Kernel::applyPerformanceProfile(uint8_t profile) {
    if (profile == 0) {
        power_.setTimeouts(15000, 45000);
    } else if (profile == 2) {
        power_.setTimeouts(45000, 180000);
    } else {
        power_.setTimeouts(25000, 90000);
    }
}

void Kernel::applyConfig() {
    applyTheme(config_.darkTheme);
    bios_.setDarkTheme(config_.darkTheme);
    bios_.setDebugOverlay(config_.debugOverlay);
    input_.setCursorSpeed(config_.cursorSpeed);
    power_.setBrightness(config_.brightness);
    applyPerformanceProfile(config_.performanceProfile);
    applyTimezoneOffset();
    if (!config_.autoTime) {
        applyManualTime();
    } else {
        timeSynced_ = false;
    }
    syncDesktopState();
}

void Kernel::syncDesktopState() {
    desktop_.setSystemState(config_.darkTheme, config_.brightness, config_.cursorSpeed, config_.performanceProfile, config_.debugOverlay,
                            config_.animationsEnabled, config_.autoTime, config_.timezoneOffset, config_.manualYear, config_.manualMonth,
                            config_.manualDay, config_.manualHour, config_.manualMinute);
}

void Kernel::applyTimezoneOffset() {
    int8_t offset = config_.timezoneOffset;
    if (offset < -12) offset = -12;
    if (offset > 14) offset = 14;
    config_.timezoneOffset = offset;

    char tzValue[12] = "";
    snprintf(tzValue, sizeof(tzValue), "UTC%+d", -static_cast<int>(offset));
    setenv("TZ", tzValue, 1);
    tzset();
}

void Kernel::applyManualTime() {
    struct tm tmv{};
    tmv.tm_year = static_cast<int>(config_.manualYear) - 1900;
    tmv.tm_mon = static_cast<int>(config_.manualMonth > 0 ? config_.manualMonth - 1 : 0);
    tmv.tm_mday = static_cast<int>(config_.manualDay);
    tmv.tm_hour = static_cast<int>(config_.manualHour);
    tmv.tm_min = static_cast<int>(config_.manualMinute);
    tmv.tm_sec = 0;

    time_t utcTs = mktime(&tmv);
    if (utcTs < 0) return;

    timeval tv;
    tv.tv_sec = utcTs;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

void Kernel::trySyncTime(uint32_t now) {
    if (!config_.autoTime || timeSynced_) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (now - lastTimeSyncAttemptMs_ < 30000U) return;

    lastTimeSyncAttemptMs_ = now;

    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    struct tm tmv;
    if (getLocalTime(&tmv, 2500)) {
        timeSynced_ = true;
        return;
    }

    WiFiClient client;
    if (!client.connect("google.com", 80, 1500)) {
        return;
    }

    client.print("HEAD / HTTP/1.1\r\nHost: google.com\r\nConnection: close\r\n\r\n");

    char dateLine[48] = "";
    const uint32_t start = millis();
    while (client.connected() && millis() - start < 1800) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.startsWith("Date:")) {
            line.remove(0, 5);
            line.trim();
            strlcpy(dateLine, line.c_str(), sizeof(dateLine));
            break;
        }
        if (line.length() == 0) break;
    }
    client.stop();

    if (!dateLine[0]) {
        return;
    }

    char wk[4] = "";
    char mon[4] = "";
    int day = 0;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (sscanf(dateLine, "%3s, %d %3s %d %d:%d:%d GMT", wk, &day, mon, &year, &hh, &mm, &ss) != 7) {
        return;
    }

    static const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int monthIndex = -1;
    for (int i = 0; i < 12; ++i) {
        if (strncmp(mon, months[i], 3) == 0) {
            monthIndex = i;
            break;
        }
    }
    if (monthIndex < 0) {
        return;
    }

    struct tm httpTm{};
    httpTm.tm_year = year - 1900;
    httpTm.tm_mon = monthIndex;
    httpTm.tm_mday = day;
    httpTm.tm_hour = hh;
    httpTm.tm_min = mm;
    httpTm.tm_sec = ss;

    char previousTz[32] = "";
    const char* tz = getenv("TZ");
    if (tz) {
        strlcpy(previousTz, tz, sizeof(previousTz));
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t utcTs = mktime(&httpTm);
    if (previousTz[0]) {
        setenv("TZ", previousTz, 1);
    } else {
        setenv("TZ", "UTC0", 1);
    }
    tzset();

    if (utcTs < 0) {
        return;
    }

    timeval tv;
    tv.tv_sec = utcTs;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timeSynced_ = true;
}

}