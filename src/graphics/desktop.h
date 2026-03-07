#pragma once

#include "renderer.h"
#include "theme.h"
#include "compositor.h"
#include "window_manager.h"
#include "../core/event_manager.h"

namespace katux::graphics {

class Desktop {
   public:
    void begin(katux::core::EventManager* events, bool safeMode);
    bool onEvent(const katux::core::Event& event, int16_t cursorX, int16_t cursorY);
    bool render(Renderer& renderer, bool fullRedraw, int16_t cursorX, int16_t cursorY);
    void renderClip(Renderer& renderer, const Rect& clip);
    CursorStyle cursorStyle() const;
    bool needsFrameTick() const;
    void invalidateAll();
    void setSystemState(bool darkTheme, uint8_t brightness, uint8_t cursorSpeed, uint8_t performanceProfile, bool debugOverlay, bool animationsEnabled,
                        bool autoTime, int8_t timezoneOffset, uint16_t manualYear, uint8_t manualMonth, uint8_t manualDay, uint8_t manualHour,
                        uint8_t manualMinute);
    void setRuntimeStats(uint8_t fps, uint32_t freeHeap, uint32_t uptimeMs, uint8_t queueDepth);

   private:
    struct AppEntry {
        const char* name;
        WindowKind kind;
        Rect icon;
        Rect label;
        uint8_t accent;
    };

    WindowManager windows_;
    Compositor compositor_;
    katux::core::EventManager* events_ = nullptr;

    bool initialized_ = false;
    bool safeMode_ = false;
    bool startOpen_ = false;
    bool startPowerOpen_ = false;
    uint8_t startMenuScroll_ = 0;
    bool resetConfirmArmed_ = false;
    uint32_t resetConfirmUntilMs_ = 0;
    char toast_[28] = "";
    uint32_t toastUntilMs_ = 0;
    uint32_t quickActionPulseUntilMs_ = 0;
    int8_t quickActionPulseIndex_ = -1;
    int8_t quickActionHoverIndex_ = -1;
    int16_t lastCursorX_ = 0;
    int16_t lastCursorY_ = 0;

    static constexpr uint8_t kMaxApps = 16;
    static constexpr uint8_t kDesktopSlotCount = 6;
    AppEntry apps_[kMaxApps]{};
    uint8_t appCount_ = 0;
    int8_t appWindowIds_[kMaxApps]{};
    uint8_t selectedIcon_ = 0;
    int8_t hoverIcon_ = -1;
    bool animationsEnabled_ = true;
    bool desktopShowIcons_ = true;
    WindowKind desktopSlots_[kDesktopSlotCount]{WindowKind::AppHub, WindowKind::Settings, WindowKind::Explorer, WindowKind::Notepad, WindowKind::WifiManager, WindowKind::TaskManager};
    bool contextMenuOpen_ = false;
    static constexpr uint8_t kContextMenuItemCount = 5;
    static constexpr int16_t kContextMenuItemHeight = 10;
    Rect contextMenuRect_{0, 0, 168, static_cast<int16_t>(kContextMenuItemCount * kContextMenuItemHeight + 4)};

    Rect startRect_{0, 0, 44, 15};
    Rect menuRect_{2, 26, 118, 74};

    int8_t appIndexForKind(WindowKind kind) const;
    void refreshDesktopConfig();
    int8_t hitIcon(int16_t x, int16_t y) const;
    int8_t hitStartMenuItem(int16_t x, int16_t y) const;
    int8_t hitTaskbarItem(int16_t x, int16_t y) const;
    int8_t hitTaskbarQuickAction(int16_t x, int16_t y) const;
    int8_t hitContextMenuItem(int16_t x, int16_t y) const;
    bool openWindowForApp(uint8_t appIndex);
    void showToast(const char* text, uint16_t durationMs = 1300);

    void emit(katux::core::EventType type, int32_t d0 = 0, int32_t d1 = 0);

    void renderWallpaper(Renderer& renderer, const Rect& clip);
    void renderRegion(Renderer& renderer, const Rect& clip);
    void renderIcons(Renderer& renderer, const Rect* clip);
    void renderTaskbar(Renderer& renderer, const Rect* clip);
    void renderStartMenu(Renderer& renderer, const Rect* clip);
    void renderContextMenu(Renderer& renderer, const Rect* clip);
};

}