#include "desktop.h"

#include <WiFi.h>
#include <string.h>
#include <time.h>

namespace katux::graphics {

static bool intersects(const Rect& a, const Rect& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

void Desktop::begin(katux::core::EventManager* events, bool safeMode) {
    events_ = events;
    safeMode_ = safeMode;
    windows_.begin(events);
    compositor_.begin(240, 135);
    compositor_.invalidateAll();
    selectedIcon_ = 0;
    hoverIcon_ = -1;
    startOpen_ = false;
    startPowerOpen_ = false;
    startMenuScroll_ = 0;
    startRect_.y = 120;
    menuRect_ = {2, 10, 176, 122};
    resetConfirmArmed_ = false;
    resetConfirmUntilMs_ = 0;
    toast_[0] = '\0';
    toastUntilMs_ = 0;
    quickActionPulseUntilMs_ = 0;
    quickActionPulseIndex_ = -1;
    quickActionHoverIndex_ = -1;
    lastCursorX_ = 0;
    lastCursorY_ = 0;

    appCount_ = 0;
    apps_[appCount_++] = {"Apps", WindowKind::AppHub, {8, 20, 24, 20}, {8, 44, 34, 11}, 6};
    apps_[appCount_++] = {"Settings", WindowKind::Settings, {48, 20, 24, 20}, {48, 44, 56, 11}, 1};
    apps_[appCount_++] = {"Files", WindowKind::Explorer, {88, 20, 24, 20}, {88, 44, 40, 11}, 3};
    apps_[appCount_++] = {"Notes", WindowKind::Notepad, {128, 20, 24, 20}, {126, 44, 42, 11}, 7};
    apps_[appCount_++] = {"WiFi", WindowKind::WifiManager, {8, 62, 24, 20}, {8, 86, 32, 11}, 5};
    apps_[appCount_++] = {"Tasks", WindowKind::TaskManager, {48, 62, 24, 20}, {48, 86, 40, 11}, 2};
    apps_[appCount_++] = {"Browser", WindowKind::Browser, {88, 62, 24, 20}, {86, 86, 48, 11}, 4};
    apps_[appCount_++] = {"DateTime", WindowKind::DateTime, {128, 62, 24, 20}, {124, 86, 58, 11}, 11};
    apps_[appCount_++] = {"Desktop", WindowKind::DesktopConfig, {8, 20, 24, 20}, {8, 44, 48, 11}, 1};
    apps_[appCount_++] = {"Alerts", WindowKind::Notifications, {8, 20, 24, 20}, {8, 44, 34, 11}, 0};
    apps_[appCount_++] = {"Reboot", WindowKind::Reboot, {48, 20, 24, 20}, {46, 44, 40, 11}, 1};
    apps_[appCount_++] = {"Demo", WindowKind::Demo, {48, 20, 24, 20}, {48, 44, 32, 11}, 6};
    apps_[appCount_++] = {"Pixel", WindowKind::GamePixel, {88, 20, 24, 20}, {88, 44, 32, 11}, 8};
    apps_[appCount_++] = {"Orbit", WindowKind::GameOrbit, {128, 20, 24, 20}, {128, 44, 32, 11}, 9};
    apps_[appCount_++] = {"Plinko", WindowKind::GamePlinko, {8, 62, 24, 20}, {8, 86, 40, 11}, 10};

    for (uint8_t i = 0; i < kMaxApps; ++i) {
        appWindowIds_[i] = -1;
    }

    contextMenuOpen_ = false;
    refreshDesktopConfig();

    initialized_ = true;
}

bool Desktop::onEvent(const katux::core::Event& event, int16_t cursorX, int16_t cursorY) {
    if (!initialized_) return false;

    refreshDesktopConfig();
    hoverIcon_ = hitIcon(cursorX, cursorY);

    const uint32_t now = millis();
    if (resetConfirmArmed_ && now > resetConfirmUntilMs_) {
        resetConfirmArmed_ = false;
    }

    if (event.type == katux::core::EventType::SystemTick) {
        static uint32_t lastBatteryCheck = 0;
        if (now - lastBatteryCheck > 8000U) {
            lastBatteryCheck = now;
            int level = StickCP2.Power.getBatteryLevel();
            if (level > 0 && level < 20) {
                windows_.notify("Batterie faible");
                showToast("Low battery", 1700);
            }
        }
    }

    if (event.type == katux::core::EventType::WindowClosed) {
        const WindowKind kind = static_cast<WindowKind>(event.data1);
        for (uint8_t i = 0; i < appCount_; ++i) {
            if (apps_[i].kind == kind) {
                appWindowIds_[i] = -1;
            }
        }
        windows_.notify("App fermee");
        showToast("Window closed");
        return true;
    }

    if (event.type == katux::core::EventType::WindowMinimized) {
        windows_.notify("Fenetre reduite");
        showToast("Window minimized");
        return true;
    }

    if (event.type == katux::core::EventType::WindowRestored) {
        showToast("Window restored");
        return true;
    }

    if (event.type == katux::core::EventType::FsMountFailed) {
        windows_.notify("SPIFFS mount failed");
        showToast("SPIFFS failed", 1900);
        return true;
    }

    if (event.type == katux::core::EventType::OpenQuickMenu) {
        startOpen_ = !startOpen_;
        return true;
    }

    if (event.type == katux::core::EventType::LaunchDemo) {
        for (uint8_t i = 0; i < appCount_; ++i) {
            if (apps_[i].kind == WindowKind::Demo) {
                const bool changed = openWindowForApp(i);
                if (changed) showToast("Demo launched");
                return changed;
            }
        }
        return false;
    }

    if (event.type == katux::core::EventType::LaunchSettings) {
        for (uint8_t i = 0; i < appCount_; ++i) {
            if (apps_[i].kind == WindowKind::Settings) {
                const bool changed = openWindowForApp(i);
                if (changed) showToast("Settings open");
                return changed;
            }
        }
        return false;
    }

    if (event.type == katux::core::EventType::ButtonClick && event.source == katux::core::EventSource::ButtonA) {
        const Rect startAbs{startRect_.x, startRect_.y, startRect_.w, startRect_.h};
        const int16_t taskbarTop = static_cast<int16_t>(startRect_.y - 4);
        const bool inTaskbar = cursorY >= taskbarTop;

        if (contextMenuOpen_) {
            const int8_t item = hitContextMenuItem(cursorX, cursorY);
            if (item == 0) {
                desktopShowIcons_ = !desktopShowIcons_;
                windows_.setDesktopIconsVisible(desktopShowIcons_);
                showToast(desktopShowIcons_ ? "Desktop icons on" : "Desktop icons off");
            } else if (item == 1) {
                invalidateAll();
                showToast("Desktop refreshed");
            } else if (item == 2) {
                windows_.cleanDesktopToAppsOnly();
                refreshDesktopConfig();
                selectedIcon_ = 0;
                showToast("Desktop cleaned");
            } else if (item == 3) {
                const int8_t desktopCfg = appIndexForKind(WindowKind::DesktopConfig);
                if (desktopCfg >= 0) {
                    openWindowForApp(static_cast<uint8_t>(desktopCfg));
                    showToast("Desktop setup");
                }
            } else if (item == 4) {
                startOpen_ = true;
                startMenuScroll_ = 0;
                startPowerOpen_ = false;
                showToast("Start menu");
            }
            contextMenuOpen_ = false;
            return true;
        }

        if (startOpen_) {
            const int8_t item = hitStartMenuItem(cursorX, cursorY);
            if (item == -2) {
                if (startMenuScroll_ > 0) {
                    --startMenuScroll_;
                }
                return true;
            }
            if (item == -3) {
                const uint8_t totalRows = appCount_;
                const uint8_t visibleRows = 7;
                const uint8_t maxScroll = totalRows > visibleRows ? static_cast<uint8_t>(totalRows - visibleRows) : 0;
                if (startMenuScroll_ < maxScroll) {
                    ++startMenuScroll_;
                }
                return true;
            }
            if (item == -10) {
                startPowerOpen_ = !startPowerOpen_;
                return true;
            }
            if (item == -15) {
                return true;
            }
            if (item == -11) {
                emit(katux::core::EventType::ShutdownRequest, 0, 0);
                startOpen_ = false;
                startPowerOpen_ = false;
                showToast("Shutdown");
                return true;
            }
            if (item == -12) {
                emit(katux::core::EventType::ShutdownRequest, 1, 0);
                startOpen_ = false;
                startPowerOpen_ = false;
                showToast("Restart");
                return true;
            }
            if (item == -13) {
                emit(katux::core::EventType::ShutdownRequest, 2, 0);
                startOpen_ = false;
                startPowerOpen_ = false;
                showToast("Sleep");
                return true;
            }
            if (item == -14) {
                emit(katux::core::EventType::ShutdownRequest, 3, 0);
                startOpen_ = false;
                startPowerOpen_ = false;
                showToast("Deep sleep");
                return true;
            }
            if (item >= 0 && item < static_cast<int8_t>(appCount_)) {
                const bool opened = openWindowForApp(static_cast<uint8_t>(item));
                startOpen_ = false;
                startPowerOpen_ = false;
                if (opened) {
                    windows_.notify("Nouvel evenement");
                    showToast(apps_[item].name);
                }
                return true;
            }

            const bool inMenu = cursorX >= menuRect_.x && cursorY >= menuRect_.y && cursorX < menuRect_.x + menuRect_.w && cursorY < menuRect_.y + menuRect_.h;
            if (!inMenu) {
                startOpen_ = false;
                startPowerOpen_ = false;
                return true;
            }
            return true;
        }

        if (cursorX >= startAbs.x && cursorY >= startAbs.y && cursorX < startAbs.x + startAbs.w && cursorY < startAbs.y + startAbs.h) {
            startOpen_ = true;
            startMenuScroll_ = 0;
            startPowerOpen_ = false;
            return true;
        }

        if (inTaskbar) {
            const int8_t quickHit = hitTaskbarQuickAction(cursorX, cursorY);
            if (quickHit >= 0) {
                WindowKind quickKind = WindowKind::Settings;
                const char* toast = "Settings";
                if (quickHit == 0) {
                    quickKind = WindowKind::WifiManager;
                    toast = "WiFi Manager";
                } else if (quickHit == 1) {
                    quickKind = WindowKind::Settings;
                    toast = "Power settings";
                } else if (quickHit == 2) {
                    quickKind = WindowKind::DateTime;
                    toast = "Date & Time";
                }
                const int8_t appIndex = appIndexForKind(quickKind);
                if (appIndex >= 0 && openWindowForApp(static_cast<uint8_t>(appIndex))) {
                    quickActionPulseIndex_ = quickHit;
                    quickActionPulseUntilMs_ = millis() + 240U;
                    showToast(toast);
                }
                return true;
            }

            const int8_t taskHit = hitTaskbarItem(cursorX, cursorY);
            if (taskHit >= 0) {
                const uint8_t targetId = static_cast<uint8_t>(taskHit);
                windows_.restoreWindowById(targetId);
                windows_.focusWindowById(targetId);
            }
            return true;
        }

        const int8_t icon = hitIcon(cursorX, cursorY);
        if (icon >= 0) {
            selectedIcon_ = static_cast<uint8_t>(icon);
            const WindowKind kind = desktopSlots_[icon];
            const int8_t appIndex = appIndexForKind(kind);
            if (appIndex >= 0) {
                openWindowForApp(static_cast<uint8_t>(appIndex));
            }
            return true;
        }
    }

    if (event.type == katux::core::EventType::ButtonLong && event.source == katux::core::EventSource::ButtonA) {
        const int16_t taskbarTop = static_cast<int16_t>(startRect_.y - 4);
        if (startOpen_) {
            startOpen_ = false;
            startPowerOpen_ = false;
            return true;
        }
        if (cursorY < taskbarTop && hitIcon(cursorX, cursorY) < 0 && !windows_.hasWindowAt(cursorX, cursorY)) {
            int16_t mx = cursorX;
            int16_t my = cursorY;
            if (mx + contextMenuRect_.w > 238) mx = static_cast<int16_t>(238 - contextMenuRect_.w);
            if (mx < 2) mx = 2;
            if (my + contextMenuRect_.h > taskbarTop - 1) my = static_cast<int16_t>(taskbarTop - 1 - contextMenuRect_.h);
            if (my < 2) my = 2;
            contextMenuRect_.x = mx;
            contextMenuRect_.y = my;
            contextMenuOpen_ = true;
            return true;
        }
    }

    windows_.onEvent(event, cursorX, cursorY);

    if (event.type == katux::core::EventType::ButtonDouble && event.source == katux::core::EventSource::ButtonA) {
        for (uint8_t step = 0; step < kDesktopSlotCount; ++step) {
            selectedIcon_ = static_cast<uint8_t>((selectedIcon_ + 1U) % kDesktopSlotCount);
            const int8_t appIndex = appIndexForKind(desktopSlots_[selectedIcon_]);
            if (appIndex >= 0) {
                showToast(apps_[appIndex].name);
                break;
            }
        }
        return true;
    }

    if (event.type == katux::core::EventType::SettingChanged) {
        windows_.notify("Setting changed");
        showToast("Setting updated");
        return true;
    }

    if (event.type == katux::core::EventType::ButtonDouble ||
        event.type == katux::core::EventType::ButtonLong ||
        event.type == katux::core::EventType::ButtonUp ||
        event.type == katux::core::EventType::DragStart ||
        event.type == katux::core::EventType::DragMove ||
        event.type == katux::core::EventType::DragEnd ||
        event.type == katux::core::EventType::FocusNext ||
        event.type == katux::core::EventType::WindowMinimized ||
        event.type == katux::core::EventType::WindowRestored ||
        event.type == katux::core::EventType::SettingChanged) {
        return true;
    }

    return false;
}

int8_t Desktop::appIndexForKind(WindowKind kind) const {
    for (uint8_t i = 0; i < appCount_; ++i) {
        if (apps_[i].kind == kind) return static_cast<int8_t>(i);
    }
    return -1;
}

void Desktop::refreshDesktopConfig() {
    windows_.getDesktopConfig(desktopShowIcons_, desktopSlots_, kDesktopSlotCount);
}

int8_t Desktop::hitIcon(int16_t x, int16_t y) const {
    if (!desktopShowIcons_) return -1;
    for (uint8_t slot = 0; slot < kDesktopSlotCount; ++slot) {
        const int8_t appIndex = appIndexForKind(desktopSlots_[slot]);
        if (appIndex < 0) continue;
        const Rect& r = apps_[appIndex].icon;
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
            return static_cast<int8_t>(slot);
        }
    }
    return -1;
}

int8_t Desktop::hitStartMenuItem(int16_t x, int16_t y) const {
    if (x < menuRect_.x || y < menuRect_.y || x >= menuRect_.x + menuRect_.w || y >= menuRect_.y + menuRect_.h) {
        return -1;
    }

    const Rect sideBar{static_cast<int16_t>(menuRect_.x + 2), static_cast<int16_t>(menuRect_.y + 2), 28, static_cast<int16_t>(menuRect_.h - 4)};
    const Rect powerBtn{static_cast<int16_t>(sideBar.x + 3), static_cast<int16_t>(sideBar.y + sideBar.h - 16), 22, 12};
    if (x >= powerBtn.x && y >= powerBtn.y && x < powerBtn.x + powerBtn.w && y < powerBtn.y + powerBtn.h) {
        return -10;
    }

    const Rect appsPane{static_cast<int16_t>(menuRect_.x + 34), static_cast<int16_t>(menuRect_.y + 2), static_cast<int16_t>(menuRect_.w - 36), static_cast<int16_t>(menuRect_.h - 4)};
    const Rect upBtn{static_cast<int16_t>(appsPane.x + appsPane.w - 14), static_cast<int16_t>(appsPane.y + 16), 10, 10};
    const Rect downBtn{static_cast<int16_t>(appsPane.x + appsPane.w - 14), static_cast<int16_t>(appsPane.y + appsPane.h - 14), 10, 10};
    if (x >= upBtn.x && y >= upBtn.y && x < upBtn.x + upBtn.w && y < upBtn.y + upBtn.h) {
        return -2;
    }
    if (x >= downBtn.x && y >= downBtn.y && x < downBtn.x + downBtn.w && y < downBtn.y + downBtn.h) {
        return -3;
    }

    if (startPowerOpen_) {
        const Rect modal{static_cast<int16_t>(appsPane.x + 16), static_cast<int16_t>(appsPane.y + 30), static_cast<int16_t>(appsPane.w - 24), 52};
        const Rect p0{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 5), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p1{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 16), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p2{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 27), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p3{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 38), static_cast<int16_t>(modal.w - 8), 10};
        if (x >= p0.x && y >= p0.y && x < p0.x + p0.w && y < p0.y + p0.h) return -11;
        if (x >= p1.x && y >= p1.y && x < p1.x + p1.w && y < p1.y + p1.h) return -12;
        if (x >= p2.x && y >= p2.y && x < p2.x + p2.w && y < p2.y + p2.h) return -13;
        if (x >= p3.x && y >= p3.y && x < p3.x + p3.w && y < p3.y + p3.h) return -14;
        return -15;
    }

    const uint8_t totalRows = appCount_;
    const uint8_t visibleRows = 7;
    const uint8_t start = startMenuScroll_;
    const uint8_t end = static_cast<uint8_t>((start + visibleRows) > totalRows ? totalRows : (start + visibleRows));

    uint8_t row = 0;
    for (uint8_t i = start; i < end; ++i, ++row) {
        const Rect item{static_cast<int16_t>(appsPane.x + 6), static_cast<int16_t>(appsPane.y + 18 + row * 12), static_cast<int16_t>(appsPane.w - 22), 10};
        if (x >= item.x && y >= item.y && x < item.x + item.w && y < item.y + item.h) {
            return static_cast<int8_t>(i);
        }
    }

    return -1;
}

int8_t Desktop::hitTaskbarItem(int16_t x, int16_t y) const {
    WindowTaskItem items[WindowManager::kMaxWindows]{};
    const uint8_t count = windows_.listTaskItems(items, WindowManager::kMaxWindows, false);
    const int16_t y0 = 111;
    const int16_t slotsX = 52;
    const int16_t slotsW = 126;
    const int16_t slotW = 22;
    const int16_t slotH = 9;
    const uint8_t perRow = static_cast<uint8_t>(slotsW / (slotW + 2));

    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t row = static_cast<uint8_t>(i / perRow);
        const uint8_t col = static_cast<uint8_t>(i % perRow);
        if (row > 1) break;
        const Rect slot{static_cast<int16_t>(slotsX + col * (slotW + 2)), static_cast<int16_t>(y0 + row * 11), slotW, slotH};
        if (x >= slot.x && y >= slot.y && x < slot.x + slot.w && y < slot.y + slot.h) {
            return static_cast<int8_t>(items[i].id);
        }
    }
    return -1;
}

int8_t Desktop::hitTaskbarQuickAction(int16_t x, int16_t y) const {
    const int16_t y0 = static_cast<int16_t>(startRect_.y - 4);
    const Rect wifiRect{186, static_cast<int16_t>(y0 + 3), 18, 8};
    const Rect batteryRect{206, static_cast<int16_t>(y0 + 3), 30, 8};
    const Rect clockRect{186, static_cast<int16_t>(y0 + 13), 50, 9};

    if (x >= wifiRect.x && y >= wifiRect.y && x < wifiRect.x + wifiRect.w && y < wifiRect.y + wifiRect.h) {
        return 0;
    }
    if (x >= batteryRect.x && y >= batteryRect.y && x < batteryRect.x + batteryRect.w && y < batteryRect.y + batteryRect.h) {
        return 1;
    }
    if (x >= clockRect.x && y >= clockRect.y && x < clockRect.x + clockRect.w && y < clockRect.y + clockRect.h) {
        return 2;
    }
    return -1;
}

int8_t Desktop::hitContextMenuItem(int16_t x, int16_t y) const {
    if (!contextMenuOpen_) return -1;
    if (x < contextMenuRect_.x || y < contextMenuRect_.y || x >= contextMenuRect_.x + contextMenuRect_.w || y >= contextMenuRect_.y + contextMenuRect_.h) {
        return -1;
    }
    const int16_t relativeY = static_cast<int16_t>(y - (contextMenuRect_.y + 2));
    if (relativeY < 0) return -1;
    const int8_t idx = static_cast<int8_t>(relativeY / kContextMenuItemHeight);
    if (idx < 0 || idx >= static_cast<int8_t>(kContextMenuItemCount)) return -1;
    return idx;
}

bool Desktop::openWindowForApp(uint8_t appIndex) {
    if (appIndex >= appCount_) return false;

    int8_t& winId = appWindowIds_[appIndex];
    if (winId >= 0) {
        if (windows_.focusWindowById(static_cast<uint8_t>(winId)) || windows_.restoreWindowById(static_cast<uint8_t>(winId))) {
            return true;
        }
        winId = -1;
    }

    WindowKind kind = apps_[appIndex].kind;
    if (kind == WindowKind::Demo) {
        winId = windows_.createWindow("Demo", 46, 16, 158, 98, true, kind);
    } else if (kind == WindowKind::Settings) {
        winId = windows_.createWindow("Settings", 30, 16, 176, 118, true, kind);
    } else if (kind == WindowKind::TaskManager) {
        winId = windows_.createWindow("Task Manager", 20, 8, 198, 118, true, kind);
    } else if (kind == WindowKind::Explorer) {
        winId = windows_.createWindow("Explorer", 18, 8, 202, 118, true, kind);
    } else if (kind == WindowKind::Notepad) {
        winId = windows_.createWindow("Notepad", 16, 8, 208, 118, true, kind);
    } else if (kind == WindowKind::AppHub) {
        winId = windows_.createWindow("Applications", 12, 8, 216, 120, true, kind);
    } else if (kind == WindowKind::GamePixel) {
        winId = windows_.createWindow("Pixel Snake", 22, 8, 196, 120, true, kind);
    } else if (kind == WindowKind::GameOrbit) {
        winId = windows_.createWindow("Orbit Pong", 22, 8, 196, 120, true, kind);
    } else if (kind == WindowKind::GamePlinko) {
        winId = windows_.createWindow("Plinko Rush", 22, 8, 196, 120, true, kind);
    } else if (kind == WindowKind::Notifications) {
        winId = windows_.createWindow("Notifications", 24, 10, 192, 114, true, kind);
    } else if (kind == WindowKind::WifiManager) {
        winId = windows_.createWindow("WiFi Manager", 12, 6, 216, 124, true, kind);
    } else if (kind == WindowKind::Browser) {
        winId = windows_.createWindow("Navigator", 10, 6, 220, 124, true, kind);
    } else if (kind == WindowKind::DateTime) {
        winId = windows_.createWindow("Date and Time", 14, 8, 212, 122, true, kind);
    } else if (kind == WindowKind::Reboot) {
        winId = windows_.createWindow("Reboot", 22, 12, 196, 104, true, kind);
    } else {
        winId = windows_.createWindow(apps_[appIndex].name, 36, 18, 164, 98, true, kind);
    }

    return winId >= 0;
}

void Desktop::showToast(const char* text, uint16_t durationMs) {
    if (!text) return;
    strlcpy(toast_, text, sizeof(toast_));
    toastUntilMs_ = millis() + durationMs;
}

void Desktop::emit(katux::core::EventType type, int32_t d0, int32_t d1) {
    if (!events_) return;
    katux::core::Event e;
    e.type = type;
    e.source = katux::core::EventSource::Desktop;
    e.timestampMs = millis();
    e.data0 = d0;
    e.data1 = d1;
    events_->push(e);
}

void Desktop::renderWallpaper(Renderer& renderer, const Rect& clip) {
    const int16_t top = clip.y;
    const int16_t bottom = static_cast<int16_t>(clip.y + clip.h);

    for (int16_t y = top; y < bottom; ++y) {
        uint16_t c;
        if (y < 76) {
            const uint16_t g = static_cast<uint16_t>((y * 20) / 76);
            c = static_cast<uint16_t>(0x03BF + (g << 5));
        } else {
            const int16_t gy = static_cast<int16_t>(y - 76);
            const uint16_t g = static_cast<uint16_t>((gy * 18) / 59);
            c = static_cast<uint16_t>(0x1C80 + (g << 5));
        }
        renderer.fillRect({clip.x, y, clip.w, 1}, c);
    }

    const Rect hillBack{-20, 72, 210, 70};
    const Rect hillFront{52, 76, 220, 66};
    if (intersects(clip, hillBack)) {
        for (int16_t y = hillBack.y; y < hillBack.y + hillBack.h; ++y) {
            if (y < clip.y || y >= clip.y + clip.h) continue;
            const int16_t dx = static_cast<int16_t>((y - hillBack.y) * 2);
            const int16_t x0 = static_cast<int16_t>(hillBack.x - dx / 2);
            const int16_t w = static_cast<int16_t>(hillBack.w + dx);
            renderer.fillRect({x0, y, w, 1}, 0x2D63);
        }
    }
    if (intersects(clip, hillFront)) {
        for (int16_t y = hillFront.y; y < hillFront.y + hillFront.h; ++y) {
            if (y < clip.y || y >= clip.y + clip.h) continue;
            const int16_t dx = static_cast<int16_t>((y - hillFront.y) * 3);
            const int16_t x0 = static_cast<int16_t>(hillFront.x - dx / 3);
            const int16_t w = static_cast<int16_t>(hillFront.w + dx);
            renderer.fillRect({x0, y, w, 1}, 0x3666);
        }
    }

    const Rect cloud1{18, 14, 44, 12};
    const Rect cloud2{108, 10, 52, 14};
    const Rect cloud3{176, 22, 36, 10};
    if (intersects(clip, cloud1)) {
        renderer.fillRect(cloud1, 0xFFFF);
        renderer.fillRect({static_cast<int16_t>(cloud1.x + 8), static_cast<int16_t>(cloud1.y - 4), 20, 6}, 0xFFFF);
    }
    if (intersects(clip, cloud2)) {
        renderer.fillRect(cloud2, 0xFFFF);
        renderer.fillRect({static_cast<int16_t>(cloud2.x + 10), static_cast<int16_t>(cloud2.y - 4), 24, 6}, 0xFFFF);
    }
    if (intersects(clip, cloud3)) {
        renderer.fillRect(cloud3, 0xFFFF);
        renderer.fillRect({static_cast<int16_t>(cloud3.x + 8), static_cast<int16_t>(cloud3.y - 3), 14, 5}, 0xFFFF);
    }
}

bool Desktop::render(Renderer& renderer, bool fullRedraw, int16_t cursorX, int16_t cursorY) {
    if (!initialized_) return false;

    const uint32_t now = millis();

    if (toastUntilMs_ > 0 && now > toastUntilMs_) {
        const Rect toastRect{68, static_cast<int16_t>(renderer.height() - 35), 112, 10};
        compositor_.invalidate(toastRect);
        toast_[0] = '\0';
        toastUntilMs_ = 0;
    }

    if (quickActionPulseUntilMs_ > 0 && now > quickActionPulseUntilMs_) {
        quickActionPulseUntilMs_ = 0;
        quickActionPulseIndex_ = -1;
        compositor_.invalidate({182, 111, 58, 24});
    }

    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
    const int8_t quickHover = hitTaskbarQuickAction(cursorX, cursorY);
    if (quickHover != quickActionHoverIndex_) {
        quickActionHoverIndex_ = quickHover;
        compositor_.invalidate({182, 111, 58, 24});
    }

    if (fullRedraw) {
        compositor_.invalidateAll();
    }

    Rect winDirty[WindowManager::kDirtyCapacity]{};
    const uint8_t winDirtyCount = windows_.consumeDirtyRegions(winDirty, WindowManager::kDirtyCapacity);
    for (uint8_t i = 0; i < winDirtyCount; ++i) {
        compositor_.invalidate(winDirty[i]);
    }

    const Rect cursorRect{cursorX, cursorY, 10, 14};
    bool cursorAreaTouched = fullRedraw;

    Rect dirty[Compositor::kMaxDirtyRects]{};
    const uint8_t dirtyCount = compositor_.consume(dirty, Compositor::kMaxDirtyRects);
    for (uint8_t i = 0; i < dirtyCount; ++i) {
        if (!cursorAreaTouched && intersects(dirty[i], cursorRect)) {
            cursorAreaTouched = true;
        }
        renderRegion(renderer, dirty[i]);
    }
    return cursorAreaTouched;
}

void Desktop::renderClip(Renderer& renderer, const Rect& clip) {
    if (!initialized_) return;
    renderRegion(renderer, clip);
}

void Desktop::renderRegion(Renderer& renderer, const Rect& clip) {
    if (windows_.hasCapturedApp()) {
        windows_.render(renderer, theme(), &clip);
        return;
    }

    renderWallpaper(renderer, clip);
    renderIcons(renderer, &clip);
    windows_.render(renderer, theme(), &clip);
    renderTaskbar(renderer, &clip);
    renderStartMenu(renderer, &clip);
    renderContextMenu(renderer, &clip);
}

void Desktop::renderIcons(Renderer& renderer, const Rect* clip) {
    if (desktopShowIcons_) {
        for (uint8_t slot = 0; slot < kDesktopSlotCount; ++slot) {
            const int8_t appIndex = appIndexForKind(desktopSlots_[slot]);
            if (appIndex < 0) continue;
            const AppEntry& app = apps_[appIndex];
            const bool selected = selectedIcon_ == slot;
            const bool hovered = hoverIcon_ == static_cast<int8_t>(slot);
            const uint16_t frame = selected ? 0xFFFF : (hovered ? 0xBDF7 : 0x6B4D);
            const uint16_t bg = selected ? 0x2B74 : (hovered ? 0x45D8 : 0x5CF4);
            if (!clip || intersects(*clip, app.icon)) {
                renderer.fillRect(app.icon, bg);
                renderer.drawRect(app.icon, frame);
                if (app.accent == 0) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 2), static_cast<int16_t>(app.icon.y + 4), 18, 3}, 0xFFE0);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 2), static_cast<int16_t>(app.icon.y + 9), 14, 6}, 0x001F);
                } else if (app.accent == 1) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 3), static_cast<int16_t>(app.icon.y + 5), 14, 2}, 0xC618);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 3), static_cast<int16_t>(app.icon.y + 10), 14, 2}, 0xC618);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 3), static_cast<int16_t>(app.icon.y + 15), 14, 2}, 0xC618);
                } else if (app.accent == 2) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 5), static_cast<int16_t>(app.icon.y + 13), 3, 4}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 10), static_cast<int16_t>(app.icon.y + 10), 3, 7}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 15), static_cast<int16_t>(app.icon.y + 7), 3, 10}, 0xFFFF);
                } else if (app.accent == 3) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 3), static_cast<int16_t>(app.icon.y + 8), 14, 8}, 0xFFE0);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 3), static_cast<int16_t>(app.icon.y + 6), 6, 2}, 0xFFE0);
                } else if (app.accent == 4) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 4), static_cast<int16_t>(app.icon.y + 8), 12, 8}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 8), static_cast<int16_t>(app.icon.y + 5), 4, 3}, 0xFFFF);
                } else if (app.accent == 6) {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 4), static_cast<int16_t>(app.icon.y + 4), 6, 5}, 0xFFE0);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 11), static_cast<int16_t>(app.icon.y + 4), 8, 10}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 12), static_cast<int16_t>(app.icon.y + 6), 6, 1}, 0x3186);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 12), static_cast<int16_t>(app.icon.y + 9), 6, 1}, 0x3186);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 12), static_cast<int16_t>(app.icon.y + 12), 4, 1}, 0x3186);
                } else if (app.accent == 11) {
                    renderer.drawRect({static_cast<int16_t>(app.icon.x + 4), static_cast<int16_t>(app.icon.y + 5), 12, 12}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 10), static_cast<int16_t>(app.icon.y + 7), 1, 5}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 10), static_cast<int16_t>(app.icon.y + 10), 3, 1}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 9), static_cast<int16_t>(app.icon.y + 3), 3, 2}, 0xFFFF);
                } else {
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 5), static_cast<int16_t>(app.icon.y + 14), 10, 2}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 7), static_cast<int16_t>(app.icon.y + 11), 6, 2}, 0xFFFF);
                    renderer.fillRect({static_cast<int16_t>(app.icon.x + 9), static_cast<int16_t>(app.icon.y + 8), 2, 2}, 0xFFFF);
                }
            }
            if (!clip || intersects(*clip, app.label)) {
                const uint16_t labelBg = selected ? 0x2B74 : (hovered ? 0x45D8 : 0x2B74);
                renderer.drawText(app.label.x, app.label.y, app.name, 0xFFFF, labelBg);
            }
        }
    }

    if (safeMode_) {
        const Rect safeTag{8, 2, 62, 11};
        if (!clip || intersects(*clip, safeTag)) {
            renderer.fillRect(safeTag, 0x8000);
            renderer.drawText(10, 4, "SAFE MODE", 0xFFFF, 0x8000);
        }
    }
}

void Desktop::renderTaskbar(Renderer& renderer, const Rect* clip) {
    if (windows_.hasCapturedApp()) {
        return;
    }

    const int16_t y = renderer.height() - 24;
    const Rect taskbar{0, y, renderer.width(), 24};
    startRect_ = {2, static_cast<int16_t>(y + 4), 48, 16};

    if (clip && !intersects(*clip, taskbar)) {
        return;
    }

    renderer.fillRect(taskbar, 0x1082);
    renderer.fillRect({0, y, renderer.width(), 1}, 0xBDF7);
    renderer.fillRect({182, y, 58, 24}, 0x18C3);

    renderer.fillRect(startRect_, startOpen_ ? 0x1A5F : 0x07A8);
    renderer.drawRect(startRect_, 0xFFFF);
    renderer.drawText(11, y + 10, "Start", 0xFFFF, startOpen_ ? 0x1A5F : 0x07A8);

    WindowTaskItem items[WindowManager::kMaxWindows]{};
    const uint8_t count = windows_.listTaskItems(items, WindowManager::kMaxWindows, false);
    const int16_t slotsX = 52;
    const int16_t slotW = 22;
    const int16_t slotH = 9;
    const uint8_t perRow = 5;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t row = static_cast<uint8_t>(i / perRow);
        const uint8_t col = static_cast<uint8_t>(i % perRow);
        if (row > 1) break;
        const Rect slot{static_cast<int16_t>(slotsX + col * (slotW + 2)), static_cast<int16_t>(y + 3 + row * 11), slotW, slotH};
        const uint16_t bg = items[i].focused ? 0x07E0 : (items[i].minimized ? 0x39E7 : 0x5AEB);
        renderer.fillRect(slot, bg);
        renderer.drawRect(slot, 0xFFFF);

        const int16_t ix = static_cast<int16_t>(slot.x + 7);
        const int16_t iy = static_cast<int16_t>(slot.y + 1);
        if (items[i].kind == WindowKind::Settings) {
            renderer.fillRect({static_cast<int16_t>(ix + 2), iy, 2, 6}, 0x0000);
            renderer.fillRect({ix, static_cast<int16_t>(iy + 2), 6, 2}, 0x0000);
        } else if (items[i].kind == WindowKind::Explorer) {
            renderer.fillRect({ix, static_cast<int16_t>(iy + 2), 7, 4}, 0x0000);
            renderer.fillRect({ix, static_cast<int16_t>(iy + 1), 3, 1}, 0x0000);
        } else if (items[i].kind == WindowKind::TaskManager) {
            renderer.fillRect({ix, static_cast<int16_t>(iy + 4), 1, 2}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 2), static_cast<int16_t>(iy + 2), 1, 4}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 4), iy, 1, 6}, 0x0000);
        } else if (items[i].kind == WindowKind::Browser) {
            renderer.drawRect({ix, iy, 6, 6}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 2), static_cast<int16_t>(iy + 2), 2, 2}, 0x0000);
        } else if (items[i].kind == WindowKind::WifiManager) {
            renderer.fillRect({ix, static_cast<int16_t>(iy + 5), 6, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 1), static_cast<int16_t>(iy + 3), 4, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 2), static_cast<int16_t>(iy + 1), 2, 1}, 0x0000);
        } else if (items[i].kind == WindowKind::DateTime) {
            renderer.drawRect({ix, iy, 6, 6}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 3), static_cast<int16_t>(iy + 1), 1, 3}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 3), static_cast<int16_t>(iy + 3), 2, 1}, 0x0000);
        } else if (items[i].kind == WindowKind::GamePixel || items[i].kind == WindowKind::GameOrbit || items[i].kind == WindowKind::GamePlinko) {
            renderer.drawRect({ix, iy, 6, 6}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 2), static_cast<int16_t>(iy + 2), 2, 2}, 0x0000);
        } else if (items[i].kind == WindowKind::Notepad) {
            renderer.fillRect({ix, iy, 6, 6}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(ix + 1), static_cast<int16_t>(iy + 1), 4, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(ix + 1), static_cast<int16_t>(iy + 3), 4, 1}, 0x0000);
        } else {
            renderer.fillRect({ix, iy, 6, 6}, 0x0000);
        }
    }

    const bool pulseActive = quickActionPulseUntilMs_ > millis();
    const Rect wifiRect{186, static_cast<int16_t>(y + 3), 18, 8};
    const Rect batteryRect{206, static_cast<int16_t>(y + 3), 30, 8};
    const Rect clockRect{186, static_cast<int16_t>(y + 13), 50, 9};

    const bool wifiPulse = pulseActive && quickActionPulseIndex_ == 0;
    const bool batteryPulse = pulseActive && quickActionPulseIndex_ == 1;
    const bool clockPulse = pulseActive && quickActionPulseIndex_ == 2;
    const bool wifiHover = quickActionHoverIndex_ == 0;
    const bool batteryHover = quickActionHoverIndex_ == 1;
    const bool clockHover = quickActionHoverIndex_ == 2;

    const bool wifiOn = WiFi.status() == WL_CONNECTED;
    const uint16_t wifiBg = wifiPulse ? 0xA7E0 : (wifiHover ? 0x56F0 : (wifiOn ? 0x07E0 : 0x6B4D));
    renderer.fillRect(wifiRect, wifiBg);
    renderer.drawRect(wifiRect, (wifiPulse || wifiHover) ? 0xFFFF : 0xBDF7);
    renderer.fillRect({188, static_cast<int16_t>(y + 9), 2, 1}, 0x0000);
    renderer.fillRect({191, static_cast<int16_t>(y + 7), 2, 3}, 0x0000);
    renderer.fillRect({194, static_cast<int16_t>(y + 5), 2, 5}, 0x0000);
    renderer.fillRect({197, static_cast<int16_t>(y + 3), 2, 7}, 0x0000);

    int level = StickCP2.Power.getBatteryLevel();
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    const uint16_t batBg = batteryPulse ? 0xFFE0 : (batteryHover ? 0x7BEF : (level < 20 ? 0xF800 : 0x39E7));
    renderer.fillRect(batteryRect, batBg);
    renderer.drawRect(batteryRect, (batteryPulse || batteryHover) ? 0xFFFF : 0xBDF7);
    char bat[8] = "0%";
    snprintf(bat, sizeof(bat), "%d%%", level);
    renderer.drawText(209, y + 4, bat, 0x0000, batBg);

    time_t nowTs = time(nullptr);
    struct tm tmv;
    char hm[8] = "--:--";
    if (nowTs > 1000 && localtime_r(&nowTs, &tmv)) {
        strftime(hm, sizeof(hm), "%H:%M", &tmv);
    }
    const uint16_t clockBg = clockPulse ? 0x4C9F : (clockHover ? 0x3AAF : 0x31A6);
    renderer.fillRect(clockRect, clockBg);
    renderer.drawRect(clockRect, (clockPulse || clockHover) ? 0xFFFF : 0x7BEF);
    renderer.drawText(195, y + 14, hm, 0xFFFF, clockBg);

    if (toast_[0] != '\0' && toastUntilMs_ > millis()) {
        const Rect toastRect{68, static_cast<int16_t>(y - 11), 112, 10};
        renderer.fillRect(toastRect, 0x18C3);
        renderer.drawRect(toastRect, 0xFFFF);
        renderer.drawText(static_cast<int16_t>(toastRect.x + 3), static_cast<int16_t>(toastRect.y + 2), toast_, 0xFFFF, 0x18C3);
    }
}

void Desktop::renderStartMenu(Renderer& renderer, const Rect* clip) {
    if (!startOpen_) return;

    if (clip && !intersects(*clip, menuRect_)) {
        return;
    }

    renderWallpaper(renderer, menuRect_);
    renderer.fillRect(menuRect_, 0x18C3);
    renderer.drawRect(menuRect_, 0xFFFF);

    const Rect sideBar{static_cast<int16_t>(menuRect_.x + 2), static_cast<int16_t>(menuRect_.y + 2), 28, static_cast<int16_t>(menuRect_.h - 4)};
    renderer.fillRect(sideBar, 0x1082);
    renderer.drawRect(sideBar, 0x7BEF);
    renderer.fillRect({static_cast<int16_t>(sideBar.x + 4), static_cast<int16_t>(sideBar.y + 4), 20, 10}, 0x1A5F);
    renderer.drawText(static_cast<int16_t>(sideBar.x + 7), static_cast<int16_t>(sideBar.y + 6), "K", 0xFFFF, 0x1A5F);

    const Rect powerBtn{static_cast<int16_t>(sideBar.x + 3), static_cast<int16_t>(sideBar.y + sideBar.h - 16), 22, 12};
    renderer.fillRect(powerBtn, startPowerOpen_ ? 0xF8E4 : 0x2965);
    renderer.drawRect(powerBtn, 0x7BEF);
    renderer.drawText(static_cast<int16_t>(powerBtn.x + 5), static_cast<int16_t>(powerBtn.y + 2), "PWR", 0xFFFF, startPowerOpen_ ? 0xF8E4 : 0x2965);

    const Rect appsPane{static_cast<int16_t>(menuRect_.x + 34), static_cast<int16_t>(menuRect_.y + 2), static_cast<int16_t>(menuRect_.w - 36), static_cast<int16_t>(menuRect_.h - 4)};
    renderer.fillRect(appsPane, 0x2145);
    renderer.drawRect(appsPane, 0x7BEF);
    renderer.fillRect({static_cast<int16_t>(appsPane.x + 2), static_cast<int16_t>(appsPane.y + 2), static_cast<int16_t>(appsPane.w - 4), 12}, 0x1A5F);
    renderer.drawText(static_cast<int16_t>(appsPane.x + 6), static_cast<int16_t>(appsPane.y + 5), "Applications", 0xFFFF, 0x1A5F);

    const Rect upBtn{static_cast<int16_t>(appsPane.x + appsPane.w - 14), static_cast<int16_t>(appsPane.y + 16), 10, 10};
    const Rect downBtn{static_cast<int16_t>(appsPane.x + appsPane.w - 14), static_cast<int16_t>(appsPane.y + appsPane.h - 14), 10, 10};
    renderer.fillRect(upBtn, 0x5AEB);
    renderer.drawText(upBtn.x + 3, upBtn.y + 1, "^", 0xFFFF, 0x5AEB);
    renderer.fillRect(downBtn, 0x5AEB);
    renderer.drawText(downBtn.x + 3, downBtn.y + 1, "v", 0xFFFF, 0x5AEB);

    const uint8_t totalRows = appCount_;
    const uint8_t visibleRows = 7;
    const uint8_t start = startMenuScroll_;
    const uint8_t end = static_cast<uint8_t>((start + visibleRows) > totalRows ? totalRows : (start + visibleRows));

    uint8_t row = 0;
    for (uint8_t idx = start; idx < end; ++idx, ++row) {
        const Rect r{static_cast<int16_t>(appsPane.x + 6), static_cast<int16_t>(appsPane.y + 18 + row * 12), static_cast<int16_t>(appsPane.w - 22), 10};
        const uint16_t bg = idx == selectedIcon_ ? 0x5AEB : 0x31A6;
        renderer.fillRect(r, bg);
        renderer.drawRect(r, 0x6B4D);
        renderer.fillRect({static_cast<int16_t>(r.x + 2), static_cast<int16_t>(r.y + 2), 6, 6}, 0xFFFF);
        renderer.drawText(static_cast<int16_t>(r.x + 11), static_cast<int16_t>(r.y + 1), apps_[idx].name, 0xFFFF, bg);
    }

    if (startPowerOpen_) {
        renderer.fillRect(appsPane, 0x4208);
        const Rect modal{static_cast<int16_t>(appsPane.x + 16), static_cast<int16_t>(appsPane.y + 30), static_cast<int16_t>(appsPane.w - 24), 52};
        renderer.fillRect(modal, 0x18C3);
        renderer.drawRect(modal, 0xFFFF);
        renderer.drawText(static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 2), "Power", 0xFFFF, 0x18C3);

        const Rect p0{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 5), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p1{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 16), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p2{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 27), static_cast<int16_t>(modal.w - 8), 10};
        const Rect p3{static_cast<int16_t>(modal.x + 4), static_cast<int16_t>(modal.y + 38), static_cast<int16_t>(modal.w - 8), 10};
        renderer.fillRect(p0, 0x2965);
        renderer.fillRect(p1, 0x2965);
        renderer.fillRect(p2, 0x2965);
        renderer.fillRect(p3, 0x2965);
        renderer.drawText(static_cast<int16_t>(p0.x + 3), static_cast<int16_t>(p0.y + 1), "Shutdown", 0xFFFF, 0x2965);
        renderer.drawText(static_cast<int16_t>(p1.x + 3), static_cast<int16_t>(p1.y + 1), "Restart", 0xFFFF, 0x2965);
        renderer.drawText(static_cast<int16_t>(p2.x + 3), static_cast<int16_t>(p2.y + 1), "Sleep", 0xFFFF, 0x2965);
        renderer.drawText(static_cast<int16_t>(p3.x + 3), static_cast<int16_t>(p3.y + 1), "Deep sleep", 0xFFFF, 0x2965);
    }
}

void Desktop::renderContextMenu(Renderer& renderer, const Rect* clip) {
    if (!contextMenuOpen_) return;

    const Rect taskbarMask{0, 0, 240, static_cast<int16_t>(startRect_.y - 4)};
    if (clip && (!intersects(*clip, contextMenuRect_) || !intersects(*clip, taskbarMask))) {
        return;
    }

    renderer.fillRect(contextMenuRect_, 0x18C3);
    renderer.drawRect(contextMenuRect_, 0xFFFF);

    const char* labels[kContextMenuItemCount] = {
        "Toggle desktop icons",
        "Refresh",
        "Clean up desktop",
        "Desktop app",
        "Open Start menu"
    };

    for (uint8_t i = 0; i < kContextMenuItemCount; ++i) {
        const Rect row{contextMenuRect_.x + 1, static_cast<int16_t>(contextMenuRect_.y + 2 + i * kContextMenuItemHeight),
                       static_cast<int16_t>(contextMenuRect_.w - 2), static_cast<int16_t>(kContextMenuItemHeight - 1)};
        const uint16_t rowBg = (i & 1U) ? 0x2145 : 0x2965;
        renderer.fillRect(row, rowBg);

        if (i == 0) {
            const Rect checkbox{static_cast<int16_t>(row.x + 2), static_cast<int16_t>(row.y + 1), 7, 7};
            renderer.fillRect(checkbox, 0x0000);
            renderer.drawRect(checkbox, 0xFFFF);
            if (desktopShowIcons_) {
                renderer.drawText(static_cast<int16_t>(checkbox.x + 1), static_cast<int16_t>(checkbox.y - 1), "x", 0x07E0, 0x0000);
            }
            renderer.drawText(static_cast<int16_t>(row.x + 12), static_cast<int16_t>(row.y + 1), labels[i], 0xFFFF, rowBg);
        } else {
            renderer.drawText(static_cast<int16_t>(row.x + 4), static_cast<int16_t>(row.y + 1), labels[i], 0xFFFF, rowBg);
        }
    }
}

CursorStyle Desktop::cursorStyle() const {
    return windows_.cursorStyle();
}

void Desktop::invalidateAll() {
    compositor_.invalidateAll();
    windows_.invalidateAll();
}

void Desktop::setSystemState(bool darkTheme, uint8_t brightness, uint8_t cursorSpeed, uint8_t performanceProfile, bool debugOverlay, bool animationsEnabled,
                             bool autoTime, int8_t timezoneOffset, uint16_t manualYear, uint8_t manualMonth, uint8_t manualDay, uint8_t manualHour,
                             uint8_t manualMinute) {
    animationsEnabled_ = animationsEnabled;
    windows_.setSystemState(darkTheme, brightness, cursorSpeed, performanceProfile, debugOverlay, animationsEnabled, autoTime, timezoneOffset, manualYear,
                            manualMonth, manualDay, manualHour, manualMinute);
    invalidateAll();
}

void Desktop::setRuntimeStats(uint8_t fps, uint32_t freeHeap, uint32_t uptimeMs, uint8_t queueDepth) {
    windows_.setRuntimeStats(fps, freeHeap, uptimeMs, queueDepth);
}

}
