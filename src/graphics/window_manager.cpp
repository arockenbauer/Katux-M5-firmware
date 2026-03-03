#include "window_manager.h"

#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <lgfx/v1/misc/DataWrapper.hpp>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace katux::graphics {

static bool intersects(const Rect& a, const Rect& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

static const char* kAutoWifiSsid = "Seychelles";
static const char* kAutoWifiPassword = "Sylvestre87!wifi";
static const char* kBrowserPresets[] = {
    "https://example.com/",
    "http://neverssl.com/",
    "https://info.cern.ch/"
};
static constexpr uint8_t kBrowserPresetCount = sizeof(kBrowserPresets) / sizeof(kBrowserPresets[0]);

struct HubEntry {
    const char* label;
    WindowKind kind;
    uint8_t accent;
};

static const HubEntry kHubEntries[] = {
    {"Settings", WindowKind::Settings, 1},
    {"Explorer", WindowKind::Explorer, 3},
    {"Notepad", WindowKind::Notepad, 7},
    {"Task Mgr", WindowKind::TaskManager, 2},
    {"WiFi", WindowKind::WifiManager, 5},
    {"Browser", WindowKind::Browser, 4},
    {"Desktop", WindowKind::DesktopConfig, 1},
    {"Alerts", WindowKind::Notifications, 0},
    {"Demo", WindowKind::Demo, 6},
    {"Pixel Snake", WindowKind::GamePixel, 8},
    {"Orbit Pong", WindowKind::GameOrbit, 9}
};
static constexpr uint8_t kHubEntryCount = sizeof(kHubEntries) / sizeof(kHubEntries[0]);
static constexpr uint8_t kHubCols = 3;
static constexpr uint8_t kHubRows = 2;
static constexpr uint8_t kHubPerPage = kHubCols * kHubRows;

static void copyTrim(char* dst, size_t dstLen, const char* src, size_t maxVisible) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n > maxVisible) n = maxVisible;
    if (n >= dstLen) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char* stripUrlScheme(const char* url) {
    if (!url) return "";
    if (strncmp(url, "http://", 7) == 0) return url + 7;
    if (strncmp(url, "https://", 8) == 0) return url + 8;
    return url;
}

static bool isGameKind(WindowKind kind) {
    return kind == WindowKind::GamePixel || kind == WindowKind::GameOrbit;
}

static bool startsWith(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool isHttpUrl(const char* s) {
    return startsWith(s, "http://") || startsWith(s, "https://");
}

class StreamDataWrapper : public lgfx::v1::DataWrapper {
   public:
    StreamDataWrapper(Stream* stream, uint32_t length, HTTPClient* httpClient = nullptr)
        : stream_(stream), length_(length), index_(0), httpClient_(httpClient) {}

    int read(uint8_t* buf, uint32_t len) override {
        if (!stream_ || !buf || len == 0) return 0;
        if (length_ != 0xFFFFFFFFu && len > length_ - index_) {
            len = length_ - index_;
        }
        if (len == 0) return 0;
        if (httpClient_) {
            while (httpClient_->connected() && stream_->available() == 0 && (length_ == 0xFFFFFFFFu || index_ < length_)) {
                delay(1);
            }
        }
        const size_t readLen = stream_->readBytes(buf, len);
        index_ += static_cast<uint32_t>(readLen);
        return static_cast<int>(readLen);
    }

    void skip(int32_t offset) override {
        if (!stream_ || offset <= 0) return;
        uint8_t dummy[32];
        while (offset > 0) {
            const uint32_t chunk = static_cast<uint32_t>(offset > static_cast<int32_t>(sizeof(dummy)) ? sizeof(dummy) : offset);
            const int n = read(dummy, chunk);
            if (n <= 0) return;
            offset -= n;
        }
    }

    bool seek(uint32_t offset) override {
        if (offset < index_) return false;
        skip(static_cast<int32_t>(offset - index_));
        return true;
    }

    void close() override {}

    int32_t tell(void) override {
        return static_cast<int32_t>(index_);
    }

   private:
    Stream* stream_ = nullptr;
    uint32_t length_ = 0;
    uint32_t index_ = 0;
    HTTPClient* httpClient_ = nullptr;
};

class FileDataWrapper : public lgfx::v1::DataWrapper {
   public:
    explicit FileDataWrapper(fs::File* file) : file_(file) {
        need_transaction = true;
    }

    int read(uint8_t* buf, uint32_t len) override {
        if (!file_ || !buf || len == 0) return 0;
        return static_cast<int>(file_->read(buf, len));
    }

    void skip(int32_t offset) override {
        if (!file_ || offset == 0) return;
        const int32_t pos = static_cast<int32_t>(file_->position());
        const int32_t target = pos + offset;
        if (target < 0) return;
        file_->seek(static_cast<size_t>(target), fs::SeekSet);
    }

    bool seek(uint32_t offset) override {
        if (!file_) return false;
        return file_->seek(static_cast<size_t>(offset), fs::SeekSet);
    }

    void close() override {
        if (file_) file_->close();
    }

    int32_t tell(void) override {
        if (!file_) return 0;
        return static_cast<int32_t>(file_->position());
    }

   private:
    fs::File* file_ = nullptr;
};

static bool drawEncodedWithWrapper(bool isJpeg, lgfx::v1::DataWrapper& wrapper, const Rect& rect) {
    if (isJpeg) {
        return StickCP2.Display.drawJpg(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
    }
    return StickCP2.Display.drawPng(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
}

static bool detectImageFormat(const char* src, bool& isJpeg, bool& isPng, bool& isWebp) {
    isJpeg = false;
    isPng = false;
    isWebp = false;
    if (!src || !src[0]) return false;
    const char* dot = strrchr(src, '.');
    if (!dot) return false;
    char ext[8] = "";
    size_t n = 0;
    for (const char* p = dot + 1; *p && *p != '?' && *p != '#' && n + 1 < sizeof(ext); ++p) {
        ext[n++] = static_cast<char>(tolower(*p));
    }
    ext[n] = '\0';
    isJpeg = strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0;
    isPng = strcmp(ext, "png") == 0;
    isWebp = strcmp(ext, "webp") == 0;
    return isJpeg || isPng || isWebp;
}

static const WindowKind kDesktopAssignableKinds[] = {
    WindowKind::Generic,
    WindowKind::AppHub,
    WindowKind::Settings,
    WindowKind::Explorer,
    WindowKind::Notepad,
    WindowKind::WifiManager,
    WindowKind::TaskManager,
    WindowKind::Browser,
    WindowKind::ImageVisualizer,
    WindowKind::DesktopConfig,
    WindowKind::Notifications,
    WindowKind::Demo,
    WindowKind::GamePixel,
    WindowKind::GameOrbit
};
static constexpr uint8_t kDesktopAssignableCount = sizeof(kDesktopAssignableKinds) / sizeof(kDesktopAssignableKinds[0]);

static const char* desktopKindLabel(WindowKind kind) {
    if (kind == WindowKind::Generic) return "None";
    if (kind == WindowKind::AppHub) return "Apps";
    if (kind == WindowKind::Settings) return "Settings";
    if (kind == WindowKind::Explorer) return "Files";
    if (kind == WindowKind::Notepad) return "Notes";
    if (kind == WindowKind::WifiManager) return "WiFi";
    if (kind == WindowKind::TaskManager) return "Tasks";
    if (kind == WindowKind::Browser) return "Browser";
    if (kind == WindowKind::ImageVisualizer) return "Image viewer";
    if (kind == WindowKind::DesktopConfig) return "Desktop";
    if (kind == WindowKind::Notifications) return "Alerts";
    if (kind == WindowKind::Demo) return "Demo";
    if (kind == WindowKind::GamePixel) return "Pixel Snake";
    if (kind == WindowKind::GameOrbit) return "Orbit Pong";
    return "App";
}

static void configureWindowLimits(Window& win) {
    win.minWidth = 96;
    win.minHeight = 68;
    win.maxWidth = 240;
    win.maxHeight = 135;
    win.resizable = true;
    win.fullscreen = false;

    if (win.kind == WindowKind::Browser) {
        win.minWidth = 180;
        win.minHeight = 96;
        win.maxWidth = 240;
        win.maxHeight = 131;
    } else if (win.kind == WindowKind::ImageVisualizer) {
        win.minWidth = 180;
        win.minHeight = 96;
        win.maxWidth = 240;
        win.maxHeight = 131;
    } else if (win.kind == WindowKind::WifiManager) {
        win.minWidth = 180;
        win.minHeight = 96;
    } else if (win.kind == WindowKind::TaskManager) {
        win.minWidth = 176;
        win.minHeight = 96;
    } else if (win.kind == WindowKind::DesktopConfig) {
        win.minWidth = 186;
        win.minHeight = 104;
    } else if (isGameKind(win.kind)) {
        win.minWidth = 240;
        win.maxWidth = 240;
        win.minHeight = 135;
        win.maxHeight = 135;
        win.fullscreen = true;
        win.resizable = false;
        win.draggable = false;
    }
}

void WindowManager::begin(katux::core::EventManager* events) {
    events_ = events;
    count_ = 0;
    focusedZ_ = -1;
    draggingZ_ = -1;
    hoverZ_ = -1;
    hoverControl_ = 0;
    dirtyCount_ = 0;

    explorerCount_ = 0;
    explorerFolder_ = 0;
    explorerSelected_ = -1;
    trashAnimUntilMs_ = 0;

    notepadText_[0] = '\0';
    notepadKeyboardOpen_ = false;
    notepadDirty_ = false;
    notepadToastUntilMs_ = 0;

    File note = SPIFFS.open("/notes.txt", FILE_READ);
    if (note) {
        const size_t read = note.readBytes(notepadText_, sizeof(notepadText_) - 1);
        notepadText_[read] = '\0';
        note.close();
    }

    refreshExplorerEntries();

    notificationCount_ = 0;
    notificationSlideY_ = -16;
    notificationHoldUntilMs_ = 0;

    wifiCount_ = 0;
    wifiSelected_ = 0;
    wifiPassword_[0] = '\0';
    wifiKeyboardOpen_ = false;
    wifiConnecting_ = false;
    wifiSilentAttempt_ = false;
    wifiConnectStartMs_ = 0;
    wifiPingMs_ = 0;
    wifiPingOk_ = false;
    wifiAutoAttempted_ = false;

    taskTargetIndex_ = 0;
    taskTab_ = 0;
    taskPerfLastSampleMs_ = millis();
    const uint32_t totalHeap = ESP.getHeapSize();
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint8_t ramInit = totalHeap > 0 ? static_cast<uint8_t>(((totalHeap - (freeHeap > totalHeap ? totalHeap : freeHeap)) * 100UL) / totalHeap) : 0;
    const uint32_t freePsramInit = ESP.getFreePsram();
    uint8_t psramInit = 0;
    if (freePsramInit > 0) {
        const uint32_t totalPsram = ESP.getPsramSize();
        if (totalPsram > 0 && totalPsram >= freePsramInit) {
            psramInit = static_cast<uint8_t>(((totalPsram - freePsramInit) * 100UL) / totalPsram);
        }
    }
    uint8_t cpuInit = static_cast<uint8_t>(fps_ > 60 ? 95 : (fps_ > 40 ? 70 : 45));
    for (uint8_t i = 0; i < kTaskPerfPoints; ++i) {
        taskCpuSeries_[i] = cpuInit;
        taskRamSeries_[i] = ramInit;
        taskPsramSeries_[i] = psramInit;
    }
    appHubSelected_ = 0;
    appHubPage_ = 0;
    desktopShowIcons_ = true;
    desktopSlots_[0] = WindowKind::AppHub;
    desktopSlots_[1] = WindowKind::Settings;
    desktopSlots_[2] = WindowKind::Explorer;
    desktopSlots_[3] = WindowKind::Notepad;
    desktopSlots_[4] = WindowKind::WifiManager;
    desktopSlots_[5] = WindowKind::TaskManager;
    gameCloseHintVisible_ = false;
    gameCloseHintUntilMs_ = 0;
    gameCloseHoldStartMs_ = 0;
    gameCloseProgressMs_ = 0;
    gamePixelX_ = 2;
    gamePixelY_ = 2;
    gamePixelFoodX_ = 8;
    gamePixelFoodY_ = 5;
    gamePixelDx_ = 1;
    gamePixelDy_ = 0;
    gamePixelScore_ = 0;
    gamePixelStepMs_ = millis();
    gamePixelOver_ = false;
    orbitPaddleX_ = 44;
    orbitBallX_ = 60;
    orbitBallY_ = 48;
    orbitDx_ = 1;
    orbitDy_ = -1;
    orbitScore_ = 0;
    orbitStepMs_ = millis();
    orbitOver_ = false;
    browserPresetIndex_ = 0;
    browserLoading_ = false;
    browserLoaded_ = false;
    browserHttpsBlocked_ = false;
    browserLoadMs_ = 0;
    browserScrollY_ = 0;
    browserKeyboardOpen_ = false;
    browserKeyboardEditingForm_ = false;
    for (uint8_t i = 0; i < kMaxWindows; ++i) {
        imageViewerSrcByWindow_[i][0] = '\0';
    }
    browserAlert_[0] = '\0';
    browserAlertUntilMs_ = 0;
    browserItemCount_ = 0;
    browserClickCount_ = 0;
    browserHistoryCount_ = 0;
    browserHistoryIndex_ = -1;
    browserFavoriteCount_ = 0;
    for (uint8_t i = 0; i < kBrowserFavoriteMax; ++i) {
        browserFavorites_[i][0] = '\0';
    }
    strlcpy(browserUrl_, kBrowserPresets[0], sizeof(browserUrl_));
    strlcpy(browserTitle_, "Navigator", sizeof(browserTitle_));
    strlcpy(browserStatus_, "Ready", sizeof(browserStatus_));
    settingsPage_ = 0;
    autoTime_ = true;
    timezoneOffset_ = 0;
    manualYear_ = 2026;
    manualMonth_ = 1;
    manualDay_ = 1;
    manualHour_ = 0;
    manualMinute_ = 0;

    keyboard_.begin();

    wifiPrefsReady_ = prefs_.begin("katuxwifi", false);
    if (wifiPrefsReady_) {
        loadSavedWifi();
    }

    browserPrefsReady_ = browserPrefs_.begin("katuxbrowser", false);
    if (browserPrefsReady_) {
        browserLoadState();
    }

    desktopPrefsReady_ = desktopPrefs_.begin("katuxdesk", false);
    if (desktopPrefsReady_) {
        desktopLoadConfig();
    }

    browserApp_.begin(browserUrl_, kBrowserPresets, kBrowserPresetCount);

    invalidateAll();
}

int8_t WindowManager::createWindow(const char* title, int16_t x, int16_t y, int16_t w, int16_t h, bool draggable, WindowKind kind) {
    uint8_t slot = count_;
    bool reusingSlot = false;
    for (uint8_t i = 0; i < count_; ++i) {
        if (!windows_[i].visible) {
            slot = i;
            reusingSlot = true;
            break;
        }
    }

    if (!reusingSlot) {
        if (count_ >= kMaxWindows) return -1;
        ++count_;
    }

    Window& win = windows_[slot];
    win.id = slot;
    win.kind = kind;
    win.x = x;
    win.y = y;
    win.width = w;
    win.height = h;
    win.targetWidth = w;
    win.targetHeight = h;
    win.draggable = draggable;
    win.focused = false;
    win.visible = true;
    win.minimized = false;
    win.selected = 0;
    win.toggle = false;
    win.meter = 32;
    win.resizePreset = 0;
    if (title && title[0] != '\0') {
        strlcpy(win.title, title, sizeof(win.title));
    }

    configureWindowLimits(win);
    if (win.width < win.minWidth) win.width = win.minWidth;
    if (win.height < win.minHeight) win.height = win.minHeight;
    if (win.width > win.maxWidth) win.width = win.maxWidth;
    if (win.height > win.maxHeight) win.height = win.maxHeight;
    win.targetWidth = win.width;
    win.targetHeight = win.height;

    if (win.fullscreen) {
        win.x = 0;
        win.y = 0;
        win.width = 240;
        win.height = 135;
        win.targetWidth = 240;
        win.targetHeight = 135;
    } else {
        const int16_t taskbarTop = 111;
        if (win.y + win.height > taskbarTop) {
            win.y = static_cast<int16_t>(taskbarTop - win.height);
        }
    }

    if (kind == WindowKind::GamePixel) {
        gamePixelX_ = 2;
        gamePixelY_ = 2;
        gamePixelFoodX_ = 8;
        gamePixelFoodY_ = 5;
        gamePixelDx_ = 1;
        gamePixelDy_ = 0;
        gamePixelScore_ = 0;
        gamePixelStepMs_ = millis();
        gamePixelOver_ = false;
    } else if (kind == WindowKind::GameOrbit) {
        orbitPaddleX_ = 44;
        orbitBallX_ = 60;
        orbitBallY_ = 48;
        orbitDx_ = 1;
        orbitDy_ = -1;
        orbitScore_ = 0;
        orbitStepMs_ = millis();
        orbitOver_ = false;
    } else if (kind == WindowKind::Browser && !browserLoaded_ && !browserLoading_) {
        browserOpenUrl(browserUrl_, true);
    }

    bool inZ = false;
    for (uint8_t i = 0; i < count_; ++i) {
        if (zOrder_[i] == slot) {
            inZ = true;
            break;
        }
    }
    if (!inZ) {
        zOrder_[count_ - 1] = slot;
    }

    int8_t zFound = -1;
    for (uint8_t i = 0; i < count_; ++i) {
        if (zOrder_[i] == slot) {
            zFound = static_cast<int8_t>(i);
            break;
        }
    }
    if (zFound >= 0) {
        bringToFront(zFound);
    }

    for (uint8_t i = 0; i < count_; ++i) {
        if (windows_[zOrder_[i]].visible && !windows_[zOrder_[i]].minimized) {
            focusedZ_ = static_cast<int8_t>(i);
        }
    }
    syncFocusFlags();
    markDirty(frameRect(win));
    markDirty({0, 111, 240, 24});

    return static_cast<int8_t>(slot);
}

void WindowManager::focusNext() {
    if (count_ == 0) return;

    int8_t next = focusedZ_;
    for (uint8_t i = 0; i < count_; ++i) {
        next = static_cast<int8_t>((next + 1 + count_) % count_);
        Window& cand = windows_[zOrder_[next]];
        if (cand.visible && !cand.minimized) {
            focusedZ_ = next;
            bringToFront(next);
            return;
        }
    }
}

bool WindowManager::focusWindowById(uint8_t id) {
    for (uint8_t z = 0; z < count_; ++z) {
        if (windows_[zOrder_[z]].id == id) {
            Window& w = windows_[zOrder_[z]];
            if (!w.visible || w.minimized) {
                return false;
            }
            bringToFront(z);
            return true;
        }
    }
    return false;
}

bool WindowManager::restoreWindowById(uint8_t id) {
    for (uint8_t z = 0; z < count_; ++z) {
        if (windows_[zOrder_[z]].id == id) {
            Window& w = windows_[zOrder_[z]];
            if (!w.visible) return false;
            w.minimized = false;
            bringToFront(z);
            markDirty(frameRect(w));
            markDirty({0, 111, 240, 24});
            emit(katux::core::EventType::WindowRestored, id, static_cast<int32_t>(w.kind));
            return true;
        }
    }
    return false;
}

bool WindowManager::closeWindowById(uint8_t id) {
    for (uint8_t z = 0; z < count_; ++z) {
        Window& w = windows_[zOrder_[z]];
        if (w.id != id || !w.visible) continue;
        const Rect prev = frameRect(w);
        w.visible = false;
        w.minimized = false;
        markDirty(prev);
        markDirty({0, 111, 240, 24});
        if (w.kind == WindowKind::WifiManager) wifiKeyboardOpen_ = false;
        if (w.kind == WindowKind::Notepad) notepadKeyboardOpen_ = false;
        if (w.kind == WindowKind::Browser) {
            browserKeyboardOpen_ = false;
            browserKeyboardEditingForm_ = false;
        }
        if (w.kind == WindowKind::ImageVisualizer) imageViewerSrcByWindow_[w.id][0] = '\0';
        if (!wifiKeyboardOpen_ && !notepadKeyboardOpen_ && !browserKeyboardOpen_ && keyboard_.isOpen()) {
            keyboard_.close();
        }
        if (draggingZ_ == z) draggingZ_ = -1;
        emit(katux::core::EventType::WindowClosed, w.id, static_cast<int32_t>(w.kind));
        focusedZ_ = -1;
        for (int8_t i = static_cast<int8_t>(count_) - 1; i >= 0; --i) {
            Window& cand = windows_[zOrder_[i]];
            if (cand.visible && !cand.minimized) {
                focusedZ_ = i;
                break;
            }
        }
        syncFocusFlags();
        return true;
    }
    return false;
}

uint8_t WindowManager::closeAllVisible() {
    uint8_t closed = 0;
    for (uint8_t i = 0; i < count_; ++i) {
        Window& w = windows_[i];
        if (!w.visible) continue;
        markDirty(frameRect(w));
        w.visible = false;
        w.minimized = false;
        if (w.kind == WindowKind::ImageVisualizer) imageViewerSrcByWindow_[w.id][0] = '\0';
        emit(katux::core::EventType::WindowClosed, w.id, static_cast<int32_t>(w.kind));
        ++closed;
    }

    focusedZ_ = -1;
    draggingZ_ = -1;
    hoverZ_ = -1;
    hoverControl_ = 0;
    wifiKeyboardOpen_ = false;
    notepadKeyboardOpen_ = false;
    browserKeyboardOpen_ = false;
    browserKeyboardEditingForm_ = false;
    if (keyboard_.isOpen()) keyboard_.close();
    syncFocusFlags();
    markDirty({0, 111, 240, 24});
    return closed;
}

void WindowManager::onEvent(const katux::core::Event& event, int16_t cursorX, int16_t cursorY) {
    cursorX_ = cursorX;
    cursorY_ = cursorY;
    updateHover(cursorX, cursorY);

    if (event.type == katux::core::EventType::SystemTick) {
        const uint32_t now = millis();
        const int16_t prevNotifY = notificationSlideY_;
        if (notificationHoldUntilMs_ > 0 && now > notificationHoldUntilMs_) {
            notificationSlideY_ = -16;
            notificationHoldUntilMs_ = 0;
        }
        if (animationsEnabled_) {
            if (notificationSlideY_ < 0 && notificationCount_ > 0 && notificationHoldUntilMs_ > now) {
                notificationSlideY_ += 2;
                if (notificationSlideY_ > 0) notificationSlideY_ = 0;
            }
            if (notificationSlideY_ > -16 && notificationHoldUntilMs_ == 0) {
                notificationSlideY_ -= 2;
            }
        } else {
            notificationSlideY_ = notificationHoldUntilMs_ > now ? 0 : -16;
        }
        if (notificationSlideY_ != prevNotifY) {
            markDirty({0, prevNotifY, 240, 16});
            markDirty({0, notificationSlideY_, 240, 16});
        }

        for (uint8_t i = 0; i < count_; ++i) {
            Window& anim = windows_[i];
            if (!anim.visible || anim.minimized) continue;
            const Window before = anim;
            if (anim.width != anim.targetWidth) {
                const int16_t step = anim.width < anim.targetWidth ? 6 : -6;
                const int16_t next = static_cast<int16_t>(anim.width + step);
                if ((step > 0 && next > anim.targetWidth) || (step < 0 && next < anim.targetWidth)) {
                    anim.width = anim.targetWidth;
                } else {
                    anim.width = next;
                }
            }
            if (anim.height != anim.targetHeight) {
                const int16_t step = anim.height < anim.targetHeight ? 4 : -4;
                const int16_t next = static_cast<int16_t>(anim.height + step);
                if ((step > 0 && next > anim.targetHeight) || (step < 0 && next < anim.targetHeight)) {
                    anim.height = anim.targetHeight;
                } else {
                    anim.height = next;
                }
            }
            if (anim.width != before.width || anim.height != before.height) {
                markDirtyWindow(before, anim);
            }
        }

        bool pixelActive = false;
        bool orbitActive = false;
        for (uint8_t i = 0; i < count_; ++i) {
            const Window& w = windows_[i];
            if (!w.visible || w.minimized) continue;
            if (w.kind == WindowKind::GamePixel) pixelActive = true;
            if (w.kind == WindowKind::GameOrbit) orbitActive = true;
        }

        if (focusedZ_ >= 0) {
            Window& focused = windows_[zOrder_[focusedZ_]];
            if (focused.visible && !focused.minimized && focused.fullscreen && isGameKind(focused.kind)) {
                if (StickCP2.BtnA.isPressed() && gameCloseHoldStartMs_ == 0) {
                    gameCloseHoldStartMs_ = now;
                    gameCloseHintVisible_ = true;
                    gameCloseHintUntilMs_ = now + 2000U;
                }
                if (StickCP2.BtnA.isPressed() && gameCloseHoldStartMs_ > 0) {
                    uint32_t held = now - gameCloseHoldStartMs_;
                    if (held > 2000U) held = 2000U;
                    gameCloseProgressMs_ = static_cast<uint16_t>(held);
                    if (held >= 2000U) {
                        closeWindowById(focused.id);
                        gameCloseHoldStartMs_ = 0;
                        gameCloseProgressMs_ = 0;
                        gameCloseHintVisible_ = false;
                    }
                } else {
                    gameCloseHoldStartMs_ = 0;
                    gameCloseProgressMs_ = 0;
                }
            } else {
                gameCloseHoldStartMs_ = 0;
                gameCloseProgressMs_ = 0;
            }
        }

        if (gameCloseHintVisible_ && now > gameCloseHintUntilMs_) {
            gameCloseHintVisible_ = false;
        }

        if (wifiCount_ == 0) {
            strlcpy(wifiSsid_[0], kAutoWifiSsid, sizeof(wifiSsid_[0]));
            wifiRssi_[0] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -45;
            wifiCount_ = 1;
            wifiSelected_ = 0;
        }

        if (pixelActive && !gamePixelOver_ && now - gamePixelStepMs_ >= 160U) {
            gamePixelStepMs_ = now;
            const int8_t nx = static_cast<int8_t>(gamePixelX_) + gamePixelDx_;
            const int8_t ny = static_cast<int8_t>(gamePixelY_) + gamePixelDy_;
            if (nx < 0 || nx > 14 || ny < 0 || ny > 8) {
                gamePixelOver_ = true;
            } else {
                gamePixelX_ = static_cast<uint8_t>(nx);
                gamePixelY_ = static_cast<uint8_t>(ny);
                if (gamePixelX_ == gamePixelFoodX_ && gamePixelY_ == gamePixelFoodY_) {
                    ++gamePixelScore_;
                    gamePixelFoodX_ = static_cast<uint8_t>((gamePixelFoodX_ + 5 + gamePixelScore_) % 15U);
                    gamePixelFoodY_ = static_cast<uint8_t>((gamePixelFoodY_ + 3 + gamePixelScore_) % 9U);
                    if (gamePixelFoodX_ == gamePixelX_ && gamePixelFoodY_ == gamePixelY_) {
                        gamePixelFoodX_ = static_cast<uint8_t>((gamePixelFoodX_ + 2) % 15U);
                    }
                }
            }
        }

        if (orbitActive && !orbitOver_ && now - orbitStepMs_ >= 42U) {
            orbitStepMs_ = now;
            orbitBallX_ = static_cast<int16_t>(orbitBallX_ + orbitDx_);
            orbitBallY_ = static_cast<int16_t>(orbitBallY_ + orbitDy_);
            if (orbitBallX_ <= 0) {
                orbitBallX_ = 0;
                orbitDx_ = 1;
            } else if (orbitBallX_ >= 114) {
                orbitBallX_ = 114;
                orbitDx_ = -1;
            }
            if (orbitBallY_ <= 0) {
                orbitBallY_ = 0;
                orbitDy_ = 1;
            } else if (orbitBallY_ >= 66) {
                if (orbitBallX_ + 6 >= orbitPaddleX_ && orbitBallX_ <= orbitPaddleX_ + 24) {
                    orbitBallY_ = 66;
                    orbitDy_ = -1;
                    ++orbitScore_;
                } else {
                    orbitOver_ = true;
                }
            }
        }

        if (!wifiAutoAttempted_ && WiFi.status() != WL_CONNECTED) {
            WiFi.mode(WIFI_STA);
            WiFi.begin(kAutoWifiSsid, kAutoWifiPassword);
            wifiAutoAttempted_ = true;
            wifiConnecting_ = true;
            wifiSilentAttempt_ = true;
            wifiConnectStartMs_ = now;
        }

        if (wifiConnecting_) {
            if (WiFi.status() == WL_CONNECTED) {
                wifiConnecting_ = false;
                if (wifiPassword_[0] != '\0' && wifiCount_ > 0 && wifiSelected_ < wifiCount_) {
                    saveWifiCredential(wifiSsid_[wifiSelected_], wifiPassword_);
                }
                if (!wifiSilentAttempt_) {
                    notify("WiFi connected");
                }
                wifiSilentAttempt_ = false;
            } else if (now - wifiConnectStartMs_ >= 7000U) {
                wifiConnecting_ = false;
                WiFi.disconnect(false, false);
                if (!wifiSilentAttempt_) {
                    notify("WiFi failed");
                }
                wifiSilentAttempt_ = false;
            }
        }

        const bool wasBrowserLoading = browserLoading_;
        const bool wasBrowserLoaded = browserLoaded_;
        browserApp_.tick(now);
        browserLoading_ = browserApp_.isLoading();
        browserLoaded_ = browserApp_.isLoaded();
        strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
        strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));

        for (uint8_t i = 0; i < count_; ++i) {
            const Window& wb = windows_[i];
            if (!wb.visible || wb.minimized || wb.kind != WindowKind::Browser) continue;
            const Rect body = windowBody(wb);
            Rect dirtyRegions[3]{};
            const uint8_t dirtyCount = browserApp_.consumeDirtyRegions(dirtyRegions, 3, body);
            for (uint8_t d = 0; d < dirtyCount; ++d) {
                markDirty(dirtyRegions[d]);
            }
            if ((wasBrowserLoading != browserLoading_ || wasBrowserLoaded != browserLoaded_) && dirtyCount == 0) {
                markDirty(body);
            }
        }

        if (focusedZ_ >= 0) {
            const Window& focused = windows_[zOrder_[focusedZ_]];
            if (focused.visible && !focused.minimized && focused.kind == WindowKind::Browser && !browserKeyboardOpen_) {
                const Rect body = windowBody(focused);
                const Rect upBtn{static_cast<int16_t>(body.x + 118), static_cast<int16_t>(body.y + 16), 14, 10};
                const Rect downBtn{static_cast<int16_t>(body.x + 134), static_cast<int16_t>(body.y + 16), 14, 10};

                const int16_t mainTop = static_cast<int16_t>(body.y + 30);
                const int16_t mainH = static_cast<int16_t>(body.h - 32);
                const Rect sidePanel{static_cast<int16_t>(body.x + 4), mainTop, 70, mainH};
                const Rect viewport{static_cast<int16_t>(sidePanel.x + sidePanel.w + 2), mainTop, static_cast<int16_t>(body.w - sidePanel.w - 8), mainH};
                const Rect scrollUp{static_cast<int16_t>(viewport.x + viewport.w - 10), static_cast<int16_t>(viewport.y + 2), 8, 8};
                const Rect scrollDown{static_cast<int16_t>(viewport.x + viewport.w - 10), static_cast<int16_t>(viewport.y + viewport.h - 22), 8, 8};

                int8_t holdDir = 0;
                if (StickCP2.BtnA.isPressed()) {
                    if (cursorX >= upBtn.x && cursorY >= upBtn.y && cursorX < upBtn.x + upBtn.w && cursorY < upBtn.y + upBtn.h) holdDir = -1;
                    else if (cursorX >= downBtn.x && cursorY >= downBtn.y && cursorX < downBtn.x + downBtn.w && cursorY < downBtn.y + downBtn.h)
                        holdDir = 1;
                    else if (cursorX >= scrollUp.x && cursorY >= scrollUp.y && cursorX < scrollUp.x + scrollUp.w && cursorY < scrollUp.y + scrollUp.h)
                        holdDir = -1;
                    else if (cursorX >= scrollDown.x && cursorY >= scrollDown.y && cursorX < scrollDown.x + scrollDown.w && cursorY < scrollDown.y + scrollDown.h)
                        holdDir = 1;
                }

                if (holdDir != 0) {
                    if (browserHoldScrollDir_ != holdDir) {
                        browserHoldScrollDir_ = holdDir;
                        browserHoldScrollStartMs_ = now;
                        browserHoldScrollNextMs_ = now;
                    }
                    if (now >= browserHoldScrollNextMs_) {
                        const uint32_t heldMs = now - browserHoldScrollStartMs_;
                        uint8_t stage = static_cast<uint8_t>(heldMs / 320U);
                        if (stage > 5) stage = 5;

                        const int16_t impulse = static_cast<int16_t>(4 + stage * 2);
                        uint16_t cadenceMs = 22U;
                        if (stage >= 4) cadenceMs = 12U;
                        else if (stage >= 2) cadenceMs = 16U;

                        browserApp_.scroll(static_cast<int16_t>(holdDir * impulse));
                        browserHoldScrollNextMs_ = now + cadenceMs;
                    }
                } else {
                    browserHoldScrollDir_ = 0;
                    browserHoldScrollNextMs_ = 0;
                    browserHoldScrollStartMs_ = 0;
                }
            } else {
                browserHoldScrollDir_ = 0;
                browserHoldScrollNextMs_ = 0;
                browserHoldScrollStartMs_ = 0;
            }
        } else {
            browserHoldScrollDir_ = 0;
            browserHoldScrollNextMs_ = 0;
            browserHoldScrollStartMs_ = 0;
        }

        static uint32_t lastMemTrimMs = 0;
        const uint32_t freeHeapNow = ESP.getFreeHeap();
        if (freeHeapNow < 24000U && now - lastMemTrimMs >= 1500U) {
            lastMemTrimMs = now;
            browserClearDocument();
            browserLoading_ = false;
            browserLoaded_ = false;
            browserSetStatus("Low RAM cache cleared");
            if (wifiKeyboardOpen_ || notepadKeyboardOpen_ || browserKeyboardOpen_) {
                wifiKeyboardOpen_ = false;
                notepadKeyboardOpen_ = false;
                browserKeyboardOpen_ = false;
                browserKeyboardEditingForm_ = false;
                if (keyboard_.isOpen()) {
                    keyboard_.close();
                }
            }
            if (notificationCount_ > 2) {
                notificationCount_ = 2;
            }
        }
    }

    if (count_ == 0) return;

    if (draggingZ_ >= 0) {
        Window& dragged = windows_[zOrder_[draggingZ_]];
        if (!dragged.visible || dragged.minimized || !dragged.draggable) {
            draggingZ_ = -1;
        } else {
            const Window before = dragged;
            dragged.x = cursorX - dragOffsetX_;
            dragged.y = cursorY - dragOffsetY_;
            const int16_t minX = static_cast<int16_t>(16 - dragged.width);
            const int16_t maxX = 224;
            const int16_t minY = -12;
            const int16_t maxY = 117;
            if (dragged.x < minX) dragged.x = minX;
            if (dragged.x > maxX) dragged.x = maxX;
            if (dragged.y < minY) dragged.y = minY;
            if (dragged.y > maxY) dragged.y = maxY;
            if (dragged.x != before.x || dragged.y != before.y) {
                markDirtyWindow(before, dragged);
            }
            updateHover(cursorX, cursorY);
        }
    }

    if (event.type == katux::core::EventType::ButtonDouble && event.source == katux::core::EventSource::ButtonB) {
        focusNext();
        return;
    }

    if (event.type == katux::core::EventType::ButtonClick && event.source == katux::core::EventSource::ButtonA) {
        clickPulseUntilMs_ = millis() + 140U;
        int8_t z = hitTest(cursorX, cursorY);
        if (z < 0) {
            if (keyboard_.isOpen()) {
                keyboard_.close();
                wifiKeyboardOpen_ = false;
                notepadKeyboardOpen_ = false;
            }
            draggingZ_ = -1;
            return;
        }

        if (z != static_cast<int8_t>(count_ - 1)) {
            bringToFront(z);
            updateHover(cursorX, cursorY);
            return;
        }

        bringToFront(z);
        Window& w = windows_[zOrder_[focusedZ_]];
        markDirty(frameRect(w));

        const Rect resizeBox{static_cast<int16_t>(w.x + w.width - 40), static_cast<int16_t>(w.y + 4), 10, 10};
        const Rect minBox{static_cast<int16_t>(w.x + w.width - 28), static_cast<int16_t>(w.y + 4), 10, 10};
        const Rect closeBox{static_cast<int16_t>(w.x + w.width - 16), static_cast<int16_t>(w.y + 4), 10, 10};

        if (!w.fullscreen && cursorX >= closeBox.x && cursorY >= closeBox.y && cursorX < closeBox.x + closeBox.w && cursorY < closeBox.y + closeBox.h) {
            closeWindowById(w.id);
            updateHover(cursorX, cursorY);
            return;
        }

        if (!w.fullscreen && cursorX >= minBox.x && cursorY >= minBox.y && cursorX < minBox.x + minBox.w && cursorY < minBox.y + minBox.h) {
            const Rect prev = frameRect(w);
            w.minimized = true;
            markDirty(prev);
            markDirty({0, 111, 240, 24});
            emit(katux::core::EventType::WindowMinimized, w.id, static_cast<int32_t>(w.kind));
            if (draggingZ_ == focusedZ_) {
                draggingZ_ = -1;
            }
            focusedZ_ = -1;
            for (int8_t i = static_cast<int8_t>(count_) - 1; i >= 0; --i) {
                Window& cand = windows_[zOrder_[i]];
                if (cand.visible && !cand.minimized) {
                    focusedZ_ = i;
                    break;
                }
            }
            syncFocusFlags();
            updateHover(cursorX, cursorY);
            return;
        }

        if (!w.fullscreen && w.resizable && cursorX >= resizeBox.x && cursorY >= resizeBox.y && cursorX < resizeBox.x + resizeBox.w && cursorY < resizeBox.y + resizeBox.h) {
            w.resizePreset = static_cast<uint8_t>((w.resizePreset + 1U) % 4U);
            if (w.resizePreset == 0) {
                w.targetWidth = w.minWidth;
                w.targetHeight = w.minHeight;
            } else if (w.resizePreset == 1) {
                w.targetWidth = static_cast<int16_t>((w.minWidth + w.maxWidth) / 2);
                w.targetHeight = static_cast<int16_t>((w.minHeight + w.maxHeight) / 2);
            } else if (w.resizePreset == 2) {
                w.targetWidth = w.maxWidth;
                w.targetHeight = w.maxHeight;
            } else {
                w.targetWidth = static_cast<int16_t>(w.maxWidth - 18);
                w.targetHeight = static_cast<int16_t>(w.maxHeight - 12);
            }
            if (w.targetWidth < w.minWidth) w.targetWidth = w.minWidth;
            if (w.targetWidth > w.maxWidth) w.targetWidth = w.maxWidth;
            if (w.targetHeight < w.minHeight) w.targetHeight = w.minHeight;
            if (w.targetHeight > w.maxHeight) w.targetHeight = w.maxHeight;
            const int16_t taskbarTop = 111;
            if (w.y + w.targetHeight > taskbarTop) {
                w.y = static_cast<int16_t>(taskbarTop - w.targetHeight);
            }
            notify("Window resized");
            updateHover(cursorX, cursorY);
            return;
        }

        if (inTitleBar(w, cursorX, cursorY) && w.draggable) {
            if (draggingZ_ == focusedZ_) {
                draggingZ_ = -1;
            } else {
                draggingZ_ = focusedZ_;
                dragOffsetX_ = cursorX - w.x;
                dragOffsetY_ = cursorY - w.y;
            }
            updateHover(cursorX, cursorY);
            return;
        }

        const Rect body = windowBody(w);
        if (!(cursorX >= body.x && cursorY >= body.y && cursorX < body.x + body.w && cursorY < body.y + body.h)) {
            updateHover(cursorX, cursorY);
            return;
        }

        if (w.kind == WindowKind::Demo) {
            w.toggle = !w.toggle;
            w.meter = static_cast<uint8_t>((w.meter + 17U) % 101U);
        } else if (w.kind == WindowKind::Settings) {
            const Rect leftArrow{static_cast<int16_t>(body.x + body.w - 62), static_cast<int16_t>(body.y + 4), 12, 10};
            const Rect rightArrow{static_cast<int16_t>(body.x + body.w - 46), static_cast<int16_t>(body.y + 4), 12, 10};
            if (cursorX >= leftArrow.x && cursorY >= leftArrow.y && cursorX < leftArrow.x + leftArrow.w && cursorY < leftArrow.y + leftArrow.h) {
                settingsPage_ = settingsPage_ == 0 ? 2 : static_cast<uint8_t>(settingsPage_ - 1);
                updateHover(cursorX, cursorY);
                return;
            }
            if (cursorX >= rightArrow.x && cursorY >= rightArrow.y && cursorX < rightArrow.x + rightArrow.w && cursorY < rightArrow.y + rightArrow.h) {
                settingsPage_ = static_cast<uint8_t>((settingsPage_ + 1U) % 3U);
                updateHover(cursorX, cursorY);
                return;
            }

            const int16_t localY = cursorY - body.y;
            if (localY >= 20 && localY < 90) {
                const uint8_t row = static_cast<uint8_t>((localY - 20) / 14);
                if (settingsPage_ == 0) {
                    if (row == 0) {
                        darkTheme_ = !darkTheme_;
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ThemeDark), darkTheme_ ? 1 : 0);
                    } else if (row == 1) {
                        brightness_ = static_cast<uint8_t>(brightness_ >= 100 ? 30 : brightness_ + 10);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::Brightness), brightness_);
                    } else if (row == 2) {
                        cursorSpeed_ = static_cast<uint8_t>((cursorSpeed_ % 5U) + 1U);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::CursorSpeed), cursorSpeed_);
                    } else if (row == 3) {
                        performanceProfile_ = static_cast<uint8_t>((performanceProfile_ + 1U) % 3U);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::PerformanceProfile), performanceProfile_);
                    } else if (row == 4) {
                        animationsEnabled_ = !animationsEnabled_;
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::AnimationsEnabled), animationsEnabled_ ? 1 : 0);
                    }
                } else if (settingsPage_ == 1) {
                    if (row == 0) {
                        autoTime_ = !autoTime_;
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::AutoTime), autoTime_ ? 1 : 0);
                    } else if (row == 1) {
                        timezoneOffset_ = static_cast<int8_t>(timezoneOffset_ >= 14 ? -12 : timezoneOffset_ + 1);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::TimezoneOffset), timezoneOffset_);
                    } else if (row == 2) {
                        manualYear_ = static_cast<uint16_t>(manualYear_ >= 2099 ? 2024 : manualYear_ + 1);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ManualYear), manualYear_);
                    } else if (row == 3) {
                        manualMonth_ = static_cast<uint8_t>(manualMonth_ >= 12 ? 1 : manualMonth_ + 1);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ManualMonth), manualMonth_);
                    } else if (row == 4) {
                        manualDay_ = static_cast<uint8_t>(manualDay_ >= 31 ? 1 : manualDay_ + 1);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ManualDay), manualDay_);
                    }
                } else {
                    if (row == 0) {
                        manualHour_ = static_cast<uint8_t>((manualHour_ + 1U) % 24U);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ManualHour), manualHour_);
                    } else if (row == 1) {
                        manualMinute_ = static_cast<uint8_t>((manualMinute_ + 5U) % 60U);
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ManualMinute), manualMinute_);
                    } else if (row == 2) {
                        clearSavedWifi();
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::ClearSavedWifi), 1);
                        notify("Saved WiFi cleared");
                    } else if (row == 3) {
                        debugOverlay_ = !debugOverlay_;
                        emit(katux::core::EventType::SettingChanged, static_cast<int32_t>(katux::core::SettingKey::DebugOverlay), debugOverlay_ ? 1 : 0);
                    }
                }
            }
        } else if (w.kind == WindowKind::TaskManager) {
            const Rect processTab{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 16), 54, 10};
            const Rect perfTab{static_cast<int16_t>(body.x + 62), static_cast<int16_t>(body.y + 16), 54, 10};
            if (cursorX >= processTab.x && cursorY >= processTab.y && cursorX < processTab.x + processTab.w && cursorY < processTab.y + processTab.h) {
                taskTab_ = 0;
                updateHover(cursorX, cursorY);
                return;
            }
            if (cursorX >= perfTab.x && cursorY >= perfTab.y && cursorX < perfTab.x + perfTab.w && cursorY < perfTab.y + perfTab.h) {
                taskTab_ = 1;
                updateHover(cursorX, cursorY);
                return;
            }

            if (taskTab_ == 0) {
                WindowTaskItem items[kMaxWindows]{};
                uint8_t active = listTaskItems(items, kMaxWindows, false);
                uint8_t mapped[kMaxWindows]{};
                uint8_t rows = 0;
                for (uint8_t i = 0; i < active && rows < 6; ++i) {
                    mapped[rows++] = items[i].id;
                }

                const int16_t listY = static_cast<int16_t>(body.y + 44);
                if (cursorY >= listY && cursorY < listY + 54) {
                    const uint8_t row = static_cast<uint8_t>((cursorY - listY) / 9);
                    if (row < rows) taskTargetIndex_ = row;
                }

                const Rect endBtn{static_cast<int16_t>(body.x + body.w - 66), static_cast<int16_t>(body.y + body.h - 14), 60, 10};
                if (cursorX >= endBtn.x && cursorY >= endBtn.y && cursorX < endBtn.x + endBtn.w && cursorY < endBtn.y + endBtn.h && rows > 0) {
                    if (taskTargetIndex_ >= rows) taskTargetIndex_ = 0;
                    closeWindowById(mapped[taskTargetIndex_]);
                    notify("Task ended");
                }
            }
        } else if (w.kind == WindowKind::Explorer) {
            const int16_t tabY = static_cast<int16_t>(body.y + 12);
            for (uint8_t folder = 0; folder < kFolderCount; ++folder) {
                const Rect tab{static_cast<int16_t>(body.x + 6 + folder * 42), tabY, 38, 10};
                if (cursorX >= tab.x && cursorY >= tab.y && cursorX < tab.x + tab.w && cursorY < tab.y + tab.h) {
                    explorerFolder_ = folder;
                    explorerSelected_ = -1;
                    updateHover(cursorX, cursorY);
                    return;
                }
            }

            const Rect refreshBtn{static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + 12), 20, 10};
            if (cursorX >= refreshBtn.x && cursorY >= refreshBtn.y && cursorX < refreshBtn.x + refreshBtn.w && cursorY < refreshBtn.y + refreshBtn.h) {
                refreshExplorerEntries();
                notify("Explorer refreshed");
                updateHover(cursorX, cursorY);
                return;
            }

            const Rect trash{static_cast<int16_t>(body.x + body.w - 28), static_cast<int16_t>(body.y + body.h - 18), 20, 14};
            if (cursorX >= trash.x && cursorY >= trash.y && cursorX < trash.x + trash.w && cursorY < trash.y + trash.h && explorerSelected_ >= 0) {
                char path[40] = "/";
                strlcpy(path + 1, explorer_[explorerSelected_].name, sizeof(path) - 1);
                if (SPIFFS.exists(path) && SPIFFS.remove(path)) {
                    trashAnimUntilMs_ = millis() + 700U;
                    notify("File deleted");
                } else {
                    notify("Delete failed");
                }
                explorerSelected_ = -1;
                refreshExplorerEntries();
                updateHover(cursorX, cursorY);
                return;
            }

            uint8_t row = 0;
            for (uint8_t i = 0; i < explorerCount_; ++i) {
                if (explorer_[i].folder != explorerFolder_) continue;
                const Rect slot{static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + 29 + row * 10), static_cast<int16_t>(body.w - 44), 9};
                if (cursorX >= slot.x && cursorY >= slot.y && cursorX < slot.x + slot.w && cursorY < slot.y + slot.h) {
                    explorerSelected_ = static_cast<int8_t>(i);
                    updateHover(cursorX, cursorY);
                    return;
                }
                ++row;
                if (row >= 8) break;
            }
        } else if (w.kind == WindowKind::Notepad) {
            const Rect editBtn{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 4), 32, 10};
            const Rect saveBtn{static_cast<int16_t>(body.x + 42), static_cast<int16_t>(body.y + 4), 32, 10};
            const Rect loadBtn{static_cast<int16_t>(body.x + 78), static_cast<int16_t>(body.y + 4), 32, 10};

            if (cursorX >= editBtn.x && cursorY >= editBtn.y && cursorX < editBtn.x + editBtn.w && cursorY < editBtn.y + editBtn.h) {
                keyboard_.open(notepadText_);
                notepadKeyboardOpen_ = true;
                wifiKeyboardOpen_ = false;
                browserKeyboardOpen_ = false;
                browserKeyboardEditingForm_ = false;
            } else if (cursorX >= saveBtn.x && cursorY >= saveBtn.y && cursorX < saveBtn.x + saveBtn.w && cursorY < saveBtn.y + saveBtn.h) {
                File note = SPIFFS.open("/notes.txt", FILE_WRITE);
                if (note) {
                    note.print(notepadText_);
                    note.close();
                    notepadDirty_ = false;
                    notepadToastUntilMs_ = millis() + 1200U;
                    refreshExplorerEntries();
                    notify("Note saved");
                } else {
                    notify("Save failed");
                }
            } else if (cursorX >= loadBtn.x && cursorY >= loadBtn.y && cursorX < loadBtn.x + loadBtn.w && cursorY < loadBtn.y + loadBtn.h) {
                File note = SPIFFS.open("/notes.txt", FILE_READ);
                if (note) {
                    const size_t read = note.readBytes(notepadText_, sizeof(notepadText_) - 1);
                    notepadText_[read] = '\0';
                    note.close();
                    notepadDirty_ = false;
                    notify("Note loaded");
                } else {
                    notify("No note file");
                }
            }

            if (notepadKeyboardOpen_) {
                const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};
                if (keyboard_.onClick(cursorX, cursorY, keyRect)) {
                    strlcpy(notepadText_, keyboard_.value(), sizeof(notepadText_));
                    notepadDirty_ = true;
                    if (keyboard_.consumeAccepted()) {
                        notepadKeyboardOpen_ = false;
                    }
                }
            }
        } else if (w.kind == WindowKind::DesktopConfig) {
            const Rect toggleRow{static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + 18), static_cast<int16_t>(body.w - 16), 11};
            if (cursorX >= toggleRow.x && cursorY >= toggleRow.y && cursorX < toggleRow.x + toggleRow.w && cursorY < toggleRow.y + toggleRow.h) {
                desktopShowIcons_ = !desktopShowIcons_;
                desktopSaveConfig();
                notify(desktopShowIcons_ ? "Desktop icons shown" : "Desktop icons hidden");
                markDirty({0, 0, 240, 111});
                updateHover(cursorX, cursorY);
                return;
            }

            for (uint8_t slot = 0; slot < kDesktopSlotCount; ++slot) {
                const Rect row{static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + 34 + slot * 11), static_cast<int16_t>(body.w - 16), 10};
                if (cursorX >= row.x && cursorY >= row.y && cursorX < row.x + row.w && cursorY < row.y + row.h) {
                    WindowKind current = desktopSlots_[slot];
                    uint8_t idx = 0;
                    for (uint8_t i = 0; i < kDesktopAssignableCount; ++i) {
                        if (kDesktopAssignableKinds[i] == current) {
                            idx = i;
                            break;
                        }
                    }
                    idx = static_cast<uint8_t>((idx + 1U) % kDesktopAssignableCount);
                    desktopSlots_[slot] = kDesktopAssignableKinds[idx];
                    desktopSaveConfig();
                    notify("Desktop slot updated");
                    markDirty({0, 0, 240, 111});
                    updateHover(cursorX, cursorY);
                    return;
                }
            }
        } else if (w.kind == WindowKind::AppHub) {
            const uint8_t totalPages = static_cast<uint8_t>((kHubEntryCount + kHubPerPage - 1U) / kHubPerPage);
            const Rect prevBtn{static_cast<int16_t>(body.x + body.w - 56), static_cast<int16_t>(body.y + 4), 12, 10};
            const Rect nextBtn{static_cast<int16_t>(body.x + body.w - 40), static_cast<int16_t>(body.y + 4), 12, 10};

            if (cursorX >= prevBtn.x && cursorY >= prevBtn.y && cursorX < prevBtn.x + prevBtn.w && cursorY < prevBtn.y + prevBtn.h) {
                if (totalPages > 0) {
                    appHubPage_ = appHubPage_ == 0 ? static_cast<uint8_t>(totalPages - 1U) : static_cast<uint8_t>(appHubPage_ - 1U);
                }
                appHubSelected_ = static_cast<uint8_t>(appHubPage_ * kHubPerPage);
                updateHover(cursorX, cursorY);
                return;
            }
            if (cursorX >= nextBtn.x && cursorY >= nextBtn.y && cursorX < nextBtn.x + nextBtn.w && cursorY < nextBtn.y + nextBtn.h) {
                if (totalPages > 0) {
                    appHubPage_ = static_cast<uint8_t>((appHubPage_ + 1U) % totalPages);
                }
                appHubSelected_ = static_cast<uint8_t>(appHubPage_ * kHubPerPage);
                updateHover(cursorX, cursorY);
                return;
            }

            const int16_t gridX = static_cast<int16_t>(body.x + 8);
            const int16_t gridY = static_cast<int16_t>(body.y + 22);
            const uint8_t pageStart = static_cast<uint8_t>(appHubPage_ * kHubPerPage);
            for (uint8_t slot = 0; slot < kHubPerPage; ++slot) {
                const uint8_t idx = static_cast<uint8_t>(pageStart + slot);
                if (idx >= kHubEntryCount) break;
                const uint8_t col = static_cast<uint8_t>(slot % kHubCols);
                const uint8_t row = static_cast<uint8_t>(slot / kHubCols);
                const Rect card{static_cast<int16_t>(gridX + col * 66), static_cast<int16_t>(gridY + row * 34), 62, 30};
                if (cursorX >= card.x && cursorY >= card.y && cursorX < card.x + card.w && cursorY < card.y + card.h) {
                    appHubSelected_ = idx;
                    if (createKindWindow(kHubEntries[idx].kind) >= 0) {
                        notify("App launched");
                    } else {
                        notify("No free slot");
                    }
                    updateHover(cursorX, cursorY);
                    return;
                }
            }
        } else if (w.kind == WindowKind::GamePixel) {
            const int16_t gridW = 15;
            const int16_t gridH = 9;
            const int16_t topPad = 20;
            int16_t cell = static_cast<int16_t>((body.h - topPad - 10) / gridH);
            const int16_t cellByWidth = static_cast<int16_t>((body.w - 10) / gridW);
            if (cellByWidth < cell) cell = cellByWidth;
            if (cell < 6) cell = 6;
            const int16_t arenaW = static_cast<int16_t>(gridW * cell);
            const int16_t arenaH = static_cast<int16_t>(gridH * cell);
            const int16_t arenaX = static_cast<int16_t>(body.x + (body.w - arenaW) / 2);
            const int16_t arenaY = static_cast<int16_t>(body.y + topPad);

            if (gamePixelOver_) {
                const Rect resetBtn{static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + body.h - 14), 46, 10};
                if (cursorX >= resetBtn.x && cursorY >= resetBtn.y && cursorX < resetBtn.x + resetBtn.w && cursorY < resetBtn.y + resetBtn.h) {
                    gamePixelX_ = 2;
                    gamePixelY_ = 2;
                    gamePixelFoodX_ = 8;
                    gamePixelFoodY_ = 5;
                    gamePixelDx_ = 1;
                    gamePixelDy_ = 0;
                    gamePixelScore_ = 0;
                    gamePixelOver_ = false;
                }
            } else {
                const int16_t px = static_cast<int16_t>(arenaX + gamePixelX_ * cell);
                const int16_t py = static_cast<int16_t>(arenaY + gamePixelY_ * cell);
                if (cursorX < px && gamePixelDx_ != 1) {
                    gamePixelDx_ = -1;
                    gamePixelDy_ = 0;
                } else if (cursorX > px + cell - 1 && gamePixelDx_ != -1) {
                    gamePixelDx_ = 1;
                    gamePixelDy_ = 0;
                } else if (cursorY < py && gamePixelDy_ != 1) {
                    gamePixelDx_ = 0;
                    gamePixelDy_ = -1;
                } else if (cursorY > py + cell - 1 && gamePixelDy_ != -1) {
                    gamePixelDx_ = 0;
                    gamePixelDy_ = 1;
                }
            }
        } else if (w.kind == WindowKind::GameOrbit) {
            const int16_t logicalW = 120;
            const int16_t logicalH = 72;
            const int16_t topPad = 20;
            int16_t scale = static_cast<int16_t>((body.h - topPad - 10) / logicalH);
            const int16_t scaleByWidth = static_cast<int16_t>((body.w - 10) / logicalW);
            if (scaleByWidth < scale) scale = scaleByWidth;
            if (scale < 1) scale = 1;
            const int16_t arenaW = static_cast<int16_t>(logicalW * scale);
            const int16_t arenaX = static_cast<int16_t>(body.x + (body.w - arenaW) / 2);

            const int16_t target = static_cast<int16_t>((cursorX - arenaX) / scale - 12);
            if (target < 0) {
                orbitPaddleX_ = 0;
            } else {
                orbitPaddleX_ = target;
                const int16_t maxX = 96;
                if (orbitPaddleX_ > maxX) orbitPaddleX_ = maxX;
            }
            if (orbitOver_) {
                const Rect resetBtn{static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + body.h - 14), 46, 10};
                if (cursorX >= resetBtn.x && cursorY >= resetBtn.y && cursorX < resetBtn.x + resetBtn.w && cursorY < resetBtn.y + resetBtn.h) {
                    orbitBallX_ = 60;
                    orbitBallY_ = 48;
                    orbitDx_ = 1;
                    orbitDy_ = -1;
                    orbitScore_ = 0;
                    orbitOver_ = false;
                }
            }
        } else if (w.kind == WindowKind::Notifications) {
            const Rect clearBtn{static_cast<int16_t>(body.x + body.w - 50), static_cast<int16_t>(body.y + body.h - 13), 44, 9};
            if (cursorX >= clearBtn.x && cursorY >= clearBtn.y && cursorX < clearBtn.x + clearBtn.w && cursorY < clearBtn.y + clearBtn.h) {
                notificationCount_ = 0;
                notificationSlideY_ = -16;
                notificationHoldUntilMs_ = 0;
                notify("Notifications cleared");
            }
        } else if (w.kind == WindowKind::WifiManager) {
            const Rect scanBtn{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 18), 34, 10};
            const Rect connBtn{static_cast<int16_t>(body.x + 44), static_cast<int16_t>(body.y + 18), 34, 10};
            const Rect kbBtn{static_cast<int16_t>(body.x + 82), static_cast<int16_t>(body.y + 18), 34, 10};
            const Rect pingBtn{static_cast<int16_t>(body.x + 120), static_cast<int16_t>(body.y + 18), 28, 10};
            const Rect clearBtn{static_cast<int16_t>(body.x + 152), static_cast<int16_t>(body.y + 18), 40, 10};

            if (cursorX >= scanBtn.x && cursorY >= scanBtn.y && cursorX < scanBtn.x + scanBtn.w && cursorY < scanBtn.y + scanBtn.h) {
                int found = WiFi.scanNetworks(false, true);
                if (found < 0) found = 0;
                wifiCount_ = static_cast<uint8_t>(found > static_cast<int>(kWifiMaxNetworks) ? kWifiMaxNetworks : found);
                for (uint8_t i = 0; i < wifiCount_; ++i) {
                    String ssid = WiFi.SSID(i);
                    strlcpy(wifiSsid_[i], ssid.c_str(), sizeof(wifiSsid_[i]));
                    wifiRssi_[i] = WiFi.RSSI(i);
                }
                if (wifiCount_ == 0) {
                    strlcpy(wifiSsid_[0], kAutoWifiSsid, sizeof(wifiSsid_[0]));
                    wifiRssi_[0] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -42;
                    wifiCount_ = 1;
                }
                WiFi.scanDelete();
                wifiSelected_ = 0;
                notify("Scan complete");
            } else if (cursorX >= connBtn.x && cursorY >= connBtn.y && cursorX < connBtn.x + connBtn.w && cursorY < connBtn.y + connBtn.h) {
                if (wifiCount_ > 0 && !wifiConnecting_) {
                    WiFi.mode(WIFI_STA);
                    if (strncmp(wifiSsid_[wifiSelected_], kAutoWifiSsid, sizeof(wifiSsid_[wifiSelected_])) == 0 && wifiPassword_[0] == '\0') {
                        WiFi.begin(kAutoWifiSsid, kAutoWifiPassword);
                    } else {
                        WiFi.begin(wifiSsid_[wifiSelected_], wifiPassword_);
                    }
                    wifiConnecting_ = true;
                    wifiSilentAttempt_ = false;
                    wifiConnectStartMs_ = millis();
                    notify("Connecting...");
                }
            } else if (cursorX >= kbBtn.x && cursorY >= kbBtn.y && cursorX < kbBtn.x + kbBtn.w && cursorY < kbBtn.y + kbBtn.h) {
                keyboard_.open(wifiPassword_);
                wifiKeyboardOpen_ = true;
                notepadKeyboardOpen_ = false;
                browserKeyboardOpen_ = false;
                browserKeyboardEditingForm_ = false;
            } else if (cursorX >= pingBtn.x && cursorY >= pingBtn.y && cursorX < pingBtn.x + pingBtn.w && cursorY < pingBtn.y + pingBtn.h) {
                wifiPingOk_ = false;
                wifiPingMs_ = 0;
                if (WiFi.status() == WL_CONNECTED) {
                    WiFiClient client;
                    const uint32_t start = millis();
                    wifiPingOk_ = client.connect("8.8.8.8", 53, 500);
                    wifiPingMs_ = static_cast<uint16_t>(millis() - start);
                    client.stop();
                }
                notify(wifiPingOk_ ? "Ping ok" : "Ping failed");
            } else if (cursorX >= clearBtn.x && cursorY >= clearBtn.y && cursorX < clearBtn.x + clearBtn.w && cursorY < clearBtn.y + clearBtn.h) {
                clearSavedWifi();
                notify("Saved WiFi cleared");
            }

            const int16_t listY = static_cast<int16_t>(body.y + 34);
            if (cursorY >= listY && cursorY < listY + 52) {
                const uint8_t row = static_cast<uint8_t>((cursorY - listY) / 8);
                if (row < wifiCount_) {
                    wifiSelected_ = row;
                    for (uint8_t i = 0; i < savedWifiCount_; ++i) {
                        if (strncmp(savedWifiSsid_[i], wifiSsid_[wifiSelected_], sizeof(savedWifiSsid_[i])) == 0) {
                            strlcpy(wifiPassword_, savedWifiPwd_[i], sizeof(wifiPassword_));
                            break;
                        }
                    }
                }
            }

            if (wifiKeyboardOpen_) {
                const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};
                if (keyboard_.onClick(cursorX, cursorY, keyRect)) {
                    if (keyboard_.consumeAccepted()) {
                        strlcpy(wifiPassword_, keyboard_.value(), sizeof(wifiPassword_));
                        wifiKeyboardOpen_ = false;
                    }
                }
            }
        } else if (w.kind == WindowKind::Browser) {
            const Rect urlBar{static_cast<int16_t>(body.x + 4), static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 26), 12};
            const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};

            if (cursorX >= urlBar.x && cursorY >= urlBar.y && cursorX < urlBar.x + urlBar.w && cursorY < urlBar.y + urlBar.h) {
                keyboard_.open(browserUrl_, true);
                browserKeyboardOpen_ = true;
                browserKeyboardEditingForm_ = false;
                wifiKeyboardOpen_ = false;
                notepadKeyboardOpen_ = false;
                markDirty(windowBody(w));
                updateHover(cursorX, cursorY);
                return;
            }

            if (browserKeyboardOpen_ && keyboard_.onClick(cursorX, cursorY, keyRect)) {
                if (browserKeyboardEditingForm_) {
                    if (keyboard_.consumeAccepted()) {
                        browserApp_.applyFormKeyboardValue(keyboard_.value(), true);
                        browserKeyboardOpen_ = false;
                        browserKeyboardEditingForm_ = false;
                    } else if (!keyboard_.isOpen()) {
                        browserApp_.applyFormKeyboardValue(keyboard_.value(), false);
                        browserKeyboardOpen_ = false;
                        browserKeyboardEditingForm_ = false;
                    }
                } else {
                    strlcpy(browserUrl_, keyboard_.value(), sizeof(browserUrl_));
                    browserApp_.setCurrentUrlText(browserUrl_);
                    if (keyboard_.consumeAccepted()) {
                        browserKeyboardOpen_ = false;
                        browserKeyboardEditingForm_ = false;
                        browserOpenUrl(browserUrl_, true);
                    } else if (!keyboard_.isOpen()) {
                        browserKeyboardOpen_ = false;
                        browserKeyboardEditingForm_ = false;
                    }
                }
                markDirty(windowBody(w));
                updateHover(cursorX, cursorY);
                return;
            }

            if (browserApp_.handleClick(cursorX, cursorY, body)) {
                strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
                strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
                browserLoading_ = browserApp_.isLoading();
                browserLoaded_ = browserApp_.isLoaded();

                char formInitial[512] = "";
                if (browserApp_.consumeFormKeyboardRequest(formInitial, sizeof(formInitial))) {
                    keyboard_.open(formInitial, false);
                    browserKeyboardOpen_ = true;
                    browserKeyboardEditingForm_ = true;
                }

                char requestedImage[100] = "";
                if (browserApp_.consumeOpenImageRequest(requestedImage, sizeof(requestedImage))) {
                    int8_t imgWin = createKindWindow(WindowKind::ImageVisualizer);
                    if (imgWin >= 0) {
                        Window& iw = windows_[static_cast<uint8_t>(imgWin)];
                        strlcpy(iw.title, "Image visualizer", sizeof(iw.title));
                        strlcpy(imageViewerSrcByWindow_[iw.id], requestedImage, sizeof(imageViewerSrcByWindow_[iw.id]));
                    }
                }
                markDirty(windowBody(w));
            }
        }

        updateHover(cursorX, cursorY);
        return;
    }

    if (event.type == katux::core::EventType::ButtonUp && event.source == katux::core::EventSource::ButtonA) {
        browserHoldScrollDir_ = 0;
        browserHoldScrollNextMs_ = 0;
        browserHoldScrollStartMs_ = 0;
    }

    if (event.type == katux::core::EventType::ButtonUp && event.source == katux::core::EventSource::ButtonB && focusedZ_ >= 0) {
        Window& w = windows_[zOrder_[focusedZ_]];
        if (!w.visible || w.minimized) return;
        if (w.kind == WindowKind::Demo) {
            w.meter = static_cast<uint8_t>((w.meter + 9U) % 101U);
        }
    }
}

void WindowManager::render(Renderer& renderer, const Theme& t, const Rect* clip) {
    if (notificationCount_ > 0 && notificationSlideY_ > -16) {
        const Rect top{0, notificationSlideY_, 240, 16};
        if (!clip || intersects(*clip, top)) {
            renderer.fillRect(top, 0x18C3);
            renderer.drawRect(top, 0xFFFF);
            renderer.drawText(4, static_cast<int16_t>(notificationSlideY_ + 4), notifications_[notificationCount_ - 1], 0xFFFF, 0x18C3);
        }
    }

    for (uint8_t z = 0; z < count_; ++z) {
        Window& w = windows_[zOrder_[z]];
        if (!w.visible || w.minimized) continue;

        Rect wr{w.x, w.y, w.width, w.height};
        if (clip && !intersects(*clip, wr)) {
            continue;
        }
        if (!clip && isCoveredByTopWindow(wr, z)) {
            continue;
        }

        const bool hovered = (hoverZ_ == static_cast<int8_t>(z));
        const Rect shadow{static_cast<int16_t>(w.x + 2), static_cast<int16_t>(w.y + 2), w.width, w.height};
        renderer.fillRect(shadow, hovered ? 0x3186 : 0x18A3);

        renderer.fillRect(wr, t.windowBg);
        renderer.drawRect(wr, hovered ? 0xFFFF : t.windowBorder);

        if (!w.fullscreen) {
            const uint16_t titleColor = w.focused ? t.titleActive : (hovered ? 0x5AEB : t.titleInactive);
            renderer.fillRect({w.x + 1, w.y + 1, static_cast<int16_t>(w.width - 2), 18}, titleColor);

            const Rect iconBox{static_cast<int16_t>(w.x + 3), static_cast<int16_t>(w.y + 5), 8, 8};
            renderer.fillRect(iconBox, 0x0000);
            if (w.kind == WindowKind::Demo) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 1}, 0xFFE0);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 3), 4, 1}, 0x07FF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 5), 5, 1}, 0x07E0);
            } else if (w.kind == WindowKind::Settings) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 1), 2, 6}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 3), 6, 2}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 3), 2, 2}, 0x0000);
            } else if (w.kind == WindowKind::TaskManager) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 5), 2, 2}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 4), static_cast<int16_t>(iconBox.y + 3), 2, 4}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 7), static_cast<int16_t>(iconBox.y + 1), 1, 6}, 0xFFFF);
            } else if (w.kind == WindowKind::Explorer) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 2), 6, 5}, 0xFFE0);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 3, 1}, 0xFFE0);
            } else if (w.kind == WindowKind::Notepad) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 6}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 2), 4, 1}, 0x3186);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 4), 4, 1}, 0x3186);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 6), 4, 1}, 0x3186);
            } else if (w.kind == WindowKind::AppHub) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 3, 5}, 0xFFE0);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 4), static_cast<int16_t>(iconBox.y + 1), 4, 6}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 5), static_cast<int16_t>(iconBox.y + 3), 2, 1}, 0x3186);
            } else if (w.kind == WindowKind::GamePixel) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 6}, 0x07E0);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 4), static_cast<int16_t>(iconBox.y + 4), 2, 2}, 0xF800);
            } else if (w.kind == WindowKind::GameOrbit) {
                renderer.drawRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 6}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 3), 2, 2}, 0xFFE0);
            } else if (w.kind == WindowKind::Notifications) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 2), 6, 5}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 1), 2, 1}, 0xFFFF);
            } else if (w.kind == WindowKind::WifiManager) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 6), 6, 1}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 4), 4, 1}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 2), 2, 1}, 0xFFFF);
            } else if (w.kind == WindowKind::Browser) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 6}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 2), 4, 4}, 0x39E7);
            } else if (w.kind == WindowKind::ImageVisualizer) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 6}, 0xFFFF);
                renderer.drawRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 2), 4, 4}, 0x001F);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 3), static_cast<int16_t>(iconBox.y + 3), 1, 1}, 0x07E0);
            } else if (w.kind == WindowKind::DesktopConfig) {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 1), 6, 1}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 3), 6, 1}, 0xFFFF);
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 1), static_cast<int16_t>(iconBox.y + 5), 6, 1}, 0xFFFF);
            } else {
                renderer.fillRect({static_cast<int16_t>(iconBox.x + 2), static_cast<int16_t>(iconBox.y + 2), 4, 4}, 0xFFFF);
            }

            renderer.drawText(w.x + 14, w.y + 5, w.title, t.text, titleColor);

            const bool hoverResize = hovered && hoverControl_ == 3;
            const bool hoverMin = hovered && hoverControl_ == 1;
            const bool hoverClose = hovered && hoverControl_ == 2;
            const uint16_t resizeBg = hoverResize ? 0xFFE0 : (w.focused ? 0xBDF7 : 0x9CD3);
            const uint16_t minBg = hoverMin ? 0xA514 : (w.focused ? 0x7BCF : 0x8C71);
            const uint16_t closeBg = hoverClose ? 0xF8E4 : (w.focused ? 0xFD20 : 0x9CD3);
            const Rect resizeRect{static_cast<int16_t>(w.x + w.width - 40), static_cast<int16_t>(w.y + 4), 10, 10};
            const Rect minRect{static_cast<int16_t>(w.x + w.width - 28), static_cast<int16_t>(w.y + 4), 10, 10};
            const Rect closeRect{static_cast<int16_t>(w.x + w.width - 16), static_cast<int16_t>(w.y + 4), 10, 10};
            renderer.fillRect(resizeRect, resizeBg);
            renderer.fillRect(minRect, minBg);
            renderer.fillRect(closeRect, closeBg);
            renderer.drawRect(resizeRect, 0x0000);
            renderer.fillRect({static_cast<int16_t>(resizeRect.x + 2), static_cast<int16_t>(resizeRect.y + 6), 5, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(resizeRect.x + 6), static_cast<int16_t>(resizeRect.y + 3), 1, 4}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(minRect.x + 2), static_cast<int16_t>(minRect.y + 6), 6, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 2), static_cast<int16_t>(closeRect.y + 2), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 3), static_cast<int16_t>(closeRect.y + 3), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 6), static_cast<int16_t>(closeRect.y + 2), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 5), static_cast<int16_t>(closeRect.y + 3), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 2), static_cast<int16_t>(closeRect.y + 6), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 3), static_cast<int16_t>(closeRect.y + 5), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 6), static_cast<int16_t>(closeRect.y + 6), 1, 1}, 0x0000);
            renderer.fillRect({static_cast<int16_t>(closeRect.x + 5), static_cast<int16_t>(closeRect.y + 5), 1, 1}, 0x0000);

            if (draggingZ_ == static_cast<int8_t>(z)) {
                renderer.fillRect({static_cast<int16_t>(w.x + w.width - 42), static_cast<int16_t>(w.y + 7), 10, 4}, 0xFFE0);
                renderer.drawRect({static_cast<int16_t>(w.x + w.width - 42), static_cast<int16_t>(w.y + 7), 10, 4}, 0x0000);
            }
        }

        if (w.kind == WindowKind::Demo) {
            renderDemo(renderer, t, w, clip);
        } else if (w.kind == WindowKind::Settings) {
            renderSettings(renderer, t, w, clip);
        } else if (w.kind == WindowKind::TaskManager) {
            renderTaskManager(renderer, t, w, clip);
        } else if (w.kind == WindowKind::Explorer) {
            renderExplorer(renderer, t, w, clip);
        } else if (w.kind == WindowKind::Notepad) {
            renderNotepad(renderer, t, w, clip);
        } else if (w.kind == WindowKind::AppHub) {
            renderAppHub(renderer, t, w, clip);
        } else if (w.kind == WindowKind::GamePixel) {
            renderGamePixel(renderer, t, w, clip);
        } else if (w.kind == WindowKind::GameOrbit) {
            renderGameOrbit(renderer, t, w, clip);
        } else if (w.kind == WindowKind::Notifications) {
            renderNotifications(renderer, t, w, clip);
        } else if (w.kind == WindowKind::WifiManager) {
            renderWifiManager(renderer, t, w, clip);
        } else if (w.kind == WindowKind::Browser) {
            renderBrowser(renderer, t, w, clip);
        } else if (w.kind == WindowKind::ImageVisualizer) {
            renderImageVisualizer(renderer, t, w, clip);
        } else if (w.kind == WindowKind::DesktopConfig) {
            renderDesktopConfig(renderer, t, w, clip);
        }
    }
}

uint8_t WindowManager::listTaskItems(WindowTaskItem* out, uint8_t maxItems, bool minimizedOnly) const {
    if (!out || maxItems == 0) return 0;

    uint8_t written = 0;
    for (uint8_t i = 0; i < count_ && written < maxItems; ++i) {
        const Window& w = windows_[zOrder_[i]];
        if (!w.visible) continue;
        if (minimizedOnly && !w.minimized) continue;
        out[written].id = w.id;
        strlcpy(out[written].title, w.title, sizeof(out[written].title));
        out[written].kind = w.kind;
        out[written].minimized = w.minimized;
        out[written].focused = w.focused;
        ++written;
    }
    return written;
}

bool WindowManager::windowIsActive(uint8_t id) const {
    for (uint8_t i = 0; i < count_; ++i) {
        const Window& w = windows_[i];
        if (w.id == id) {
            return w.visible;
        }
    }
    return false;
}

void WindowManager::setSystemState(bool darkTheme, uint8_t brightness, uint8_t cursorSpeed, uint8_t performanceProfile, bool debugOverlay, bool animationsEnabled) {
    darkTheme_ = darkTheme;
    brightness_ = brightness;
    cursorSpeed_ = cursorSpeed;
    performanceProfile_ = performanceProfile;
    debugOverlay_ = debugOverlay;
    animationsEnabled_ = animationsEnabled;
}

void WindowManager::setRuntimeStats(uint8_t fps, uint32_t freeHeap, uint32_t uptimeMs, uint8_t queueDepth) {
    fps_ = fps;
    freeHeap_ = freeHeap;
    uptimeMs_ = uptimeMs;
    queueDepth_ = queueDepth;
}

void WindowManager::notify(const char* text) {
    if (!text || !text[0]) return;
    if (notificationCount_ >= 8) {
        for (uint8_t i = 1; i < 8; ++i) {
            strlcpy(notifications_[i - 1], notifications_[i], sizeof(notifications_[0]));
        }
        notificationCount_ = 7;
    }
    strlcpy(notifications_[notificationCount_], text, sizeof(notifications_[0]));
    ++notificationCount_;
    notificationSlideY_ = -16;
    notificationHoldUntilMs_ = millis() + 1800U;
    markDirty({0, -16, 240, 32});
}

uint8_t WindowManager::consumeDirtyRegions(Rect* out, uint8_t maxItems) {
    if (!out || maxItems == 0 || dirtyCount_ == 0) return 0;
    const uint8_t n = dirtyCount_ < maxItems ? dirtyCount_ : maxItems;
    for (uint8_t i = 0; i < n; ++i) {
        out[i] = dirty_[i];
    }
    dirtyCount_ = 0;
    return n;
}

void WindowManager::invalidateAll() {
    dirty_[0] = {0, 0, 240, 135};
    dirtyCount_ = 1;
}

CursorStyle WindowManager::cursorStyle() const {
    if (hasCapturedApp()) {
        return CursorStyle::Input;
    }
    if (millis() < clickPulseUntilMs_) {
        return CursorStyle::Click;
    }
    if (draggingZ_ >= 0) {
        return CursorStyle::Drag;
    }
    if (hoverInput_) {
        return CursorStyle::Input;
    }
    if (wifiConnecting_ || browserLoading_) {
        return CursorStyle::Busy;
    }
    if (hoverControl_ != 0 || hoverZ_ >= 0) {
        return CursorStyle::Hover;
    }
    return CursorStyle::Idle;
}

bool WindowManager::hasCapturedApp() const {
    if (focusedZ_ < 0 || focusedZ_ >= count_) return false;
    const Window& w = windows_[zOrder_[focusedZ_]];
    return w.visible && !w.minimized && w.fullscreen && isGameKind(w.kind);
}

bool WindowManager::needsFrameTick() const {
    if (wifiConnecting_ || browserLoading_) return true;
    if (notificationCount_ > 0 && notificationSlideY_ > -16) return true;
    if (hasCapturedApp()) return true;

    for (uint8_t i = 0; i < count_; ++i) {
        const Window& w = windows_[i];
        if (!w.visible || w.minimized) continue;
        if (w.width != w.targetWidth || w.height != w.targetHeight) {
            return true;
        }
    }
    return false;
}

bool WindowManager::hasWindowAt(int16_t x, int16_t y) const {
    return hitTest(x, y) >= 0;
}

void WindowManager::getDesktopConfig(bool& showIcons, WindowKind* slots, uint8_t maxSlots) const {
    showIcons = desktopShowIcons_;
    if (!slots || maxSlots == 0) return;
    uint8_t n = maxSlots < kDesktopSlotCount ? maxSlots : kDesktopSlotCount;
    for (uint8_t i = 0; i < n; ++i) {
        slots[i] = desktopSlots_[i];
    }
}

void WindowManager::setDesktopIconsVisible(bool visible) {
    if (desktopShowIcons_ == visible) return;
    desktopShowIcons_ = visible;
    desktopSaveConfig();
    markDirty({0, 0, 240, 111});
}

void WindowManager::markDirty(const Rect& rect) {
    Rect c = rect;
    if (c.x < 0) {
        c.w += c.x;
        c.x = 0;
    }
    if (c.y < 0) {
        c.h += c.y;
        c.y = 0;
    }
    if (c.x >= 240 || c.y >= 135) return;
    if (c.x + c.w > 240) c.w = 240 - c.x;
    if (c.y + c.h > 135) c.h = 135 - c.y;
    if (c.w <= 0 || c.h <= 0) return;

    for (uint8_t i = 0; i < dirtyCount_; ++i) {
        Rect& d = dirty_[i];
        const bool overlap = !(c.x + c.w < d.x || d.x + d.w < c.x || c.y + c.h < d.y || d.y + d.h < c.y);
        if (!overlap) continue;
        const int16_t nx = c.x < d.x ? c.x : d.x;
        const int16_t ny = c.y < d.y ? c.y : d.y;
        const int16_t rx = (c.x + c.w) > (d.x + d.w) ? (c.x + c.w) : (d.x + d.w);
        const int16_t by = (c.y + c.h) > (d.y + d.h) ? (c.y + c.h) : (d.y + d.h);
        d = {nx, ny, static_cast<int16_t>(rx - nx), static_cast<int16_t>(by - ny)};
        if (d.x + d.w > 240) d.w = 240 - d.x;
        if (d.y + d.h > 135) d.h = 135 - d.y;
        return;
    }

    if (dirtyCount_ >= kDirtyCapacity) {
        invalidateAll();
        return;
    }

    dirty_[dirtyCount_++] = c;
}

void WindowManager::markDirtyWindow(const Window& before, const Window& after) {
    markDirty(frameRect(before));
    markDirty(frameRect(after));
}

Rect WindowManager::frameRect(const Window& w) const {
    return {static_cast<int16_t>(w.x - 2), static_cast<int16_t>(w.y - 2), static_cast<int16_t>(w.width + 4), static_cast<int16_t>(w.height + 4)};
}

void WindowManager::loadSavedWifi() {
    if (!wifiPrefsReady_) return;
    savedWifiCount_ = static_cast<uint8_t>(prefs_.getUChar("count", 0));
    if (savedWifiCount_ > kSavedWifiCount) savedWifiCount_ = kSavedWifiCount;
    for (uint8_t i = 0; i < savedWifiCount_; ++i) {
        char keySsid[8] = "ssid0";
        char keyPwd[8] = "pwd0";
        keySsid[4] = static_cast<char>('0' + i);
        keyPwd[3] = static_cast<char>('0' + i);
        String ssid = prefs_.getString(keySsid, "");
        String pwd = prefs_.getString(keyPwd, "");
        strlcpy(savedWifiSsid_[i], ssid.c_str(), sizeof(savedWifiSsid_[i]));
        strlcpy(savedWifiPwd_[i], pwd.c_str(), sizeof(savedWifiPwd_[i]));
    }
}

void WindowManager::saveWifiCredential(const char* ssid, const char* password) {
    if (!wifiPrefsReady_ || !ssid || !ssid[0] || !password || !password[0]) return;

    int8_t slot = -1;
    for (uint8_t i = 0; i < savedWifiCount_; ++i) {
        if (strncmp(savedWifiSsid_[i], ssid, sizeof(savedWifiSsid_[i])) == 0) {
            slot = static_cast<int8_t>(i);
            break;
        }
    }
    if (slot < 0) {
        if (savedWifiCount_ < kSavedWifiCount) {
            slot = static_cast<int8_t>(savedWifiCount_++);
        } else {
            slot = 0;
        }
    }

    strlcpy(savedWifiSsid_[slot], ssid, sizeof(savedWifiSsid_[slot]));
    strlcpy(savedWifiPwd_[slot], password, sizeof(savedWifiPwd_[slot]));

    prefs_.putUChar("count", savedWifiCount_);
    for (uint8_t i = 0; i < savedWifiCount_; ++i) {
        char keySsid[8] = "ssid0";
        char keyPwd[8] = "pwd0";
        keySsid[4] = static_cast<char>('0' + i);
        keyPwd[3] = static_cast<char>('0' + i);
        prefs_.putString(keySsid, savedWifiSsid_[i]);
        prefs_.putString(keyPwd, savedWifiPwd_[i]);
    }
}

void WindowManager::clearSavedWifi() {
    savedWifiCount_ = 0;
    for (uint8_t i = 0; i < kSavedWifiCount; ++i) {
        savedWifiSsid_[i][0] = '\0';
        savedWifiPwd_[i][0] = '\0';
    }
    wifiPassword_[0] = '\0';
    if (wifiPrefsReady_) {
        prefs_.clear();
    }
}

void WindowManager::desktopLoadConfig() {
    if (!desktopPrefsReady_) return;
    desktopShowIcons_ = desktopPrefs_.getBool("show", true);
    for (uint8_t i = 0; i < kDesktopSlotCount; ++i) {
        char key[8] = "s0";
        key[1] = static_cast<char>('0' + i);
        const uint8_t stored = desktopPrefs_.getUChar(key, static_cast<uint8_t>(desktopSlots_[i]));
        bool allowed = false;
        for (uint8_t j = 0; j < kDesktopAssignableCount; ++j) {
            if (static_cast<uint8_t>(kDesktopAssignableKinds[j]) == stored) {
                allowed = true;
                break;
            }
        }
        desktopSlots_[i] = allowed ? static_cast<WindowKind>(stored) : WindowKind::Generic;
    }
}

void WindowManager::desktopSaveConfig() {
    if (!desktopPrefsReady_) return;
    desktopPrefs_.putBool("show", desktopShowIcons_);
    for (uint8_t i = 0; i < kDesktopSlotCount; ++i) {
        char key[8] = "s0";
        key[1] = static_cast<char>('0' + i);
        desktopPrefs_.putUChar(key, static_cast<uint8_t>(desktopSlots_[i]));
    }
}

uint8_t WindowManager::explorerFolderForName(const char* name) const {
    if (!name || !name[0]) return 0;
    const char* ext = strrchr(name, '.');
    if (!ext || !ext[1]) return 0;
    if (strcmp(ext, ".txt") == 0 || strcmp(ext, ".log") == 0 || strcmp(ext, ".cfg") == 0) return 0;
    if (strcmp(ext, ".bin") == 0 || strcmp(ext, ".ino") == 0 || strcmp(ext, ".cpp") == 0 || strcmp(ext, ".h") == 0) return 1;
    return 2;
}

void WindowManager::refreshExplorerEntries() {
    explorerCount_ = 0;
    explorerSelected_ = -1;
    if (!SPIFFS.begin(false)) {
        return;
    }

    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }

    File file = root.openNextFile();
    while (file && explorerCount_ < kExplorerEntryCount) {
        if (!file.isDirectory()) {
            const char* full = file.name();
            const char* name = full;
            if (name[0] == '/') ++name;
            copyTrim(explorer_[explorerCount_].name, sizeof(explorer_[explorerCount_].name), name, 30);
            explorer_[explorerCount_].folder = explorerFolderForName(explorer_[explorerCount_].name);
            explorer_[explorerCount_].deleted = false;
            ++explorerCount_;
        }
        file = root.openNextFile();
    }
    root.close();
}

void WindowManager::browserSetStatus(const char* text) {
    if (!text || !text[0]) return;
    copyTrim(browserStatus_, sizeof(browserStatus_), text, sizeof(browserStatus_) - 1);
}

void WindowManager::browserClearDocument() {
    browserItemCount_ = 0;
    browserClickCount_ = 0;
    browserLoaded_ = false;
    browserScrollY_ = 0;
    browserAlert_[0] = '\0';
    browserApp_.clearDocument();
}

void WindowManager::browserLoadState() {
    if (!browserPrefsReady_) return;

    browserHistoryCount_ = browserPrefs_.getUChar("hcount", 0);
    if (browserHistoryCount_ > kBrowserHistoryMax) browserHistoryCount_ = kBrowserHistoryMax;
    for (uint8_t i = 0; i < kBrowserHistoryMax; ++i) {
        char key[8] = "h0";
        key[1] = static_cast<char>('0' + i);
        if (i < browserHistoryCount_) {
            String v = browserPrefs_.getString(key, "");
            strlcpy(browserHistory_[i], v.c_str(), sizeof(browserHistory_[0]));
        } else {
            browserHistory_[i][0] = '\0';
        }
    }
    browserHistoryIndex_ = browserHistoryCount_ > 0 ? static_cast<int8_t>(browserHistoryCount_ - 1U) : -1;

    browserFavoriteCount_ = browserPrefs_.getUChar("fcount", 0);
    if (browserFavoriteCount_ > kBrowserFavoriteMax) browserFavoriteCount_ = kBrowserFavoriteMax;
    for (uint8_t i = 0; i < kBrowserFavoriteMax; ++i) {
        char key[8] = "f0";
        key[1] = static_cast<char>('0' + i);
        if (i < browserFavoriteCount_) {
            String v = browserPrefs_.getString(key, "");
            strlcpy(browserFavorites_[i], v.c_str(), sizeof(browserFavorites_[0]));
        } else {
            browserFavorites_[i][0] = '\0';
        }
    }

    String lastUrl = browserPrefs_.getString("url", "");
    if (lastUrl.length() > 0) {
        strlcpy(browserUrl_, lastUrl.c_str(), sizeof(browserUrl_));
    } else if (browserHistoryCount_ > 0) {
        strlcpy(browserUrl_, browserHistory_[browserHistoryCount_ - 1U], sizeof(browserUrl_));
    }
}

void WindowManager::browserSaveState() {
    if (!browserPrefsReady_) return;

    browserPrefs_.putUChar("hcount", browserHistoryCount_);
    for (uint8_t i = 0; i < kBrowserHistoryMax; ++i) {
        char key[8] = "h0";
        key[1] = static_cast<char>('0' + i);
        browserPrefs_.putString(key, i < browserHistoryCount_ ? browserHistory_[i] : "");
    }

    browserPrefs_.putUChar("fcount", browserFavoriteCount_);
    for (uint8_t i = 0; i < kBrowserFavoriteMax; ++i) {
        char key[8] = "f0";
        key[1] = static_cast<char>('0' + i);
        browserPrefs_.putString(key, i < browserFavoriteCount_ ? browserFavorites_[i] : "");
    }

    browserPrefs_.putString("url", browserUrl_);
}

void WindowManager::browserPushHistory(const char* url) {
    if (!url || !url[0]) return;
    if (browserHistoryIndex_ >= 0 && browserHistoryIndex_ < static_cast<int8_t>(kBrowserHistoryMax) &&
        strncmp(browserHistory_[browserHistoryIndex_], url, sizeof(browserHistory_[0])) == 0) {
        browserSaveState();
        return;
    }

    if (browserHistoryIndex_ >= 0 && browserHistoryIndex_ < static_cast<int8_t>(browserHistoryCount_ - 1U)) {
        browserHistoryCount_ = static_cast<uint8_t>(browserHistoryIndex_ + 1);
    }

    if (browserHistoryCount_ < kBrowserHistoryMax) {
        strlcpy(browserHistory_[browserHistoryCount_], url, sizeof(browserHistory_[0]));
        ++browserHistoryCount_;
        browserHistoryIndex_ = static_cast<int8_t>(browserHistoryCount_ - 1U);
        browserSaveState();
        return;
    }

    for (uint8_t i = 1; i < kBrowserHistoryMax; ++i) {
        strlcpy(browserHistory_[i - 1], browserHistory_[i], sizeof(browserHistory_[0]));
    }
    strlcpy(browserHistory_[kBrowserHistoryMax - 1], url, sizeof(browserHistory_[0]));
    browserHistoryCount_ = kBrowserHistoryMax;
    browserHistoryIndex_ = static_cast<int8_t>(kBrowserHistoryMax - 1U);
    browserSaveState();
}

void WindowManager::browserNavigateHistory(int8_t step) {
    browserApp_.navigateHistory(step);
    strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
    strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
    browserLoading_ = browserApp_.isLoading();
    browserLoaded_ = browserApp_.isLoaded();
}

bool WindowManager::browserResolveUrl(const char* baseUrl, const char* href, char* out, size_t outLen) const {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!href || !href[0]) return false;

    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strlcpy(out, href, outLen);
        return true;
    }

    if (href[0] == '#' || strncmp(href, "mailto:", 7) == 0) {
        return false;
    }

    bool baseHttps = false;
    if (!baseUrl) {
        return false;
    }
    const char* hostStart = nullptr;
    if (strncmp(baseUrl, "http://", 7) == 0) {
        hostStart = baseUrl + 7;
    } else if (strncmp(baseUrl, "https://", 8) == 0) {
        hostStart = baseUrl + 8;
        baseHttps = true;
    } else {
        return false;
    }

    const char* pathStart = strchr(hostStart, '/');
    char root[100] = "http://";
    if (baseHttps) {
        strlcpy(root, "https://", sizeof(root));
    }
    const size_t schemeLen = baseHttps ? 8U : 7U;
    if (!pathStart) {
        strlcpy(root + schemeLen, hostStart, sizeof(root) - schemeLen);
    } else {
        const size_t hostLen = static_cast<size_t>(pathStart - hostStart);
        if (hostLen + schemeLen + 1U >= sizeof(root)) return false;
        memcpy(root + schemeLen, hostStart, hostLen);
        root[schemeLen + hostLen] = '\0';
    }

    if (href[0] == '/') {
        snprintf(out, outLen, "%s%s", root, href);
        return true;
    }

    char baseDir[100] = "";
    if (!pathStart) {
        strlcpy(baseDir, "/", sizeof(baseDir));
    } else {
        strlcpy(baseDir, pathStart, sizeof(baseDir));
        char* slash = strrchr(baseDir, '/');
        if (slash) {
            slash[1] = '\0';
        } else {
            strlcpy(baseDir, "/", sizeof(baseDir));
        }
    }

    snprintf(out, outLen, "%s%s%s", root, baseDir, href);
    return true;
}

bool WindowManager::browserFetchDocument(const char* url, char* outBody, size_t outBodyLen, char* outType, size_t outTypeLen) {
    if (!outBody || outBodyLen < 2) return false;
    outBody[0] = '\0';
    if (outType && outTypeLen > 0) outType[0] = '\0';

    if (WiFi.status() != WL_CONNECTED) {
        browserSetStatus("WiFi disconnected");
        return false;
    }

    browserHttpsBlocked_ = false;
    if (!url || !url[0]) {
        browserSetStatus("URL vide");
        return false;
    }

    bool useHttps = false;
    const char* hostStart = nullptr;
    if (strncmp(url, "https://", 8) == 0) {
        useHttps = true;
        hostStart = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        hostStart = url + 7;
    } else {
        browserSetStatus("URL non supportee");
        return false;
    }

    const char* pathStart = strchr(hostStart, '/');
    char host[48] = "";
    char path[96] = "/";

    if (!pathStart) {
        copyTrim(host, sizeof(host), hostStart, sizeof(host) - 1);
    } else {
        const size_t hostLen = static_cast<size_t>(pathStart - hostStart);
        if (hostLen == 0 || hostLen >= sizeof(host)) {
            browserSetStatus("Host invalide");
            return false;
        }
        memcpy(host, hostStart, hostLen);
        host[hostLen] = '\0';
        strlcpy(path, pathStart, sizeof(path));
    }

    size_t bodyCap = outBodyLen - 1;
    const uint32_t heapNow = ESP.getFreeHeap();
    if (heapNow < 36000U && bodyCap > 900) bodyCap = 900;
    else if (heapNow < 54000U && bodyCap > 1400) bodyCap = 1400;
    else if (bodyCap > 2200) bodyCap = 2200;

    WiFiClient client;
    WiFiClientSecure clientSecure;
    Client* conn = &client;
    uint16_t port = 80;
    if (useHttps) {
        port = 443;
        clientSecure.setInsecure();
        conn = &clientSecure;
    }

    conn->setTimeout(2200);
    if (!conn->connect(host, port)) {
        browserSetStatus("Connexion echouee");
        return false;
    }

    conn->print("GET ");
    conn->print(path);
    conn->print(" HTTP/1.1\r\nHost: ");
    conn->print(host);
    conn->print("\r\nUser-Agent: KatuxMini/1.1\r\nConnection: close\r\nAccept: text/html,text/plain,*/*\r\n\r\n");

    bool headersDone = false;
    bool headerOverflow = false;
    char headerLine[160] = "";
    size_t headerLen = 0;
    size_t bodyLen = 0;
    bool truncated = false;
    const uint32_t start = millis();

    while (millis() - start < 2600U) {
        if (!conn->available()) {
            if (!conn->connected()) break;
            delay(1);
            continue;
        }

        const char c = static_cast<char>(conn->read());

        if (!headersDone) {
            if (c == '\r') continue;
            if (c == '\n') {
                headerLine[headerLen] = '\0';
                if (headerLen == 0) {
                    headersDone = true;
                } else if (outType && outTypeLen > 0) {
                    char lower[160] = "";
                    strlcpy(lower, headerLine, sizeof(lower));
                    for (size_t i = 0; lower[i]; ++i) lower[i] = static_cast<char>(tolower(lower[i]));
                    if (strncmp(lower, "content-type:", 13) == 0) {
                        const char* type = headerLine + 13;
                        while (*type == ' ' || *type == '\t') ++type;
                        char clean[40] = "";
                        copyTrim(clean, sizeof(clean), type, sizeof(clean) - 1);
                        for (size_t i = 0; clean[i]; ++i) clean[i] = static_cast<char>(tolower(clean[i]));
                        strlcpy(outType, clean, outTypeLen);
                    }
                }
                headerLen = 0;
                headerOverflow = false;
                continue;
            }

            if (!headerOverflow) {
                if (headerLen + 1 < sizeof(headerLine)) {
                    headerLine[headerLen++] = c;
                } else {
                    headerOverflow = true;
                }
            }
            continue;
        }

        if (bodyLen >= bodyCap) {
            truncated = true;
            break;
        }
        if (c != '\0') {
            outBody[bodyLen++] = c;
        }
    }

    conn->stop();
    outBody[bodyLen] = '\0';
    if (truncated) {
        browserSetStatus("Doc truncated");
    }
    return bodyLen > 0;
}

bool WindowManager::browserExtractAttr(const char* tag, const char* attr, char* out, size_t outLen) const {
    if (!tag || !attr || !out || outLen == 0) return false;
    out[0] = '\0';

    char lowerTag[180] = "";
    char lowerAttr[24] = "";
    copyTrim(lowerTag, sizeof(lowerTag), tag, sizeof(lowerTag) - 1);
    copyTrim(lowerAttr, sizeof(lowerAttr), attr, sizeof(lowerAttr) - 1);
    for (size_t i = 0; lowerTag[i]; ++i) lowerTag[i] = static_cast<char>(tolower(lowerTag[i]));
    for (size_t i = 0; lowerAttr[i]; ++i) lowerAttr[i] = static_cast<char>(tolower(lowerAttr[i]));

    const char* pos = strstr(lowerTag, lowerAttr);
    if (!pos) return false;
    const size_t idx = static_cast<size_t>(pos - lowerTag);
    const char* real = tag + idx;
    const char* eq = strchr(real, '=');
    if (!eq) return false;
    ++eq;
    while (*eq == ' ' || *eq == '\t') ++eq;

    char quote = 0;
    if (*eq == '"' || *eq == '\'') {
        quote = *eq;
        ++eq;
    }

    size_t w = 0;
    while (eq[w] && w + 1 < outLen) {
        if (quote) {
            if (eq[w] == quote) break;
        } else if (eq[w] == ' ' || eq[w] == '\t' || eq[w] == '>') {
            break;
        }
        out[w] = eq[w];
        ++w;
    }
    out[w] = '\0';
    return w > 0;
}

uint16_t WindowManager::browserParseCssColor(const char* value, uint16_t fallback) const {
    if (!value || !value[0]) return fallback;
    if (value[0] == '#' && strlen(value) >= 7) {
        char hex[7] = "";
        memcpy(hex, value + 1, 6);
        char* end = nullptr;
        long v = strtol(hex, &end, 16);
        if (!end || *end != '\0') return fallback;
        const uint8_t r = static_cast<uint8_t>((v >> 16) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((v >> 8) & 0xFF);
        const uint8_t b = static_cast<uint8_t>(v & 0xFF);
        return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    if (strncmp(value, "red", 3) == 0) return 0xF800;
    if (strncmp(value, "green", 5) == 0) return 0x07E0;
    if (strncmp(value, "blue", 4) == 0) return 0x001F;
    if (strncmp(value, "yellow", 6) == 0) return 0xFFE0;
    if (strncmp(value, "black", 5) == 0) return 0x0000;
    if (strncmp(value, "white", 5) == 0) return 0xFFFF;
    if (strncmp(value, "gray", 4) == 0 || strncmp(value, "grey", 4) == 0) return 0x8410;
    return fallback;
}

void WindowManager::browserApplyStyle(const char* style, uint16_t& fg, uint16_t& bg, bool& bold, bool& hidden) const {
    if (!style || !style[0]) return;

    char work[120] = "";
    copyTrim(work, sizeof(work), style, sizeof(work) - 1);
    for (size_t i = 0; work[i]; ++i) {
        work[i] = static_cast<char>(tolower(work[i]));
    }

    const char* color = strstr(work, "color:");
    if (color) {
        color += 6;
        while (*color == ' ' || *color == '\t') ++color;
        char value[16] = "";
        size_t n = 0;
        while (color[n] && color[n] != ';' && color[n] != ' ' && color[n] != '\t' && n + 1 < sizeof(value)) {
            value[n] = color[n];
            ++n;
        }
        value[n] = '\0';
        if (value[0]) fg = browserParseCssColor(value, fg);
    }

    const char* bgc = strstr(work, "background-color:");
    if (!bgc) bgc = strstr(work, "background:");
    if (bgc) {
        bgc = strchr(bgc, ':');
        if (bgc) {
            ++bgc;
            while (*bgc == ' ' || *bgc == '\t') ++bgc;
            char value[16] = "";
            size_t n = 0;
            while (bgc[n] && bgc[n] != ';' && bgc[n] != ' ' && bgc[n] != '\t' && n + 1 < sizeof(value)) {
                value[n] = bgc[n];
                ++n;
            }
            value[n] = '\0';
            if (value[0]) bg = browserParseCssColor(value, bg);
        }
    }

    if (strstr(work, "font-weight:bold") || strstr(work, "font-weight:700") || strstr(work, "font-weight:800")) {
        bold = true;
    }
    if (strstr(work, "display:none") || strstr(work, "visibility:hidden")) {
        hidden = true;
    }
}

void WindowManager::browserDecodeEntities(char* text) const {
    if (!text || !text[0]) return;
    char out[80] = "";
    size_t w = 0;
    for (size_t i = 0; text[i] && w + 1 < sizeof(out); ++i) {
        if (text[i] == '&') {
            if (strncmp(text + i, "&amp;", 5) == 0) {
                out[w++] = '&';
                i += 4;
                continue;
            }
            if (strncmp(text + i, "&lt;", 4) == 0) {
                out[w++] = '<';
                i += 3;
                continue;
            }
            if (strncmp(text + i, "&gt;", 4) == 0) {
                out[w++] = '>';
                i += 3;
                continue;
            }
            if (strncmp(text + i, "&nbsp;", 6) == 0) {
                out[w++] = ' ';
                i += 5;
                continue;
            }
        }
        out[w++] = text[i];
    }
    out[w] = '\0';
    strlcpy(text, out, 72);
}

void WindowManager::browserRunScript(const char* script, const char* contextUrl) {
    if (!script || !script[0]) return;

    const char* alertPos = strstr(script, "alert(");
    if (alertPos) {
        const char* q1 = strchr(alertPos, '\'');
        char quote = '\'';
        if (!q1) {
            q1 = strchr(alertPos, '"');
            quote = '"';
        }
        if (q1) {
            const char* q2 = strchr(q1 + 1, quote);
            if (q2 && q2 > q1 + 1) {
                const size_t n = static_cast<size_t>(q2 - (q1 + 1));
                char msg[56] = "";
                memcpy(msg, q1 + 1, n > sizeof(msg) - 1 ? sizeof(msg) - 1 : n);
                browserDecodeEntities(msg);
                strlcpy(browserAlert_, msg, sizeof(browserAlert_));
                browserAlertUntilMs_ = millis() + 2500U;
                browserSetStatus("JS alert");
            }
        }
    }

    const char* titlePos = strstr(script, "document.title");
    if (titlePos) {
        const char* eq = strchr(titlePos, '=');
        if (eq) {
            const char* q1 = strchr(eq, '\'');
            char quote = '\'';
            if (!q1) {
                q1 = strchr(eq, '"');
                quote = '"';
            }
            if (q1) {
                const char* q2 = strchr(q1 + 1, quote);
                if (q2 && q2 > q1 + 1) {
                    char title[40] = "";
                    const size_t n = static_cast<size_t>(q2 - (q1 + 1));
                    memcpy(title, q1 + 1, n > sizeof(title) - 1 ? sizeof(title) - 1 : n);
                    browserDecodeEntities(title);
                    strlcpy(browserTitle_, title, sizeof(browserTitle_));
                }
            }
        }
    }

    const char* locationPos = strstr(script, "location.href");
    if (locationPos) {
        const char* eq = strchr(locationPos, '=');
        if (eq) {
            const char* q1 = strchr(eq, '\'');
            char quote = '\'';
            if (!q1) {
                q1 = strchr(eq, '"');
                quote = '"';
            }
            if (q1) {
                const char* q2 = strchr(q1 + 1, quote);
                if (q2 && q2 > q1 + 1) {
                    char href[100] = "";
                    const size_t n = static_cast<size_t>(q2 - (q1 + 1));
                    memcpy(href, q1 + 1, n > sizeof(href) - 1 ? sizeof(href) - 1 : n);
                    char abs[100] = "";
                    if (browserResolveUrl(contextUrl, href, abs, sizeof(abs))) {
                        browserOpenUrl(abs, true);
                    }
                }
            }
        }
    }
}

void WindowManager::browserParseDocument(const char* html, const char* activeUrl) {
    browserItemCount_ = 0;
    browserScrollY_ = 0;

    if (!html || !html[0]) {
        browserSetStatus("Page vide");
        return;
    }

    const char* p = html;
    char activeHref[100] = "";
    bool inHeader = false;
    bool inHiddenBlock = false;

    uint16_t bodyFg = 0x2104;
    uint16_t bodyBg = 0xFFFF;
    uint16_t linkFg = 0x001F;

    uint16_t currentFg = bodyFg;
    uint16_t currentBg = bodyBg;
    bool currentBold = false;
    bool currentHidden = false;

    while (*p && browserItemCount_ < kBrowserItemMax) {
        if (*p == '<') {
            const char* gt = strchr(p, '>');
            if (!gt) break;

            char tag[180] = "";
            const size_t n = static_cast<size_t>(gt - p - 1);
            memcpy(tag, p + 1, n > sizeof(tag) - 1 ? sizeof(tag) - 1 : n);

            char lower[180] = "";
            strlcpy(lower, tag, sizeof(lower));
            for (size_t i = 0; lower[i]; ++i) lower[i] = static_cast<char>(tolower(lower[i]));

            const bool closing = lower[0] == '/';

            if (strncmp(lower, "script", 6) == 0) {
                const char* end = strstr(gt + 1, "</script>");
                if (!end) break;
                char script[120] = "";
                const size_t sn = static_cast<size_t>(end - (gt + 1));
                memcpy(script, gt + 1, sn > sizeof(script) - 1 ? sizeof(script) - 1 : sn);
                browserRunScript(script, activeUrl);
                p = end + 9;
                continue;
            }

            if (!closing && strncmp(lower, "style", 5) == 0) {
                const char* end = strstr(gt + 1, "</style>");
                if (!end) break;
                char css[240] = "";
                const size_t cn = static_cast<size_t>(end - (gt + 1));
                memcpy(css, gt + 1, cn > sizeof(css) - 1 ? sizeof(css) - 1 : cn);
                for (size_t i = 0; css[i]; ++i) css[i] = static_cast<char>(tolower(css[i]));

                const char* bodyRule = strstr(css, "body");
                if (bodyRule) {
                    const char* b0 = strchr(bodyRule, '{');
                    const char* b1 = b0 ? strchr(b0 + 1, '}') : nullptr;
                    if (b0 && b1 && b1 > b0 + 1) {
                        char decl[120] = "";
                        const size_t dn = static_cast<size_t>(b1 - (b0 + 1));
                        memcpy(decl, b0 + 1, dn > sizeof(decl) - 1 ? sizeof(decl) - 1 : dn);
                        bool bBold = false;
                        bool bHidden = false;
                        browserApplyStyle(decl, bodyFg, bodyBg, bBold, bHidden);
                    }
                }

                const char* aRule = strstr(css, "a{");
                if (!aRule) aRule = strstr(css, "a {");
                if (aRule) {
                    const char* a0 = strchr(aRule, '{');
                    const char* a1 = a0 ? strchr(a0 + 1, '}') : nullptr;
                    if (a0 && a1 && a1 > a0 + 1) {
                        char decl[120] = "";
                        const size_t dn = static_cast<size_t>(a1 - (a0 + 1));
                        memcpy(decl, a0 + 1, dn > sizeof(decl) - 1 ? sizeof(decl) - 1 : dn);
                        bool aBold = false;
                        bool aHidden = false;
                        uint16_t aBg = bodyBg;
                        browserApplyStyle(decl, linkFg, aBg, aBold, aHidden);
                    }
                }

                currentFg = activeHref[0] ? linkFg : (inHeader ? 0x0000 : bodyFg);
                currentBg = bodyBg;
                p = end + 8;
                continue;
            }

            if (!closing && strncmp(lower, "title", 5) == 0) {
                const char* end = strstr(gt + 1, "</title>");
                if (!end) break;
                char title[40] = "";
                const size_t tn = static_cast<size_t>(end - (gt + 1));
                memcpy(title, gt + 1, tn > sizeof(title) - 1 ? sizeof(title) - 1 : tn);
                browserDecodeEntities(title);
                strlcpy(browserTitle_, title, sizeof(browserTitle_));
                p = end + 8;
                continue;
            }

            if (!closing && (strncmp(lower, "head", 4) == 0 || strncmp(lower, "noscript", 8) == 0)) {
                inHiddenBlock = true;
            } else if (closing && (strncmp(lower, "/head", 5) == 0 || strncmp(lower, "/noscript", 9) == 0)) {
                inHiddenBlock = false;
            }

            if (!closing && (strncmp(lower, "a ", 2) == 0 || strncmp(lower, "a href", 6) == 0)) {
                if (!browserExtractAttr(tag, "href", activeHref, sizeof(activeHref))) {
                    activeHref[0] = '\0';
                }
                currentFg = linkFg;
            } else if (closing && strncmp(lower, "/a", 2) == 0) {
                activeHref[0] = '\0';
                currentFg = inHeader ? 0x0000 : bodyFg;
            } else if (!closing && (strncmp(lower, "h1", 2) == 0 || strncmp(lower, "h2", 2) == 0 || strncmp(lower, "h3", 2) == 0)) {
                inHeader = true;
                currentBold = true;
                currentFg = 0x0000;
            } else if (closing && (strncmp(lower, "/h1", 3) == 0 || strncmp(lower, "/h2", 3) == 0 || strncmp(lower, "/h3", 3) == 0)) {
                inHeader = false;
                currentBold = false;
                currentFg = activeHref[0] ? linkFg : bodyFg;
            } else if (!closing && strncmp(lower, "body", 4) == 0) {
                char bodyStyle[100] = "";
                if (browserExtractAttr(tag, "style", bodyStyle, sizeof(bodyStyle))) {
                    bool bBold = false;
                    bool bHidden = false;
                    browserApplyStyle(bodyStyle, bodyFg, bodyBg, bBold, bHidden);
                }
                currentFg = activeHref[0] ? linkFg : (inHeader ? 0x0000 : bodyFg);
                currentBg = bodyBg;
            } else if (closing && (strncmp(lower, "/p", 2) == 0 || strncmp(lower, "/div", 4) == 0 || strncmp(lower, "/span", 5) == 0 || strncmp(lower, "/li", 3) == 0)) {
                currentFg = activeHref[0] ? linkFg : (inHeader ? 0x0000 : bodyFg);
                currentBg = bodyBg;
                currentBold = inHeader;
                currentHidden = inHiddenBlock;
            }

            if (!closing) {
                char inlineStyle[100] = "";
                if (browserExtractAttr(tag, "style", inlineStyle, sizeof(inlineStyle))) {
                    uint16_t nextFg = activeHref[0] ? linkFg : (inHeader ? 0x0000 : currentFg);
                    uint16_t nextBg = currentBg;
                    bool nextBold = currentBold || inHeader;
                    bool nextHidden = inHiddenBlock || currentHidden;
                    browserApplyStyle(inlineStyle, nextFg, nextBg, nextBold, nextHidden);
                    currentFg = nextFg;
                    currentBg = nextBg;
                    currentBold = nextBold;
                    currentHidden = nextHidden;
                }
            }

            if (!closing && strncmp(lower, "img", 3) == 0) {
                BrowserItem& item = browserItems_[browserItemCount_++];
                item.kind = 2;
                item.bold = currentBold || inHeader;
                item.hidden = inHiddenBlock || currentHidden;
                item.imageLoaded = false;
                item.fg = currentFg;
                item.bg = currentBg;
                item.imageW = 56;
                item.imageH = 24;
                item.text[0] = '\0';
                item.href[0] = '\0';
                item.script[0] = '\0';

                char src[100] = "";
                char alt[72] = "";
                char width[12] = "";
                char height[12] = "";
                char style[100] = "";
                browserExtractAttr(tag, "src", src, sizeof(src));
                browserExtractAttr(tag, "alt", alt, sizeof(alt));
                browserExtractAttr(tag, "width", width, sizeof(width));
                browserExtractAttr(tag, "height", height, sizeof(height));
                browserExtractAttr(tag, "style", style, sizeof(style));
                if (style[0]) {
                    browserApplyStyle(style, item.fg, item.bg, item.bold, item.hidden);
                }
                if (src[0]) {
                    browserResolveUrl(activeUrl, src, item.href, sizeof(item.href));
                }
                if (alt[0]) {
                    strlcpy(item.text, alt, sizeof(item.text));
                    browserDecodeEntities(item.text);
                } else {
                    strlcpy(item.text, "image", sizeof(item.text));
                }
                if (width[0]) {
                    const int w = atoi(width);
                    if (w > 6) item.imageW = static_cast<uint16_t>(w > 72 ? 72 : w);
                }
                if (height[0]) {
                    const int h = atoi(height);
                    if (h > 6) item.imageH = static_cast<uint16_t>(h > 52 ? 52 : h);
                }
            }

            if ((strncmp(lower, "br", 2) == 0 || strncmp(lower, "/p", 2) == 0 || strncmp(lower, "/div", 4) == 0 ||
                 strncmp(lower, "/li", 3) == 0) && browserItemCount_ < kBrowserItemMax) {
                BrowserItem& sep = browserItems_[browserItemCount_++];
                sep.kind = 0;
                sep.bold = false;
                sep.hidden = false;
                sep.imageLoaded = false;
                sep.fg = bodyFg;
                sep.bg = bodyBg;
                sep.imageW = 0;
                sep.imageH = 0;
                strlcpy(sep.text, " ", sizeof(sep.text));
                sep.href[0] = '\0';
                sep.script[0] = '\0';
            }

            p = gt + 1;
            continue;
        }

        const char* lt = strchr(p, '<');
        const size_t n = lt ? static_cast<size_t>(lt - p) : strlen(p);
        if (!inHiddenBlock && n > 0 && browserItemCount_ < kBrowserItemMax) {
            char text[72] = "";
            memcpy(text, p, n > sizeof(text) - 1 ? sizeof(text) - 1 : n);

            for (size_t i = 0; text[i]; ++i) {
                if (text[i] == '\r' || text[i] == '\n' || text[i] == '\t') text[i] = ' ';
            }
            while (text[0] == ' ') {
                memmove(text, text + 1, strlen(text));
            }
            size_t len = strlen(text);
            while (len > 0 && text[len - 1] == ' ') {
                text[len - 1] = '\0';
                --len;
            }

            browserDecodeEntities(text);
            if (text[0]) {
                BrowserItem& item = browserItems_[browserItemCount_++];
                item.kind = activeHref[0] ? 1 : 0;
                item.bold = currentBold || inHeader;
                item.hidden = inHiddenBlock || currentHidden;
                item.imageLoaded = false;
                item.fg = activeHref[0] ? linkFg : (inHeader ? 0x0000 : currentFg);
                item.bg = currentBg;
                item.imageW = 0;
                item.imageH = 0;
                strlcpy(item.text, text, sizeof(item.text));
                item.script[0] = '\0';
                if (activeHref[0]) {
                    browserResolveUrl(activeUrl, activeHref, item.href, sizeof(item.href));
                } else {
                    item.href[0] = '\0';
                }
            }
        }

        p = lt ? lt : p + n;
    }

    if (browserItemCount_ == 0) {
        BrowserItem& item = browserItems_[browserItemCount_++];
        item.kind = 0;
        item.bold = false;
        item.hidden = false;
        item.imageLoaded = false;
        item.fg = 0x2104;
        item.bg = 0xFFFF;
        item.imageW = 0;
        item.imageH = 0;
        strlcpy(item.text, "No readable content", sizeof(item.text));
        item.href[0] = '\0';
        item.script[0] = '\0';
    }
}

void WindowManager::browserOpenUrl(const char* url, bool addToHistory) {
    if (!url || !url[0]) return;
    browserLoadMs_ = millis();
    browserApp_.openUrl(url, addToHistory);
    strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
    strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
    browserLoading_ = browserApp_.isLoading();
    browserLoaded_ = browserApp_.isLoaded();
    browserSaveState();
}

void WindowManager::browserOpenPreset(int8_t delta) {
    browserApp_.openPreset(delta);
    strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
    strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
    browserLoading_ = browserApp_.isLoading();
    browserLoaded_ = browserApp_.isLoaded();
    browserSaveState();
}

void WindowManager::browserLoadImagePreview(uint8_t itemIndex) {
    if (itemIndex >= browserItemCount_) return;
    BrowserItem& item = browserItems_[itemIndex];
    if (item.kind != 2) return;

    item.imageLoaded = true;
    if (item.imageW < 24) item.imageW = 24;
    if (item.imageH < 16) item.imageH = 16;
    if (item.imageW > 72) item.imageW = 72;
    if (item.imageH > 52) item.imageH = 52;
    browserSetStatus("Image resized before draw");
}

void WindowManager::browserHandleContentClick(int16_t x, int16_t y) {
    for (uint8_t i = 0; i < browserClickCount_; ++i) {
        const BrowserClickTarget& target = browserClickTargets_[i];
        if (x < target.rect.x || y < target.rect.y || x >= target.rect.x + target.rect.w || y >= target.rect.y + target.rect.h) {
            continue;
        }

        if (target.kind == 3) {
            browserToggleFavorite(browserUrl_);
            return;
        }

        if (target.kind == 4) {
            if (target.itemIndex < browserFavoriteCount_) {
                browserOpenUrl(browserFavorites_[target.itemIndex], true);
            }
            return;
        }

        if (target.kind == 5) {
            if (target.itemIndex < browserHistoryCount_) {
                const uint8_t idx = static_cast<uint8_t>(browserHistoryCount_ - 1U - target.itemIndex);
                browserHistoryIndex_ = static_cast<int8_t>(idx);
                browserOpenUrl(browserHistory_[idx], false);
            }
            return;
        }

        if (target.itemIndex >= browserItemCount_) return;
        const BrowserItem& item = browserItems_[target.itemIndex];

        if (target.kind == 2) {
            browserLoadImagePreview(target.itemIndex);
            return;
        }

        if (item.script[0]) {
            browserRunScript(item.script, browserUrl_);
            return;
        }

        if (strncmp(item.href, "javascript:", 11) == 0) {
            browserRunScript(item.href + 11, browserUrl_);
            return;
        }

        if (item.href[0]) {
            browserOpenUrl(item.href, true);
            return;
        }
    }
}

bool WindowManager::browserIsFavorite(const char* url) const {
    return browserApp_.isFavorite(url);
}

void WindowManager::browserToggleFavorite(const char* url) {
    if (!url || !url[0]) return;
    browserApp_.toggleFavorite();
    strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
    browserSaveState();
}

Rect WindowManager::windowBody(const Window& w) const {
    if (w.fullscreen) {
        return {w.x, w.y, w.width, w.height};
    }
    return {static_cast<int16_t>(w.x + 1), static_cast<int16_t>(w.y + 20), static_cast<int16_t>(w.width - 2), static_cast<int16_t>(w.height - 21)};
}

void WindowManager::renderDemo(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xE71C);
    renderer.drawText(body.x + 6, body.y + 6, "Demo Console", 0x0000, 0xE71C);

    const Rect meterBox{body.x + 6, body.y + 20, static_cast<int16_t>(body.w - 12), 10};
    renderer.fillRect(meterBox, 0xB596);
    renderer.drawRect(meterBox, 0x4A69);

    const int16_t meterW = static_cast<int16_t>(((meterBox.w - 2) * w.meter) / 100);
    if (meterW > 0) {
        const uint16_t meterColor = w.toggle ? 0x07E0 : 0x051D;
        renderer.fillRect({static_cast<int16_t>(meterBox.x + 1), static_cast<int16_t>(meterBox.y + 1), meterW, static_cast<int16_t>(meterBox.h - 2)}, meterColor);
    }

    const Rect badge{body.x + 6, body.y + 36, 54, 14};
    const uint16_t badgeBg = w.toggle ? 0x07E0 : 0x7BEF;
    const uint16_t badgeFg = w.toggle ? 0x0000 : 0x3186;
    renderer.fillRect(badge, badgeBg);
    renderer.drawRect(badge, 0x4A69);
    renderer.drawText(badge.x + 6, badge.y + 4, w.toggle ? "ACTIVE" : "IDLE", badgeFg, badgeBg);

    renderer.drawText(body.x + 66, body.y + 40, "A: toggle", 0x3186, 0xE71C);
    renderer.drawText(body.x + 66, body.y + 54, "B: boost", 0x3186, 0xE71C);
}

void WindowManager::renderSettings(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xEF5D);
    const char* title = settingsPage_ == 0 ? "System" : (settingsPage_ == 1 ? "Time" : "Advanced");
    renderer.drawText(body.x + 6, body.y + 6, title, 0x0000, 0xEF5D);

    const Rect leftArrow{static_cast<int16_t>(body.x + body.w - 62), static_cast<int16_t>(body.y + 4), 12, 10};
    const Rect rightArrow{static_cast<int16_t>(body.x + body.w - 46), static_cast<int16_t>(body.y + 4), 12, 10};
    renderer.fillRect(leftArrow, 0x7BEF);
    renderer.fillRect(rightArrow, 0x7BEF);
    renderer.drawText(leftArrow.x + 4, leftArrow.y + 1, "<", 0x0000, 0x7BEF);
    renderer.drawText(rightArrow.x + 4, rightArrow.y + 1, ">", 0x0000, 0x7BEF);

    char line[28] = "";
    for (uint8_t row = 0; row < 5; ++row) {
        const int16_t rowY = static_cast<int16_t>(body.y + 20 + row * 14);
        const uint16_t bg = (hoverZ_ == focusedZ_ && hoverControl_ == 0) ? 0xD69A : 0xCE18;
        renderer.fillRect({static_cast<int16_t>(body.x + 5), rowY, static_cast<int16_t>(body.w - 10), 12}, bg);

        line[0] = '\0';
        if (settingsPage_ == 0) {
            if (row == 0) snprintf(line, sizeof(line), "Theme: %s", darkTheme_ ? "Dark" : "Light");
            if (row == 1) snprintf(line, sizeof(line), "Brightness: %u", brightness_);
            if (row == 2) snprintf(line, sizeof(line), "Cursor speed: x%u", cursorSpeed_);
            if (row == 3) {
                const char* profiles[3] = {"Eco", "Balanced", "Turbo"};
                snprintf(line, sizeof(line), "Profile: %s", profiles[performanceProfile_ % 3]);
            }
            if (row == 4) snprintf(line, sizeof(line), "Animations: %s", animationsEnabled_ ? "ON" : "OFF");
        } else if (settingsPage_ == 1) {
            if (row == 0) snprintf(line, sizeof(line), "Auto time: %s", autoTime_ ? "ON" : "OFF");
            if (row == 1) snprintf(line, sizeof(line), "Timezone UTC%+d", timezoneOffset_);
            if (row == 2) snprintf(line, sizeof(line), "Year: %u", static_cast<unsigned>(manualYear_));
            if (row == 3) snprintf(line, sizeof(line), "Month: %02u", manualMonth_);
            if (row == 4) snprintf(line, sizeof(line), "Day: %02u", manualDay_);
        } else {
            if (row == 0) snprintf(line, sizeof(line), "Hour: %02u", manualHour_);
            if (row == 1) snprintf(line, sizeof(line), "Minute: %02u", manualMinute_);
            if (row == 2) snprintf(line, sizeof(line), "Clear saved WiFi");
            if (row == 3) snprintf(line, sizeof(line), "Debug overlay: %s", debugOverlay_ ? "ON" : "OFF");
            if (row == 4) snprintf(line, sizeof(line), "NTP fallback: HTTP date");
        }

        if (line[0]) {
            renderer.drawText(body.x + 8, rowY + 2, line, 0x2104, bg);
        }
    }
}

void WindowManager::renderTaskManager(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xE71C);
    renderer.fillRect({body.x + 2, static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 4), 12}, 0x2104);
    renderer.drawText(body.x + 6, body.y + 4, "Task Manager", 0xFFFF, 0x2104);

    const Rect processTab{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 16), 54, 10};
    const Rect perfTab{static_cast<int16_t>(body.x + 62), static_cast<int16_t>(body.y + 16), 54, 10};
    renderer.fillRect(processTab, taskTab_ == 0 ? 0x007D : 0x7BEF);
    renderer.fillRect(perfTab, taskTab_ == 1 ? 0x007D : 0x7BEF);
    renderer.drawText(processTab.x + 7, processTab.y + 1, "Process", taskTab_ == 0 ? 0xFFFF : 0x2104, taskTab_ == 0 ? 0x007D : 0x7BEF);
    renderer.drawText(perfTab.x + 10, perfTab.y + 1, "Perfs", taskTab_ == 1 ? 0xFFFF : 0x2104, taskTab_ == 1 ? 0x007D : 0x7BEF);

    if (taskTab_ == 0) {
        WindowTaskItem items[kMaxWindows]{};
        const uint8_t active = listTaskItems(items, kMaxWindows, false);
        uint8_t rows = 0;

        renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 30), static_cast<int16_t>(body.w - 12), static_cast<int16_t>(body.h - 46)}, 0xFFFF);
        renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 30), static_cast<int16_t>(body.w - 12), 10}, 0xBDF7);
        renderer.drawText(body.x + 8, body.y + 31, "Name", 0x0000, 0xBDF7);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 52), body.y + 31, "Status", 0x0000, 0xBDF7);

        for (uint8_t i = 0; i < active && rows < 6; ++i) {
            const bool sel = rows == taskTargetIndex_;
            const int16_t y = static_cast<int16_t>(body.y + 42 + rows * 9);
            const uint16_t bg = sel ? 0x5AEB : 0xFFFF;
            renderer.fillRect({static_cast<int16_t>(body.x + 8), y, static_cast<int16_t>(body.w - 16), 8}, bg);
            char titleShort[20] = "";
            copyTrim(titleShort, sizeof(titleShort), items[i].title, 14);
            renderer.drawText(body.x + 10, y + 1, titleShort, sel ? 0xFFFF : 0x2104, bg);
            renderer.drawText(static_cast<int16_t>(body.x + body.w - 50), y + 1, "Run", sel ? 0xFFFF : 0x39E7, bg);
            ++rows;
        }

        if (rows == 0) {
            renderer.drawText(body.x + 10, body.y + 44, "No process", 0x4208, 0xFFFF);
        }

        const Rect endBtn{static_cast<int16_t>(body.x + body.w - 66), static_cast<int16_t>(body.y + body.h - 14), 60, 10};
        renderer.fillRect(endBtn, 0xF800);
        renderer.drawText(endBtn.x + 4, endBtn.y + 1, "End Task", 0xFFFF, 0xF800);
    } else {
        const uint32_t now = millis();
        const uint32_t heapTotal = ESP.getHeapSize();
        const uint32_t heapFree = ESP.getFreeHeap();
        const uint32_t heapMin = ESP.getMinFreeHeap();
        const uint32_t heapMaxAlloc = ESP.getMaxAllocHeap();
        const uint32_t heapUsed = heapTotal > heapFree ? (heapTotal - heapFree) : 0;
        uint8_t ramPct = heapTotal > 0 ? static_cast<uint8_t>((heapUsed * 100UL) / heapTotal) : 0;
        if (ramPct > 100) ramPct = 100;

        const uint32_t psramTotal = ESP.getPsramSize();
        const uint32_t psramFree = ESP.getFreePsram();
        uint8_t psramPct = 0;
        bool psramAvailable = false;
        if (psramTotal > 0 && psramFree <= psramTotal) {
            psramAvailable = true;
            psramPct = static_cast<uint8_t>(((psramTotal - psramFree) * 100UL) / psramTotal);
            if (psramPct > 100) psramPct = 100;
        } else {
            psramPct = ramPct;
        }

        if (w.focused && now - taskPerfLastSampleMs_ >= 1000U) {
            taskPerfLastSampleMs_ = now;
            uint8_t cpuPct = static_cast<uint8_t>(fps_ > 70 ? 96 : (fps_ > 50 ? 74 : (fps_ > 30 ? 52 : 33)));
            const uint8_t queueBoost = queueDepth_ > 20 ? 25 : static_cast<uint8_t>(queueDepth_);
            cpuPct = static_cast<uint8_t>(cpuPct + queueBoost > 100 ? 100 : cpuPct + queueBoost);

            memmove(taskCpuSeries_, taskCpuSeries_ + 1, kTaskPerfPoints - 1);
            memmove(taskRamSeries_, taskRamSeries_ + 1, kTaskPerfPoints - 1);
            memmove(taskPsramSeries_, taskPsramSeries_ + 1, kTaskPerfPoints - 1);
            taskCpuSeries_[kTaskPerfPoints - 1] = cpuPct;
            taskRamSeries_[kTaskPerfPoints - 1] = ramPct;
            taskPsramSeries_[kTaskPerfPoints - 1] = psramPct;
        }

        auto drawSeries = [&](int16_t x, int16_t y, int16_t wBox, int16_t hBox, uint16_t bg, uint16_t lineColor, const uint8_t* series, const char* label) {
            renderer.fillRect({x, y, wBox, hBox}, bg);
            renderer.drawRect({x, y, wBox, hBox}, 0x8410);
            renderer.drawText(static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 1), label, 0x0000, bg);
            const int16_t graphY = static_cast<int16_t>(y + 9);
            const int16_t graphH = static_cast<int16_t>(hBox - 11);
            const int16_t graphW = static_cast<int16_t>(wBox - 4);
            const int16_t points = graphW < static_cast<int16_t>(kTaskPerfPoints) ? graphW : static_cast<int16_t>(kTaskPerfPoints);
            const int16_t start = static_cast<int16_t>(kTaskPerfPoints - points);
            for (int16_t i = 0; i < points; ++i) {
                const uint8_t v = series[start + i];
                int16_t bar = static_cast<int16_t>((graphH * v) / 100U);
                if (bar < 1) bar = 1;
                renderer.fillRect({static_cast<int16_t>(x + 2 + i), static_cast<int16_t>(graphY + graphH - bar), 1, bar}, lineColor);
            }
        };

        const int16_t boxW = static_cast<int16_t>(body.w - 12);
        drawSeries(static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 30), boxW, 18, 0xE7FF, 0x001F, taskCpuSeries_, "CPU");
        drawSeries(static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 50), boxW, 18, 0xEFFF, 0xF800, taskRamSeries_, "RAM");
        drawSeries(static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 70), boxW, 18, 0xE7F0, 0x07E0, taskPsramSeries_, psramAvailable ? "PSRAM" : "PSRAM(FB)");

        char perfLineA[44] = "";
        char perfLineB[44] = "";
        snprintf(perfLineA, sizeof(perfLineA), "H:%lu F:%lu M:%lu", static_cast<unsigned long>(heapTotal), static_cast<unsigned long>(heapFree), static_cast<unsigned long>(heapMin));
        snprintf(perfLineB, sizeof(perfLineB), "Max:%lu P:%lu", static_cast<unsigned long>(heapMaxAlloc), static_cast<unsigned long>(psramFree));
        renderer.drawText(body.x + 8, static_cast<int16_t>(body.y + body.h - 18), perfLineA, 0x2104, 0xE71C);
        renderer.drawText(body.x + 8, static_cast<int16_t>(body.y + body.h - 10), perfLineB, 0x2104, 0xE71C);
    }
}

void WindowManager::renderExplorer(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xC638);

    const char* folders[kFolderCount] = {"Home", "Apps", "Media"};
    for (uint8_t i = 0; i < kFolderCount; ++i) {
        const bool sel = i == explorerFolder_;
        const Rect tab{static_cast<int16_t>(body.x + 6 + i * 42), static_cast<int16_t>(body.y + 12), 38, 10};
        renderer.fillRect(tab, sel ? 0x5AEB : 0x7BEF);
        renderer.drawText(tab.x + 2, tab.y + 1, folders[i], sel ? 0xFFFF : 0x2104, sel ? 0x5AEB : 0x7BEF);
    }

    const Rect refreshBtn{static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + 12), 20, 10};
    renderer.fillRect(refreshBtn, 0xBDF7);
    renderer.drawText(refreshBtn.x + 3, refreshBtn.y + 1, "REF", 0x0000, 0xBDF7);

    renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 26), static_cast<int16_t>(body.w - 40), static_cast<int16_t>(body.h - 32)}, 0xE71C);

    uint8_t row = 0;
    for (uint8_t i = 0; i < explorerCount_; ++i) {
        if (explorer_[i].folder != explorerFolder_) continue;
        if (row >= 8) break;
        const bool selected = explorerSelected_ == static_cast<int8_t>(i);
        const int16_t y = static_cast<int16_t>(body.y + 29 + row * 10);
        const uint16_t bg = selected ? 0xFFE0 : 0xE71C;
        const uint16_t fg = selected ? 0x0000 : 0x2104;
        renderer.fillRect({static_cast<int16_t>(body.x + 8), y, static_cast<int16_t>(body.w - 44), 9}, bg);
        renderer.fillRect({static_cast<int16_t>(body.x + 10), static_cast<int16_t>(y + 2), 6, 5}, selected ? 0xF800 : 0x39E7);
        renderer.drawText(body.x + 19, static_cast<int16_t>(y + 1), explorer_[i].name, fg, bg);
        ++row;
    }

    if (row == 0) {
        renderer.drawText(body.x + 10, body.y + 31, "No file in folder", 0x4208, 0xE71C);
    }

    const bool trashAnim = millis() < trashAnimUntilMs_;
    const Rect trash{static_cast<int16_t>(body.x + body.w - 28), static_cast<int16_t>(body.y + body.h - 18), 20, 14};
    renderer.fillRect(trash, trashAnim ? 0xF800 : 0x7BEF);
    renderer.drawRect(trash, 0x0000);
    renderer.fillRect({static_cast<int16_t>(trash.x + 5), static_cast<int16_t>(trash.y + 4), 10, 7}, 0x39E7);
    renderer.fillRect({static_cast<int16_t>(trash.x + 4), static_cast<int16_t>(trash.y + 2), 12, 1}, 0x0000);
    renderer.drawText(trash.x + 2, trash.y + 1, "BIN", 0x0000, trashAnim ? 0xF800 : 0x7BEF);
}

void WindowManager::renderNotepad(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xFFDF);

    const Rect editBtn{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 4), 32, 10};
    const Rect saveBtn{static_cast<int16_t>(body.x + 42), static_cast<int16_t>(body.y + 4), 32, 10};
    const Rect loadBtn{static_cast<int16_t>(body.x + 78), static_cast<int16_t>(body.y + 4), 32, 10};

    renderer.fillRect(editBtn, 0x7BEF);
    renderer.fillRect(saveBtn, 0x07E0);
    renderer.fillRect(loadBtn, 0xFFE0);
    renderer.drawText(editBtn.x + 4, editBtn.y + 1, "Edit", 0x0000, 0x7BEF);
    renderer.drawText(saveBtn.x + 4, saveBtn.y + 1, "Save", 0x0000, 0x07E0);
    renderer.drawText(loadBtn.x + 4, loadBtn.y + 1, "Load", 0x0000, 0xFFE0);

    const Rect textBox{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 18), static_cast<int16_t>(body.w - 12), static_cast<int16_t>(body.h - 24)};
    renderer.fillRect(textBox, 0xFFFF);
    renderer.drawRect(textBox, 0x9CD3);
    renderer.drawText(textBox.x + 4, textBox.y + 4, notepadText_[0] ? notepadText_ : "(empty)", 0x2104, 0xFFFF);

    if (notepadDirty_) {
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 24), static_cast<int16_t>(body.y + 4), "*", 0xF800, 0xFFDF);
    }

    if (notepadToastUntilMs_ > millis()) {
        renderer.fillRect({static_cast<int16_t>(body.x + body.w - 64), static_cast<int16_t>(body.y + body.h - 14), 58, 10}, 0x07E0);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 60), static_cast<int16_t>(body.y + body.h - 12), "Saved", 0x0000, 0x07E0);
    }

    if (notepadKeyboardOpen_ && keyboard_.isOpen()) {
        const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};
        keyboard_.render(renderer, keyRect, darkTheme_, cursorX_, cursorY_);
    }
}

void WindowManager::renderNotifications(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0x3186);
    renderer.drawText(body.x + 6, body.y + 4, "System alerts", 0xFFFF, 0x3186);

    renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 16), static_cast<int16_t>(body.w - 12), static_cast<int16_t>(body.h - 30)}, 0x18C3);

    if (notificationCount_ == 0) {
        renderer.drawText(body.x + 10, body.y + 24, "No notifications", 0xC638, 0x18C3);
    } else {
        const uint8_t max = notificationCount_ > 6 ? 6 : notificationCount_;
        for (uint8_t i = 0; i < max; ++i) {
            const uint8_t idx = static_cast<uint8_t>(notificationCount_ - 1 - i);
            renderer.drawText(body.x + 10, static_cast<int16_t>(body.y + 22 + i * 9), notifications_[idx], 0xFFFF, 0x18C3);
        }
    }

    const Rect clearBtn{static_cast<int16_t>(body.x + body.w - 50), static_cast<int16_t>(body.y + body.h - 13), 44, 9};
    renderer.fillRect(clearBtn, 0xF800);
    renderer.drawText(clearBtn.x + 4, clearBtn.y + 1, "Clear", 0xFFFF, 0xF800);
}

void WindowManager::renderWifiManager(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xE73C);

    renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 4), static_cast<int16_t>(body.w - 12), 10}, 0xBDF7);
    if (WiFi.status() == WL_CONNECTED) {
        char ip[30] = "";
        char ipLine[30] = "";
        snprintf(ipLine, sizeof(ipLine), "Connected %s", WiFi.localIP().toString().c_str());
        copyTrim(ip, sizeof(ip), ipLine, 26);
        renderer.drawText(body.x + 8, body.y + 5, ip, 0x0000, 0xBDF7);
    } else {
        renderer.drawText(body.x + 8, body.y + 5, wifiConnecting_ ? "Connecting..." : "Disconnected", 0x0000, 0xBDF7);
    }

    const Rect scanBtn{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 18), 34, 10};
    const Rect connBtn{static_cast<int16_t>(body.x + 44), static_cast<int16_t>(body.y + 18), 34, 10};
    const Rect kbBtn{static_cast<int16_t>(body.x + 82), static_cast<int16_t>(body.y + 18), 34, 10};
    const Rect pingBtn{static_cast<int16_t>(body.x + 120), static_cast<int16_t>(body.y + 18), 28, 10};
    const Rect clearBtn{static_cast<int16_t>(body.x + 152), static_cast<int16_t>(body.y + 18), 40, 10};

    renderer.fillRect(scanBtn, 0x39E7);
    renderer.drawText(scanBtn.x + 5, scanBtn.y + 1, "Scan", 0x0000, 0x39E7);
    renderer.fillRect(connBtn, 0x07E0);
    renderer.drawText(connBtn.x + 4, connBtn.y + 1, "Conn", 0x0000, 0x07E0);
    renderer.fillRect(kbBtn, 0x5AEB);
    renderer.drawText(kbBtn.x + 4, kbBtn.y + 1, "Key", 0xFFFF, 0x5AEB);
    renderer.fillRect(pingBtn, 0xFFE0);
    renderer.drawText(pingBtn.x + 3, pingBtn.y + 1, "Ping", 0x0000, 0xFFE0);
    renderer.fillRect(clearBtn, 0xF8E4);
    renderer.drawText(clearBtn.x + 3, clearBtn.y + 1, "Clear", 0x0000, 0xF8E4);

    renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + 32), static_cast<int16_t>(body.w - 12), 54}, 0xFFFF);
    renderer.drawRect({body.x + 6, static_cast<int16_t>(body.y + 32), static_cast<int16_t>(body.w - 12), 54}, 0x8410);

    if (wifiCount_ == 0) {
        renderer.drawText(body.x + 10, body.y + 38, "No networks - press Scan", 0x8410, 0xFFFF);
    } else {
        for (uint8_t i = 0; i < wifiCount_ && i < 6; ++i) {
            const bool sel = i == wifiSelected_;
            const int16_t y = static_cast<int16_t>(body.y + 34 + i * 8);
            const uint16_t bg = sel ? 0x5AEB : 0xFFFF;
            renderer.fillRect({static_cast<int16_t>(body.x + 8), y, static_cast<int16_t>(body.w - 16), 7}, bg);

            char ssidShort[16] = "";
            copyTrim(ssidShort, sizeof(ssidShort), wifiSsid_[i], 12);
            char line[30] = "";
            snprintf(line, sizeof(line), "%s %ldd", ssidShort, static_cast<long>(wifiRssi_[i]));
            renderer.drawText(body.x + 10, static_cast<int16_t>(y + 1), line, sel ? 0xFFFF : 0x2104, bg);

            uint8_t bars = 1;
            if (wifiRssi_[i] > -60) bars = 4;
            else if (wifiRssi_[i] > -70) bars = 3;
            else if (wifiRssi_[i] > -80) bars = 2;
            for (uint8_t b = 0; b < bars; ++b) {
                renderer.fillRect({static_cast<int16_t>(body.x + body.w - 20 + b * 2), static_cast<int16_t>(y + 6 - b), 1, static_cast<int16_t>(b + 1)}, sel ? 0xFFFF : 0x39E7);
            }
        }
    }

    renderer.fillRect({body.x + 6, static_cast<int16_t>(body.y + body.h - 16), static_cast<int16_t>(body.w - 12), 12}, 0xC618);
    char state[36] = "";
    const char* connState = wifiConnecting_ ? "..." : (WiFi.status() == WL_CONNECTED ? "ok" : "-");
    snprintf(state, sizeof(state), "Pwd:%s C:%s P:%ums S:%u", wifiPassword_[0] ? "*" : "-", connState, static_cast<unsigned>(wifiPingMs_), savedWifiCount_);
    renderer.drawText(body.x + 8, static_cast<int16_t>(body.y + body.h - 14), state, 0x0000, 0xC618);

    if (wifiKeyboardOpen_ && keyboard_.isOpen()) {
        const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};
        keyboard_.render(renderer, keyRect, darkTheme_, cursorX_, cursorY_);
    }
}

void WindowManager::renderBrowser(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    (void)t;
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    browserApp_.render(renderer, body, millis(), clip);
    strlcpy(browserUrl_, browserApp_.currentUrl(), sizeof(browserUrl_));
    strlcpy(browserStatus_, browserApp_.status(), sizeof(browserStatus_));
    browserLoading_ = browserApp_.isLoading();
    browserLoaded_ = browserApp_.isLoaded();

    if (browserKeyboardOpen_ && keyboard_.isOpen()) {
        const Rect keyRect{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + body.h - 62), static_cast<int16_t>(body.w - 12), 60};
        keyboard_.render(renderer, keyRect, darkTheme_, cursorX_, cursorY_);
    }
}

void WindowManager::renderImageVisualizer(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    (void)t;
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xD6FA);

    const Rect top{static_cast<int16_t>(body.x + 2), static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 4), 14};
    renderer.fillRect(top, 0x5AEB);
    renderer.drawText(static_cast<int16_t>(top.x + 4), static_cast<int16_t>(top.y + 3), "Image visualizer", 0xFFFF, 0x5AEB);

    const char* src = imageViewerSrcByWindow_[w.id];

    char srcShort[42] = "";
    copyTrim(srcShort, sizeof(srcShort), src[0] ? src : "(no image)", 38);
    renderer.drawText(static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 20), srcShort, 0x2104, 0xD6FA);

    const Rect frame{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 30), static_cast<int16_t>(body.w - 12), static_cast<int16_t>(body.h - 36)};
    renderer.fillRect(frame, 0xFFFF);
    renderer.drawRect(frame, 0x7BEF);

    if (!src[0]) {
        renderer.drawText(static_cast<int16_t>(frame.x + 6), static_cast<int16_t>(frame.y + 8), "No image selected", 0x8410, 0xFFFF);
        return;
    }

    bool isJpeg = false;
    bool isPng = false;
    bool isWebp = false;
    detectImageFormat(src, isJpeg, isPng, isWebp);

    bool drawOk = false;
    const Rect imgRect{static_cast<int16_t>(frame.x + 2), static_cast<int16_t>(frame.y + 2), static_cast<int16_t>(frame.w - 4), static_cast<int16_t>(frame.h - 4)};
    if (imgRect.w > 0 && imgRect.h > 0) {
        if ((isJpeg || isPng) && isHttpUrl(src)) {
            HTTPClient http;
            http.setTimeout(1200);
            if (http.begin(src)) {
                const int code = http.GET();
                if (code == HTTP_CODE_OK) {
                    Stream* stream = http.getStreamPtr();
                    if (stream) {
                        const int32_t len = http.getSize();
                        const uint32_t streamLen = len > 0 ? static_cast<uint32_t>(len) : 0xFFFFFFFFu;
                        StreamDataWrapper wrapper(stream, streamLen, &http);
                        drawOk = drawEncodedWithWrapper(isJpeg, wrapper, imgRect);
                    }
                }
                http.end();
            }
        } else if ((isJpeg || isPng) && SPIFFS.exists(src)) {
            File file = SPIFFS.open(src, "r");
            if (file) {
                FileDataWrapper wrapper(&file);
                drawOk = drawEncodedWithWrapper(isJpeg, wrapper, imgRect);
                wrapper.close();
            }
        }
    }

    if (!drawOk) {
        if (isWebp) {
            renderer.drawText(static_cast<int16_t>(frame.x + 6), static_cast<int16_t>(frame.y + 8), "WEBP decoder missing", 0x8410, 0xFFFF);
        } else {
            renderer.drawText(static_cast<int16_t>(frame.x + 6), static_cast<int16_t>(frame.y + 8), "Streaming decode failed", 0x8410, 0xFFFF);
        }
    }
}

void WindowManager::renderDesktopConfig(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    (void)t;
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    renderer.fillRect(body, 0xD6FA);
    renderer.fillRect({body.x + 2, static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 4), 14}, 0x5AEB);
    renderer.drawText(body.x + 6, body.y + 6, "Desktop setup", 0xFFFF, 0x5AEB);

    const Rect toggleRow{static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + 18), static_cast<int16_t>(body.w - 16), 11};
    renderer.fillRect(toggleRow, 0xE71C);
    renderer.drawRect(toggleRow, 0x7BEF);
    const Rect cb{static_cast<int16_t>(toggleRow.x + 2), static_cast<int16_t>(toggleRow.y + 2), 7, 7};
    renderer.fillRect(cb, 0xFFFF);
    renderer.drawRect(cb, 0x0000);
    if (desktopShowIcons_) {
        renderer.fillRect({static_cast<int16_t>(cb.x + 1), static_cast<int16_t>(cb.y + 3), 2, 2}, 0x0000);
        renderer.fillRect({static_cast<int16_t>(cb.x + 3), static_cast<int16_t>(cb.y + 5), 3, 1}, 0x0000);
        renderer.fillRect({static_cast<int16_t>(cb.x + 5), static_cast<int16_t>(cb.y + 2), 1, 4}, 0x0000);
    }
    renderer.drawText(static_cast<int16_t>(toggleRow.x + 12), static_cast<int16_t>(toggleRow.y + 2), "Afficher les icones", 0x0000, 0xE71C);

    for (uint8_t slot = 0; slot < kDesktopSlotCount; ++slot) {
        const Rect row{static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + 34 + slot * 11), static_cast<int16_t>(body.w - 16), 10};
        renderer.fillRect(row, 0xFFFF);
        renderer.drawRect(row, 0x7BEF);
        char line[44] = "";
        snprintf(line, sizeof(line), "Case %u: %s", static_cast<unsigned>(slot + 1U), desktopKindLabel(desktopSlots_[slot]));
        renderer.drawText(static_cast<int16_t>(row.x + 2), static_cast<int16_t>(row.y + 1), line, 0x0000, 0xFFFF);
    }

    renderer.drawText(static_cast<int16_t>(body.x + 8), static_cast<int16_t>(body.y + body.h - 10), "Click row to cycle", 0x2104, 0xD6FA);
}

void WindowManager::renderAppHub(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    const uint8_t totalPages = static_cast<uint8_t>((kHubEntryCount + kHubPerPage - 1U) / kHubPerPage);
    if (totalPages > 0 && appHubPage_ >= totalPages) {
        appHubPage_ = static_cast<uint8_t>(totalPages - 1U);
    }

    renderer.fillRect(body, 0xD6FA);
    renderer.fillRect({body.x + 2, static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 4), 14}, 0x5AEB);
    renderer.drawText(body.x + 6, body.y + 6, "Applications", 0xFFFF, 0x5AEB);

    const Rect prevBtn{static_cast<int16_t>(body.x + body.w - 56), static_cast<int16_t>(body.y + 4), 12, 10};
    const Rect nextBtn{static_cast<int16_t>(body.x + body.w - 40), static_cast<int16_t>(body.y + 4), 12, 10};
    renderer.fillRect(prevBtn, 0x39E7);
    renderer.fillRect(nextBtn, 0x39E7);
    renderer.drawText(prevBtn.x + 4, prevBtn.y + 1, "<", 0x0000, 0x39E7);
    renderer.drawText(nextBtn.x + 4, nextBtn.y + 1, ">", 0x0000, 0x39E7);

    char pageLine[20] = "";
    snprintf(pageLine, sizeof(pageLine), "%u/%u", static_cast<unsigned>(appHubPage_ + 1U), static_cast<unsigned>(totalPages == 0 ? 1 : totalPages));
    renderer.drawText(static_cast<int16_t>(body.x + body.w - 84), static_cast<int16_t>(body.y + 6), pageLine, 0xFFFF, 0x5AEB);

    const int16_t gridX = static_cast<int16_t>(body.x + 8);
    const int16_t gridY = static_cast<int16_t>(body.y + 22);
    const uint8_t pageStart = static_cast<uint8_t>(appHubPage_ * kHubPerPage);

    for (uint8_t slot = 0; slot < kHubPerPage; ++slot) {
        const uint8_t idx = static_cast<uint8_t>(pageStart + slot);
        if (idx >= kHubEntryCount) break;
        const uint8_t col = static_cast<uint8_t>(slot % kHubCols);
        const uint8_t row = static_cast<uint8_t>(slot / kHubCols);
        const Rect card{static_cast<int16_t>(gridX + col * 66), static_cast<int16_t>(gridY + row * 34), 62, 30};
        const bool sel = idx == appHubSelected_;
        const uint16_t bg = sel ? 0x7D7C : 0xE79D;
        renderer.fillRect(card, bg);
        renderer.drawRect(card, sel ? 0xFFFF : 0x94B2);

        const Rect icon{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 5), 14, 14};
        renderer.fillRect(icon, 0x1082);
        renderer.drawRect(icon, 0xFFFF);

        const uint8_t accent = kHubEntries[idx].accent;
        if (accent == 1) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 6), static_cast<int16_t>(icon.y + 2), 2, 10}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 6), 10, 2}, 0xFFFF);
        } else if (accent == 2) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 3), static_cast<int16_t>(icon.y + 9), 2, 3}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 7), static_cast<int16_t>(icon.y + 7), 2, 5}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 11), static_cast<int16_t>(icon.y + 4), 1, 8}, 0xFFFF);
        } else if (accent == 3) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 4), 10, 7}, 0xFFE0);
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 3), 4, 1}, 0xFFE0);
        } else if (accent == 4) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 2), 10, 10}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 4), static_cast<int16_t>(icon.y + 4), 6, 6}, 0x39E7);
        } else if (accent == 5) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 11), 10, 1}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 4), static_cast<int16_t>(icon.y + 8), 6, 1}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 6), static_cast<int16_t>(icon.y + 5), 2, 1}, 0xFFFF);
        } else if (accent == 6) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 2), 10, 2}, 0xFFE0);
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 6), 8, 2}, 0x07FF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 10), 9, 2}, 0x07E0);
        } else if (accent == 7) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 2), 10, 10}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 3), static_cast<int16_t>(icon.y + 4), 8, 1}, 0x3186);
            renderer.fillRect({static_cast<int16_t>(icon.x + 3), static_cast<int16_t>(icon.y + 7), 8, 1}, 0x3186);
        } else if (accent == 8) {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 2), 10, 10}, 0x07E0);
            renderer.fillRect({static_cast<int16_t>(icon.x + 7), static_cast<int16_t>(icon.y + 7), 3, 3}, 0xF800);
        } else if (accent == 9) {
            renderer.drawRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 2), 10, 10}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 6), static_cast<int16_t>(icon.y + 6), 2, 2}, 0xFFE0);
        } else {
            renderer.fillRect({static_cast<int16_t>(icon.x + 2), static_cast<int16_t>(icon.y + 3), 10, 8}, 0xFFFF);
            renderer.fillRect({static_cast<int16_t>(icon.x + 5), static_cast<int16_t>(icon.y + 2), 4, 1}, 0xFFFF);
        }

        renderer.drawText(static_cast<int16_t>(card.x + 22), static_cast<int16_t>(card.y + 10), kHubEntries[idx].label, sel ? 0xFFFF : 0x2104, bg);
    }
}

void WindowManager::renderGamePixel(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    const int16_t gridW = 15;
    const int16_t gridH = 9;
    const int16_t topPad = 20;
    int16_t cell = static_cast<int16_t>((body.h - topPad - 10) / gridH);
    const int16_t cellByWidth = static_cast<int16_t>((body.w - 10) / gridW);
    if (cellByWidth < cell) cell = cellByWidth;
    if (cell < 6) cell = 6;

    const int16_t arenaW = static_cast<int16_t>(gridW * cell);
    const int16_t arenaH = static_cast<int16_t>(gridH * cell);
    const Rect arena{static_cast<int16_t>(body.x + (body.w - arenaW) / 2), static_cast<int16_t>(body.y + topPad), arenaW, arenaH};

    renderer.fillRect(body, 0x1082);
    renderer.drawText(body.x + 6, body.y + 6, "Pixel Snake", 0xFFFF, 0x1082);
    char line[20] = "";
    snprintf(line, sizeof(line), "Score %u", static_cast<unsigned>(gamePixelScore_));
    renderer.drawText(static_cast<int16_t>(body.x + body.w - 58), body.y + 6, line, 0xFFFF, 0x1082);

    renderer.fillRect(arena, 0x0000);
    renderer.drawRect(arena, 0x7BEF);

    const int16_t foodPad = cell > 4 ? static_cast<int16_t>(cell / 4) : 1;
    const int16_t snakePad = cell > 3 ? static_cast<int16_t>(cell / 6) : 1;
    const int16_t foodSize = static_cast<int16_t>(cell - foodPad * 2);
    const int16_t snakeSize = static_cast<int16_t>(cell - snakePad * 2);

    renderer.fillRect({static_cast<int16_t>(arena.x + gamePixelFoodX_ * cell + foodPad), static_cast<int16_t>(arena.y + gamePixelFoodY_ * cell + foodPad), foodSize, foodSize}, 0xF800);
    renderer.fillRect({static_cast<int16_t>(arena.x + gamePixelX_ * cell + snakePad), static_cast<int16_t>(arena.y + gamePixelY_ * cell + snakePad), snakeSize, snakeSize}, 0x07E0);

    if (gameCloseHintVisible_) {
        renderer.fillRect({8, 4, 140, 10}, 0x18C3);
        renderer.drawText(10, 5, "Hold BtnA 2s to close", 0xFFFF, 0x18C3);
    }
    if (gameCloseProgressMs_ > 0) {
        const int16_t barW = static_cast<int16_t>((120 * gameCloseProgressMs_) / 2000U);
        renderer.fillRect({8, 16, 122, 7}, 0x0000);
        renderer.drawRect({8, 16, 122, 7}, 0xBDF7);
        renderer.fillRect({9, 17, barW, 5}, 0xF800);
    }

    if (gamePixelOver_) {
        renderer.fillRect({static_cast<int16_t>(body.x + body.w - 58), static_cast<int16_t>(body.y + body.h - 28), 52, 10}, 0xF800);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 54), static_cast<int16_t>(body.y + body.h - 26), "Game over", 0xFFFF, 0xF800);
        renderer.fillRect({static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + body.h - 14), 46, 10}, 0xFFE0);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 48), static_cast<int16_t>(body.y + body.h - 12), "Reset", 0x0000, 0xFFE0);
    }
}

void WindowManager::renderGameOrbit(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip) {
    const Rect body = windowBody(w);
    if (clip && !intersects(*clip, body)) return;

    const int16_t logicalW = 120;
    const int16_t logicalH = 72;
    const int16_t topPad = 20;
    int16_t scale = static_cast<int16_t>((body.h - topPad - 10) / logicalH);
    const int16_t scaleByWidth = static_cast<int16_t>((body.w - 10) / logicalW);
    if (scaleByWidth < scale) scale = scaleByWidth;
    if (scale < 1) scale = 1;

    const int16_t arenaW = static_cast<int16_t>(logicalW * scale);
    const int16_t arenaH = static_cast<int16_t>(logicalH * scale);
    const Rect arena{static_cast<int16_t>(body.x + (body.w - arenaW) / 2), static_cast<int16_t>(body.y + topPad), arenaW, arenaH};

    renderer.fillRect(body, 0x18C3);
    renderer.drawText(body.x + 6, body.y + 6, "Orbit Pong", 0xFFFF, 0x18C3);
    char line[20] = "";
    snprintf(line, sizeof(line), "Score %u", static_cast<unsigned>(orbitScore_));
    renderer.drawText(static_cast<int16_t>(body.x + body.w - 58), body.y + 6, line, 0xFFFF, 0x18C3);

    renderer.fillRect(arena, 0x0000);
    renderer.drawRect(arena, 0x7BEF);

    const int16_t paddleX = static_cast<int16_t>(arena.x + orbitPaddleX_ * scale);
    const int16_t paddleY = static_cast<int16_t>(arena.y + 68 * scale);
    const int16_t paddleW = static_cast<int16_t>(24 * scale);
    int16_t paddleH = static_cast<int16_t>(3 * scale);
    if (paddleH < 2) paddleH = 2;
    renderer.fillRect({paddleX, paddleY, paddleW, paddleH}, 0x07E0);

    const int16_t ballX = static_cast<int16_t>(arena.x + orbitBallX_ * scale);
    const int16_t ballY = static_cast<int16_t>(arena.y + orbitBallY_ * scale);
    int16_t ballS = static_cast<int16_t>(6 * scale);
    if (ballS < 4) ballS = 4;
    renderer.fillRect({ballX, ballY, ballS, ballS}, 0xFFE0);

    if (gameCloseHintVisible_) {
        renderer.fillRect({8, 4, 140, 10}, 0x18C3);
        renderer.drawText(10, 5, "Hold BtnA 2s to close", 0xFFFF, 0x18C3);
    }
    if (gameCloseProgressMs_ > 0) {
        const int16_t barW = static_cast<int16_t>((120 * gameCloseProgressMs_) / 2000U);
        renderer.fillRect({8, 16, 122, 7}, 0x0000);
        renderer.drawRect({8, 16, 122, 7}, 0xBDF7);
        renderer.fillRect({9, 17, barW, 5}, 0xF800);
    }

    if (orbitOver_) {
        renderer.fillRect({static_cast<int16_t>(body.x + body.w - 58), static_cast<int16_t>(body.y + body.h - 28), 52, 10}, 0xF800);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 54), static_cast<int16_t>(body.y + body.h - 26), "Game over", 0xFFFF, 0xF800);
        renderer.fillRect({static_cast<int16_t>(body.x + body.w - 52), static_cast<int16_t>(body.y + body.h - 14), 46, 10}, 0xFFE0);
        renderer.drawText(static_cast<int16_t>(body.x + body.w - 48), static_cast<int16_t>(body.y + body.h - 12), "Reset", 0x0000, 0xFFE0);
    }
}

bool WindowManager::isCoveredByTopWindow(const Rect& rect, uint8_t zIndex) const {
    for (uint8_t z = static_cast<uint8_t>(zIndex + 1); z < count_; ++z) {
        const Window& top = windows_[zOrder_[z]];
        if (!top.visible || top.minimized) continue;
        if (top.x <= rect.x && top.y <= rect.y && top.x + top.width >= rect.x + rect.w && top.y + top.height >= rect.y + rect.h) {
            return true;
        }
    }
    return false;
}

int8_t WindowManager::createKindWindow(WindowKind kind) {
    if (kind == WindowKind::Demo) return createWindow("Demo", 46, 16, 158, 98, true, kind);
    if (kind == WindowKind::Settings) return createWindow("Settings", 30, 16, 176, 118, true, kind);
    if (kind == WindowKind::TaskManager) return createWindow("Task Manager", 20, 8, 198, 118, true, kind);
    if (kind == WindowKind::Explorer) return createWindow("Explorer", 18, 8, 202, 118, true, kind);
    if (kind == WindowKind::Notepad) return createWindow("Notepad", 16, 8, 208, 118, true, kind);
    if (kind == WindowKind::Notifications) return createWindow("Notifications", 24, 10, 192, 114, true, kind);
    if (kind == WindowKind::WifiManager) return createWindow("WiFi Manager", 12, 6, 216, 124, true, kind);
    if (kind == WindowKind::Browser) return createWindow("Navigator", 10, 6, 220, 124, true, kind);
    if (kind == WindowKind::ImageVisualizer) return createWindow("Image visualizer", 14, 8, 212, 122, true, kind);
    if (kind == WindowKind::DesktopConfig) return createWindow("Desktop", 14, 8, 212, 122, true, kind);
    if (kind == WindowKind::GamePixel) return createWindow("Pixel Snake", 22, 8, 196, 120, true, kind);
    if (kind == WindowKind::GameOrbit) return createWindow("Orbit Pong", 22, 8, 196, 120, true, kind);
    if (kind == WindowKind::AppHub) return createWindow("Applications", 12, 8, 216, 120, true, kind);
    return createWindow("Window", 24, 10, 184, 112, true, kind);
}

int8_t WindowManager::hitTest(int16_t x, int16_t y) const {
    for (int8_t z = static_cast<int8_t>(count_) - 1; z >= 0; --z) {
        const Window& w = windows_[zOrder_[z]];
        if (!w.visible || w.minimized) continue;
        if (x >= w.x && y >= w.y && x < w.x + w.width && y < w.y + w.height) {
            return z;
        }
    }
    return -1;
}

bool WindowManager::inTitleBar(const Window& w, int16_t x, int16_t y) const {
    if (w.fullscreen) return false;
    return x >= w.x && x < w.x + w.width && y >= w.y && y < w.y + 20;
}

void WindowManager::bringToFront(int8_t zIndex) {
    if (zIndex < 0 || zIndex >= count_) return;

    const uint8_t oldTopId = zOrder_[count_ - 1];
    const uint8_t value = zOrder_[zIndex];
    for (uint8_t i = zIndex; i + 1 < count_; ++i) {
        zOrder_[i] = zOrder_[i + 1];
    }
    zOrder_[count_ - 1] = value;
    focusedZ_ = count_ - 1;
    syncFocusFlags();

    markDirty(frameRect(windows_[value]));
    markDirty(frameRect(windows_[oldTopId]));
    markDirty({0, 111, 240, 24});
}

void WindowManager::emit(katux::core::EventType type, int32_t data0, int32_t data1) {
    if (!events_) return;
    katux::core::Event e;
    e.type = type;
    e.source = katux::core::EventSource::WindowManager;
    e.timestampMs = millis();
    e.data0 = data0;
    e.data1 = data1;
    events_->push(e);
}

void WindowManager::syncFocusFlags() {
    for (uint8_t i = 0; i < count_; ++i) {
        windows_[zOrder_[i]].focused = (i == focusedZ_);
    }
}

void WindowManager::updateHover(int16_t cursorX, int16_t cursorY) {
    hoverZ_ = hitTest(cursorX, cursorY);
    hoverControl_ = 0;
    hoverInput_ = false;
    if (hoverZ_ < 0) return;

    const Window& w = windows_[zOrder_[hoverZ_]];
    const Rect resizeBox{static_cast<int16_t>(w.x + w.width - 40), static_cast<int16_t>(w.y + 4), 10, 10};
    const Rect minBox{static_cast<int16_t>(w.x + w.width - 28), static_cast<int16_t>(w.y + 4), 10, 10};
    const Rect closeBox{static_cast<int16_t>(w.x + w.width - 16), static_cast<int16_t>(w.y + 4), 10, 10};

    if (!w.fullscreen) {
        if (cursorX >= closeBox.x && cursorY >= closeBox.y && cursorX < closeBox.x + closeBox.w && cursorY < closeBox.y + closeBox.h) {
            hoverControl_ = 2;
        } else if (cursorX >= minBox.x && cursorY >= minBox.y && cursorX < minBox.x + minBox.w && cursorY < minBox.y + minBox.h) {
            hoverControl_ = 1;
        } else if (w.resizable && cursorX >= resizeBox.x && cursorY >= resizeBox.y && cursorX < resizeBox.x + resizeBox.w && cursorY < resizeBox.y + resizeBox.h) {
            hoverControl_ = 3;
        }
    }

    if (w.kind == WindowKind::WifiManager) {
        const Rect body = windowBody(w);
        const Rect kbBtn{static_cast<int16_t>(body.x + 82), static_cast<int16_t>(body.y + 18), 34, 10};
        if ((cursorX >= kbBtn.x && cursorY >= kbBtn.y && cursorX < kbBtn.x + kbBtn.w && cursorY < kbBtn.y + kbBtn.h) ||
            (wifiKeyboardOpen_ && keyboard_.isOpen() && cursorY >= body.y + body.h - 62)) {
            hoverInput_ = true;
        }
    } else if (w.kind == WindowKind::Notepad) {
        const Rect body = windowBody(w);
        const Rect editBtn{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 4), 32, 10};
        if ((cursorX >= editBtn.x && cursorY >= editBtn.y && cursorX < editBtn.x + editBtn.w && cursorY < editBtn.y + editBtn.h) ||
            (notepadKeyboardOpen_ && keyboard_.isOpen() && cursorY >= body.y + body.h - 62)) {
            hoverInput_ = true;
        }
    } else if (w.kind == WindowKind::Settings) {
        const Rect body = windowBody(w);
        const Rect leftArrow{static_cast<int16_t>(body.x + body.w - 62), static_cast<int16_t>(body.y + 4), 12, 10};
        const Rect rightArrow{static_cast<int16_t>(body.x + body.w - 46), static_cast<int16_t>(body.y + 4), 12, 10};
        if ((cursorX >= leftArrow.x && cursorY >= leftArrow.y && cursorX < leftArrow.x + leftArrow.w && cursorY < leftArrow.y + leftArrow.h) ||
            (cursorX >= rightArrow.x && cursorY >= rightArrow.y && cursorX < rightArrow.x + rightArrow.w && cursorY < rightArrow.y + rightArrow.h)) {
            hoverInput_ = true;
        }
    } else if (w.kind == WindowKind::TaskManager) {
        const Rect body = windowBody(w);
        const Rect processTab{static_cast<int16_t>(body.x + 6), static_cast<int16_t>(body.y + 16), 54, 10};
        const Rect perfTab{static_cast<int16_t>(body.x + 62), static_cast<int16_t>(body.y + 16), 54, 10};
        const Rect endBtn{static_cast<int16_t>(body.x + body.w - 66), static_cast<int16_t>(body.y + body.h - 14), 60, 10};
        if ((cursorX >= processTab.x && cursorY >= processTab.y && cursorX < processTab.x + processTab.w && cursorY < processTab.y + processTab.h) ||
            (cursorX >= perfTab.x && cursorY >= perfTab.y && cursorX < perfTab.x + perfTab.w && cursorY < perfTab.y + perfTab.h) ||
            (taskTab_ == 0 && cursorX >= endBtn.x && cursorY >= endBtn.y && cursorX < endBtn.x + endBtn.w && cursorY < endBtn.y + endBtn.h)) {
            hoverInput_ = true;
            return;
        }

        if (taskTab_ == 0) {
            const int16_t listY = static_cast<int16_t>(body.y + 44);
            if (cursorY >= listY && cursorY < listY + 54 && cursorX >= body.x + 8 && cursorX < body.x + body.w - 8) {
                hoverInput_ = true;
                return;
            }
        }
    } else if (w.kind == WindowKind::AppHub) {
        const Rect body = windowBody(w);
        const Rect prevBtn{static_cast<int16_t>(body.x + body.w - 56), static_cast<int16_t>(body.y + 4), 12, 10};
        const Rect nextBtn{static_cast<int16_t>(body.x + body.w - 40), static_cast<int16_t>(body.y + 4), 12, 10};
        if ((cursorX >= prevBtn.x && cursorY >= prevBtn.y && cursorX < prevBtn.x + prevBtn.w && cursorY < prevBtn.y + prevBtn.h) ||
            (cursorX >= nextBtn.x && cursorY >= nextBtn.y && cursorX < nextBtn.x + nextBtn.w && cursorY < nextBtn.y + nextBtn.h)) {
            hoverInput_ = true;
            return;
        }

        const int16_t gridX = static_cast<int16_t>(body.x + 8);
        const int16_t gridY = static_cast<int16_t>(body.y + 22);
        const uint8_t pageStart = static_cast<uint8_t>(appHubPage_ * kHubPerPage);
        for (uint8_t slot = 0; slot < kHubPerPage; ++slot) {
            const uint8_t idx = static_cast<uint8_t>(pageStart + slot);
            if (idx >= kHubEntryCount) break;
            const uint8_t col = static_cast<uint8_t>(slot % kHubCols);
            const uint8_t row = static_cast<uint8_t>(slot / kHubCols);
            const Rect card{static_cast<int16_t>(gridX + col * 66), static_cast<int16_t>(gridY + row * 34), 62, 30};
            if (cursorX >= card.x && cursorY >= card.y && cursorX < card.x + card.w && cursorY < card.y + card.h) {
                hoverInput_ = true;
                return;
            }
        }
    } else if (w.kind == WindowKind::Browser) {
        const Rect body = windowBody(w);
        const Rect urlBar{static_cast<int16_t>(body.x + 4), static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 26), 12};
        const Rect favBtn{static_cast<int16_t>(body.x + body.w - 20), static_cast<int16_t>(body.y + 2), 16, 12};
        const Rect backBtn{static_cast<int16_t>(body.x + 4), static_cast<int16_t>(body.y + 16), 18, 10};
        const Rect fwdBtn{static_cast<int16_t>(body.x + 24), static_cast<int16_t>(body.y + 16), 18, 10};
        const Rect prevBtn{static_cast<int16_t>(body.x + 44), static_cast<int16_t>(body.y + 16), 18, 10};
        const Rect nextBtn{static_cast<int16_t>(body.x + 64), static_cast<int16_t>(body.y + 16), 18, 10};
        const Rect openBtn{static_cast<int16_t>(body.x + 86), static_cast<int16_t>(body.y + 16), 28, 10};
        const Rect upBtn{static_cast<int16_t>(body.x + 118), static_cast<int16_t>(body.y + 16), 14, 10};
        const Rect downBtn{static_cast<int16_t>(body.x + 134), static_cast<int16_t>(body.y + 16), 14, 10};

        const int16_t mainTop = static_cast<int16_t>(body.y + 30);
        const int16_t mainH = static_cast<int16_t>(body.h - 32);
        const Rect sidePanel{static_cast<int16_t>(body.x + 4), mainTop, 70, mainH};
        const Rect viewport{static_cast<int16_t>(sidePanel.x + sidePanel.w + 2), mainTop, static_cast<int16_t>(body.w - sidePanel.w - 8), mainH};
        const Rect scrollUp{static_cast<int16_t>(viewport.x + viewport.w - 10), static_cast<int16_t>(viewport.y + 2), 8, 8};
        const Rect scrollDown{static_cast<int16_t>(viewport.x + viewport.w - 10), static_cast<int16_t>(viewport.y + viewport.h - 22), 8, 8};

        if ((cursorX >= urlBar.x && cursorY >= urlBar.y && cursorX < urlBar.x + urlBar.w && cursorY < urlBar.y + urlBar.h) ||
            (cursorX >= favBtn.x && cursorY >= favBtn.y && cursorX < favBtn.x + favBtn.w && cursorY < favBtn.y + favBtn.h) ||
            (cursorX >= backBtn.x && cursorY >= backBtn.y && cursorX < backBtn.x + backBtn.w && cursorY < backBtn.y + backBtn.h) ||
            (cursorX >= fwdBtn.x && cursorY >= fwdBtn.y && cursorX < fwdBtn.x + fwdBtn.w && cursorY < fwdBtn.y + fwdBtn.h) ||
            (cursorX >= prevBtn.x && cursorY >= prevBtn.y && cursorX < prevBtn.x + prevBtn.w && cursorY < prevBtn.y + prevBtn.h) ||
            (cursorX >= nextBtn.x && cursorY >= nextBtn.y && cursorX < nextBtn.x + nextBtn.w && cursorY < nextBtn.y + nextBtn.h) ||
            (cursorX >= openBtn.x && cursorY >= openBtn.y && cursorX < openBtn.x + openBtn.w && cursorY < openBtn.y + openBtn.h) ||
            (cursorX >= upBtn.x && cursorY >= upBtn.y && cursorX < upBtn.x + upBtn.w && cursorY < upBtn.y + upBtn.h) ||
            (cursorX >= downBtn.x && cursorY >= downBtn.y && cursorX < downBtn.x + downBtn.w && cursorY < downBtn.y + downBtn.h) ||
            (cursorX >= scrollUp.x && cursorY >= scrollUp.y && cursorX < scrollUp.x + scrollUp.w && cursorY < scrollUp.y + scrollUp.h) ||
            (cursorX >= scrollDown.x && cursorY >= scrollDown.y && cursorX < scrollDown.x + scrollDown.w && cursorY < scrollDown.y + scrollDown.h) ||
            (browserKeyboardOpen_ && keyboard_.isOpen() && cursorY >= body.y + body.h - 62)) {
            hoverInput_ = true;
            return;
        }

        for (uint8_t i = 0; i < browserClickCount_; ++i) {
            const Rect& r = browserClickTargets_[i].rect;
            if (cursorX >= r.x && cursorY >= r.y && cursorX < r.x + r.w && cursorY < r.y + r.h) {
                hoverInput_ = true;
                return;
            }
        }
    }
}

}
