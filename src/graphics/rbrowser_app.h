#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "renderer.h"

namespace katux::graphics::rbrowser {

class RemoteBrowserApp {
   public:
    static constexpr uint16_t kDefaultPort = 16767;

    void begin();
    void tick(uint32_t nowMs, const Rect& body, int16_t cursorX, int16_t cursorY, bool focused);
    bool handleClick(int16_t x, int16_t y, const Rect& body);
    bool consumeKeyboardRequest(char* outInitial, size_t outLen);
    void applyKeyboardValue(const char* value, bool accepted);
    uint8_t consumeDirtyRegions(Rect* out, uint8_t maxItems, const Rect& body);
    void render(katux::graphics::Renderer& renderer, const Rect& body, uint32_t nowMs, const Rect* clip = nullptr);
    bool isBusy() const;
    bool isConnected() const;
    bool needsFrameTick() const;
    const char* status() const;
    const char* pageUrl() const;
    const char* serverAddress() const;
    void disconnect();

   private:
    enum class UiMode : uint8_t {
        Disconnected = 0,
        Scanning,
        Connecting,
        Connected,
        Error
    };

    enum class KeyboardTarget : uint8_t {
        None = 0,
        ServerHost,
        ServerPort,
        PageUrl,
        TextInput
    };

    struct LocalRect {
        int16_t x = 0;
        int16_t y = 0;
        int16_t w = 0;
        int16_t h = 0;
    };

    struct ServerEntry {
        bool used = false;
        char host[48] = "";
        uint16_t port = kDefaultPort;
    };

    struct RenderLayout {
        Rect headerSection{0, 0, 0, 0};
        Rect urlBar{0, 0, 0, 0};
        Rect favBtn{0, 0, 0, 0};
        Rect backBtn{0, 0, 0, 0};
        Rect fwdBtn{0, 0, 0, 0};
        Rect prevBtn{0, 0, 0, 0};
        Rect nextBtn{0, 0, 0, 0};
        Rect openBtn{0, 0, 0, 0};
        Rect upBtn{0, 0, 0, 0};
        Rect downBtn{0, 0, 0, 0};
        Rect sidePanel{0, 0, 0, 0};
        Rect viewport{0, 0, 0, 0};
        Rect statusBar{0, 0, 0, 0};
        Rect contentRect{0, 0, 0, 0};
        Rect schemeBadge{0, 0, 0, 0};
    };

    struct ConnectModalLayout {
        Rect modal{0, 0, 0, 0};
        Rect titleRect{0, 0, 0, 0};
        Rect addressRect{0, 0, 0, 0};
        Rect scanBtn{0, 0, 0, 0};
        Rect hostBtn{0, 0, 0, 0};
        Rect portBtn{0, 0, 0, 0};
        Rect linkBtn{0, 0, 0, 0};
    };

    struct ScanWorkerContext {
        RemoteBrowserApp* app = nullptr;
        uint8_t index = 0;
    };

    enum class NetworkCommandType : uint8_t {
        None = 0,
        Resize,
        Click,
        Scroll,
        Navigate,
        Action,
        Text
    };

    struct NetworkCommand {
        NetworkCommandType type = NetworkCommandType::None;
        int16_t x = 0;
        int16_t y = 0;
        int16_t delta = 0;
        char text[192] = "";
    };

    static constexpr uint8_t kMaxServers = 6;
    static constexpr uint8_t kPatchDirtyMax = 12;
    static constexpr uint8_t kScanWorkerCount = 8;
    static constexpr uint8_t kNetworkCommandMax = 10;

    RenderLayout buildLayout(const Rect& body) const;
    ConnectModalLayout buildConnectModal(const Rect& contentRect) const;
    void queueChromeDirty();
    void queueViewportDirty(const LocalRect& rect);
    void queueViewportFullDirty();
    void addServer(const char* host, uint16_t port);
    void loadKnownHosts();
    void saveKnownHosts();
    bool claimNextScanHost(uint8_t& host);
    void markScanHostComplete(const char* discoveredHost, uint16_t port);
    void notifyScanWorkerStopped();
    static void scanWorkerEntry(void* param);
    static void networkWorkerEntry(void* param);
    void ensureNetworkWorker();
    void stopNetworkWorker();
    bool enqueueCommand(NetworkCommandType type, const char* text = nullptr, int16_t x = 0, int16_t y = 0, int16_t delta = 0);
    void queuePointer(int16_t x, int16_t y);
    bool openSession(const Rect& contentRect);
    bool resizeSession(const Rect& contentRect);
    bool pollFrame(const Rect& contentRect);
    bool sendPointer(int16_t x, int16_t y);
    bool sendClick(int16_t x, int16_t y);
    bool sendScroll(int16_t deltaY);
    bool sendNavigate(const char* url);
    bool sendAction(const char* action);
    bool sendText(const char* text);
    bool closeSession();
    void startScan();
    void stopScan();
    void scanStep();
    void stopScanWorkers();
    void ensureAddressText();
    bool ensureFrameBuffers(uint16_t width, uint16_t height);
    void releaseFrameBuffers();
    bool applyRegionPayload(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Stream& stream, uint32_t payloadLen);
    void renderBufferedViewport(katux::graphics::Renderer& renderer, const Rect& target, const Rect* clip);
    void setStatus(const char* text);
    void requestKeyboard(KeyboardTarget target, const char* initial);
    bool pointInRect(int16_t x, int16_t y, const Rect& rect) const;

    UiMode mode_ = UiMode::Disconnected;
    KeyboardTarget keyboardTarget_ = KeyboardTarget::None;
    char pendingKeyboardInitial_[192] = "";

    char serverHost_[48] = "";
    uint16_t serverPort_ = kDefaultPort;
    char address_[64] = "";
    char pageUrl_[192] = "about:blank";
    char pageTitle_[44] = "R-Browser";
    char status_[44] = "Serveur non connecte";
    char sessionId_[32] = "";
    bool textFocus_ = false;
    bool requestFullFrame_ = true;

    uint32_t lastPollMs_ = 0;
    uint32_t lastPointerMs_ = 0;
    uint32_t lastMetaMs_ = 0;
    int16_t lastPointerX_ = -1;
    int16_t lastPointerY_ = -1;
    bool pointerInside_ = false;

    uint8_t currentSubnetA_ = 192;
    uint8_t currentSubnetB_ = 168;
    uint8_t currentSubnetC_ = 0;
    uint16_t scanNextHost_ = 1;
    uint16_t scanProgress_ = 0;
    uint16_t scanCompletedHosts_ = 0;
    uint8_t scanActiveWorkers_ = 0;
    bool scanRunning_ = false;
    bool scanStopRequested_ = false;
    ServerEntry servers_[kMaxServers]{};
    uint8_t selectedServer_ = 0;
    uint8_t serverCount_ = 0;
    int8_t serverScroll_ = 0;
    bool knownHostsDirty_ = false;
    Preferences prefs_;
    bool prefsReady_ = false;
    portMUX_TYPE scanMux_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t scanWorkers_[kScanWorkerCount]{};
    ScanWorkerContext scanWorkerCtx_[kScanWorkerCount]{};
    portMUX_TYPE networkMux_ = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE frameMux_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t networkWorker_ = nullptr;
    bool networkWorkerRunning_ = false;
    bool networkStopRequested_ = false;
    uint16_t networkContentW_ = 0;
    uint16_t networkContentH_ = 0;
    bool pointerPending_ = false;
    int16_t pendingPointerX_ = -1;
    int16_t pendingPointerY_ = -1;
    NetworkCommand networkCommands_[kNetworkCommandMax]{};
    uint8_t networkCommandHead_ = 0;
    uint8_t networkCommandTail_ = 0;
    uint8_t networkCommandCount_ = 0;

    uint16_t frameW_ = 0;
    uint16_t frameH_ = 0;
    uint16_t* frameBuffer_ = nullptr;
    uint16_t* lineBuffer_ = nullptr;
    uint32_t lastFrameSeq_ = 0;

    bool dirtyChrome_ = true;
    bool dirtySidePanel_ = true;
    bool dirtyStatus_ = true;
    bool dirtyViewportFull_ = true;
    LocalRect patchDirty_[kPatchDirtyMax]{};
    uint8_t patchDirtyCount_ = 0;
};

}