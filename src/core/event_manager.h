#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace katux::core {

enum class EventType : uint8_t {
    None = 0,
    SystemTick,
    BootProgress,
    BootComplete,
    FsMounted,
    FsMountFailed,
    ButtonDown,
    ButtonUp,
    ButtonClick,
    ButtonDouble,
    ButtonTriple,
    ButtonLong,
    DragStart,
    DragMove,
    DragEnd,
    FocusNext,
    OpenBios,
    OpenQuickMenu,
    ShutdownRequest,
    LaunchSettings,
    LaunchDemo,
    WindowClosed,
    WindowMinimized,
    WindowRestored,
    ShortcutSafeMode,
    ShortcutDiagnostics,
    SettingChanged,
    ResetConfigRequest,
    FatalError
};

enum class EventSource : uint8_t {
    None = 0,
    ButtonA,
    ButtonB,
    ButtonPower,
    Kernel,
    Boot,
    Bios,
    Desktop,
    WindowManager,
    App
};

enum class SettingKey : uint8_t {
    ThemeDark = 0,
    Brightness,
    CursorSpeed,
    PerformanceProfile,
    DebugOverlay,
    AutoTime,
    TimezoneOffset,
    ManualYear,
    ManualMonth,
    ManualDay,
    ManualHour,
    ManualMinute,
    ClearSavedWifi,
    AnimationsEnabled
};

struct Event {
    EventType type = EventType::None;
    EventSource source = EventSource::None;
    uint32_t timestampMs = 0;
    int16_t x = 0;
    int16_t y = 0;
    int32_t data0 = 0;
    int32_t data1 = 0;
};

class EventManager {
   public:
    static constexpr uint8_t kQueueSize = 64;

    void begin();
    bool push(const Event& event);
    bool pop(Event& event);
    bool peek(Event& event) const;
    uint8_t size() const;
    bool empty() const;
    void clear();

   private:
    Event queue_[kQueueSize]{};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
};

}