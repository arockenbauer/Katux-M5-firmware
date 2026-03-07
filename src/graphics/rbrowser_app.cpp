#include "rbrowser_app.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace katux::graphics::rbrowser {

namespace {

static void rbLog(const char* fmt, ...) {
    if (!fmt) return;
    char line[192] = "";
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Serial.printf("[RBrowser] %s\n", line);
}

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

static bool startsWith(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int16_t clamp16(int16_t v, int16_t lo, int16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool rectsIntersect(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

static Rect rectIntersection(const Rect& a, const Rect& b) {
    const int16_t x1 = a.x > b.x ? a.x : b.x;
    const int16_t y1 = a.y > b.y ? a.y : b.y;
    const int16_t x2a = static_cast<int16_t>(a.x + a.w);
    const int16_t x2b = static_cast<int16_t>(b.x + b.w);
    const int16_t y2a = static_cast<int16_t>(a.y + a.h);
    const int16_t y2b = static_cast<int16_t>(b.y + b.h);
    const int16_t x2 = x2a < x2b ? x2a : x2b;
    const int16_t y2 = y2a < y2b ? y2a : y2b;
    if (x2 <= x1 || y2 <= y1) return Rect{0, 0, 0, 0};
    return Rect{x1, y1, static_cast<int16_t>(x2 - x1), static_cast<int16_t>(y2 - y1)};
}

static Rect clipRect(const Rect& rect, const Rect* clip) {
    if (!clip) return rect;
    return rectIntersection(rect, *clip);
}

static void fillRectClipped(katux::graphics::Renderer& renderer, const Rect& rect, uint16_t color, const Rect* clip) {
    const Rect paint = clipRect(rect, clip);
    if (paint.w > 0 && paint.h > 0) {
        renderer.fillRect(paint, color);
    }
}

static void drawFrameClipped(katux::graphics::Renderer& renderer, const Rect& rect, uint16_t color, const Rect* clip) {
    if (rect.w <= 0 || rect.h <= 0) return;
    fillRectClipped(renderer, Rect{rect.x, rect.y, rect.w, 1}, color, clip);
    if (rect.h > 1) {
        fillRectClipped(renderer, Rect{rect.x, static_cast<int16_t>(rect.y + rect.h - 1), rect.w, 1}, color, clip);
    }
    if (rect.h > 2) {
        const int16_t sideH = static_cast<int16_t>(rect.h - 2);
        fillRectClipped(renderer, Rect{rect.x, static_cast<int16_t>(rect.y + 1), 1, sideH}, color, clip);
        if (rect.w > 1) {
            fillRectClipped(renderer, Rect{static_cast<int16_t>(rect.x + rect.w - 1), static_cast<int16_t>(rect.y + 1), 1, sideH}, color, clip);
        }
    }
}

static void drawTextClipped(katux::graphics::Renderer& renderer,
                            int16_t x,
                            int16_t y,
                            const char* text,
                            uint16_t fg,
                            uint16_t bg,
                            const Rect* clip,
                            int16_t glyphW = 6,
                            int16_t glyphH = 8) {
    if (!text || !text[0]) return;
    const int16_t len = static_cast<int16_t>(strlen(text));
    if (len <= 0) return;
    const Rect bounds{x, y, static_cast<int16_t>(len * glyphW), glyphH};
    if (!clip || rectsIntersect(bounds, *clip)) {
        renderer.drawText(x, y, text, fg, bg);
    }
}

static bool readExact(Stream& stream, uint8_t* out, size_t len, uint32_t timeoutMs) {
    if (!out && len != 0) return false;
    size_t done = 0;
    uint32_t lastActivity = millis();
    while (done < len) {
        if (stream.available()) {
            const int c = stream.read();
            if (c < 0) continue;
            out[done++] = static_cast<uint8_t>(c);
            lastActivity = millis();
            continue;
        }
        if (millis() - lastActivity > timeoutMs) return false;
        delay(1);
    }
    return true;
}

static bool drainStream(Stream& stream, uint32_t timeoutMs, int32_t bytesToDrain = -1) {
    uint8_t scratch[64];
    uint32_t lastActivity = millis();
    int32_t remaining = bytesToDrain;
    while (remaining != 0) {
        const size_t available = static_cast<size_t>(stream.available());
        if (available == 0) {
            if (millis() - lastActivity > timeoutMs) {
                return bytesToDrain < 0;
            }
            delay(1);
            continue;
        }

        size_t toRead = available > sizeof(scratch) ? sizeof(scratch) : available;
        if (remaining > 0 && static_cast<int32_t>(toRead) > remaining) {
            toRead = static_cast<size_t>(remaining);
        }

        const size_t n = stream.readBytes(scratch, toRead);
        if (n == 0) {
            if (millis() - lastActivity > timeoutMs) {
                return bytesToDrain < 0;
            }
            delay(1);
            continue;
        }

        lastActivity = millis();
        if (remaining > 0) {
            remaining -= static_cast<int32_t>(n);
        }
    }
    return true;
}

static bool readU16(Stream& stream, uint16_t& out) {
    uint8_t buf[2];
    if (!readExact(stream, buf, sizeof(buf), 240U)) return false;
    out = static_cast<uint16_t>(buf[0] | (static_cast<uint16_t>(buf[1]) << 8));
    return true;
}

static bool readU32(Stream& stream, uint32_t& out) {
    uint8_t buf[4];
    if (!readExact(stream, buf, sizeof(buf), 240U)) return false;
    out = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) | (static_cast<uint32_t>(buf[2]) << 16) |
          (static_cast<uint32_t>(buf[3]) << 24);
    return true;
}

static bool parseKeyValue(const char* body, const char* key, char* out, size_t outLen) {
    if (!body || !key || !out || outLen == 0) return false;
    out[0] = '\0';
    char needle[40] = "";
    snprintf(needle, sizeof(needle), "%s=", key);
    const char* pos = strstr(body, needle);
    if (!pos) return false;
    pos += strlen(needle);
    size_t w = 0;
    while (pos[w] && pos[w] != '\n' && pos[w] != '\r' && w + 1 < outLen) {
        out[w] = pos[w];
        ++w;
    }
    out[w] = '\0';
    return w > 0;
}

static bool isDigits(const char* s) {
    if (!s || !s[0]) return false;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static bool readResponseText(HTTPClient& http, char* out, size_t outLen, uint32_t timeoutMs) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';

    Stream* stream = http.getStreamPtr();
    if (!stream) return false;

    size_t written = 0;
    uint8_t chunk[64];
    uint32_t lastActivity = millis();
    while (http.connected() || stream->available() > 0) {
        const size_t available = static_cast<size_t>(stream->available());
        if (available == 0) {
            if (millis() - lastActivity > timeoutMs) break;
            delay(1);
            continue;
        }
        const size_t toRead = available > sizeof(chunk) ? sizeof(chunk) : available;
        const size_t n = stream->readBytes(chunk, toRead);
        if (n == 0) {
            if (millis() - lastActivity > timeoutMs) break;
            delay(1);
            continue;
        }
        lastActivity = millis();
        if (written + 1 < outLen) {
            size_t copyLen = n;
            if (copyLen > (outLen - 1U - written)) copyLen = outLen - 1U - written;
            memcpy(out + written, chunk, copyLen);
            written += copyLen;
            out[written] = '\0';
        }
    }
    return true;
}

static bool streamContains(HTTPClient& http, const char* needle, uint32_t timeoutMs) {
    if (!needle || !needle[0]) return false;
    Stream* stream = http.getStreamPtr();
    if (!stream) return false;

    const size_t needleLen = strlen(needle);
    if (needleLen == 0 || needleLen > 63) return false;

    char window[64] = "";
    size_t windowLen = 0;
    uint8_t chunk[64];
    uint32_t lastActivity = millis();
    while (http.connected() || stream->available() > 0) {
        const size_t available = static_cast<size_t>(stream->available());
        if (available == 0) {
            if (millis() - lastActivity > timeoutMs) break;
            delay(1);
            continue;
        }
        const size_t toRead = available > sizeof(chunk) ? sizeof(chunk) : available;
        const size_t n = stream->readBytes(chunk, toRead);
        if (n == 0) {
            if (millis() - lastActivity > timeoutMs) break;
            delay(1);
            continue;
        }
        lastActivity = millis();
        for (size_t i = 0; i < n; ++i) {
            if (windowLen + 1U >= sizeof(window)) {
                const size_t keep = needleLen > 1 ? needleLen - 1U : 0U;
                if (keep > 0 && windowLen > keep) {
                    memmove(window, window + windowLen - keep, keep);
                }
                windowLen = keep;
                window[windowLen] = '\0';
            }
            window[windowLen++] = static_cast<char>(chunk[i]);
            window[windowLen] = '\0';
            if (strstr(window, needle)) return true;
        }
    }
    return strstr(window, needle) != nullptr;
}

static bool httpCollectText(const char* host, uint16_t port, const char* path, const char* method, const char* body, char* out, size_t outLen, uint16_t timeoutMs) {
    if (!host || !host[0] || !path || !out || outLen == 0) return false;
    out[0] = '\0';

    char url[192] = "";
    snprintf(url, sizeof(url), "http://%s:%u%s", host, static_cast<unsigned>(port), path);

    HTTPClient http;
    http.setTimeout(timeoutMs);
    if (!http.begin(url)) return false;

    int code = -1;
    if (method && strcmp(method, "POST") == 0) {
        if (body) {
            code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), strlen(body));
        } else {
            code = http.POST("");
        }
    } else {
        code = http.GET();
    }

    bool ok = false;
    if (code > 0 && code < 400) {
        ok = readResponseText(http, out, outLen, timeoutMs);
    }
    http.end();
    return ok;
}

static bool probeServer(const char* host, uint16_t port, uint16_t timeoutMs) {
    if (!host || !host[0]) return false;
    char url[160] = "";
    snprintf(url, sizeof(url), "http://%s:%u/rb/discover", host, static_cast<unsigned>(port));

    HTTPClient http;
    http.setTimeout(timeoutMs);
    if (!http.begin(url)) return false;
    const int code = http.GET();
    bool ok = false;
    if (code == 200) {
        ok = streamContains(http, "Katux R-Browser", timeoutMs);
    }
    http.end();
    return ok;
}

static constexpr const char* kRBrowserPrefsNamespace = "katuxrb";

}  // namespace

RemoteBrowserApp::RenderLayout RemoteBrowserApp::buildLayout(const Rect& body) const {
    RenderLayout layout{};
    layout.headerSection = Rect{body.x, body.y, body.w, 28};
    layout.urlBar = Rect{static_cast<int16_t>(body.x + 4), static_cast<int16_t>(body.y + 2), static_cast<int16_t>(body.w - 26), 12};
    layout.favBtn = Rect{static_cast<int16_t>(body.x + body.w - 20), static_cast<int16_t>(body.y + 2), 16, 12};
    layout.backBtn = Rect{static_cast<int16_t>(body.x + 4), static_cast<int16_t>(body.y + 16), 18, 10};
    layout.fwdBtn = Rect{static_cast<int16_t>(body.x + 24), static_cast<int16_t>(body.y + 16), 18, 10};
    layout.prevBtn = Rect{static_cast<int16_t>(body.x + 44), static_cast<int16_t>(body.y + 16), 18, 10};
    layout.nextBtn = Rect{static_cast<int16_t>(body.x + 64), static_cast<int16_t>(body.y + 16), 18, 10};
    layout.openBtn = Rect{static_cast<int16_t>(body.x + 86), static_cast<int16_t>(body.y + 16), 28, 10};
    layout.upBtn = Rect{static_cast<int16_t>(body.x + 118), static_cast<int16_t>(body.y + 16), 14, 10};
    layout.downBtn = Rect{static_cast<int16_t>(body.x + 134), static_cast<int16_t>(body.y + 16), 14, 10};

    const int16_t mainTop = static_cast<int16_t>(body.y + 30);
    const int16_t mainH = static_cast<int16_t>(body.h - 32);
    layout.sidePanel = Rect{static_cast<int16_t>(body.x + 4), mainTop, 70, mainH};
    layout.viewport = Rect{static_cast<int16_t>(layout.sidePanel.x + layout.sidePanel.w + 2), mainTop,
                           static_cast<int16_t>(body.w - layout.sidePanel.w - 8), mainH};
    layout.statusBar = Rect{layout.viewport.x, static_cast<int16_t>(layout.viewport.y + layout.viewport.h - 12), layout.viewport.w, 12};
    layout.contentRect = Rect{layout.viewport.x, static_cast<int16_t>(layout.viewport.y + 2), layout.viewport.w,
                              static_cast<int16_t>(layout.viewport.h - 14)};
    layout.schemeBadge = Rect{static_cast<int16_t>(layout.urlBar.x + 1), static_cast<int16_t>(layout.urlBar.y + 1), 10, 10};
    return layout;
}

RemoteBrowserApp::ConnectModalLayout RemoteBrowserApp::buildConnectModal(const Rect& contentRect) const {
    ConnectModalLayout modal{};
    const int16_t insetX = contentRect.w > 56 ? 6 : 2;
    const int16_t insetY = contentRect.h > 58 ? 6 : 2;
    const int16_t modalW = contentRect.w - insetX * 2;
    int16_t modalH = contentRect.h - insetY * 2;
    if (modalH > 76) modalH = 76;
    if (modalH < 48) modalH = 48;
    modal.modal = Rect{static_cast<int16_t>(contentRect.x + insetX), static_cast<int16_t>(contentRect.y + (contentRect.h - modalH) / 2), modalW, modalH};

    modal.titleRect = Rect{static_cast<int16_t>(modal.modal.x + 6), static_cast<int16_t>(modal.modal.y + 4), static_cast<int16_t>(modal.modal.w - 12), 8};
    modal.addressRect = Rect{static_cast<int16_t>(modal.modal.x + 6), static_cast<int16_t>(modal.modal.y + 14), static_cast<int16_t>(modal.modal.w - 12), 8};

    const int16_t buttonTop = static_cast<int16_t>(modal.modal.y + 24);
    int16_t availableH = static_cast<int16_t>(modal.modal.y + modal.modal.h - 4 - buttonTop);
    if (availableH < 24) availableH = 24;
    int16_t slotH = static_cast<int16_t>(availableH / 4);
    if (slotH < 6) slotH = 6;
    int16_t buttonH = static_cast<int16_t>(slotH - 1);
    if (buttonH > 10) buttonH = 10;
    const int16_t buttonW = static_cast<int16_t>(modal.modal.w - 8);
    const int16_t buttonX = static_cast<int16_t>(modal.modal.x + 4);
    modal.scanBtn = Rect{buttonX, buttonTop, buttonW, buttonH};
    modal.hostBtn = Rect{buttonX, static_cast<int16_t>(buttonTop + slotH), buttonW, buttonH};
    modal.portBtn = Rect{buttonX, static_cast<int16_t>(buttonTop + slotH * 2), buttonW, buttonH};
    modal.linkBtn = Rect{buttonX, static_cast<int16_t>(buttonTop + slotH * 3), buttonW, buttonH};
    return modal;
}

bool RemoteBrowserApp::pointInRect(int16_t x, int16_t y, const Rect& rect) const {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

void RemoteBrowserApp::queueChromeDirty() {
    dirtyChrome_ = true;
    dirtySidePanel_ = true;
    dirtyStatus_ = true;
}

void RemoteBrowserApp::queueViewportFullDirty() {
    dirtyViewportFull_ = true;
    patchDirtyCount_ = 0;
}

void RemoteBrowserApp::queueViewportDirty(const LocalRect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return;
    if (dirtyViewportFull_) return;
    if (patchDirtyCount_ >= kPatchDirtyMax) {
        dirtyViewportFull_ = true;
        patchDirtyCount_ = 0;
        return;
    }
    patchDirty_[patchDirtyCount_++] = rect;
}

void RemoteBrowserApp::setStatus(const char* text) {
    if (!text) return;
    if (strncmp(status_, text, sizeof(status_) - 1) == 0) return;
    copyTrim(status_, sizeof(status_), text, sizeof(status_) - 1);
    dirtyStatus_ = true;
    dirtySidePanel_ = true;
}

void RemoteBrowserApp::ensureAddressText() {
    snprintf(address_, sizeof(address_), "%s:%u", serverHost_[0] ? serverHost_ : "0.0.0.0", static_cast<unsigned>(serverPort_));
}

void RemoteBrowserApp::begin() {
    const IPAddress local = WiFi.localIP();
    currentSubnetA_ = 192;
    currentSubnetB_ = 168;
    currentSubnetC_ = (local[0] == 192 && local[1] == 168) ? local[2] : 0;
    snprintf(serverHost_, sizeof(serverHost_), "192.168.%u.1", static_cast<unsigned>(currentSubnetC_));
    serverPort_ = kDefaultPort;
    ensureAddressText();
    copyTrim(pageUrl_, sizeof(pageUrl_), "google.com", sizeof(pageUrl_) - 1);
    copyTrim(pageTitle_, sizeof(pageTitle_), "Google", sizeof(pageTitle_) - 1);
    rbLog("begin local=%u.%u.%u.%u default=%s:%u", static_cast<unsigned>(local[0]), static_cast<unsigned>(local[1]), static_cast<unsigned>(local[2]),
          static_cast<unsigned>(local[3]), serverHost_, static_cast<unsigned>(serverPort_));
    copyTrim(status_, sizeof(status_), "Pret a scanner le LAN", sizeof(status_) - 1);
    sessionId_[0] = '\0';
    keyboardTarget_ = KeyboardTarget::None;
    pendingKeyboardInitial_[0] = '\0';
    textFocus_ = false;
    requestFullFrame_ = true;
    lastPollMs_ = 0;
    lastPointerMs_ = 0;
    lastMetaMs_ = 0;
    lastPointerX_ = -1;
    lastPointerY_ = -1;
    pointerInside_ = false;
    scanNextHost_ = 1;
    scanProgress_ = 0;
    scanCompletedHosts_ = 0;
    scanActiveWorkers_ = 0;
    scanRunning_ = false;
    scanStopRequested_ = false;
    serverCount_ = 0;
    selectedServer_ = 0;
    serverScroll_ = 0;
    knownHostsDirty_ = false;
    memset(servers_, 0, sizeof(servers_));
    memset(scanWorkers_, 0, sizeof(scanWorkers_));
    memset(scanWorkerCtx_, 0, sizeof(scanWorkerCtx_));
    networkWorker_ = nullptr;
    networkWorkerRunning_ = false;
    networkStopRequested_ = false;
    networkContentW_ = 0;
    networkContentH_ = 0;
    pointerPending_ = false;
    pendingPointerX_ = -1;
    pendingPointerY_ = -1;
    networkCommandHead_ = 0;
    networkCommandTail_ = 0;
    networkCommandCount_ = 0;
    memset(networkCommands_, 0, sizeof(networkCommands_));
    prefsReady_ = prefs_.begin(kRBrowserPrefsNamespace, false);
    if (prefsReady_) {
        loadKnownHosts();
    }
    releaseFrameBuffers();
    mode_ = UiMode::Disconnected;
    queueChromeDirty();
    queueViewportFullDirty();
}

void RemoteBrowserApp::addServer(const char* host, uint16_t port) {
    if (!host || !host[0]) return;
    for (uint8_t i = 0; i < kMaxServers; ++i) {
        if (!servers_[i].used) continue;
        if (strcmp(servers_[i].host, host) == 0 && servers_[i].port == port) return;
    }
    if (serverCount_ < kMaxServers) {
        ServerEntry& slot = servers_[serverCount_++];
        slot.used = true;
        copyTrim(slot.host, sizeof(slot.host), host, sizeof(slot.host) - 1);
        slot.port = port;
        knownHostsDirty_ = true;
        return;
    }
    for (uint8_t i = 1; i < kMaxServers; ++i) {
        servers_[i - 1] = servers_[i];
    }
    ServerEntry& slot = servers_[kMaxServers - 1];
    slot.used = true;
    copyTrim(slot.host, sizeof(slot.host), host, sizeof(slot.host) - 1);
    slot.port = port;
    knownHostsDirty_ = true;
}

void RemoteBrowserApp::loadKnownHosts() {
    if (!prefsReady_) return;
    const uint8_t storedCount = static_cast<uint8_t>(prefs_.getUChar("count", 0));
    serverCount_ = 0;
    memset(servers_, 0, sizeof(servers_));
    for (uint8_t i = 0; i < storedCount && i < kMaxServers; ++i) {
        char hostKey[8] = "h0";
        char portKey[8] = "p0";
        hostKey[1] = static_cast<char>('0' + i);
        portKey[1] = static_cast<char>('0' + i);
        const String host = prefs_.getString(hostKey, "");
        const uint16_t port = static_cast<uint16_t>(prefs_.getUShort(portKey, kDefaultPort));
        if (host.length() == 0) continue;
        ServerEntry& slot = servers_[serverCount_++];
        slot.used = true;
        copyTrim(slot.host, sizeof(slot.host), host.c_str(), sizeof(slot.host) - 1);
        slot.port = port;
    }
    knownHostsDirty_ = false;
    if (serverCount_ > 0) {
        selectedServer_ = static_cast<uint8_t>(serverCount_ - 1);
        copyTrim(serverHost_, sizeof(serverHost_), servers_[selectedServer_].host, sizeof(serverHost_) - 1);
        serverPort_ = servers_[selectedServer_].port;
        ensureAddressText();
    }
}

void RemoteBrowserApp::saveKnownHosts() {
    if (!prefsReady_ || !knownHostsDirty_) return;
    prefs_.putUChar("count", serverCount_);
    for (uint8_t i = 0; i < kMaxServers; ++i) {
        char hostKey[8] = "h0";
        char portKey[8] = "p0";
        hostKey[1] = static_cast<char>('0' + i);
        portKey[1] = static_cast<char>('0' + i);
        if (i < serverCount_ && servers_[i].used && servers_[i].host[0]) {
            prefs_.putString(hostKey, servers_[i].host);
            prefs_.putUShort(portKey, servers_[i].port);
        } else {
            prefs_.remove(hostKey);
            prefs_.remove(portKey);
        }
    }
    knownHostsDirty_ = false;
}

bool RemoteBrowserApp::claimNextScanHost(uint8_t& host) {
    bool claimed = false;
    portENTER_CRITICAL(&scanMux_);
    if (!scanStopRequested_ && scanNextHost_ <= 254) {
        host = scanNextHost_++;
        claimed = true;
    }
    portEXIT_CRITICAL(&scanMux_);
    return claimed;
}

void RemoteBrowserApp::markScanHostComplete(const char* discoveredHost, uint16_t port) {
    portENTER_CRITICAL(&scanMux_);
    if (discoveredHost && discoveredHost[0]) {
        addServer(discoveredHost, port);
    }
    if (scanCompletedHosts_ < 254) {
        ++scanCompletedHosts_;
    }
    if (scanCompletedHosts_ >= 254) {
        scanRunning_ = false;
    }
    portEXIT_CRITICAL(&scanMux_);
}

void RemoteBrowserApp::notifyScanWorkerStopped() {
    portENTER_CRITICAL(&scanMux_);
    if (scanActiveWorkers_ > 0) {
        --scanActiveWorkers_;
    }
    if (scanActiveWorkers_ == 0) {
        scanRunning_ = false;
    }
    portEXIT_CRITICAL(&scanMux_);
}

void RemoteBrowserApp::scanWorkerEntry(void* param) {
    ScanWorkerContext* ctx = static_cast<ScanWorkerContext*>(param);
    if (!ctx || !ctx->app) {
        vTaskDelete(nullptr);
        return;
    }

    RemoteBrowserApp* app = ctx->app;
    char host[32];
    for (;;) {
        uint8_t hostId = 0;
        if (!app->claimNextScanHost(hostId)) {
            break;
        }

        snprintf(host, sizeof(host), "%u.%u.%u.%u", static_cast<unsigned>(app->currentSubnetA_), static_cast<unsigned>(app->currentSubnetB_),
                 static_cast<unsigned>(app->currentSubnetC_), static_cast<unsigned>(hostId));
        if (probeServer(host, kDefaultPort, 180)) {
            app->markScanHostComplete(host, kDefaultPort);
        } else {
            app->markScanHostComplete(nullptr, kDefaultPort);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    app->notifyScanWorkerStopped();
    if (ctx->index < kScanWorkerCount) {
        app->scanWorkers_[ctx->index] = nullptr;
    }
    vTaskDelete(nullptr);
}

void RemoteBrowserApp::stopScanWorkers() {
    portENTER_CRITICAL(&scanMux_);
    scanStopRequested_ = true;
    scanRunning_ = false;
    portEXIT_CRITICAL(&scanMux_);

    uint8_t activeWorkers = 0;
    const uint32_t start = millis();
    for (;;) {
        portENTER_CRITICAL(&scanMux_);
        activeWorkers = scanActiveWorkers_;
        portEXIT_CRITICAL(&scanMux_);
        if (activeWorkers == 0) break;
        if (millis() - start > 1200U) break;
        delay(2);
    }

    if (activeWorkers == 0) {
        for (uint8_t i = 0; i < kScanWorkerCount; ++i) {
            scanWorkers_[i] = nullptr;
        }
    }
}

void RemoteBrowserApp::networkWorkerEntry(void* param) {
    RemoteBrowserApp* app = static_cast<RemoteBrowserApp*>(param);
    if (!app) {
        vTaskDelete(nullptr);
        return;
    }

    portENTER_CRITICAL(&app->networkMux_);
    app->networkWorkerRunning_ = true;
    portEXIT_CRITICAL(&app->networkMux_);

    for (;;) {
        bool stopRequested = false;
        bool connected = false;
        bool pointerPending = false;
        int16_t pointerX = -1;
        int16_t pointerY = -1;
        uint16_t contentW = 0;
        uint16_t contentH = 0;
        NetworkCommand command{};
        bool haveCommand = false;

        portENTER_CRITICAL(&app->networkMux_);
        stopRequested = app->networkStopRequested_;
        connected = app->mode_ == UiMode::Connected;
        contentW = app->networkContentW_;
        contentH = app->networkContentH_;
        if (app->networkCommandCount_ > 0) {
            command = app->networkCommands_[app->networkCommandHead_];
            app->networkCommandHead_ = static_cast<uint8_t>((app->networkCommandHead_ + 1U) % kNetworkCommandMax);
            --app->networkCommandCount_;
            haveCommand = true;
        } else if (app->pointerPending_) {
            pointerPending = true;
            pointerX = app->pendingPointerX_;
            pointerY = app->pendingPointerY_;
            app->pointerPending_ = false;
        }
        portEXIT_CRITICAL(&app->networkMux_);

        if (stopRequested) break;

        Rect contentRect{0, 0, static_cast<int16_t>(contentW), static_cast<int16_t>(contentH)};
        bool didWork = false;
        if (haveCommand) {
            didWork = true;
            if (command.type == NetworkCommandType::Resize && contentRect.w > 0 && contentRect.h > 0) {
                app->resizeSession(contentRect);
            } else if (command.type == NetworkCommandType::Click) {
                app->sendClick(command.x, command.y);
            } else if (command.type == NetworkCommandType::Scroll) {
                app->sendScroll(command.delta);
            } else if (command.type == NetworkCommandType::Navigate) {
                app->sendNavigate(command.text);
            } else if (command.type == NetworkCommandType::Action) {
                app->sendAction(command.text);
            } else if (command.type == NetworkCommandType::Text) {
                app->sendText(command.text);
            }
        } else if (pointerPending && connected) {
            didWork = true;
            app->sendPointer(pointerX, pointerY);
        } else if (connected && contentRect.w > 0 && contentRect.h > 0) {
            const uint32_t nowMs = millis();
            if (nowMs - app->lastPollMs_ >= 55U) {
                didWork = true;
                app->pollFrame(contentRect);
                app->lastPollMs_ = nowMs;
            }
        }

        if (!didWork) {
            vTaskDelay(pdMS_TO_TICKS(6));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    portENTER_CRITICAL(&app->networkMux_);
    app->networkWorkerRunning_ = false;
    app->networkWorker_ = nullptr;
    portEXIT_CRITICAL(&app->networkMux_);
    vTaskDelete(nullptr);
}

void RemoteBrowserApp::ensureNetworkWorker() {
    portENTER_CRITICAL(&networkMux_);
    if (networkWorker_) {
        portEXIT_CRITICAL(&networkMux_);
        return;
    }
    networkStopRequested_ = false;
    portEXIT_CRITICAL(&networkMux_);

    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(networkWorkerEntry, "rbNet", 7168, this, 1, &handle, 0) == pdPASS) {
        portENTER_CRITICAL(&networkMux_);
        networkWorker_ = handle;
        portEXIT_CRITICAL(&networkMux_);
        rbLog("network worker started");
    } else {
        rbLog("network worker start failed");
    }
}

void RemoteBrowserApp::stopNetworkWorker() {
    portENTER_CRITICAL(&networkMux_);
    networkStopRequested_ = true;
    pointerPending_ = false;
    networkCommandHead_ = 0;
    networkCommandTail_ = 0;
    networkCommandCount_ = 0;
    portEXIT_CRITICAL(&networkMux_);

    const uint32_t start = millis();
    for (;;) {
        portENTER_CRITICAL(&networkMux_);
        const bool running = networkWorkerRunning_ || networkWorker_ != nullptr;
        portEXIT_CRITICAL(&networkMux_);
        if (!running) break;
        if (millis() - start > 1500U) break;
        delay(2);
    }
}

bool RemoteBrowserApp::enqueueCommand(NetworkCommandType type, const char* text, int16_t x, int16_t y, int16_t delta) {
    if (type == NetworkCommandType::None) return false;
    bool queued = false;
    portENTER_CRITICAL(&networkMux_);
    if (networkCommandCount_ < kNetworkCommandMax) {
        NetworkCommand& slot = networkCommands_[networkCommandTail_];
        slot.type = type;
        slot.x = x;
        slot.y = y;
        slot.delta = delta;
        copyTrim(slot.text, sizeof(slot.text), text ? text : "", sizeof(slot.text) - 1);
        networkCommandTail_ = static_cast<uint8_t>((networkCommandTail_ + 1U) % kNetworkCommandMax);
        ++networkCommandCount_;
        queued = true;
    }
    portEXIT_CRITICAL(&networkMux_);
    if (!queued) {
        rbLog("network queue full type=%u", static_cast<unsigned>(type));
    }
    return queued;
}

void RemoteBrowserApp::queuePointer(int16_t x, int16_t y) {
    portENTER_CRITICAL(&networkMux_);
    pendingPointerX_ = x;
    pendingPointerY_ = y;
    pointerPending_ = true;
    portEXIT_CRITICAL(&networkMux_);
}

bool RemoteBrowserApp::ensureFrameBuffers(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) return false;

    portENTER_CRITICAL(&frameMux_);
    const bool alreadyReady = frameBuffer_ && lineBuffer_ && width == frameW_ && height == frameH_;
    portEXIT_CRITICAL(&frameMux_);
    if (alreadyReady) return true;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    uint16_t* newFrame = reinterpret_cast<uint16_t*>(malloc(pixelCount * sizeof(uint16_t)));
    uint16_t* newLine = reinterpret_cast<uint16_t*>(malloc(static_cast<size_t>(width) * sizeof(uint16_t)));
    if (!newFrame || !newLine) {
        if (newFrame) free(newFrame);
        if (newLine) free(newLine);
        setStatus("Memoire frame KO");
        mode_ = UiMode::Error;
        return false;
    }

    memset(newFrame, 0xFF, pixelCount * sizeof(uint16_t));
    portENTER_CRITICAL(&frameMux_);
    uint16_t* oldFrame = frameBuffer_;
    uint16_t* oldLine = lineBuffer_;
    frameBuffer_ = newFrame;
    lineBuffer_ = newLine;
    frameW_ = width;
    frameH_ = height;
    lastFrameSeq_ = 0;
    portEXIT_CRITICAL(&frameMux_);

    if (oldFrame) free(oldFrame);
    if (oldLine) free(oldLine);
    queueViewportFullDirty();
    return true;
}

void RemoteBrowserApp::releaseFrameBuffers() {
    portENTER_CRITICAL(&frameMux_);
    uint16_t* oldFrame = frameBuffer_;
    uint16_t* oldLine = lineBuffer_;
    frameBuffer_ = nullptr;
    lineBuffer_ = nullptr;
    frameW_ = 0;
    frameH_ = 0;
    portEXIT_CRITICAL(&frameMux_);
    if (oldFrame) free(oldFrame);
    if (oldLine) free(oldLine);
}

void RemoteBrowserApp::disconnect() {
    rbLog("disconnect mode=%u session=%s", static_cast<unsigned>(mode_), sessionId_[0] ? sessionId_ : "-");
    stopScanWorkers();
    stopNetworkWorker();
    closeSession();
    sessionId_[0] = '\0';
    textFocus_ = false;
    requestFullFrame_ = true;
    lastFrameSeq_ = 0;
    lastPointerX_ = -1;
    lastPointerY_ = -1;
    pointerInside_ = false;
    portENTER_CRITICAL(&networkMux_);
    networkContentW_ = 0;
    networkContentH_ = 0;
    portEXIT_CRITICAL(&networkMux_);
    releaseFrameBuffers();
    if (mode_ != UiMode::Scanning) mode_ = UiMode::Disconnected;
    setStatus("Connexion fermee");
    queueChromeDirty();
    queueViewportFullDirty();
}

void RemoteBrowserApp::startScan() {
    if (WiFi.status() != WL_CONNECTED) {
        mode_ = UiMode::Error;
        setStatus("WiFi indisponible");
        return;
    }

    stopScanWorkers();
    const IPAddress local = WiFi.localIP();
    currentSubnetA_ = 192;
    currentSubnetB_ = 168;
    currentSubnetC_ = (local[0] == 192 && local[1] == 168) ? local[2] : 0;
    scanNextHost_ = 1;
    scanProgress_ = 0;
    scanCompletedHosts_ = 0;
    scanStopRequested_ = false;
    scanRunning_ = true;
    scanActiveWorkers_ = 0;
    mode_ = UiMode::Scanning;
    if (serverCount_ == 0) {
        selectedServer_ = 0;
    } else if (selectedServer_ >= serverCount_) {
        selectedServer_ = static_cast<uint8_t>(serverCount_ - 1);
    }
    serverScroll_ = 0;

    uint8_t launched = 0;
    for (uint8_t i = 0; i < kScanWorkerCount; ++i) {
        scanWorkerCtx_[i].app = this;
        scanWorkerCtx_[i].index = i;
        scanWorkers_[i] = nullptr;
        const BaseType_t result = xTaskCreatePinnedToCore(scanWorkerEntry, "rbScan", 4096, &scanWorkerCtx_[i], 1, &scanWorkers_[i], i & 1U);
        if (result == pdPASS) {
            ++launched;
        }
    }

    portENTER_CRITICAL(&scanMux_);
    scanActiveWorkers_ = launched;
    if (launched == 0) {
        scanRunning_ = false;
        scanStopRequested_ = true;
    }
    portEXIT_CRITICAL(&scanMux_);

    if (launched == 0) {
        mode_ = UiMode::Error;
        setStatus("Impossible de lancer le scan");
        queueChromeDirty();
        queueViewportFullDirty();
        return;
    }

    setStatus("Scan local async en cours");
    queueChromeDirty();
    queueViewportFullDirty();
}

void RemoteBrowserApp::stopScan() {
    stopScanWorkers();
    saveKnownHosts();
    if (scanCompletedHosts_ >= 254) scanProgress_ = 100;
    if (mode_ == UiMode::Scanning) {
        mode_ = UiMode::Disconnected;
        setStatus(serverCount_ > 0 ? "Scan termine" : "Aucun serveur trouve");
    }
    queueChromeDirty();
    queueViewportFullDirty();
}

void RemoteBrowserApp::scanStep() {
    uint16_t completed = 0;
    uint8_t activeWorkers = 0;
    uint8_t serverCount = 0;
    bool running = false;
    char latestHost[32] = "";
    uint16_t latestPort = kDefaultPort;

    portENTER_CRITICAL(&scanMux_);
    completed = scanCompletedHosts_;
    activeWorkers = scanActiveWorkers_;
    running = scanRunning_;
    serverCount = serverCount_;
    if (serverCount > 0) {
        const ServerEntry& latest = servers_[serverCount - 1];
        copyTrim(latestHost, sizeof(latestHost), latest.host, sizeof(latestHost) - 1);
        latestPort = latest.port;
    }
    portEXIT_CRITICAL(&scanMux_);

    scanProgress_ = static_cast<uint16_t>((completed * 100U) / 254U);
    if (scanProgress_ > 100) scanProgress_ = 100;

    if (serverCount > 0 && latestHost[0]) {
        selectedServer_ = static_cast<uint8_t>(serverCount - 1);
        copyTrim(serverHost_, sizeof(serverHost_), latestHost, sizeof(serverHost_) - 1);
        serverPort_ = latestPort;
        ensureAddressText();
        setStatus("Serveur detecte");
    } else if (running) {
        setStatus("Scan local async en cours");
    }

    dirtySidePanel_ = true;
    dirtyStatus_ = true;
    queueViewportFullDirty();

    if (!running && activeWorkers == 0 && mode_ == UiMode::Scanning) {
        stopScan();
    }
}

void RemoteBrowserApp::requestKeyboard(KeyboardTarget target, const char* initial) {
    keyboardTarget_ = target;
    copyTrim(pendingKeyboardInitial_, sizeof(pendingKeyboardInitial_), initial ? initial : "", sizeof(pendingKeyboardInitial_) - 1);
    dirtyStatus_ = true;
}

bool RemoteBrowserApp::consumeKeyboardRequest(char* outInitial, size_t outLen) {
    if (!outInitial || outLen == 0) return false;
    outInitial[0] = '\0';
    if (keyboardTarget_ == KeyboardTarget::None) return false;
    copyTrim(outInitial, outLen, pendingKeyboardInitial_, outLen - 1);
    pendingKeyboardInitial_[0] = '\0';
    return true;
}

void RemoteBrowserApp::applyKeyboardValue(const char* value, bool accepted) {
    const KeyboardTarget target = keyboardTarget_;
    keyboardTarget_ = KeyboardTarget::None;
    if (!accepted) return;
    if (target == KeyboardTarget::ServerHost) {
        if (value && value[0]) {
            copyTrim(serverHost_, sizeof(serverHost_), value, sizeof(serverHost_) - 1);
            ensureAddressText();
            setStatus("Hote modifie");
            queueChromeDirty();
            dirtySidePanel_ = true;
        }
        return;
    }
    if (target == KeyboardTarget::ServerPort) {
        if (value && isDigits(value)) {
            const long v = strtol(value, nullptr, 10);
            if (v > 0 && v <= 65535L) {
                serverPort_ = static_cast<uint16_t>(v);
                ensureAddressText();
                setStatus("Port modifie");
                queueChromeDirty();
                dirtySidePanel_ = true;
            }
        }
        return;
    }
    if (target == KeyboardTarget::PageUrl) {
        if (value && value[0]) {
            copyTrim(pageUrl_, sizeof(pageUrl_), value, sizeof(pageUrl_) - 1);
            enqueueCommand(NetworkCommandType::Navigate, pageUrl_);
            queueChromeDirty();
        }
        return;
    }
    if (target == KeyboardTarget::TextInput) {
        if (value && value[0]) {
            enqueueCommand(NetworkCommandType::Text, value);
        }
        return;
    }
}

bool RemoteBrowserApp::openSession(const Rect& contentRect) {
    if (WiFi.status() != WL_CONNECTED) {
        rbLog("openSession aborted: wifi unavailable");
        mode_ = UiMode::Error;
        setStatus("WiFi indisponible");
        return false;
    }
    stopScanWorkers();
    closeSession();
    sessionId_[0] = '\0';
    ensureAddressText();
    mode_ = UiMode::Connecting;
    setStatus("Connexion au serveur");
    queueChromeDirty();
    queueViewportFullDirty();
    rbLog("openSession host=%s port=%u viewport=%dx%d", serverHost_, static_cast<unsigned>(serverPort_), contentRect.w, contentRect.h);

    char path[160] = "";
    snprintf(path, sizeof(path), "/rb/session/open?width=%d&height=%d", contentRect.w > 4 ? contentRect.w : 4, contentRect.h > 4 ? contentRect.h : 4);
    char body[512] = "";
    if (!httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, body, sizeof(body), 1600U)) {
        rbLog("openSession failed: server unreachable %s:%u", serverHost_, static_cast<unsigned>(serverPort_));
        mode_ = UiMode::Error;
        setStatus("Serveur injoignable");
        return false;
    }

    char session[40] = "";
    if (!parseKeyValue(body, "session", session, sizeof(session))) {
        rbLog("openSession failed: invalid response body='%s'", body);
        mode_ = UiMode::Error;
        setStatus("Session invalide");
        return false;
    }

    char pageUrl[192] = "";
    char title[44] = "";
    char widthStr[12] = "";
    char heightStr[12] = "";
    parseKeyValue(body, "url", pageUrl, sizeof(pageUrl));
    parseKeyValue(body, "title", title, sizeof(title));
    parseKeyValue(body, "width", widthStr, sizeof(widthStr));
    parseKeyValue(body, "height", heightStr, sizeof(heightStr));

    const uint16_t width = static_cast<uint16_t>(atoi(widthStr));
    const uint16_t height = static_cast<uint16_t>(atoi(heightStr));
    const uint16_t targetW = width ? width : static_cast<uint16_t>(contentRect.w);
    const uint16_t targetH = height ? height : static_cast<uint16_t>(contentRect.h);
    if (!ensureFrameBuffers(targetW, targetH)) {
        return false;
    }

    copyTrim(sessionId_, sizeof(sessionId_), session, sizeof(sessionId_) - 1);
    if (pageUrl[0]) copyTrim(pageUrl_, sizeof(pageUrl_), pageUrl, sizeof(pageUrl_) - 1);
    if (title[0]) copyTrim(pageTitle_, sizeof(pageTitle_), title, sizeof(pageTitle_) - 1);
    addServer(serverHost_, serverPort_);
    saveKnownHosts();
    mode_ = UiMode::Connected;
    textFocus_ = false;
    requestFullFrame_ = true;
    lastFrameSeq_ = 0;
    portENTER_CRITICAL(&networkMux_);
    networkContentW_ = targetW;
    networkContentH_ = targetH;
    portEXIT_CRITICAL(&networkMux_);
    ensureNetworkWorker();
    setStatus("Connecte");
    queueChromeDirty();
    queueViewportFullDirty();
    rbLog("openSession ok session=%s size=%ux%u url=%s title=%s", sessionId_, static_cast<unsigned>(targetW), static_cast<unsigned>(targetH), pageUrl_, pageTitle_);
    return true;
}

bool RemoteBrowserApp::resizeSession(const Rect& contentRect) {
    if (!sessionId_[0]) return false;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/resize?session=%s&width=%d&height=%d", sessionId_, contentRect.w, contentRect.h);
    char body[128] = "";
    if (!httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, body, sizeof(body), 1200U)) {
        rbLog("resizeSession failed session=%s target=%dx%d", sessionId_, contentRect.w, contentRect.h);
        return false;
    }
    if (!ensureFrameBuffers(static_cast<uint16_t>(contentRect.w), static_cast<uint16_t>(contentRect.h))) {
        rbLog("resizeSession buffer allocation failed target=%dx%d", contentRect.w, contentRect.h);
        return false;
    }
    portENTER_CRITICAL(&networkMux_);
    networkContentW_ = static_cast<uint16_t>(contentRect.w);
    networkContentH_ = static_cast<uint16_t>(contentRect.h);
    portEXIT_CRITICAL(&networkMux_);
    requestFullFrame_ = true;
    queueViewportFullDirty();
    rbLog("resizeSession ok session=%s size=%dx%d", sessionId_, contentRect.w, contentRect.h);
    return true;
}

bool RemoteBrowserApp::applyRegionPayload(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Stream& stream, uint32_t payloadLen) {
    portENTER_CRITICAL(&frameMux_);
    const bool frameReady = frameBuffer_ && w > 0 && h > 0 && x < frameW_ && y < frameH_ && x + w <= frameW_ && y + h <= frameH_;
    uint16_t* frame = frameBuffer_;
    const uint16_t frameWidth = frameW_;
    portEXIT_CRITICAL(&frameMux_);
    if (!frameReady) return false;

    uint32_t consumed = 0;
    for (uint16_t row = 0; row < h; ++row) {
        uint16_t rowBytes = 0;
        if (!readU16(stream, rowBytes)) return false;
        consumed += 2U;
        if (consumed > payloadLen) return false;

        uint16_t* dst = frame + static_cast<size_t>(y + row) * frameWidth + x;
        uint16_t written = 0;
        uint16_t used = 0;
        while (used < rowBytes) {
            uint8_t control = 0;
            if (!readExact(stream, &control, 1, 240U)) return false;
            ++used;
            ++consumed;
            if (used > rowBytes || consumed > payloadLen) return false;

            const uint16_t runLen = static_cast<uint16_t>((control & 0x7F) + 1U);
            if (written + runLen > w) return false;

            if (control & 0x80U) {
                if (used + 2U > rowBytes || consumed + 2U > payloadLen) return false;
                uint16_t color = 0;
                if (!readU16(stream, color)) return false;
                used += 2U;
                consumed += 2U;
                for (uint16_t i = 0; i < runLen; ++i) {
                    dst[written++] = color;
                }
            } else {
                const uint32_t bytesNeeded = static_cast<uint32_t>(runLen) * 2U;
                if (used + bytesNeeded > rowBytes || consumed + bytesNeeded > payloadLen) return false;
                for (uint16_t i = 0; i < runLen; ++i) {
                    uint16_t color = 0;
                    if (!readU16(stream, color)) return false;
                    used += 2U;
                    consumed += 2U;
                    dst[written++] = color;
                }
            }
        }

        if (used != rowBytes || written != w) return false;
    }

    if (consumed < payloadLen) {
        uint32_t remaining = payloadLen - consumed;
        uint8_t skip[16];
        while (remaining > 0) {
            const size_t chunk = remaining > sizeof(skip) ? sizeof(skip) : static_cast<size_t>(remaining);
            if (!readExact(stream, skip, chunk, 240U)) return false;
            remaining -= static_cast<uint32_t>(chunk);
        }
    } else if (consumed > payloadLen) {
        return false;
    }

    queueViewportDirty(LocalRect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)});
    return true;
}

bool RemoteBrowserApp::pollFrame(const Rect& contentRect) {
    if (!sessionId_[0]) return false;
    if (contentRect.w <= 0 || contentRect.h <= 0) return false;
    if (!ensureFrameBuffers(static_cast<uint16_t>(contentRect.w), static_cast<uint16_t>(contentRect.h))) return false;

    static const char* headerKeys[] = {
        "X-RBrowser-Seq",        "X-RBrowser-Width",      "X-RBrowser-Height",      "X-RBrowser-Regions",
        "X-RBrowser-Focus-Text", "X-RBrowser-Full",       "X-RBrowser-Url",         "X-RBrowser-Title"};

    char path[224] = "";
    snprintf(path, sizeof(path), "/rb/session/frame?session=%s&seq=%lu&full=%u", sessionId_, static_cast<unsigned long>(lastFrameSeq_), requestFullFrame_ ? 1U : 0U);
    char url[256] = "";
    snprintf(url, sizeof(url), "http://%s:%u%s", serverHost_, static_cast<unsigned>(serverPort_), path);

    HTTPClient http;
    http.setTimeout(900U);
    http.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));
    if (!http.begin(url)) return false;

    const int code = http.GET();
    if (code == 204) {
        http.end();
        requestFullFrame_ = false;
        return true;
    }
    if (code != 200) {
        rbLog("pollFrame http error session=%s code=%d", sessionId_, code);
        http.end();
        setStatus("Flux interrompu");
        mode_ = UiMode::Error;
        return false;
    }

    const uint32_t seq = static_cast<uint32_t>(strtoul(http.header("X-RBrowser-Seq").c_str(), nullptr, 10));
    const uint16_t width = static_cast<uint16_t>(atoi(http.header("X-RBrowser-Width").c_str()));
    const uint16_t height = static_cast<uint16_t>(atoi(http.header("X-RBrowser-Height").c_str()));
    const uint16_t regions = static_cast<uint16_t>(atoi(http.header("X-RBrowser-Regions").c_str()));
    const bool focusText = atoi(http.header("X-RBrowser-Focus-Text").c_str()) != 0;
    const bool fullFrame = atoi(http.header("X-RBrowser-Full").c_str()) != 0;
    const String urlHeader = http.header("X-RBrowser-Url");
    const String titleHeader = http.header("X-RBrowser-Title");
    const int32_t contentLen = http.getSize();

    if (width != 0 && height != 0 && (width != frameW_ || height != frameH_)) {
        if (!ensureFrameBuffers(width, height)) {
            Stream* abortStream = http.getStreamPtr();
            if (abortStream && contentLen > 0) {
                drainStream(*abortStream, 220U, contentLen);
            }
            http.end();
            return false;
        }
    }

    if (urlHeader.length() > 0) copyTrim(pageUrl_, sizeof(pageUrl_), urlHeader.c_str(), sizeof(pageUrl_) - 1);
    if (titleHeader.length() > 0) copyTrim(pageTitle_, sizeof(pageTitle_), titleHeader.c_str(), sizeof(pageTitle_) - 1);
    textFocus_ = focusText;

    if (fullFrame) queueViewportFullDirty();

    Stream* streamPtr = http.getStreamPtr();
    if (!streamPtr) {
        rbLog("pollFrame missing stream session=%s", sessionId_);
        http.end();
        setStatus("Flux absent");
        mode_ = UiMode::Error;
        return false;
    }

    Stream& stream = *streamPtr;
    bool ok = true;
    int32_t remainingBody = contentLen >= 0 ? contentLen : -1;
    for (uint16_t i = 0; i < regions && ok; ++i) {
        uint16_t rx = 0;
        uint16_t ry = 0;
        uint16_t rw = 0;
        uint16_t rh = 0;
        uint32_t payloadLen = 0;
        ok = readU16(stream, rx) && readU16(stream, ry) && readU16(stream, rw) && readU16(stream, rh) && readU32(stream, payloadLen);
        if (!ok) break;
        if (remainingBody > 0) {
            const int32_t chunkBytes = static_cast<int32_t>(payloadLen) + 12;
            if (chunkBytes > remainingBody) {
                ok = false;
                break;
            }
            remainingBody -= chunkBytes;
        }
        ok = applyRegionPayload(rx, ry, rw, rh, stream, payloadLen);
    }

    if (remainingBody > 0) {
        ok = drainStream(stream, 220U, remainingBody) && ok;
    } else if (remainingBody < 0) {
        ok = drainStream(stream, 120U) && ok;
    } else {
        ok = drainStream(stream, 80U, 0) && ok;
    }

    http.end();
    if (!ok) {
        rbLog("pollFrame invalid patch session=%s seq=%lu regions=%u len=%ld", sessionId_, static_cast<unsigned long>(seq), static_cast<unsigned>(regions),
              static_cast<long>(contentLen));
        requestFullFrame_ = true;
        setStatus("Patch invalide");
        mode_ = UiMode::Error;
        return false;
    }

    requestFullFrame_ = false;
    if (seq > lastFrameSeq_) {
        if (seq <= 2 || fullFrame) {
            rbLog("pollFrame ok session=%s seq=%lu regions=%u full=%u focus=%u len=%ld", sessionId_, static_cast<unsigned long>(seq),
                  static_cast<unsigned>(regions), fullFrame ? 1U : 0U, focusText ? 1U : 0U, static_cast<long>(contentLen));
        }
        lastFrameSeq_ = seq;
    }
    dirtyStatus_ = true;
    return true;
}

bool RemoteBrowserApp::sendPointer(int16_t x, int16_t y) {
    if (!sessionId_[0]) return false;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/pointer?session=%s&x=%d&y=%d", sessionId_, x, y);
    char out[32] = "";
    return httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, out, sizeof(out), 120U);
}

bool RemoteBrowserApp::sendClick(int16_t x, int16_t y) {
    if (!sessionId_[0]) return false;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/click?session=%s&x=%d&y=%d", sessionId_, x, y);
    char out[32] = "";
    const bool ok = httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, out, sizeof(out), 240U);
    if (ok) requestFullFrame_ = false;
    return ok;
}

bool RemoteBrowserApp::sendScroll(int16_t deltaY) {
    if (!sessionId_[0]) return false;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/scroll?session=%s&dy=%d", sessionId_, deltaY);
    char out[32] = "";
    return httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, out, sizeof(out), 240U);
}

bool RemoteBrowserApp::sendNavigate(const char* url) {
    if (!sessionId_[0] || !url || !url[0]) return false;
    char path[160] = "";
    snprintf(path, sizeof(path), "/rb/session/navigate?session=%s", sessionId_);
    char out[128] = "";
    const bool ok = httpCollectText(serverHost_, serverPort_, path, "POST", url, out, sizeof(out), 2000U);
    if (ok) {
        setStatus("Navigation distante");
        requestFullFrame_ = true;
        queueChromeDirty();
    }
    return ok;
}

bool RemoteBrowserApp::sendAction(const char* action) {
    if (!sessionId_[0] || !action || !action[0]) return false;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/action?session=%s&kind=%s", sessionId_, action);
    char out[64] = "";
    const bool ok = httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, out, sizeof(out), 1200U);
    if (ok) {
        requestFullFrame_ = true;
        setStatus("Action distante");
        queueChromeDirty();
    }
    return ok;
}

bool RemoteBrowserApp::sendText(const char* text) {
    if (!sessionId_[0] || !text) return false;
    char path[160] = "";
    snprintf(path, sizeof(path), "/rb/session/text?session=%s", sessionId_);
    char out[64] = "";
    const bool ok = httpCollectText(serverHost_, serverPort_, path, "POST", text, out, sizeof(out), 1800U);
    if (ok) {
        setStatus("Texte envoye");
        requestFullFrame_ = true;
    }
    return ok;
}

bool RemoteBrowserApp::closeSession() {
    if (!sessionId_[0]) return true;
    char path[192] = "";
    snprintf(path, sizeof(path), "/rb/session/close?session=%s", sessionId_);
    char out[32] = "";
    const bool ok = httpCollectText(serverHost_, serverPort_, path, "POST", nullptr, out, sizeof(out), 1200U);
    rbLog("closeSession %s session=%s", ok ? "ok" : "failed", sessionId_);
    return ok;
}

void RemoteBrowserApp::tick(uint32_t nowMs, const Rect& body, int16_t cursorX, int16_t cursorY, bool focused) {
    const RenderLayout layout = buildLayout(body);
    const Rect& contentRect = layout.contentRect;

    if (scanRunning_) {
        scanStep();
    }

    if (mode_ == UiMode::Connected && contentRect.w > 0 && contentRect.h > 0) {
        ensureNetworkWorker();
        portENTER_CRITICAL(&networkMux_);
        networkContentW_ = static_cast<uint16_t>(contentRect.w);
        networkContentH_ = static_cast<uint16_t>(contentRect.h);
        portEXIT_CRITICAL(&networkMux_);

        portENTER_CRITICAL(&frameMux_);
        const bool sizeMismatch = frameW_ != static_cast<uint16_t>(contentRect.w) || frameH_ != static_cast<uint16_t>(contentRect.h);
        portEXIT_CRITICAL(&frameMux_);
        if (sizeMismatch) {
            enqueueCommand(NetworkCommandType::Resize);
        }

        const bool inside = pointInRect(cursorX, cursorY, contentRect);
        if (focused && inside) {
            const int16_t relX = clamp16(static_cast<int16_t>(cursorX - contentRect.x), 0, static_cast<int16_t>(contentRect.w - 1));
            const int16_t relY = clamp16(static_cast<int16_t>(cursorY - contentRect.y), 0, static_cast<int16_t>(contentRect.h - 1));
            if ((!pointerInside_ || relX != lastPointerX_ || relY != lastPointerY_) && nowMs - lastPointerMs_ >= 40U) {
                queuePointer(relX, relY);
                lastPointerX_ = relX;
                lastPointerY_ = relY;
                pointerInside_ = true;
                lastPointerMs_ = nowMs;
            }
        } else {
            pointerInside_ = false;
        }

        if (nowMs - lastMetaMs_ >= 800U) {
            dirtyStatus_ = true;
            dirtySidePanel_ = true;
            lastMetaMs_ = nowMs;
        }
    }
}

bool RemoteBrowserApp::handleClick(int16_t x, int16_t y, const Rect& body) {
    const RenderLayout layout = buildLayout(body);
    const ConnectModalLayout modal = buildConnectModal(layout.contentRect);

    if (pointInRect(x, y, layout.urlBar)) {
        if (mode_ == UiMode::Connected) requestKeyboard(KeyboardTarget::PageUrl, pageUrl_);
        else requestKeyboard(KeyboardTarget::ServerHost, serverHost_);
        return true;
    }

    if (pointInRect(x, y, layout.favBtn)) {
        if (mode_ == UiMode::Connected) {
            requestKeyboard(KeyboardTarget::TextInput, "");
        } else {
            char portBuf[12] = "";
            snprintf(portBuf, sizeof(portBuf), "%u", static_cast<unsigned>(serverPort_));
            requestKeyboard(KeyboardTarget::ServerPort, portBuf);
        }
        return true;
    }

    if (mode_ == UiMode::Connected) {
        if (pointInRect(x, y, layout.backBtn)) return enqueueCommand(NetworkCommandType::Action, "back");
        if (pointInRect(x, y, layout.fwdBtn)) return enqueueCommand(NetworkCommandType::Action, "forward");
        if (pointInRect(x, y, layout.prevBtn)) return enqueueCommand(NetworkCommandType::Action, "reload");
        if (pointInRect(x, y, layout.nextBtn)) return enqueueCommand(NetworkCommandType::Action, "home");
        if (pointInRect(x, y, layout.openBtn)) return enqueueCommand(NetworkCommandType::Navigate, pageUrl_);
        if (pointInRect(x, y, layout.upBtn)) return enqueueCommand(NetworkCommandType::Scroll, nullptr, 0, 0, -120);
        if (pointInRect(x, y, layout.downBtn)) return enqueueCommand(NetworkCommandType::Scroll, nullptr, 0, 0, 120);
        if (pointInRect(x, y, layout.contentRect)) {
            const int16_t relX = clamp16(static_cast<int16_t>(x - layout.contentRect.x), 0, static_cast<int16_t>(layout.contentRect.w - 1));
            const int16_t relY = clamp16(static_cast<int16_t>(y - layout.contentRect.y), 0, static_cast<int16_t>(layout.contentRect.h - 1));
            const bool ok = enqueueCommand(NetworkCommandType::Click, nullptr, relX, relY);
            if (ok) {
                if (textFocus_) requestKeyboard(KeyboardTarget::TextInput, "");
                dirtyStatus_ = true;
            }
            return ok;
        }
        return false;
    }

    if (pointInRect(x, y, layout.backBtn) || pointInRect(x, y, modal.scanBtn)) {
        startScan();
        return true;
    }
    if (pointInRect(x, y, layout.fwdBtn)) {
        stopScan();
        return true;
    }
    if (pointInRect(x, y, layout.prevBtn) || pointInRect(x, y, modal.hostBtn)) {
        requestKeyboard(KeyboardTarget::ServerHost, serverHost_);
        return true;
    }
    if (pointInRect(x, y, layout.nextBtn) || pointInRect(x, y, modal.portBtn)) {
        char portBuf[12] = "";
        snprintf(portBuf, sizeof(portBuf), "%u", static_cast<unsigned>(serverPort_));
        requestKeyboard(KeyboardTarget::ServerPort, portBuf);
        return true;
    }
    if (pointInRect(x, y, layout.openBtn) || pointInRect(x, y, modal.linkBtn)) {
        return openSession(layout.contentRect);
    }
    if (pointInRect(x, y, layout.upBtn)) {
        if (serverScroll_ > 0) {
            --serverScroll_;
            dirtySidePanel_ = true;
            queueViewportFullDirty();
        }
        return true;
    }
    if (pointInRect(x, y, layout.downBtn)) {
        if (serverScroll_ + 1 < serverCount_) {
            ++serverScroll_;
            dirtySidePanel_ = true;
            queueViewportFullDirty();
        }
        return true;
    }

    for (uint8_t i = 0; i < serverCount_; ++i) {
        const int16_t chipY = static_cast<int16_t>(layout.sidePanel.y + 12 + i * 11 - serverScroll_ * 11);
        const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), chipY, static_cast<int16_t>(layout.sidePanel.w - 4), 9};
        if (pointInRect(x, y, chip)) {
            selectedServer_ = i;
            copyTrim(serverHost_, sizeof(serverHost_), servers_[i].host, sizeof(serverHost_) - 1);
            serverPort_ = servers_[i].port;
            ensureAddressText();
            setStatus("Serveur selectionne");
            dirtySidePanel_ = true;
            queueChromeDirty();
            queueViewportFullDirty();
            return true;
        }
    }
    return false;
}

uint8_t RemoteBrowserApp::consumeDirtyRegions(Rect* out, uint8_t maxItems, const Rect& body) {
    if (!out || maxItems == 0) return 0;
    const RenderLayout layout = buildLayout(body);
    uint8_t count = 0;
    if (dirtyChrome_ && count < maxItems) out[count++] = layout.headerSection;
    if (dirtySidePanel_ && count < maxItems) out[count++] = layout.sidePanel;
    if (dirtyStatus_ && count < maxItems) out[count++] = layout.statusBar;
    if (dirtyViewportFull_ && count < maxItems) out[count++] = layout.viewport;
    if (!dirtyViewportFull_) {
        for (uint8_t i = 0; i < patchDirtyCount_ && count < maxItems; ++i) {
            const LocalRect& r = patchDirty_[i];
            Rect abs{static_cast<int16_t>(layout.contentRect.x + r.x), static_cast<int16_t>(layout.contentRect.y + r.y), r.w, r.h};
            Rect clipped = rectIntersection(abs, layout.contentRect);
            if (clipped.w > 0 && clipped.h > 0) out[count++] = clipped;
        }
    }
    dirtyChrome_ = false;
    dirtySidePanel_ = false;
    dirtyStatus_ = false;
    dirtyViewportFull_ = false;
    patchDirtyCount_ = 0;
    return count;
}

void RemoteBrowserApp::renderBufferedViewport(katux::graphics::Renderer& renderer, const Rect& target, const Rect* clip) {
    Rect paint = clip ? rectIntersection(target, *clip) : target;
    paint = rectIntersection(paint, target);
    if (paint.w <= 0 || paint.h <= 0 || paint.w > 240) return;

    uint16_t rowBuffer[240];
    const int16_t startX = static_cast<int16_t>(paint.x - target.x);
    const int16_t startY = static_cast<int16_t>(paint.y - target.y);
    for (int16_t row = 0; row < paint.h; ++row) {
        portENTER_CRITICAL(&frameMux_);
        if (!frameBuffer_ || frameW_ == 0 || frameH_ == 0 || startX < 0 || startY < 0 || startX + paint.w > frameW_ || startY + row >= frameH_) {
            portEXIT_CRITICAL(&frameMux_);
            return;
        }
        const uint16_t* src = frameBuffer_ + static_cast<size_t>(startY + row) * frameW_ + startX;
        memcpy(rowBuffer, src, static_cast<size_t>(paint.w) * sizeof(uint16_t));
        portEXIT_CRITICAL(&frameMux_);
        renderer.pushPixels(Rect{paint.x, static_cast<int16_t>(paint.y + row), paint.w, 1}, rowBuffer);
    }
}

void RemoteBrowserApp::render(katux::graphics::Renderer& renderer, const Rect& body, uint32_t nowMs, const Rect* clip) {
    (void)nowMs;
    const RenderLayout layout = buildLayout(body);
    const bool renderHeader = !clip || rectsIntersect(layout.headerSection, *clip);
    const bool renderSide = !clip || rectsIntersect(layout.sidePanel, *clip);
    const bool renderViewport = !clip || rectsIntersect(layout.viewport, *clip);
    const bool renderStatus = !clip || rectsIntersect(layout.statusBar, *clip);

    if (!clip) renderer.fillRect(body, 0xD69A);

    if (renderHeader) {
        const bool connected = mode_ == UiMode::Connected;
        const bool busy = isBusy();
        fillRectClipped(renderer, layout.urlBar, 0xBDF7, clip);
        drawFrameClipped(renderer, layout.urlBar, 0x7BEF, clip);
        fillRectClipped(renderer, layout.favBtn, connected ? 0xFFE0 : 0xD6FF, clip);
        fillRectClipped(renderer, layout.backBtn, 0x7BEF, clip);
        fillRectClipped(renderer, layout.fwdBtn, 0x7BEF, clip);
        fillRectClipped(renderer, layout.prevBtn, 0x867F, clip);
        fillRectClipped(renderer, layout.nextBtn, 0xFD20, clip);
        fillRectClipped(renderer, layout.openBtn, busy ? 0xF800 : 0x39E7, clip);
        fillRectClipped(renderer, layout.upBtn, 0x7BEF, clip);
        fillRectClipped(renderer, layout.downBtn, 0x7BEF, clip);

        const char* backLabel = connected ? "<" : "Scan";
        const char* fwdLabel = connected ? ">" : "Stop";
        const char* prevLabel = connected ? "R" : "Host";
        const char* nextLabel = connected ? "H" : "Port";
        const char* openLabel = connected ? "Go" : "Link";
        const char* favLabel = connected ? "KB" : "#";

        drawTextClipped(renderer, static_cast<int16_t>(layout.backBtn.x + (connected ? 5 : 1)), static_cast<int16_t>(layout.backBtn.y + 1), backLabel, 0x0000,
                        0x7BEF, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.fwdBtn.x + (connected ? 5 : 1)), static_cast<int16_t>(layout.fwdBtn.y + 1), fwdLabel, 0x0000,
                        0x7BEF, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.prevBtn.x + (connected ? 6 : 1)), static_cast<int16_t>(layout.prevBtn.y + 1), prevLabel, 0x0000,
                        0x867F, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.nextBtn.x + (connected ? 6 : 1)), static_cast<int16_t>(layout.nextBtn.y + 1), nextLabel, 0x0000,
                        0xFD20, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.openBtn.x + 3), static_cast<int16_t>(layout.openBtn.y + 1), openLabel, 0x0000,
                        busy ? 0xF800 : 0x39E7, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.upBtn.x + 4), static_cast<int16_t>(layout.upBtn.y + 1), "^", 0x0000, 0x7BEF, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.downBtn.x + 4), static_cast<int16_t>(layout.downBtn.y + 1), "v", 0x0000, 0x7BEF, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.favBtn.x + 1), static_cast<int16_t>(layout.favBtn.y + 1), favLabel, 0x0000,
                        connected ? 0xFFE0 : 0xD6FF, clip);

        fillRectClipped(renderer, layout.schemeBadge, connected ? 0x07E0 : 0xC618, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.schemeBadge.x + 2), static_cast<int16_t>(layout.schemeBadge.y + 1), connected ? "R" : "L", 0x0000,
                        connected ? 0x07E0 : 0xC618, clip);

        char line[96] = "";
        const char* display = connected ? pageUrl_ : address_;
        const int16_t textX = static_cast<int16_t>(layout.urlBar.x + 13);
        const int16_t textW = static_cast<int16_t>(layout.urlBar.w - 15);
        size_t maxChars = static_cast<size_t>(textW / 6);
        if (maxChars < 4) maxChars = 4;
        if (maxChars >= sizeof(line)) maxChars = sizeof(line) - 1;
        if (strlen(display) > maxChars) {
            copyTrim(line, sizeof(line), display, maxChars > 3 ? maxChars - 3 : maxChars);
            if (maxChars > 3) strlcat(line, "...", sizeof(line));
        } else {
            copyTrim(line, sizeof(line), display, maxChars);
        }
        drawTextClipped(renderer, textX, static_cast<int16_t>(layout.urlBar.y + 2), line, 0x0000, 0xBDF7, clip);
    }

    if (renderSide) {
        fillRectClipped(renderer, layout.sidePanel, 0xE75D, clip);
        drawFrameClipped(renderer, layout.sidePanel, 0xA534, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 2), "Hosts", 0x2104, 0xE75D, clip);
        for (uint8_t i = 0; i < serverCount_; ++i) {
            const int16_t chipY = static_cast<int16_t>(layout.sidePanel.y + 12 + i * 11 - serverScroll_ * 11);
            const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), chipY, static_cast<int16_t>(layout.sidePanel.w - 4), 9};
            if (!rectsIntersect(chip, layout.sidePanel)) continue;
            const uint16_t bg = (i == selectedServer_) ? 0xFFE0 : 0xD6FF;
            fillRectClipped(renderer, chip, bg, clip);
            char chipText[20] = "";
            copyTrim(chipText, sizeof(chipText), servers_[i].host, 14);
            drawTextClipped(renderer, static_cast<int16_t>(chip.x + 1), static_cast<int16_t>(chip.y + 1), chipText, 0x0000, bg, clip);
        }
        drawTextClipped(renderer, static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + layout.sidePanel.h - 20),
                        mode_ == UiMode::Connected ? "Remote ON" : (scanRunning_ ? "Scanning" : "Idle"), 0x2104, 0xE75D, clip);
        char progress[20] = "";
        snprintf(progress, sizeof(progress), "%u%%", static_cast<unsigned>(scanProgress_));
        drawTextClipped(renderer, static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + layout.sidePanel.h - 10), progress, 0x001F,
                        0xE75D, clip);
    }

    if (renderViewport) {
        fillRectClipped(renderer, layout.viewport, 0xFFFF, clip);
        drawFrameClipped(renderer, layout.viewport, 0x7BEF, clip);
        if (mode_ == UiMode::Connected && frameBuffer_) {
            renderBufferedViewport(renderer, layout.contentRect, clip);
        } else {
            const ConnectModalLayout modal = buildConnectModal(layout.contentRect);
            fillRectClipped(renderer, modal.modal, 0xF7DE, clip);
            drawFrameClipped(renderer, modal.modal, 0x7BEF, clip);
            drawTextClipped(renderer, modal.titleRect.x, modal.titleRect.y, "Connexion R-Browser", 0x0000, 0xF7DE, clip);
            drawTextClipped(renderer, modal.addressRect.x, modal.addressRect.y, address_, 0x001F, 0xF7DE, clip);

            fillRectClipped(renderer, modal.scanBtn, 0xD6FF, clip);
            fillRectClipped(renderer, modal.hostBtn, 0xFFF4, clip);
            fillRectClipped(renderer, modal.portBtn, 0xFFF4, clip);
            fillRectClipped(renderer, modal.linkBtn, 0xB7FF, clip);
            drawTextClipped(renderer, static_cast<int16_t>(modal.scanBtn.x + 4), static_cast<int16_t>(modal.scanBtn.y + 1), "Scanner 192.168.*", 0x0000, 0xD6FF, clip);
            drawTextClipped(renderer, static_cast<int16_t>(modal.hostBtn.x + 4), static_cast<int16_t>(modal.hostBtn.y + 1), "Editer l'hote", 0x0000, 0xFFF4, clip);
            drawTextClipped(renderer, static_cast<int16_t>(modal.portBtn.x + 4), static_cast<int16_t>(modal.portBtn.y + 1), "Editer le port", 0x0000, 0xFFF4, clip);
            drawTextClipped(renderer, static_cast<int16_t>(modal.linkBtn.x + 4), static_cast<int16_t>(modal.linkBtn.y + 1), "Connecter", 0x0000, 0xB7FF, clip);
        }
    }

    if (renderStatus) {
        fillRectClipped(renderer, layout.statusBar, 0xC618, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.statusBar.x + 2), static_cast<int16_t>(layout.statusBar.y + 2), status_, 0x0000, 0xC618, clip);
        char right[24] = "";
        if (mode_ == UiMode::Connected) {
            snprintf(right, sizeof(right), "%lu %s", static_cast<unsigned long>(lastFrameSeq_), textFocus_ ? "TXT" : "PTR");
        } else if (scanRunning_) {
            snprintf(right, sizeof(right), "%u%%", static_cast<unsigned>(scanProgress_));
        } else {
            snprintf(right, sizeof(right), "%u srv", static_cast<unsigned>(serverCount_));
        }
        const int16_t len = static_cast<int16_t>(strlen(right));
        drawTextClipped(renderer, static_cast<int16_t>(layout.statusBar.x + layout.statusBar.w - 4 - len * 6), static_cast<int16_t>(layout.statusBar.y + 2), right,
                        0x0000, 0xC618, clip);
    }
}

bool RemoteBrowserApp::isBusy() const {
    return mode_ == UiMode::Connecting || mode_ == UiMode::Scanning;
}

bool RemoteBrowserApp::isConnected() const {
    return mode_ == UiMode::Connected;
}

bool RemoteBrowserApp::needsFrameTick() const {
    return mode_ == UiMode::Connected || mode_ == UiMode::Scanning || mode_ == UiMode::Connecting;
}

const char* RemoteBrowserApp::status() const {
    return status_;
}

const char* RemoteBrowserApp::pageUrl() const {
    return pageUrl_;
}

const char* RemoteBrowserApp::serverAddress() const {
    return address_;
}

}