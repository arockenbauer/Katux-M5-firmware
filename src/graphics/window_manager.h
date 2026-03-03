#pragma once

#include <stdint.h>
#include <stddef.h>
#include <Preferences.h>

#include "renderer.h"
#include "theme.h"
#include "browser_app.h"
#include "../core/event_manager.h"
#include "../system/soft_keyboard.h"

namespace katux::graphics {

enum class WindowKind : uint8_t {
    Generic = 0,
    Demo,
    Settings,
    TaskManager,
    Explorer,
    Notepad,
    AppHub,
    GamePixel,
    GameOrbit,
    Notifications,
    WifiManager,
    Browser,
    ImageVisualizer,
    DesktopConfig
};

struct WindowTaskItem {
    uint8_t id = 0;
    char title[24] = "";
    WindowKind kind = WindowKind::Generic;
    bool minimized = false;
    bool focused = false;
};

struct Window {
    uint8_t id = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 120;
    int16_t height = 80;
    int16_t targetWidth = 120;
    int16_t targetHeight = 80;
    int16_t minWidth = 96;
    int16_t minHeight = 68;
    int16_t maxWidth = 240;
    int16_t maxHeight = 135;
    char title[24] = "Window";
    bool focused = false;
    bool draggable = true;
    bool resizable = true;
    bool visible = true;
    bool minimized = false;
    bool fullscreen = false;
    uint8_t resizePreset = 0;
    WindowKind kind = WindowKind::Generic;
    bool toggle = false;
    uint8_t selected = 0;
    uint8_t meter = 32;
};

class WindowManager {
   public:
    static constexpr uint8_t kMaxWindows = 32;  // fewer simultaneous windows saves RAM
    static constexpr uint8_t kDirtyCapacity = 40;

    void begin(katux::core::EventManager* events);
    int8_t createWindow(const char* title, int16_t x, int16_t y, int16_t w, int16_t h, bool draggable, WindowKind kind = WindowKind::Generic);
    void focusNext();
    bool focusWindowById(uint8_t id);
    bool restoreWindowById(uint8_t id);
    bool closeWindowById(uint8_t id);
    uint8_t closeAllVisible();
    void onEvent(const katux::core::Event& event, int16_t cursorX, int16_t cursorY);
    void render(Renderer& renderer, const Theme& t, const Rect* clip = nullptr);

    uint8_t listTaskItems(WindowTaskItem* out, uint8_t maxItems, bool minimizedOnly) const;
    bool windowIsActive(uint8_t id) const;

    void setSystemState(bool darkTheme, uint8_t brightness, uint8_t cursorSpeed, uint8_t performanceProfile, bool debugOverlay, bool animationsEnabled);
    void setRuntimeStats(uint8_t fps, uint32_t freeHeap, uint32_t uptimeMs, uint8_t queueDepth);
    void notify(const char* text);
    uint8_t consumeDirtyRegions(Rect* out, uint8_t maxItems);
    void invalidateAll();
    CursorStyle cursorStyle() const;
    bool hasCapturedApp() const;
    bool needsFrameTick() const;
    bool hasWindowAt(int16_t x, int16_t y) const;
    void getDesktopConfig(bool& showIcons, WindowKind* slots, uint8_t maxSlots) const;
    void setDesktopIconsVisible(bool visible);

   private:
    Window windows_[kMaxWindows]{};
    uint8_t zOrder_[kMaxWindows]{};
    uint8_t count_ = 0;
    int8_t focusedZ_ = -1;
    int8_t draggingZ_ = -1;
    int16_t dragOffsetX_ = 0;
    int16_t dragOffsetY_ = 0;

    int8_t hoverZ_ = -1;
    uint8_t hoverControl_ = 0;
    bool hoverInput_ = false;
    uint32_t clickPulseUntilMs_ = 0;
    int16_t cursorX_ = 0;
    int16_t cursorY_ = 0;
    Rect dirty_[kDirtyCapacity]{};
    uint8_t dirtyCount_ = 0;

    katux::core::EventManager* events_ = nullptr;

    bool darkTheme_ = true;
    uint8_t brightness_ = 90;
    uint8_t cursorSpeed_ = 2;
    uint8_t performanceProfile_ = 1;
    bool debugOverlay_ = false;
    bool animationsEnabled_ = true;

    uint8_t fps_ = 0;
    uint32_t freeHeap_ = 0;
    uint32_t uptimeMs_ = 0;
    uint8_t queueDepth_ = 0;

    struct ExplorerEntry {
        char name[32] = "";
        uint8_t folder = 0;
        bool deleted = false;
    };

    static constexpr uint8_t kFolderCount = 3;
    static constexpr uint8_t kExplorerEntryCount = 16;
    ExplorerEntry explorer_[kExplorerEntryCount]{};
    uint8_t explorerCount_ = 0;
    uint8_t explorerFolder_ = 0;
    int8_t explorerSelected_ = -1;
    uint32_t trashAnimUntilMs_ = 0;

    char notepadText_[33] = "";
    bool notepadKeyboardOpen_ = false;
    bool notepadDirty_ = false;
    uint32_t notepadToastUntilMs_ = 0;

    char notifications_[8][30]{};
    uint8_t notificationCount_ = 0;
    int16_t notificationSlideY_ = -16;
    uint32_t notificationHoldUntilMs_ = 0;

    static constexpr uint8_t kWifiMaxNetworks = 8;
    char wifiSsid_[kWifiMaxNetworks][24]{};
    int32_t wifiRssi_[kWifiMaxNetworks]{};
    uint8_t wifiCount_ = 0;
    uint8_t wifiSelected_ = 0;
    char wifiPassword_[33] = "";
    bool wifiKeyboardOpen_ = false;
    bool wifiConnecting_ = false;
    bool wifiSilentAttempt_ = false;
    uint32_t wifiConnectStartMs_ = 0;
    uint16_t wifiPingMs_ = 0;
    bool wifiPingOk_ = false;
    bool wifiPrefsReady_ = false;
    bool wifiAutoAttempted_ = false;
    static constexpr uint8_t kSavedWifiCount = 3;
    char savedWifiSsid_[kSavedWifiCount][24]{};
    char savedWifiPwd_[kSavedWifiCount][33]{};
    uint8_t savedWifiCount_ = 0;

    uint8_t settingsPage_ = 0;
    bool autoTime_ = true;
    int8_t timezoneOffset_ = 0;
    uint16_t manualYear_ = 2026;
    uint8_t manualMonth_ = 1;
    uint8_t manualDay_ = 1;
    uint8_t manualHour_ = 0;
    uint8_t manualMinute_ = 0;

    uint8_t taskTargetIndex_ = 0;
    uint8_t taskTab_ = 0;
    uint32_t taskPerfLastSampleMs_ = 0;
    static constexpr uint8_t kTaskPerfPoints = 32;
    uint8_t taskCpuSeries_[kTaskPerfPoints]{};
    uint8_t taskRamSeries_[kTaskPerfPoints]{};
    uint8_t taskPsramSeries_[kTaskPerfPoints]{};
    uint8_t appHubSelected_ = 0;
    uint8_t appHubPage_ = 0;
    static constexpr uint8_t kDesktopSlotCount = 6;
    WindowKind desktopSlots_[kDesktopSlotCount]{WindowKind::AppHub, WindowKind::Settings, WindowKind::Explorer, WindowKind::Notepad, WindowKind::WifiManager, WindowKind::TaskManager};
    bool desktopShowIcons_ = true;
    bool desktopPrefsReady_ = false;
    bool gameCloseHintVisible_ = false;
    uint32_t gameCloseHintUntilMs_ = 0;
    uint32_t gameCloseHoldStartMs_ = 0;
    uint16_t gameCloseProgressMs_ = 0;
    uint8_t gamePixelX_ = 0;
    uint8_t gamePixelY_ = 0;
    uint8_t gamePixelFoodX_ = 6;
    uint8_t gamePixelFoodY_ = 4;
    int8_t gamePixelDx_ = 1;
    int8_t gamePixelDy_ = 0;
    uint16_t gamePixelScore_ = 0;
    uint32_t gamePixelStepMs_ = 0;
    bool gamePixelOver_ = false;
    int16_t orbitPaddleX_ = 44;
    int16_t orbitBallX_ = 60;
    int16_t orbitBallY_ = 48;
    int8_t orbitDx_ = 1;
    int8_t orbitDy_ = -1;
    uint16_t orbitScore_ = 0;
    uint32_t orbitStepMs_ = 0;
    bool orbitOver_ = false;
    static constexpr uint8_t kBrowserItemMax = 32;
    static constexpr uint8_t kBrowserClickMax = 32;
    // smaller history in the window manager as well
    static constexpr uint8_t kBrowserHistoryMax = 3;
    static constexpr uint8_t kBrowserFavoriteMax = 3;

    struct BrowserItem {
        uint8_t kind = 0;
        bool bold = false;
        bool hidden = false;
        bool imageLoaded = false;
        uint16_t fg = 0x2104;
        uint16_t bg = 0xFFFF;
        uint16_t imageW = 64;
        uint16_t imageH = 40;
        char text[64] = "";
        char href[64] = "";
        char script[64] = "";
    };

    struct BrowserClickTarget {
        Rect rect{0, 0, 0, 0};
        uint8_t kind = 0;
        uint8_t itemIndex = 0;
    };

    uint8_t browserPresetIndex_ = 0;
    bool browserLoading_ = false;
    bool browserLoaded_ = false;
    bool browserHttpsBlocked_ = false;
    uint32_t browserLoadMs_ = 0;
    int16_t browserScrollY_ = 0;
    char browserUrl_[katux::graphics::browser::BrowserApp::kUrlMaxLen] = "http://example.com/";
    char browserTitle_[32] = "Navigator";
    char browserStatus_[32] = "Ready";
    char browserAlert_[40] = "";
    uint32_t browserAlertUntilMs_ = 0;
    uint8_t browserItemCount_ = 0;
    BrowserItem browserItems_[kBrowserItemMax]{};
    uint8_t browserClickCount_ = 0;
    BrowserClickTarget browserClickTargets_[kBrowserClickMax]{};
    uint8_t browserHistoryCount_ = 0;
    int8_t browserHistoryIndex_ = -1;
    char browserHistory_[kBrowserHistoryMax][katux::graphics::browser::BrowserApp::kUrlMaxLen]{};
    uint8_t browserFavoriteCount_ = 0;
    char browserFavorites_[kBrowserFavoriteMax][katux::graphics::browser::BrowserApp::kUrlMaxLen]{};
    bool browserPrefsReady_ = false;
    bool browserKeyboardOpen_ = false;
    bool browserKeyboardEditingForm_ = false;
    int8_t browserHoldScrollDir_ = 0;
    uint32_t browserHoldScrollNextMs_ = 0;
    uint32_t browserHoldScrollStartMs_ = 0;
    char imageViewerSrcByWindow_[kMaxWindows][64]{};
    katux::graphics::browser::BrowserApp browserApp_{};

    katux::system::SoftKeyboard keyboard_;
    Preferences prefs_;
    Preferences browserPrefs_;
    Preferences desktopPrefs_;

    int8_t hitTest(int16_t x, int16_t y) const;
    bool inTitleBar(const Window& w, int16_t x, int16_t y) const;
    void bringToFront(int8_t zIndex);
    Rect windowBody(const Window& w) const;
    void renderDemo(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderSettings(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderTaskManager(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderExplorer(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderNotepad(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderNotifications(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderWifiManager(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderBrowser(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderImageVisualizer(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderDesktopConfig(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderAppHub(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderGamePixel(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);
    void renderGameOrbit(Renderer& renderer, const Theme& t, const Window& w, const Rect* clip);

    bool isCoveredByTopWindow(const Rect& rect, uint8_t zIndex) const;
    int8_t createKindWindow(WindowKind kind);

    void refreshExplorerEntries();

    void browserSetStatus(const char* text);
    void browserClearDocument();
    void browserLoadState();
    void browserSaveState();
    void browserPushHistory(const char* url);
    void browserNavigateHistory(int8_t step);
    bool browserResolveUrl(const char* baseUrl, const char* href, char* out, size_t outLen) const;
    bool browserFetchDocument(const char* url, char* outBody, size_t outBodyLen, char* outType, size_t outTypeLen);
    void browserParseDocument(const char* html, const char* activeUrl);
    void browserRunScript(const char* script, const char* contextUrl);
    void browserOpenUrl(const char* url, bool addToHistory = true);
    void browserOpenPreset(int8_t delta);
    void browserLoadImagePreview(uint8_t itemIndex);
    void browserHandleContentClick(int16_t x, int16_t y);
    bool browserIsFavorite(const char* url) const;
    void browserToggleFavorite(const char* url);
    uint16_t browserParseCssColor(const char* value, uint16_t fallback) const;
    void browserApplyStyle(const char* style, uint16_t& fg, uint16_t& bg, bool& bold, bool& hidden) const;
    bool browserExtractAttr(const char* tag, const char* attr, char* out, size_t outLen) const;
    void browserDecodeEntities(char* text) const;

    void emit(katux::core::EventType type, int32_t data0 = 0, int32_t data1 = 0);
    void syncFocusFlags();
    void updateHover(int16_t cursorX, int16_t cursorY);
    void markDirty(const Rect& rect);
    void markDirtyWindow(const Window& before, const Window& after);
    Rect frameRect(const Window& w) const;
    void loadSavedWifi();
    void saveWifiCredential(const char* ssid, const char* password);
    void clearSavedWifi();
    uint8_t explorerFolderForName(const char* name) const;
    void desktopLoadConfig();
    void desktopSaveConfig();
};

}