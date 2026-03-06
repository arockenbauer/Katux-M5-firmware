#include "browser_app.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <lgfx/v1/misc/DataWrapper.hpp>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace katux::graphics::browser {

namespace {

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

static void toLowerInPlace(char* s) {
    if (!s) return;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] + ('a' - 'A'));
    }
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

static bool isHttpUrl(const char* s) {
    return startsWith(s, "http://") || startsWith(s, "https://");
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

static bool drawEncodedWithWrapper(ImageFormat fmt, lgfx::v1::DataWrapper& wrapper, const Rect& rect) {
    if (fmt == ImageFormat::Jpeg) {
        return StickCP2.Display.drawJpg(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
    }
    if (fmt == ImageFormat::Png) {
        return StickCP2.Display.drawPng(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
    }
    if (fmt == ImageFormat::Bmp) {
        return StickCP2.Display.drawBmp(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
    }
    if (fmt == ImageFormat::Qoi) {
        return StickCP2.Display.drawQoi(&wrapper, rect.x, rect.y, rect.w, rect.h, 0, 0, 1.0f);
    }
    return false;
}

static bool hasHtmlExt(const char* s) {
    if (!s) return false;
    const char* q = strchr(s, '?');
    size_t n = q ? static_cast<size_t>(q - s) : strlen(s);
    if (n < 4) return false;
    const char* dot = s;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '.') dot = s + i;
    }
    if (!dot || *dot != '.') return false;
    char ext[8] = "";
    size_t k = 0;
    for (const char* p = dot + 1; p < s + n && k + 1 < sizeof(ext); ++p) {
        ext[k++] = *p;
    }
    ext[k] = '\0';
    toLowerInPlace(ext);
    return strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0;
}

struct BrowserRenderLayout {
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
    Rect scrollUp{0, 0, 0, 0};
    Rect scrollDown{0, 0, 0, 0};
    int16_t statusH = 0;
    int16_t contentTop = 0;
    int16_t contentBottom = 0;
    int16_t visibleH = 0;
};

static BrowserRenderLayout buildBrowserRenderLayout(const Rect& body, bool statusVisible) {
    BrowserRenderLayout layout{};
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
    layout.statusH = statusVisible ? 12 : 0;
    layout.statusBar = Rect{layout.viewport.x, static_cast<int16_t>(layout.viewport.y + layout.viewport.h - layout.statusH), layout.viewport.w,
                            layout.statusH};
    layout.contentTop = static_cast<int16_t>(layout.viewport.y + 2);
    layout.contentBottom = static_cast<int16_t>(layout.viewport.y + layout.viewport.h - layout.statusH);
    layout.visibleH = static_cast<int16_t>(layout.contentBottom - layout.contentTop);
    layout.contentRect = Rect{layout.viewport.x, layout.contentTop, layout.viewport.w, layout.visibleH};
    layout.schemeBadge = Rect{static_cast<int16_t>(layout.urlBar.x + 1), static_cast<int16_t>(layout.urlBar.y + 1), 10, 10};
    layout.scrollUp = Rect{static_cast<int16_t>(layout.viewport.x + layout.viewport.w - 10), static_cast<int16_t>(layout.viewport.y + 2), 8, 8};
    layout.scrollDown = Rect{static_cast<int16_t>(layout.viewport.x + layout.viewport.w - 10),
                             static_cast<int16_t>(layout.viewport.y + layout.viewport.h - layout.statusH - 10), 8, 8};
    return layout;
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

}  // namespace

class NetworkConn {
   public:
    WiFiClient plain{};
    WiFiClientSecure secure{};
    Client* active = nullptr;
};

static NetworkConn& connSingleton() {
    static NetworkConn c;
    return c;
}

bool NetworkManager::parseUrl(const char* url) {
    if (!url || !url[0]) return false;
    secure_ = false;
    const char* hostStart = nullptr;
    if (startsWith(url, "https://")) {
        secure_ = true;
        hostStart = url + 8;
    } else if (startsWith(url, "http://")) {
        hostStart = url + 7;
    } else {
        return false;
    }

    const char* pathStart = strchr(hostStart, '/');
    if (!pathStart) {
        copyTrim(host_, sizeof(host_), hostStart, sizeof(host_) - 1);
        strlcpy(path_, "/", kBrowserUrlMaxLen);
    } else {
        const size_t hostLen = static_cast<size_t>(pathStart - hostStart);
        if (hostLen == 0 || hostLen >= sizeof(host_)) return false;
        memcpy(host_, hostStart, hostLen);
        host_[hostLen] = '\0';
        copyTrim(path_, kBrowserUrlMaxLen, pathStart, kBrowserUrlMaxLen - 1);
    }
    return host_[0] != '\0';
}

NetworkManager::NetworkManager() {
    path_ = reinterpret_cast<char*>(malloc(kBrowserUrlMaxLen));
    redirectLocation_ = reinterpret_cast<char*>(malloc(kBrowserUrlMaxLen));
    if (path_) path_[0] = '/';
    if (redirectLocation_) redirectLocation_[0] = '\0';
}

NetworkManager::~NetworkManager() {
    free(path_);
    free(redirectLocation_);
}

void NetworkManager::reset() {
    NetworkConn& c = connSingleton();
    if (c.active) {
        c.active->stop();
    }
    c.active = nullptr;
    state_ = NetState::Idle;
    strlcpy(statusText_, "Idle", sizeof(statusText_));
    contentType_[0] = '\0';
    contentLength_ = -1;
    statusCode_ = 0;
    if (redirectLocation_) redirectLocation_[0] = '\0';
    headerStatusParsed_ = false;
    bytesReceived_ = 0;
    headerLine_[0] = '\0';
    headerLineLen_ = 0;
    host_[0] = '\0';
    if (path_) {
        path_[0] = '/';
        path_[1] = '\0';
    }
}

bool NetworkManager::open(const char* url) {
    reset();
    if (WiFi.status() != WL_CONNECTED) {
        state_ = NetState::Error;
        strlcpy(statusText_, "WiFi disconnected", sizeof(statusText_));
        return false;
    }
    if (!parseUrl(url)) {
        state_ = NetState::Error;
        strlcpy(statusText_, "Bad URL", sizeof(statusText_));
        return false;
    }
    state_ = NetState::Connecting;
    strlcpy(statusText_, "Connecting", sizeof(statusText_));
    return true;
}

bool NetworkManager::connectStep() {
    NetworkConn& c = connSingleton();
    c.active = secure_ ? static_cast<Client*>(&c.secure) : static_cast<Client*>(&c.plain);
    if (secure_) {
        c.secure.setInsecure();
    }
    c.active->setTimeout(120);
    const uint16_t port = secure_ ? 443 : 80;
    if (!c.active->connect(host_, port)) {
        state_ = NetState::Error;
        strlcpy(statusText_, "Connect failed", sizeof(statusText_));
        return false;
    }

    c.active->print("GET ");
    c.active->print(path_);
    c.active->print(" HTTP/1.1\r\nHost: ");
    c.active->print(host_);
    c.active->print("\r\nUser-Agent: KatuxBrowser/2.0\r\nConnection: close\r\nAccept: text/html,text/plain,*/*\r\n\r\n");

    state_ = NetState::Headers;
    strlcpy(statusText_, "Headers", sizeof(statusText_));
    return true;
}

bool NetworkManager::parseHeadersStep() {
    NetworkConn& c = connSingleton();
    if (!c.active) {
        state_ = NetState::Error;
        strlcpy(statusText_, "No socket", sizeof(statusText_));
        return false;
    }

    uint16_t guard = 0;
    while (c.active->available() && guard < 320) {
        ++guard;
        const char ch = static_cast<char>(c.active->read());
        if (ch == '\r') continue;
        if (ch == '\n') {
            headerLine_[headerLineLen_] = '\0';
            if (headerLineLen_ == 0) {
                const bool redirected = statusCode_ >= 300 && statusCode_ < 400 && redirectLocation_[0] != '\0';
                if (redirected) {
                    if (c.active) {
                        c.active->stop();
                        c.active = nullptr;
                    }
                    state_ = NetState::Done;
                    strlcpy(statusText_, "Redirect", sizeof(statusText_));
                } else {
                    state_ = NetState::StreamingBody;
                    strlcpy(statusText_, "Streaming", sizeof(statusText_));
                }
                return true;
            }

            if (!headerStatusParsed_) {
                headerStatusParsed_ = true;
                const char* sp = strchr(headerLine_, ' ');
                if (sp) {
                    while (*sp == ' ') ++sp;
                    statusCode_ = static_cast<int16_t>(atoi(sp));
                }
                if (statusCode_ >= 100) {
                    snprintf(statusText_, sizeof(statusText_), "HTTP %d", static_cast<int>(statusCode_));
                }
                headerLineLen_ = 0;
                continue;
            }

            char lower[256] = "";
            strlcpy(lower, headerLine_, sizeof(lower));
            toLowerInPlace(lower);
            if (startsWith(lower, "content-type:")) {
                const char* v = headerLine_ + 13;
                while (*v == ' ' || *v == '\t') ++v;
                copyTrim(contentType_, sizeof(contentType_), v, sizeof(contentType_) - 1);
                toLowerInPlace(contentType_);
            } else if (startsWith(lower, "content-length:")) {
                const char* v = headerLine_ + 15;
                while (*v == ' ' || *v == '\t') ++v;
                contentLength_ = atoi(v);
                if (contentLength_ < 0) contentLength_ = -1;
            } else if (startsWith(lower, "location:")) {
                const char* v = headerLine_ + 9;
                while (*v == ' ' || *v == '\t') ++v;
                copyTrim(redirectLocation_, kBrowserUrlMaxLen, v, kBrowserUrlMaxLen - 1);
            }
            headerLineLen_ = 0;
            continue;
        }
        if (headerLineLen_ + 1 < sizeof(headerLine_)) {
            headerLine_[headerLineLen_++] = ch;
        }
    }

    if (!c.active->connected() && !c.active->available()) {
        state_ = NetState::Error;
        strlcpy(statusText_, "Header timeout", sizeof(statusText_));
        return false;
    }
    return true;
}

bool NetworkManager::readBodyChunk(char* out, size_t outLen, size_t& outRead) {
    outRead = 0;
    if (!out || outLen == 0) return false;
    out[0] = '\0';

    if (state_ == NetState::Connecting) {
        if (!connectStep()) return false;
    }
    if (state_ == NetState::Headers) {
        if (!parseHeadersStep()) return false;
    }
    if (state_ != NetState::StreamingBody) {
        return false;
    }

    NetworkConn& c = connSingleton();
    if (!c.active) {
        state_ = NetState::Error;
        strlcpy(statusText_, "Socket lost", sizeof(statusText_));
        return false;
    }

    const size_t maxRead = outLen - 1;
    while (outRead < maxRead && c.active->available()) {
        const char ch = static_cast<char>(c.active->read());
        if (ch != '\0') {
            out[outRead++] = ch;
            ++bytesReceived_;
        }
    }
    out[outRead] = '\0';

    if (!c.active->connected() && !c.active->available()) {
        c.active->stop();
        c.active = nullptr;
        state_ = NetState::Done;
        strlcpy(statusText_, "Done", sizeof(statusText_));
    }

    return outRead > 0;
}

NetState NetworkManager::state() const {
    return state_;
}

const char* NetworkManager::statusText() const {
    return statusText_;
}

const char* NetworkManager::contentType() const {
    return contentType_;
}

uint32_t NetworkManager::bytesReceived() const {
    return bytesReceived_;
}

int32_t NetworkManager::contentLength() const {
    return contentLength_;
}

int16_t NetworkManager::statusCode() const {
    return statusCode_;
}

bool NetworkManager::isRedirect() const {
    return statusCode_ >= 300 && statusCode_ < 400 && redirectLocation_[0] != '\0';
}

const char* NetworkManager::redirectLocation() const {
    return redirectLocation_;
}

float NetworkManager::progress() const {
    if (contentLength_ <= 0) return 0.0f;
    float p = static_cast<float>(bytesReceived_) / static_cast<float>(contentLength_);
    if (p < 0.0f) return 0.0f;
    if (p > 1.0f) return 1.0f;
    return p;
}

bool NetworkManager::hasError() const {
    return state_ == NetState::Error;
}

void HtmlTokenizer::reset() {
    inTag_ = false;
    inQuote_ = false;
    quoteChar_ = 0;
    textLen_ = 0;
    tagLen_ = 0;
    textBuf_[0] = '\0';
    tagBuf_[0] = '\0';
}

bool HtmlTokenizer::flushText(Token& outToken) {
    if (textLen_ == 0) return false;
    outToken = {};
    outToken.type = TokenType::Text;
    textBuf_[textLen_] = '\0';
    copyTrim(outToken.text, sizeof(outToken.text), textBuf_, sizeof(outToken.text) - 1);
    textLen_ = 0;
    textBuf_[0] = '\0';
    return outToken.text[0] != '\0';
}

bool HtmlTokenizer::flushTag(Token& outToken) {
    if (tagLen_ == 0) return false;
    tagBuf_[tagLen_] = '\0';

    char work[160] = "";
    copyTrim(work, sizeof(work), tagBuf_, sizeof(work) - 1);

    while (work[0] == ' ' || work[0] == '\t') {
        memmove(work, work + 1, strlen(work));
    }
    size_t n = strlen(work);
    while (n > 0 && (work[n - 1] == ' ' || work[n - 1] == '\t')) {
        work[n - 1] = '\0';
        --n;
    }

    if (work[0] == '\0' || work[0] == '!') {
        tagLen_ = 0;
        return false;
    }

    outToken = {};
    bool closing = false;
    if (work[0] == '/') {
        closing = true;
        memmove(work, work + 1, strlen(work));
    }

    bool selfClosing = false;
    n = strlen(work);
    if (n > 0 && work[n - 1] == '/') {
        selfClosing = true;
        work[n - 1] = '\0';
    }

    char* sp = strchr(work, ' ');
    if (sp) {
        *sp = '\0';
        copyTrim(outToken.attrs, sizeof(outToken.attrs), sp + 1, sizeof(outToken.attrs) - 1);
    } else {
        outToken.attrs[0] = '\0';
    }
    copyTrim(outToken.name, sizeof(outToken.name), work, sizeof(outToken.name) - 1);
    toLowerInPlace(outToken.name);

    if (closing) outToken.type = TokenType::EndTag;
    else if (selfClosing) outToken.type = TokenType::SelfClosingTag;
    else outToken.type = TokenType::StartTag;

    tagLen_ = 0;
    tagBuf_[0] = '\0';
    return outToken.name[0] != '\0';
}

bool HtmlTokenizer::feedChar(char c, Token& outToken) {
    if (!inTag_) {
        if (c == '<') {
            inTag_ = true;
            if (flushText(outToken)) {
                return true;
            }
            return false;
        }
        if (textLen_ + 1 < sizeof(textBuf_)) {
            textBuf_[textLen_++] = c;
        }
        return false;
    }

    if ((c == '\'' || c == '"')) {
        if (inQuote_ && c == quoteChar_) {
            inQuote_ = false;
            quoteChar_ = 0;
        } else if (!inQuote_) {
            inQuote_ = true;
            quoteChar_ = c;
        }
    }

    if (c == '>' && !inQuote_) {
        inTag_ = false;
        return flushTag(outToken);
    }

    if (tagLen_ + 1 < sizeof(tagBuf_)) {
        tagBuf_[tagLen_++] = c;
    }
    return false;
}

bool HtmlTokenizer::flushPending(Token& outToken) {
    if (!inTag_) {
        return flushText(outToken);
    }
    return false;
}

CssStyle CssEngine::inherit(const CssStyle& parent) const {
    return parent;
}

uint16_t CssEngine::parseColor(const char* value, uint16_t fallback) const {
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

    char lower[16] = "";
    copyTrim(lower, sizeof(lower), value, sizeof(lower) - 1);
    toLowerInPlace(lower);
    if (strcmp(lower, "red") == 0) return 0xF800;
    if (strcmp(lower, "green") == 0) return 0x07E0;
    if (strcmp(lower, "blue") == 0) return 0x001F;
    if (strcmp(lower, "yellow") == 0) return 0xFFE0;
    if (strcmp(lower, "black") == 0) return 0x0000;
    if (strcmp(lower, "white") == 0) return 0xFFFF;
    if (strcmp(lower, "gray") == 0 || strcmp(lower, "grey") == 0) return 0x8410;
    return fallback;
}

void CssEngine::applyTagDefaults(const char* tag, CssStyle& style) const {
    if (!tag || !tag[0]) return;

    if (strcmp(tag, "a") == 0 || strcmp(tag, "span") == 0 || strcmp(tag, "strong") == 0 || strcmp(tag, "em") == 0 ||
        strcmp(tag, "code") == 0 || strcmp(tag, "b") == 0 || strcmp(tag, "i") == 0 || strcmp(tag, "u") == 0 || strcmp(tag, "small") == 0 ||
        strcmp(tag, "mark") == 0 || strcmp(tag, "abbr") == 0 || strcmp(tag, "cite") == 0 || strcmp(tag, "time") == 0 ||
        strcmp(tag, "label") == 0 || strcmp(tag, "sup") == 0 || strcmp(tag, "sub") == 0 || strcmp(tag, "input") == 0 ||
        strcmp(tag, "button") == 0 || strcmp(tag, "select") == 0 || strcmp(tag, "option") == 0 || strcmp(tag, "output") == 0) {
        style.display = DisplayMode::Inline;
    } else {
        style.display = DisplayMode::Block;
    }

    if (strcmp(tag, "a") == 0) style.fg = 0x001F;
    if (strcmp(tag, "strong") == 0 || strcmp(tag, "b") == 0 || strcmp(tag, "th") == 0) style.bold = true;
    if (strcmp(tag, "em") == 0 || strcmp(tag, "i") == 0 || strcmp(tag, "cite") == 0) style.italic = true;
    if (strcmp(tag, "small") == 0) style.fontSize = FontSize::Small;
    if (strcmp(tag, "mark") == 0) {
        style.bg = 0xFFE0;
        style.fg = 0x0000;
    }

    if (strcmp(tag, "h1") == 0) {
        style.bold = true;
        style.fontSize = FontSize::Large;
        style.marginV = 4;
        style.fg = 0x0000;
    } else if (strcmp(tag, "h2") == 0) {
        style.bold = true;
        style.fontSize = FontSize::Medium;
        style.marginV = 3;
        style.fg = 0x0000;
    } else if (strcmp(tag, "h3") == 0 || strcmp(tag, "h4") == 0 || strcmp(tag, "h5") == 0 || strcmp(tag, "h6") == 0) {
        style.bold = true;
        style.fontSize = FontSize::Small;
        style.marginV = 3;
        style.fg = 0x0000;
    }

    if (strcmp(tag, "pre") == 0 || strcmp(tag, "textarea") == 0) {
        style.pre = true;
        style.display = DisplayMode::Block;
        style.bg = 0xEF7D;
    }
    if (strcmp(tag, "code") == 0 || strcmp(tag, "kbd") == 0 || strcmp(tag, "samp") == 0) {
        style.pre = true;
        style.bg = 0xEF7D;
        style.display = DisplayMode::Inline;
    }

    if (strcmp(tag, "blockquote") == 0) {
        style.marginV = 4;
        style.bg = 0xF79E;
    }

    if (strcmp(tag, "nav") == 0 || strcmp(tag, "header") == 0 || strcmp(tag, "footer") == 0 || strcmp(tag, "section") == 0 ||
        strcmp(tag, "article") == 0 || strcmp(tag, "aside") == 0 || strcmp(tag, "main") == 0 || strcmp(tag, "figure") == 0 ||
        strcmp(tag, "figcaption") == 0 || strcmp(tag, "details") == 0 || strcmp(tag, "summary") == 0) {
        style.marginV = static_cast<uint8_t>(style.marginV < 3 ? 3 : style.marginV);
    }
}

void CssEngine::applyInline(const char* styleText, CssStyle& style) const {
    if (!styleText || !styleText[0]) return;

    char work[220] = "";
    copyTrim(work, sizeof(work), styleText, sizeof(work) - 1);

    char* p = work;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';' || *p == '\n' || *p == '\r') ++p;
        if (!*p) break;

        char* keyStart = p;
        while (*p && *p != ':' && *p != ';') ++p;
        if (*p != ':') {
            while (*p && *p != ';') ++p;
            continue;
        }

        *p = '\0';
        ++p;

        char* valStart = p;
        while (*p && *p != ';') ++p;
        if (*p == ';') {
            *p = '\0';
            ++p;
        }

        while (*keyStart == ' ' || *keyStart == '\t') ++keyStart;
        while (*valStart == ' ' || *valStart == '\t') ++valStart;

        size_t keyLen = strlen(keyStart);
        while (keyLen > 0 && (keyStart[keyLen - 1] == ' ' || keyStart[keyLen - 1] == '\t')) {
            keyStart[keyLen - 1] = '\0';
            --keyLen;
        }

        size_t valLen = strlen(valStart);
        while (valLen > 0 && (valStart[valLen - 1] == ' ' || valStart[valLen - 1] == '\t')) {
            valStart[valLen - 1] = '\0';
            --valLen;
        }

        toLowerInPlace(keyStart);
        toLowerInPlace(valStart);

        if (strcmp(keyStart, "color") == 0) {
            style.fg = parseColor(valStart, style.fg);
            continue;
        }

        if (strcmp(keyStart, "background") == 0 || strcmp(keyStart, "background-color") == 0) {
            style.bg = parseColor(valStart, style.bg);
            continue;
        }

        if (strcmp(keyStart, "font-size") == 0) {
            if (strstr(valStart, "small") || strstr(valStart, "10px") || strstr(valStart, "11px")) style.fontSize = FontSize::Small;
            else if (strstr(valStart, "large") || strstr(valStart, "18px") || strstr(valStart, "20px")) style.fontSize = FontSize::Large;
            else style.fontSize = FontSize::Medium;
            continue;
        }

        if (strcmp(keyStart, "text-align") == 0) {
            if (strcmp(valStart, "center") == 0) style.align = TextAlign::Center;
            else if (strcmp(valStart, "right") == 0) style.align = TextAlign::Right;
            else style.align = TextAlign::Left;
            continue;
        }

        if (strcmp(keyStart, "margin") == 0 || strcmp(keyStart, "margin-top") == 0 || strcmp(keyStart, "margin-bottom") == 0) {
            int mv = atoi(valStart);
            if (mv < 0) mv = 0;
            if (mv > 12) mv = 12;
            style.marginV = static_cast<uint8_t>(mv);
            continue;
        }

        if (strcmp(keyStart, "display") == 0) {
            if (strcmp(valStart, "inline") == 0 || strcmp(valStart, "inline-block") == 0) style.display = DisplayMode::Inline;
            else if (strcmp(valStart, "none") == 0) style.display = DisplayMode::Hidden;
            else style.display = DisplayMode::Block;
            continue;
        }

        if (strcmp(keyStart, "font-weight") == 0) {
            style.bold = strstr(valStart, "bold") || strstr(valStart, "600") || strstr(valStart, "700") || strstr(valStart, "800");
            continue;
        }

        if (strcmp(keyStart, "font-style") == 0) {
            style.italic = strstr(valStart, "italic") != nullptr;
            continue;
        }

        if (strcmp(keyStart, "white-space") == 0) {
            style.pre = strstr(valStart, "pre") != nullptr;
            continue;
        }

        if (strcmp(keyStart, "visibility") == 0) {
            if (strstr(valStart, "hidden")) style.display = DisplayMode::Hidden;
            continue;
        }

        if (strcmp(keyStart, "opacity") == 0) {
            if (strcmp(valStart, "0") == 0 || strcmp(valStart, "0.0") == 0) style.display = DisplayMode::Hidden;
            continue;
        }
    }
}

LayoutEngine::LayoutEngine() {
    blocks_ = static_cast<Block*>(malloc(kMaxBlocks * sizeof(Block)));
    if (blocks_) {
        // initialize memory
        memset(blocks_, 0, kMaxBlocks * sizeof(Block));
    }
    count_ = 0;
    cursorY_ = 2;
}

LayoutEngine::~LayoutEngine() {
    free(blocks_);
    blocks_ = nullptr;
}

void LayoutEngine::reset() {
    count_ = 0;
    cursorY_ = 2;
    // zero first block to avoid stale data optional
}

void LayoutEngine::pushLine(const char* line, const CssStyle& style, const char* href) {
    if (!line || !line[0] || count_ >= kMaxBlocks) return;
    Block& b = blocks_[count_++];
    b.kind = href && href[0] ? 1 : 0;
    b.style = style;
    const int16_t lineH = style.fontSize == FontSize::Large ? 12 : (style.fontSize == FontSize::Small ? 8 : 10);
    b.y = cursorY_;
    b.h = lineH;
    copyTrim(b.text, sizeof(b.text), line, sizeof(b.text) - 1);
    if (href && href[0]) {
        copyTrim(b.href, sizeof(b.href), href, sizeof(b.href) - 1);
    }
    cursorY_ = static_cast<int16_t>(cursorY_ + lineH + 2 + style.marginV);
}

bool LayoutEngine::appendText(const char* text, const CssStyle& style, const char* href, int16_t maxWidth) {
    if (!text || !text[0]) return true;
    if (count_ >= kMaxBlocks) return false;
    if (style.display == DisplayMode::Hidden) return true;

    int16_t usable = static_cast<int16_t>(maxWidth - 8);
    if (usable < 24) usable = 24;
    const int16_t glyph = style.fontSize == FontSize::Large ? 7 : 6;
    size_t maxChars = static_cast<size_t>(usable / glyph);
    if (maxChars < 4) maxChars = 4;
    if (maxChars > 40) maxChars = 40;

    char src[120] = "";
    copyTrim(src, sizeof(src), text, sizeof(src) - 1);

    if (style.pre) {
        const char* p = src;
        char line[96] = "";
        size_t l = 0;
        while (*p) {
            if (*p == '\n' || l >= maxChars) {
                line[l] = '\0';
                pushLine(line, style, href);
                l = 0;
                if (*p == '\n') {
                    ++p;
                    continue;
                }
            }
            line[l++] = *p++;
        }
        line[l] = '\0';
        pushLine(line, style, href);
        return true;
    }

    char line[96] = "";
    size_t l = 0;
    const char* p = src;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;

        char word[48] = "";
        size_t w = 0;
        while (*p && *p != ' ' && w + 1 < sizeof(word)) {
            word[w++] = *p++;
        }
        word[w] = '\0';

        if (w == 0) continue;

        const size_t need = l == 0 ? w : w + 1;
        if (l + need > maxChars) {
            line[l] = '\0';
            pushLine(line, style, href);
            l = 0;
        }
        if (l > 0) {
            line[l++] = ' ';
        }
        for (size_t i = 0; i < w && l + 1 < sizeof(line); ++i) {
            line[l++] = word[i];
        }
    }

    line[l] = '\0';
    if (line[0]) {
        pushLine(line, style, href);
    }
    return true;
}

bool LayoutEngine::appendImage(const char* src, uint16_t w, uint16_t h, const CssStyle& style, int16_t maxWidth) {
    if (count_ >= kMaxBlocks) return false;
    if (style.display == DisplayMode::Hidden) return true;

    int16_t maxW = static_cast<int16_t>(maxWidth - 12);
    if (maxW < 24) maxW = 24;
    if (w > static_cast<uint16_t>(maxW)) {
        const uint32_t num = static_cast<uint32_t>(h) * static_cast<uint32_t>(maxW);
        const uint16_t newH = static_cast<uint16_t>(num / (w == 0 ? 1U : static_cast<uint32_t>(w)));
        h = newH == 0 ? 1 : newH;
        w = static_cast<uint16_t>(maxW);
    }

    Block& b = blocks_[count_++];
    b.kind = 2;
    b.style = style;
    b.y = cursorY_;
    b.h = static_cast<int16_t>(h + 4 + style.marginV);
    b.imageW = w;
    b.imageH = h;
    if (src) {
        copyTrim(b.src, sizeof(b.src), src, sizeof(b.src) - 1);
    }
    cursorY_ = static_cast<int16_t>(cursorY_ + b.h);
    return true;
}

bool LayoutEngine::appendBreak() {
    cursorY_ = static_cast<int16_t>(cursorY_ + 8);
    return true;
}

bool LayoutEngine::setBlockMeta(uint8_t index, uint8_t kind, const char* href, const char* src) {
    if (index >= count_) return false;
    Block& b = blocks_[index];
    b.kind = kind;
    if (href) {
        copyTrim(b.href, sizeof(b.href), href, sizeof(b.href) - 1);
    }
    if (src) {
        copyTrim(b.src, sizeof(b.src), src, sizeof(b.src) - 1);
    }
    return true;
}

int16_t LayoutEngine::docHeight() const {
    return cursorY_ < 1 ? 1 : cursorY_;
}

uint8_t LayoutEngine::blockCount() const {
    return count_;
}

const LayoutEngine::Block* LayoutEngine::blocks() const {
    return blocks_;
}

void CacheManager::begin() {
    entries_ = local_;
    const size_t bytes = sizeof(Entry) * kEntryMax;
    if (ESP.getPsramSize() > 0) {
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            entries_ = static_cast<Entry*>(p);
        }
    }
    memset(entries_, 0, bytes);
}

bool CacheManager::find(const char* key, Entry& out, uint32_t nowMs) {
    if (!entries_ || !key || !key[0]) return false;
    for (uint8_t i = 0; i < kEntryMax; ++i) {
        Entry& e = entries_[i];
        if (!e.valid) continue;
        if (strncmp(e.key, key, sizeof(e.key)) == 0) {
            e.lastUse = nowMs;
            out = e;
            return true;
        }
    }
    return false;
}

void CacheManager::put(const char* key, ImageFormat format, uint16_t width, uint16_t height, uint32_t nowMs) {
    if (!entries_ || !key || !key[0]) return;

    int8_t slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint8_t i = 0; i < kEntryMax; ++i) {
        Entry& e = entries_[i];
        if (!e.valid) {
            slot = static_cast<int8_t>(i);
            break;
        }
        if (e.lastUse < oldest) {
            oldest = e.lastUse;
            slot = static_cast<int8_t>(i);
        }
    }
    if (slot < 0) return;

    Entry& e = entries_[slot];
    e.valid = true;
    copyTrim(e.key, sizeof(e.key), key, sizeof(e.key) - 1);
    e.format = format;
    e.width = width;
    e.height = height;
    e.lastUse = nowMs;
}

void ImageRenderer::begin(CacheManager* cache) {
    cache_ = cache;
}

ImageFormat ImageRenderer::detectFormat(const char* src) const {
    if (!src || !src[0]) return ImageFormat::Unknown;
    const char* dot = strrchr(src, '.');
    if (!dot) return ImageFormat::Unknown;
    char ext[8] = "";
    size_t n = 0;
    for (const char* p = dot + 1; *p && *p != '?' && *p != '#' && n + 1 < sizeof(ext); ++p) {
        ext[n++] = *p;
    }
    ext[n] = '\0';
    toLowerInPlace(ext);
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return ImageFormat::Jpeg;
    if (strcmp(ext, "png") == 0) return ImageFormat::Png;
    if (strcmp(ext, "bmp") == 0) return ImageFormat::Bmp;
    if (strcmp(ext, "qoi") == 0) return ImageFormat::Qoi;
    if (strcmp(ext, "webp") == 0) return ImageFormat::WebP;
    return ImageFormat::Unknown;
}

bool ImageRenderer::validateDimensions(uint16_t w, uint16_t h, int16_t viewportW, int16_t viewportH) const {
    if (w == 0 || h == 0) return false;
    if (w > 1024 || h > 1024) return false;
    if (w > static_cast<uint16_t>(viewportW * 4)) return false;
    if (h > static_cast<uint16_t>(viewportH * 6)) return false;
    return true;
}

void ImageRenderer::fitToViewport(uint16_t& w, uint16_t& h, int16_t viewportW, int16_t viewportH) const {
    if (w == 0 || h == 0) return;
    const uint16_t targetW = static_cast<uint16_t>(viewportW > 8 ? viewportW - 8 : viewportW);
    if (w > targetW) {
        const uint32_t num = static_cast<uint32_t>(h) * targetW;
        h = static_cast<uint16_t>(num / w);
        w = targetW;
    }
    const uint16_t maxH = static_cast<uint16_t>(viewportH > 12 ? viewportH - 12 : viewportH);
    if (h > maxH) {
        h = maxH;
    }
}

void ImageRenderer::render(katux::graphics::Renderer& renderer, const Rect& rect, const char* src, uint32_t nowMs) {
    if (!src || !src[0] || rect.w <= 0 || rect.h <= 0) {
        renderer.fillRect(rect, 0xCE79);
        renderer.drawRect(rect, 0x39E7);
        return;
    }

    const ImageFormat fmt = detectFormat(src);
    uint16_t bg = 0xCE79;
    if (fmt == ImageFormat::Jpeg) bg = 0xFFF2;
    else if (fmt == ImageFormat::Png) bg = 0xD6FF;
    else if (fmt == ImageFormat::Bmp) bg = 0xE71C;
    else if (fmt == ImageFormat::Qoi) bg = 0xE6B7;
    else if (fmt == ImageFormat::WebP) bg = 0xD71A;

    if (cache_) {
        CacheManager::Entry e{};
        if (!cache_->find(src, e, nowMs)) {
            cache_->put(src, fmt, static_cast<uint16_t>(rect.w), static_cast<uint16_t>(rect.h), nowMs);
        }
    }

    renderer.fillRect(rect, bg);

    bool drawOk = false;
    if (fmt == ImageFormat::Jpeg || fmt == ImageFormat::Png || fmt == ImageFormat::Bmp || fmt == ImageFormat::Qoi) {
        if (isHttpUrl(src)) {
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
                        drawOk = drawEncodedWithWrapper(fmt, wrapper, rect);
                    }
                }
                http.end();
            }
        } else if (SPIFFS.exists(src)) {
            File file = SPIFFS.open(src, "r");
            if (file) {
                FileDataWrapper wrapper(&file);
                drawOk = drawEncodedWithWrapper(fmt, wrapper, rect);
                wrapper.close();
            }
        }
    }

    renderer.drawRect(rect, 0x39E7);
    if (!drawOk) {
        const char* lbl = "IMG";
        if (fmt == ImageFormat::Jpeg) lbl = "JPEG";
        else if (fmt == ImageFormat::Png) lbl = "PNG";
        else if (fmt == ImageFormat::Bmp) lbl = "BMP";
        else if (fmt == ImageFormat::Qoi) lbl = "QOI";
        else if (fmt == ImageFormat::WebP) lbl = "WEBP";
        renderer.drawText(static_cast<int16_t>(rect.x + 3), static_cast<int16_t>(rect.y + 3), lbl, 0x2104, bg);
        if (fmt == ImageFormat::WebP) {
            renderer.drawText(static_cast<int16_t>(rect.x + 3), static_cast<int16_t>(rect.y + 12), "No decoder", 0x2104, bg);
        }
    }
}

void Renderer::clearTargets() {
    count_ = 0;
}

bool Renderer::addTarget(const Rect& r, uint8_t kind, uint8_t index) {
    if (count_ >= kTargetMax) return false;
    targets_[count_].rect = r;
    targets_[count_].kind = kind;
    targets_[count_].index = index;
    ++count_;
    return true;
}

int8_t Renderer::hit(int16_t x, int16_t y) const {
    for (int16_t i = static_cast<int16_t>(count_) - 1; i >= 0; --i) {
        const ClickTarget& t = targets_[i];
        if (x >= t.rect.x && y >= t.rect.y && x < t.rect.x + t.rect.w && y < t.rect.y + t.rect.h) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

const Renderer::ClickTarget* Renderer::targets() const {
    return targets_;
}

uint8_t Renderer::targetCount() const {
    return count_;
}

void BrowserApp::begin(const char* initialUrl, const char* const* presets, uint8_t presetCount) {
    presets_ = presets;
    presetCount_ = presetCount;
    presetIndex_ = 0;
    if (initialUrl && initialUrl[0]) {
        copyTrim(url_, sizeof(url_), initialUrl, sizeof(url_) - 1);
    }
    baseHref_[0] = '\0';
    strlcpy(title_, "Navigator", sizeof(title_));
    strlcpy(status_, "Ready", sizeof(status_));
    historyCount_ = 0;
    historyIndex_ = -1;
    favoriteCount_ = 0;
    loading_ = false;
    loaded_ = false;
    redirectDepth_ = 0;
    scrollY_ = 0;
    lastViewportH_ = 1;
    styleDepth_ = 0;
    activeHref_[0] = '\0';
    openImagePending_ = false;
    pendingImageSrc_[0] = '\0';
    dirtyChrome_ = true;
    dirtyProgress_ = true;
    dirtyContent_ = true;
    lastProgress_ = 0;
    progressDisplayed_ = 0;
    progressTarget_ = 0;
    lastProgressAnimMs_ = millis();
    lastProgressFill_ = 0;
    progressGlowPermille_ = 0;
    lastProgressGlowX_ = -32768;
    lastProgressGlowW_ = 0;
    lastScrollY_ = 0;
    lastVisibleBlockCount_ = 0;
    lastDocHeight_ = 0;
    scrollVelocity_ = 0.0f;
    lastScrollAnimMs_ = millis();
    lastStatusLine_[0] = '\0';
    inStyleTag_ = false;
    inScriptTag_ = false;
    inTitleTag_ = false;
    titleBuffer_[0] = '\0';
    listDepth_ = 0;
    memset(listOrdered_, 0, sizeof(listOrdered_));
    memset(listCounter_, 0, sizeof(listCounter_));
    cssOrder_ = 0;
    cssBuffer_[0] = '\0';
    cssBufferLen_ = 0;
    cssRuleCount_ = 0;
    memset(cssRules_, 0, sizeof(cssRules_));
    scriptBuffer_[0] = '\0';
    scriptBufferLen_ = 0;
    jsExecDepth_ = 0;
    memset(externalQueue_, 0, sizeof(externalQueue_));
    externalQueueCount_ = 0;
    externalLoading_ = false;
    externalLastAttemptMs_ = 0;
    externalLoadingUrl_[0] = '\0';
    statusActivityUntilMs_ = millis() + 2800U;
    lastStatusBarVisible_ = true;
    memset(deferredJsCache_, 0, sizeof(deferredJsCache_));
    memset(deferredCssCache_, 0, sizeof(deferredCssCache_));
    deferredJsCount_ = 0;
    deferredCssCount_ = 0;
    deferredLastFlushMs_ = 0;
    memset(formControls_, 0, sizeof(formControls_));
    formControlCount_ = 0;
    formScopeCounter_ = 0;
    activeFormScope_ = 0;
    activeFormAction_[0] = '\0';
    strlcpy(activeFormMethod_, "get", sizeof(activeFormMethod_));
    formKeyboardPending_ = false;
    formKeyboardControlIndex_ = 0xFF;
    formKeyboardInitial_[0] = '\0';
    inTextareaTag_ = false;
    activeTextareaControl_ = 0xFF;
    layout_.reset();
    tokenizer_.reset();
    cache_.begin();
    image_.begin(&cache_);
    styleStack_[0] = CssStyle{};
    styleDepth_ = 1;
}

void BrowserApp::clearDocument() {
    layout_.reset();
    tokenizer_.reset();
    loaded_ = false;
    scrollY_ = 0;
    activeHref_[0] = '\0';
    baseHref_[0] = '\0';
    styleStack_[0] = CssStyle{};
    styleDepth_ = 1;
    inStyleTag_ = false;
    inScriptTag_ = false;
    inTitleTag_ = false;
    titleBuffer_[0] = '\0';
    listDepth_ = 0;
    memset(listOrdered_, 0, sizeof(listOrdered_));
    memset(listCounter_, 0, sizeof(listCounter_));
    cssOrder_ = 0;
    cssBuffer_[0] = '\0';
    cssBufferLen_ = 0;
    cssRuleCount_ = 0;
    memset(cssRules_, 0, sizeof(cssRules_));
    scriptBuffer_[0] = '\0';
    scriptBufferLen_ = 0;
    jsExecDepth_ = 0;
    memset(externalQueue_, 0, sizeof(externalQueue_));
    externalQueueCount_ = 0;
    externalLoading_ = false;
    externalLastAttemptMs_ = 0;
    externalLoadingUrl_[0] = '\0';
    statusActivityUntilMs_ = millis() + 2800U;
    lastStatusBarVisible_ = true;
    memset(deferredJsCache_, 0, sizeof(deferredJsCache_));
    memset(deferredCssCache_, 0, sizeof(deferredCssCache_));
    deferredJsCount_ = 0;
    deferredCssCount_ = 0;
    deferredLastFlushMs_ = 0;
    memset(formControls_, 0, sizeof(formControls_));
    formControlCount_ = 0;
    formScopeCounter_ = 0;
    activeFormScope_ = 0;
    activeFormAction_[0] = '\0';
    strlcpy(activeFormMethod_, "get", sizeof(activeFormMethod_));
    formKeyboardPending_ = false;
    formKeyboardControlIndex_ = 0xFF;
    formKeyboardInitial_[0] = '\0';
    inTextareaTag_ = false;
    activeTextareaControl_ = 0xFF;
    dirtyChrome_ = true;
    dirtyProgress_ = true;
    dirtyContent_ = true;
    lastProgress_ = 0;
    progressDisplayed_ = 0;
    progressTarget_ = 0;
    lastProgressAnimMs_ = millis();
    lastProgressFill_ = 0;
    progressGlowPermille_ = 0;
    lastProgressGlowX_ = -32768;
    lastProgressGlowW_ = 0;
    lastScrollY_ = 0;
    lastVisibleBlockCount_ = 0;
    lastDocHeight_ = 0;
    scrollVelocity_ = 0.0f;
    scrollInputDir_ = 0;
    scrollInputBoost_ = 0;
    lastScrollInputMs_ = millis();
    lastScrollAnimMs_ = millis();
    lastStatusLine_[0] = '\0';
}

void BrowserApp::pushHistory(const char* url) {
    if (!url || !url[0]) return;
    if (historyIndex_ >= 0 && historyIndex_ < static_cast<int8_t>(kHistoryMax) && strcmp(history_[historyIndex_], url) == 0) {
        return;
    }

    if (historyIndex_ >= 0 && historyIndex_ < static_cast<int8_t>(historyCount_ - 1U)) {
        historyCount_ = static_cast<uint8_t>(historyIndex_ + 1);
    }

    if (historyCount_ < kHistoryMax) {
        copyTrim(history_[historyCount_], sizeof(history_[0]), url, sizeof(history_[0]) - 1);
        ++historyCount_;
        historyIndex_ = static_cast<int8_t>(historyCount_ - 1U);
        dirtyChrome_ = true;
        return;
    }

    for (uint8_t i = 1; i < kHistoryMax; ++i) {
        strlcpy(history_[i - 1], history_[i], sizeof(history_[0]));
    }
    copyTrim(history_[kHistoryMax - 1], sizeof(history_[0]), url, sizeof(history_[0]) - 1);
    historyIndex_ = static_cast<int8_t>(kHistoryMax - 1);
    dirtyChrome_ = true;
}

bool BrowserApp::normalizeUrl(const char* url, char* out, size_t outLen) const {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!url || !url[0]) return false;

    if (isHttpUrl(url)) {
        copyTrim(out, outLen, url, outLen - 1);
    } else if (hasHtmlExt(url) || strchr(url, '/')) {
        snprintf(out, outLen, "http://%s", url);
    } else {
        snprintf(out, outLen, "http://%s/", url);
    }
    return out[0] != '\0';
}

bool BrowserApp::toggleUrlScheme(const char* url, char* out, size_t outLen) const {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!url || !url[0]) return false;

    if (startsWith(url, "https://")) {
        snprintf(out, outLen, "http://%s", url + 8);
    } else if (startsWith(url, "http://")) {
        snprintf(out, outLen, "https://%s", url + 7);
    } else {
        return false;
    }
    return out[0] != '\0';
}

bool BrowserApp::tryAlternateScheme() {
    if (schemeFallbackTried_) return false;

    char fallbackUrl[kUrlMaxLen] = "";
    if (!toggleUrlScheme(url_, fallbackUrl, sizeof(fallbackUrl))) return false;

    char previousUrl[kUrlMaxLen] = "";
    copyTrim(previousUrl, sizeof(previousUrl), url_, sizeof(previousUrl) - 1);
    schemeFallbackTried_ = true;
    clearDocument();

    if (!network_.open(fallbackUrl)) {
        copyTrim(url_, sizeof(url_), previousUrl, sizeof(previousUrl) - 1);
        copyTrim(baseHref_, sizeof(baseHref_), previousUrl, sizeof(previousUrl) - 1);
        strlcpy(status_, network_.statusText(), sizeof(status_));
        loading_ = false;
        loaded_ = false;
        dirtyChrome_ = true;
        dirtyProgress_ = true;
        dirtyContent_ = true;
        return false;
    }

    copyTrim(url_, sizeof(url_), fallbackUrl, sizeof(fallbackUrl) - 1);
    dirtyChrome_ = true;
    copyTrim(baseHref_, sizeof(baseHref_), fallbackUrl, sizeof(fallbackUrl) - 1);
    if (historyIndex_ >= 0 && historyIndex_ < static_cast<int8_t>(historyCount_) && strcmp(history_[historyIndex_], previousUrl) == 0) {
        copyTrim(history_[historyIndex_], sizeof(history_[0]), fallbackUrl, sizeof(fallbackUrl) - 1);
    }
    loading_ = true;
    loaded_ = false;
    loadStartMs_ = millis();
    strlcpy(status_, startsWith(fallbackUrl, "https://") ? "Retry HTTPS" : "Retry HTTP", sizeof(status_));
    dirtyProgress_ = true;
    dirtyContent_ = true;
    return true;
}

void BrowserApp::openUrl(const char* url, bool addToHistory) {
    if (!url || !url[0]) return;

    char normalized[kUrlMaxLen] = "";
    if (!normalizeUrl(url, normalized, sizeof(normalized))) return;

    copyTrim(url_, sizeof(url_), normalized, sizeof(url_) - 1);
    dirtyChrome_ = true;
    clearDocument();
    copyTrim(baseHref_, sizeof(baseHref_), url_, sizeof(baseHref_) - 1);
    schemeFallbackTried_ = false;
    if (addToHistory) {
        redirectDepth_ = 0;
    }

    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 26000U) {
        strlcpy(status_, "Low memory", sizeof(status_));
        loading_ = false;
        loaded_ = false;
        dirtyProgress_ = true;
        dirtyContent_ = true;
        return;
    }

    if (!network_.open(url_)) {
        if (tryAlternateScheme()) {
            if (addToHistory) {
                pushHistory(url_);
            }
            return;
        }
        strlcpy(status_, network_.statusText(), sizeof(status_));
        loading_ = false;
        loaded_ = false;
        dirtyProgress_ = true;
        dirtyContent_ = true;
        return;
    }

    if (addToHistory) {
        pushHistory(url_);
    }

    loading_ = true;
    loaded_ = false;
    loadStartMs_ = millis();
    strlcpy(status_, "Loading", sizeof(status_));
}

void BrowserApp::setCurrentUrlText(const char* url) {
    if (!url) return;
    copyTrim(url_, sizeof(url_), url, sizeof(url_) - 1);
    dirtyChrome_ = true;
}

void BrowserApp::openPreset(int8_t delta) {
    if (!presets_ || presetCount_ == 0) return;
    int16_t idx = static_cast<int16_t>(presetIndex_) + delta;
    while (idx < 0) idx += presetCount_;
    while (idx >= presetCount_) idx -= presetCount_;
    presetIndex_ = static_cast<uint8_t>(idx);
    openUrl(presets_[presetIndex_], true);
}

void BrowserApp::openHome() {
    if (presets_ && presetCount_ > 0) {
        presetIndex_ = 0;
        openUrl(presets_[0], true);
    }
}

void BrowserApp::reload() {
    if (!url_[0]) return;
    redirectDepth_ = 0;
    openUrl(url_, false);
}

void BrowserApp::stopLoading() {
    if (!loading_ && !externalLoading_ && externalQueueCount_ == 0) return;
    network_.reset();
    loading_ = false;
    loaded_ = layout_.blockCount() > 0;
    schemeFallbackTried_ = false;
    redirectDepth_ = 0;
    memset(externalQueue_, 0, sizeof(externalQueue_));
    externalQueueCount_ = 0;
    externalLoading_ = false;
    externalLoadingUrl_[0] = '\0';
    strlcpy(status_, "Stopped", sizeof(status_));
    statusActivityUntilMs_ = millis() + 2800U;
    dirtyChrome_ = true;
    dirtyProgress_ = true;
}

void BrowserApp::navigateHistory(int8_t step) {
    if (historyCount_ == 0 || step == 0) return;
    const int16_t next = static_cast<int16_t>(historyIndex_) + step;
    if (next < 0 || next >= historyCount_) return;
    historyIndex_ = static_cast<int8_t>(next);
    dirtyChrome_ = true;
    openUrl(history_[historyIndex_], false);
}

void BrowserApp::scroll(int16_t delta) {
    if (delta == 0) return;

    const int8_t dir = delta > 0 ? 1 : -1;
    const uint32_t now = millis();
    const uint32_t sinceLastInput = now - lastScrollInputMs_;

    if (scrollInputDir_ == dir && sinceLastInput <= kScrollInputComboMs) {
        if (scrollInputBoost_ < 8) {
            ++scrollInputBoost_;
        }
    } else {
        scrollInputDir_ = dir;
        scrollInputBoost_ = 0;
    }

    lastScrollInputMs_ = now;

    const float gain = kScrollImpulseBase + static_cast<float>(scrollInputBoost_) * kScrollImpulseBoost;
    scrollVelocity_ += static_cast<float>(delta) * gain;

    float maxVelocity = kScrollMaxVelocityBase + static_cast<float>(scrollInputBoost_) * kScrollMaxVelocityBoost;
    if (maxVelocity > 38.0f) maxVelocity = 38.0f;

    if (scrollVelocity_ > maxVelocity) scrollVelocity_ = maxVelocity;
    if (scrollVelocity_ < -maxVelocity) scrollVelocity_ = -maxVelocity;
}

void BrowserApp::toggleFavorite() {
    for (uint8_t i = 0; i < favoriteCount_; ++i) {
        if (strcmp(favorites_[i], url_) == 0) {
            for (uint8_t j = i + 1; j < favoriteCount_; ++j) {
                strlcpy(favorites_[j - 1], favorites_[j], sizeof(favorites_[0]));
            }
            if (favoriteCount_ > 0) --favoriteCount_;
            strlcpy(status_, "Favorite removed", sizeof(status_));
            statusActivityUntilMs_ = millis() + 2800U;
            dirtyChrome_ = true;
            dirtyProgress_ = true;
            return;
        }
    }

    if (favoriteCount_ < kFavoriteMax) {
        copyTrim(favorites_[favoriteCount_], sizeof(favorites_[0]), url_, sizeof(favorites_[0]) - 1);
        ++favoriteCount_;
    } else {
        for (uint8_t i = 1; i < kFavoriteMax; ++i) {
            strlcpy(favorites_[i - 1], favorites_[i], sizeof(favorites_[0]));
        }
        copyTrim(favorites_[kFavoriteMax - 1], sizeof(favorites_[0]), url_, sizeof(favorites_[0]) - 1);
    }
    strlcpy(status_, "Favorite added", sizeof(status_));
    statusActivityUntilMs_ = millis() + 2800U;
    dirtyChrome_ = true;
    dirtyProgress_ = true;
}

bool BrowserApp::isFavorite(const char* url) const {
    if (!url || !url[0]) return false;
    for (uint8_t i = 0; i < favoriteCount_; ++i) {
        if (strcmp(favorites_[i], url) == 0) return true;
    }
    return false;
}

bool BrowserApp::isLoading() const {
    return loading_ || externalLoading_ || externalQueueCount_ > 0;
}

bool BrowserApp::isLoaded() const {
    return loaded_;
}

const char* BrowserApp::currentUrl() const {
    return url_;
}

const char* BrowserApp::status() const {
    return status_;
}

uint16_t BrowserApp::progressPermille() const {
    return progressDisplayed_;
}

uint16_t BrowserApp::computeProgressTargetPermille(uint32_t nowMs) const {
    if (loading_) {
        const float p = network_.progress();
        if (p > 0.0f) return static_cast<uint16_t>(p * 1000.0f);
        const uint32_t elapsed = nowMs - loadStartMs_;
        uint16_t pseudo = static_cast<uint16_t>((elapsed / 20U) % 900U);
        if (pseudo < 50) pseudo = 50;
        return pseudo;
    }
    if (externalLoading_ || externalQueueCount_ > 0) {
        uint16_t ratio = 0;
        if (kExternalQueueMax > 0) {
            const uint8_t done = static_cast<uint8_t>(kExternalQueueMax - (externalQueueCount_ > kExternalQueueMax ? kExternalQueueMax : externalQueueCount_));
            ratio = static_cast<uint16_t>((done * 1000U) / kExternalQueueMax);
        }
        if (ratio < 820) ratio = 820;
        if (ratio > 980) ratio = 980;
        return ratio;
    }
    return 1000;
}

void BrowserApp::updateProgressAnimation(uint32_t nowMs) {
    progressTarget_ = computeProgressTargetPermille(nowMs);
    if (lastProgressAnimMs_ == 0) {
        lastProgressAnimMs_ = nowMs;
    }

    uint32_t dt = nowMs - lastProgressAnimMs_;
    if (dt < 12U) return;
    lastProgressAnimMs_ = nowMs;

    uint16_t before = progressDisplayed_;
    if (progressDisplayed_ < progressTarget_) {
        uint16_t diff = static_cast<uint16_t>(progressTarget_ - progressDisplayed_);
        uint16_t step = static_cast<uint16_t>(diff / 5U);
        if (step < 1) step = 1;
        progressDisplayed_ = static_cast<uint16_t>(progressDisplayed_ + step);
        if (progressDisplayed_ > progressTarget_) progressDisplayed_ = progressTarget_;
    } else if (progressDisplayed_ > progressTarget_) {
        uint16_t diff = static_cast<uint16_t>(progressDisplayed_ - progressTarget_);
        uint16_t step = static_cast<uint16_t>(diff / 3U);
        if (step < 2) step = 2;
        if (step > progressDisplayed_) step = progressDisplayed_;
        progressDisplayed_ = static_cast<uint16_t>(progressDisplayed_ - step);
        if (progressDisplayed_ < progressTarget_) progressDisplayed_ = progressTarget_;
    }

    const bool loadingVisual = loading_ || externalLoading_ || externalQueueCount_ > 0;
    const uint16_t glowBefore = progressGlowPermille_;
    if (loadingVisual) {
        uint16_t glowStep = static_cast<uint16_t>(dt * 3U);
        if (glowStep < 6U) glowStep = 6U;
        progressGlowPermille_ = static_cast<uint16_t>((progressGlowPermille_ + glowStep) % 1000U);
    } else {
        progressGlowPermille_ = 0;
    }

    if (progressDisplayed_ != before || progressGlowPermille_ != glowBefore) {
        dirtyProgress_ = true;
    }
}

void BrowserApp::updateSmoothScroll(uint32_t nowMs) {
    if (lastScrollAnimMs_ == 0) {
        lastScrollAnimMs_ = nowMs;
    }
    uint32_t dt = nowMs - lastScrollAnimMs_;
    if (dt < 10U) return;
    lastScrollAnimMs_ = nowMs;

    const uint32_t sinceInput = nowMs - lastScrollInputMs_;
    const bool activeHoldInput = scrollInputDir_ != 0 && sinceInput <= (kScrollInputComboMs + 30U);

    if (!activeHoldInput && sinceInput > 280U) {
        scrollInputDir_ = 0;
        scrollInputBoost_ = 0;
    }

    if (scrollVelocity_ > -0.15f && scrollVelocity_ < 0.15f) {
        scrollVelocity_ = 0.0f;
        return;
    }

    int16_t step = static_cast<int16_t>(scrollVelocity_);
    if (step == 0) {
        step = scrollVelocity_ > 0.0f ? 1 : -1;
    }

    const int16_t docH = layout_.docHeight();
    const int16_t maxScroll = docH > lastViewportH_ ? static_cast<int16_t>(docH - lastViewportH_) : 0;
    const int16_t before = scrollY_;
    scrollY_ = clamp16(static_cast<int16_t>(scrollY_ + step), 0, maxScroll);
    if (scrollY_ != before) {
        dirtyContent_ = true;
    } else {
        scrollVelocity_ = 0.0f;
        scrollInputDir_ = 0;
        scrollInputBoost_ = 0;
        return;
    }

    const float decay = activeHoldInput ? kScrollDecayHeld : kScrollDecayFree;
    scrollVelocity_ *= decay;
}

bool BrowserApp::extractAttr(const char* attrs, const char* attr, char* out, size_t outLen) const {
    if (!attrs || !attr || !out || outLen == 0) return false;
    out[0] = '\0';

    char needle[24] = "";
    copyTrim(needle, sizeof(needle), attr, sizeof(needle) - 1);
    toLowerInPlace(needle);
    const size_t needleLen = strlen(needle);
    if (needleLen == 0) return false;

    size_t pos = 0;
    while (attrs[pos]) {
        while (attrs[pos] == ' ' || attrs[pos] == '\t' || attrs[pos] == '\n' || attrs[pos] == '\r') ++pos;
        if (!attrs[pos]) break;

        char key[24] = "";
        size_t k = 0;
        while (attrs[pos] && attrs[pos] != '=' && attrs[pos] != ' ' && attrs[pos] != '\t' && attrs[pos] != '\n' && attrs[pos] != '\r' && k + 1 < sizeof(key)) {
            key[k++] = attrs[pos++];
        }
        key[k] = '\0';
        toLowerInPlace(key);

        while (attrs[pos] == ' ' || attrs[pos] == '\t' || attrs[pos] == '\n' || attrs[pos] == '\r') ++pos;
        if (attrs[pos] != '=') {
            while (attrs[pos] && attrs[pos] != ' ') ++pos;
            continue;
        }
        ++pos;
        while (attrs[pos] == ' ' || attrs[pos] == '\t' || attrs[pos] == '\n' || attrs[pos] == '\r') ++pos;

        char q = 0;
        if (attrs[pos] == '"' || attrs[pos] == '\'') {
            q = attrs[pos++];
        }

        char value[kUrlMaxLen] = "";
        size_t v = 0;
        while (attrs[pos] && v + 1 < sizeof(value)) {
            if (q) {
                if (attrs[pos] == q) break;
            } else if (attrs[pos] == ' ' || attrs[pos] == '\t' || attrs[pos] == '\n' || attrs[pos] == '\r') {
                break;
            }
            value[v++] = attrs[pos++];
        }
        value[v] = '\0';
        if (q && attrs[pos] == q) ++pos;

        if (strcmp(key, needle) == 0) {
            copyTrim(out, outLen, value, outLen - 1);
            return out[0] != '\0';
        }
    }

    return false;
}

void BrowserApp::decodeEntities(char* text) const {
    if (!text || !text[0]) return;
    if (!strchr(text, '&')) return;
    char out[100] = "";
    size_t w = 0;

    for (size_t i = 0; text[i] && w + 1 < sizeof(out); ++i) {
        if (text[i] != '&') {
            out[w++] = text[i];
            continue;
        }

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
        if (strncmp(text + i, "&quot;", 6) == 0) {
            out[w++] = '"';
            i += 5;
            continue;
        }
        if (strncmp(text + i, "&apos;", 6) == 0 || strncmp(text + i, "&#39;", 5) == 0) {
            out[w++] = '\'';
            i += (text[i + 1] == 'a') ? 5 : 4;
            continue;
        }
        if (strncmp(text + i, "&nbsp;", 6) == 0) {
            out[w++] = ' ';
            i += 5;
            continue;
        }
        if (strncmp(text + i, "&copy;", 6) == 0) {
            out[w++] = '(';
            if (w + 2 < sizeof(out)) {
                out[w++] = 'c';
                out[w++] = ')';
            }
            i += 5;
            continue;
        }
        if (text[i + 1] == '#') {
            bool hex = (text[i + 2] == 'x' || text[i + 2] == 'X');
            size_t j = i + (hex ? 3 : 2);
            unsigned long value = 0;
            bool ok = false;
            while (text[j] && text[j] != ';') {
                char c = text[j];
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (hex && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
                else if (hex && c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
                else {
                    ok = false;
                    break;
                }
                ok = true;
                value = hex ? (value * 16UL + static_cast<unsigned long>(digit)) : (value * 10UL + static_cast<unsigned long>(digit));
                ++j;
            }
            if (ok && text[j] == ';') {
                char repl = ' ';
                if (value == 9UL) repl = ' ';
                else if (value == 10UL || value == 13UL) repl = ' ';
                else if (value >= 32UL && value <= 126UL) repl = static_cast<char>(value);
                out[w++] = repl;
                i = j;
                continue;
            }
        }

        out[w++] = '&';
    }

    out[w] = '\0';
    strlcpy(text, out, 96);
}

bool BrowserApp::resolveUrl(const char* baseUrl, const char* href, char* out, size_t outLen) const {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!href || !href[0]) return false;

    if (startsWith(href, "javascript:") || startsWith(href, "mailto:") || startsWith(href, "tel:") || startsWith(href, "data:") ||
        startsWith(href, "blob:")) {
        return false;
    }

    if (isHttpUrl(href)) {
        copyTrim(out, outLen, href, outLen - 1);
        return out[0] != '\0';
    }

    const char* effectiveBase = (baseHref_[0] ? baseHref_ : baseUrl);
    if (!effectiveBase || !effectiveBase[0]) return false;

    if (startsWith(href, "//")) {
        if (startsWith(effectiveBase, "https://")) {
            snprintf(out, outLen, "https:%s", href);
        } else {
            snprintf(out, outLen, "http:%s", href);
        }
        return out[0] != '\0';
    }

    if (href[0] == '#') {
        copyTrim(out, outLen, effectiveBase, outLen - 1);
        char* hash = strchr(out, '#');
        if (hash) *hash = '\0';
        strlcat(out, href, outLen);
        return out[0] != '\0';
    }

    if (href[0] == '/') {
        const char* host = startsWith(effectiveBase, "https://") ? effectiveBase + 8 : (startsWith(effectiveBase, "http://") ? effectiveBase + 7 : effectiveBase);
        const char* slash = strchr(host, '/');
        size_t hostLen = slash ? static_cast<size_t>(slash - effectiveBase) : strlen(effectiveBase);
        if (hostLen >= outLen) hostLen = outLen - 1;
        memcpy(out, effectiveBase, hostLen);
        out[hostLen] = '\0';
        strlcat(out, href, outLen);
        return true;
    }

    copyTrim(out, outLen, effectiveBase, outLen - 1);
    char* q = strchr(out, '?');
    if (q) *q = '\0';
    char* h = strchr(out, '#');
    if (h) *h = '\0';

    size_t len = strlen(out);
    while (len > 0 && out[len - 1] != '/') {
        out[len - 1] = '\0';
        --len;
    }
    if (len == 0 || out[len - 1] != '/') {
        strlcat(out, "/", outLen);
    }

    while (startsWith(href, "./")) href += 2;
    while (startsWith(href, "../")) {
        size_t n = strlen(out);
        if (n > 1 && out[n - 1] == '/') out[n - 1] = '\0';
        char* last = strrchr(out, '/');
        if (last && last > out + 7) {
            *(last + 1) = '\0';
        }
        href += 3;
    }

    strlcat(out, href, outLen);
    return out[0] != '\0';
}

void BrowserApp::appendHttpErrorPage(int16_t code) {
    CssStyle title{};
    title.bold = true;
    title.fontSize = FontSize::Large;
    title.fg = 0xF800;
    title.marginV = 3;

    CssStyle body{};
    body.fg = 0x2104;
    body.marginV = 2;

    char line[96] = "";
    snprintf(line, sizeof(line), "HTTP %d", static_cast<int>(code));
    layout_.appendText(line, title, "", 150);
    layout_.appendBreak();

    const char* msg = "Request failed";
    if (code == 403) msg = "Access denied";
    else if (code == 404) msg = "Page not found";
    else if (code == 408) msg = "Request timeout";
    else if (code >= 500) msg = "Server error";

    layout_.appendText(msg, body, "", 150);
    layout_.appendBreak();
    layout_.appendText(url_, body, "", 150);
    dirtyContent_ = true;
}

bool BrowserApp::parseCssMediaMatch(const char* mediaExpr) const {
    if (!mediaExpr || !mediaExpr[0]) return true;

    char expr[120] = "";
    copyTrim(expr, sizeof(expr), mediaExpr, sizeof(expr) - 1);
    toLowerInPlace(expr);

    const int16_t viewportW = 150;
    const char* maxW = strstr(expr, "max-width");
    if (maxW) {
        const char* p = strchr(maxW, ':');
        if (p) {
            int v = atoi(p + 1);
            return viewportW <= v;
        }
    }

    const char* minW = strstr(expr, "min-width");
    if (minW) {
        const char* p = strchr(minW, ':');
        if (p) {
            int v = atoi(p + 1);
            return viewportW >= v;
        }
    }

    if (strstr(expr, "screen") || strstr(expr, "all")) return true;
    return false;
}

void BrowserApp::parseCssRule(const char* selector, const char* declarations, bool mediaMatched) {
    if (!selector || !selector[0] || !declarations || !declarations[0] || cssRuleCount_ >= kCssRuleMax) return;

    char selectorList[120] = "";
    copyTrim(selectorList, sizeof(selectorList), selector, sizeof(selectorList) - 1);

    char* save = nullptr;
    char* sel = strtok_r(selectorList, ",", &save);
    while (sel && cssRuleCount_ < kCssRuleMax) {
        while (*sel == ' ' || *sel == '\t' || *sel == '\n' || *sel == '\r') ++sel;
        size_t selLen = strlen(sel);
        while (selLen > 0 && (sel[selLen - 1] == ' ' || sel[selLen - 1] == '\t' || sel[selLen - 1] == '\n' || sel[selLen - 1] == '\r')) {
            sel[selLen - 1] = '\0';
            --selLen;
        }

        if (sel[0]) {
            const char* right = strrchr(sel, ' ');
            if (!right) right = strrchr(sel, '>');
            if (right) {
                sel = const_cast<char*>(right + 1);
                while (*sel == ' ' || *sel == '\t') ++sel;
            }

            CssRule& r = cssRules_[cssRuleCount_++];
            r.valid = true;
            r.mediaMatched = mediaMatched;
            r.order = cssOrder_++;

            char work[60] = "";
            copyTrim(work, sizeof(work), sel, sizeof(work) - 1);
            toLowerInPlace(work);

            char* hash = strchr(work, '#');
            char* dot = strchr(work, '.');

            if (hash) {
                char* idStart = hash + 1;
                char* idEnd = idStart;
                while (*idEnd && *idEnd != '.' && *idEnd != '#') ++idEnd;
                char tmp = *idEnd;
                *idEnd = '\0';
                copyTrim(r.id, sizeof(r.id), idStart, sizeof(r.id) - 1);
                *idEnd = tmp;
                r.specificity = static_cast<uint16_t>(r.specificity + 100);
            }

            if (dot) {
                char* clsStart = dot + 1;
                char* clsEnd = clsStart;
                while (*clsEnd && *clsEnd != '.' && *clsEnd != '#') ++clsEnd;
                char tmp = *clsEnd;
                *clsEnd = '\0';
                copyTrim(r.cls, sizeof(r.cls), clsStart, sizeof(r.cls) - 1);
                *clsEnd = tmp;
                r.specificity = static_cast<uint16_t>(r.specificity + 10);
            }

            char tag[12] = "";
            uint8_t ti = 0;
            for (uint8_t i = 0; work[i] && ti + 1 < sizeof(tag); ++i) {
                if (work[i] == '.' || work[i] == '#') break;
                if ((work[i] >= 'a' && work[i] <= 'z') || (work[i] >= '0' && work[i] <= '9') || work[i] == '*') {
                    tag[ti++] = work[i];
                }
            }
            tag[ti] = '\0';
            if (tag[0] && strcmp(tag, "*") != 0) {
                copyTrim(r.tag, sizeof(r.tag), tag, sizeof(r.tag) - 1);
                r.specificity = static_cast<uint16_t>(r.specificity + 1);
            }

            copyTrim(r.declarations, sizeof(r.declarations), declarations, sizeof(r.declarations) - 1);
            CssStyle base{};
            r.style = base;
            css_.applyInline(r.declarations, r.style);
        }

        sel = strtok_r(nullptr, ",", &save);
    }
}

void BrowserApp::parseStylesheet(const char* cssText) {
    if (!cssText || !cssText[0]) return;

    char css[600] = "";
    copyTrim(css, sizeof(css), cssText, sizeof(css) - 1);
    toLowerInPlace(css);

    char cleaned[600] = "";
    size_t w = 0;
    bool inComment = false;
    for (size_t i = 0; css[i] && w + 1 < sizeof(cleaned); ++i) {
        if (!inComment && css[i] == '/' && css[i + 1] == '*') {
            inComment = true;
            ++i;
            continue;
        }
        if (inComment && css[i] == '*' && css[i + 1] == '/') {
            inComment = false;
            ++i;
            continue;
        }
        if (!inComment) cleaned[w++] = css[i];
    }
    cleaned[w] = '\0';

    size_t i = 0;
    while (cleaned[i]) {
        while (cleaned[i] == ' ' || cleaned[i] == '\t' || cleaned[i] == '\n' || cleaned[i] == '\r') ++i;
        if (!cleaned[i]) break;

        if (strncmp(cleaned + i, "@media", 6) == 0) {
            i += 6;
            while (cleaned[i] == ' ' || cleaned[i] == '\t') ++i;
            char mediaExpr[120] = "";
            size_t me = 0;
            while (cleaned[i] && cleaned[i] != '{' && me + 1 < sizeof(mediaExpr)) {
                mediaExpr[me++] = cleaned[i++];
            }
            mediaExpr[me] = '\0';
            if (cleaned[i] != '{') break;
            ++i;

            const bool mediaMatched = parseCssMediaMatch(mediaExpr);
            int depth = 1;
            size_t innerStart = i;
            while (cleaned[i] && depth > 0) {
                if (cleaned[i] == '{') ++depth;
                else if (cleaned[i] == '}') --depth;
                ++i;
            }
            size_t innerEnd = i > 0 ? i - 1 : i;
            if (innerEnd > innerStart) {
                char inner[360] = "";
                size_t inLen = innerEnd - innerStart;
                if (inLen >= sizeof(inner)) inLen = sizeof(inner) - 1;
                memcpy(inner, cleaned + innerStart, inLen);
                inner[inLen] = '\0';

                size_t j = 0;
                while (inner[j]) {
                    while (inner[j] == ' ' || inner[j] == '\t' || inner[j] == '\n' || inner[j] == '\r') ++j;
                    if (!inner[j]) break;
                    char selector[120] = "";
                    size_t sn = 0;
                    while (inner[j] && inner[j] != '{' && sn + 1 < sizeof(selector)) selector[sn++] = inner[j++];
                    selector[sn] = '\0';
                    if (inner[j] != '{') break;
                    ++j;
                    char decl[180] = "";
                    size_t dn = 0;
                    while (inner[j] && inner[j] != '}' && dn + 1 < sizeof(decl)) decl[dn++] = inner[j++];
                    decl[dn] = '\0';
                    if (inner[j] == '}') ++j;
                    parseCssRule(selector, decl, mediaMatched);
                }
            }
            continue;
        }

        char selector[120] = "";
        size_t sn = 0;
        while (cleaned[i] && cleaned[i] != '{' && sn + 1 < sizeof(selector)) selector[sn++] = cleaned[i++];
        selector[sn] = '\0';
        if (cleaned[i] != '{') break;
        ++i;

        char decl[200] = "";
        size_t dn = 0;
        while (cleaned[i] && cleaned[i] != '}' && dn + 1 < sizeof(decl)) decl[dn++] = cleaned[i++];
        decl[dn] = '\0';
        if (cleaned[i] == '}') ++i;

        parseCssRule(selector, decl, true);
    }
}

bool BrowserApp::selectorMatches(const CssRule& rule, const char* tag, const char* id, const char* cls) const {
    if (!rule.valid || !rule.mediaMatched) return false;
    if (rule.tag[0] && strcmp(rule.tag, tag ? tag : "") != 0) return false;
    if (rule.id[0] && strcmp(rule.id, id ? id : "") != 0) return false;
    if (rule.cls[0]) {
        if (!cls || !cls[0]) return false;
        char clsWork[30] = "";
        copyTrim(clsWork, sizeof(clsWork), cls, sizeof(clsWork) - 1);
        toLowerInPlace(clsWork);
        if (strcmp(rule.cls, clsWork) != 0) {
            if (!strstr(clsWork, rule.cls)) return false;
        }
    }
    return true;
}

void BrowserApp::applyStylesheet(const char* tag, const char* id, const char* cls, CssStyle& style) const {
    uint8_t matched[16]{};
    uint8_t matchCount = 0;

    for (uint8_t i = 0; i < cssRuleCount_ && matchCount < 16; ++i) {
        const CssRule& r = cssRules_[i];
        if (selectorMatches(r, tag, id, cls)) {
            matched[matchCount++] = i;
        }
    }

    for (uint8_t i = 0; i < matchCount; ++i) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < matchCount; ++j) {
            const CssRule& a = cssRules_[matched[i]];
            const CssRule& b = cssRules_[matched[j]];
            if (b.specificity < a.specificity) continue;
            if (b.specificity == a.specificity && b.order < a.order) continue;
            const uint8_t tmp = matched[i];
            matched[i] = matched[j];
            matched[j] = tmp;
        }
    }

    for (uint8_t i = 0; i < matchCount; ++i) {
        const CssRule& r = cssRules_[matched[i]];
        css_.applyInline(r.declarations, style);
    }
}

bool BrowserApp::isBlockTag(const char* tag) const {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "html") == 0 || strcmp(tag, "body") == 0 || strcmp(tag, "main") == 0 || strcmp(tag, "section") == 0 ||
           strcmp(tag, "article") == 0 || strcmp(tag, "aside") == 0 || strcmp(tag, "header") == 0 || strcmp(tag, "footer") == 0 ||
           strcmp(tag, "nav") == 0 || strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 || strcmp(tag, "ul") == 0 ||
           strcmp(tag, "ol") == 0 || strcmp(tag, "li") == 0 || strcmp(tag, "dl") == 0 || strcmp(tag, "dt") == 0 ||
           strcmp(tag, "dd") == 0 || strcmp(tag, "pre") == 0 || strcmp(tag, "code") == 0 || strcmp(tag, "h1") == 0 ||
           strcmp(tag, "h2") == 0 || strcmp(tag, "h3") == 0 || strcmp(tag, "h4") == 0 || strcmp(tag, "h5") == 0 ||
           strcmp(tag, "h6") == 0 || strcmp(tag, "blockquote") == 0 || strcmp(tag, "table") == 0 || strcmp(tag, "thead") == 0 ||
           strcmp(tag, "tbody") == 0 || strcmp(tag, "tfoot") == 0 || strcmp(tag, "tr") == 0 || strcmp(tag, "td") == 0 ||
           strcmp(tag, "th") == 0 || strcmp(tag, "figure") == 0 || strcmp(tag, "figcaption") == 0 || strcmp(tag, "form") == 0 ||
           strcmp(tag, "fieldset") == 0 || strcmp(tag, "legend") == 0 || strcmp(tag, "details") == 0 || strcmp(tag, "summary") == 0 ||
           strcmp(tag, "textarea") == 0 || strcmp(tag, "iframe") == 0 || strcmp(tag, "svg") == 0 || strcmp(tag, "math") == 0 ||
           strcmp(tag, "dialog") == 0 || strcmp(tag, "output") == 0 || strcmp(tag, "datalist") == 0;
}

bool BrowserApp::isInlineTag(const char* tag) const {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "span") == 0 || strcmp(tag, "a") == 0 || strcmp(tag, "em") == 0 || strcmp(tag, "strong") == 0 ||
           strcmp(tag, "b") == 0 || strcmp(tag, "i") == 0 || strcmp(tag, "u") == 0 || strcmp(tag, "small") == 0 ||
           strcmp(tag, "mark") == 0 || strcmp(tag, "abbr") == 0 || strcmp(tag, "cite") == 0 || strcmp(tag, "time") == 0 ||
           strcmp(tag, "label") == 0 || strcmp(tag, "sup") == 0 || strcmp(tag, "sub") == 0 || strcmp(tag, "kbd") == 0 ||
           strcmp(tag, "samp") == 0 || strcmp(tag, "button") == 0 || strcmp(tag, "input") == 0 || strcmp(tag, "select") == 0 ||
           strcmp(tag, "option") == 0 || strcmp(tag, "output") == 0;
}

bool BrowserApp::isVoidTag(const char* tag) const {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "br") == 0 || strcmp(tag, "hr") == 0 || strcmp(tag, "img") == 0 || strcmp(tag, "meta") == 0 ||
           strcmp(tag, "link") == 0 || strcmp(tag, "input") == 0 || strcmp(tag, "source") == 0 || strcmp(tag, "track") == 0 ||
           strcmp(tag, "wbr") == 0 || strcmp(tag, "base") == 0 || strcmp(tag, "area") == 0 || strcmp(tag, "col") == 0 ||
           strcmp(tag, "embed") == 0 || strcmp(tag, "param") == 0;
}

bool BrowserApp::isIgnoredNonVisualTag(const char* tag) const {
    if (!tag || !tag[0]) return false;
    return strcmp(tag, "head") == 0 || strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 || strcmp(tag, "base") == 0 ||
           strcmp(tag, "noscript") == 0 || strcmp(tag, "template") == 0 || strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}

bool BrowserApp::hasAttrFlag(const char* attrs, const char* attr) const {
    if (!attrs || !attrs[0] || !attr || !attr[0]) return false;

    char value[8] = "";
    if (extractAttr(attrs, attr, value, sizeof(value))) {
        if (value[0] == '\0') return true;
        char lower[8] = "";
        copyTrim(lower, sizeof(lower), value, sizeof(lower) - 1);
        toLowerInPlace(lower);
        return strcmp(lower, "true") == 0 || strcmp(lower, "1") == 0 || strcmp(lower, attr) == 0;
    }

    char lowerAttrs[140] = "";
    char lowerAttr[24] = "";
    copyTrim(lowerAttrs, sizeof(lowerAttrs), attrs, sizeof(lowerAttrs) - 1);
    copyTrim(lowerAttr, sizeof(lowerAttr), attr, sizeof(lowerAttr) - 1);
    toLowerInPlace(lowerAttrs);
    toLowerInPlace(lowerAttr);

    const char* p = strstr(lowerAttrs, lowerAttr);
    if (!p) return false;
    const char prev = (p > lowerAttrs) ? p[-1] : ' ';
    const char next = p[strlen(lowerAttr)];
    const bool prevOk = prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r';
    const bool nextOk = next == '\0' || next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '=';
    return prevOk && nextOk;
}

void BrowserApp::appendListPrefix(const CssStyle& style) {
    char prefix[20] = "";
    if (listDepth_ > 0 && listDepth_ <= 4 && listOrdered_[listDepth_ - 1]) {
        const uint16_t index = listCounter_[listDepth_ - 1] + 1;
        listCounter_[listDepth_ - 1] = index;
        snprintf(prefix, sizeof(prefix), "%u.", static_cast<unsigned>(index));
    } else {
        strlcpy(prefix, "*", sizeof(prefix));
    }
    layout_.appendText(prefix, style, "", 150);
}

void BrowserApp::appendControlBlock(const char* tag, const char* attrs, const CssStyle& style) {
    char line[96] = "";
    char label[48] = "";
    char value[48] = "";
    char src[96] = "";
    char name[32] = "";
    char placeholder[40] = "";

    extractAttr(attrs, "label", label, sizeof(label));
    extractAttr(attrs, "value", value, sizeof(value));
    if (!label[0]) extractAttr(attrs, "title", label, sizeof(label));
    if (!label[0]) extractAttr(attrs, "alt", label, sizeof(label));
    extractAttr(attrs, "src", src, sizeof(src));
    extractAttr(attrs, "name", name, sizeof(name));
    extractAttr(attrs, "placeholder", placeholder, sizeof(placeholder));

    const bool disabled = hasAttrFlag(attrs, "disabled");
    const bool checked = hasAttrFlag(attrs, "checked");
    const bool selected = hasAttrFlag(attrs, "selected");
    const bool required = hasAttrFlag(attrs, "required");
    const bool readonly = hasAttrFlag(attrs, "readonly");
    const bool multiple = hasAttrFlag(attrs, "multiple");

    if (strcmp(tag, "form") == 0) {
        snprintf(line, sizeof(line), "[form %s]", name[0] ? name : "");
    } else if (strcmp(tag, "fieldset") == 0) {
        strlcpy(line, "[fieldset]", sizeof(line));
    } else if (strcmp(tag, "legend") == 0) {
        snprintf(line, sizeof(line), "[%s]", label[0] ? label : "legend");
    } else if (strcmp(tag, "label") == 0 && label[0]) {
        snprintf(line, sizeof(line), "%s:", label);
    } else if (strcmp(tag, "input") == 0) {
        char type[20] = "";
        extractAttr(attrs, "type", type, sizeof(type));
        toLowerInPlace(type);
        if (!type[0]) strlcpy(type, "text", sizeof(type));

        if (strcmp(type, "checkbox") == 0) {
            snprintf(line, sizeof(line), "[%c] %s", checked ? 'x' : ' ', label[0] ? label : "checkbox");
        } else if (strcmp(type, "radio") == 0) {
            snprintf(line, sizeof(line), "(%c) %s", checked ? '*' : ' ', label[0] ? label : "radio");
        } else if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 || strcmp(type, "reset") == 0) {
            snprintf(line, sizeof(line), "[ %s ]", value[0] ? value : (label[0] ? label : type));
        } else if (strcmp(type, "range") == 0 || strcmp(type, "number") == 0) {
            char minV[12] = "";
            char maxV[12] = "";
            extractAttr(attrs, "min", minV, sizeof(minV));
            extractAttr(attrs, "max", maxV, sizeof(maxV));
            snprintf(line, sizeof(line), "[%s %s (%s..%s)]", type, value[0] ? value : "0", minV[0] ? minV : "0", maxV[0] ? maxV : "100");
        } else if (strcmp(type, "date") == 0 || strcmp(type, "time") == 0 || strcmp(type, "datetime-local") == 0 ||
                   strcmp(type, "month") == 0 || strcmp(type, "week") == 0 || strcmp(type, "color") == 0 ||
                   strcmp(type, "email") == 0 || strcmp(type, "tel") == 0 || strcmp(type, "url") == 0 || strcmp(type, "search") == 0 ||
                   strcmp(type, "password") == 0 || strcmp(type, "file") == 0) {
            const char* shown = value[0] ? value : (placeholder[0] ? placeholder : "");
            snprintf(line, sizeof(line), "[%s: %s]", type, shown);
        } else {
            const char* shown = value[0] ? value : (placeholder[0] ? placeholder : "");
            snprintf(line, sizeof(line), "[%s: %s]", type, shown);
        }
    } else if (strcmp(tag, "button") == 0) {
        snprintf(line, sizeof(line), "[ %s ]", value[0] ? value : (label[0] ? label : "button"));
    } else if (strcmp(tag, "select") == 0) {
        snprintf(line, sizeof(line), "[select%s %s]", multiple ? "*" : "", name[0] ? name : "");
    } else if (strcmp(tag, "optgroup") == 0) {
        snprintf(line, sizeof(line), "  <%s>", label[0] ? label : "group");
    } else if (strcmp(tag, "option") == 0) {
        const char marker = selected ? 'x' : ' ';
        snprintf(line, sizeof(line), "  [%c] %s", marker, value[0] ? value : (label[0] ? label : "option"));
    } else if (strcmp(tag, "textarea") == 0) {
        snprintf(line, sizeof(line), "[textarea %s]", placeholder[0] ? placeholder : (value[0] ? value : ""));
    } else if (strcmp(tag, "datalist") == 0) {
        snprintf(line, sizeof(line), "[datalist %s]", name[0] ? name : "");
    } else if (strcmp(tag, "video") == 0 || strcmp(tag, "audio") == 0 || strcmp(tag, "source") == 0 || strcmp(tag, "track") == 0) {
        char srcset[96] = "";
        if (!src[0]) extractAttr(attrs, "srcset", srcset, sizeof(srcset));
        const char* mediaSrc = src[0] ? src : srcset;
        snprintf(line, sizeof(line), "[%s %s]", tag, mediaSrc[0] ? mediaSrc : "stream");
    } else if (strcmp(tag, "canvas") == 0 || strcmp(tag, "svg") == 0 || strcmp(tag, "math") == 0) {
        snprintf(line, sizeof(line), "[%s]", tag);
    } else if (strcmp(tag, "iframe") == 0 || strcmp(tag, "embed") == 0 || strcmp(tag, "object") == 0) {
        snprintf(line, sizeof(line), "[%s %s]", tag, src[0] ? src : "embedded");
    } else if (strcmp(tag, "dialog") == 0) {
        strlcpy(line, "[dialog]", sizeof(line));
    } else if (strcmp(tag, "output") == 0) {
        snprintf(line, sizeof(line), "[output %s]", value[0] ? value : (label[0] ? label : ""));
    } else if (strcmp(tag, "progress") == 0 || strcmp(tag, "meter") == 0) {
        char v[20] = "";
        char m[20] = "";
        extractAttr(attrs, "value", v, sizeof(v));
        extractAttr(attrs, "max", m, sizeof(m));
        snprintf(line, sizeof(line), "[%s %s/%s]", tag, v[0] ? v : "0", m[0] ? m : "100");
    }

    if (line[0]) {
        if (disabled || readonly || required) {
            char suffix[20] = "";
            if (disabled) strlcat(suffix, " dis", sizeof(suffix));
            if (readonly) strlcat(suffix, " ro", sizeof(suffix));
            if (required) strlcat(suffix, " req", sizeof(suffix));
            if (suffix[0]) {
                size_t room = sizeof(line) - strlen(line) - 1;
                if (room > 0) strlcat(line, suffix, sizeof(line));
            }
        }
        const uint8_t before = layout_.blockCount();
        if (layout_.appendText(line, style, "", 150)) {
            dirtyContent_ = true;
            if (layout_.blockCount() > before) {
                registerInteractiveControl(before, tag, attrs);
            }
        }
    }
}

void BrowserApp::beginFormScope(const char* attrs) {
    if (formScopeCounter_ == 0xFF) {
        formScopeCounter_ = 1;
    } else {
        ++formScopeCounter_;
        if (formScopeCounter_ == 0) formScopeCounter_ = 1;
    }
    activeFormScope_ = formScopeCounter_;
    activeFormAction_[0] = '\0';
    strlcpy(activeFormMethod_, "get", sizeof(activeFormMethod_));
    extractAttr(attrs, "action", activeFormAction_, sizeof(activeFormAction_));
    char method[12] = "";
    if (extractAttr(attrs, "method", method, sizeof(method))) {
        toLowerInPlace(method);
        if (strcmp(method, "post") == 0) {
            strlcpy(activeFormMethod_, "post", sizeof(activeFormMethod_));
        }
    }
}

void BrowserApp::endFormScope() {
    activeFormScope_ = 0;
    activeFormAction_[0] = '\0';
    strlcpy(activeFormMethod_, "get", sizeof(activeFormMethod_));
}

bool BrowserApp::registerInteractiveControl(uint8_t blockIndex, const char* tag, const char* attrs) {
    if (!tag || !attrs || formControlCount_ >= kFormControlMax) return false;

    bool interactive = false;
    char inputType[20] = "";
    if (strcmp(tag, "input") == 0) {
        extractAttr(attrs, "type", inputType, sizeof(inputType));
        toLowerInPlace(inputType);
        if (!inputType[0]) strlcpy(inputType, "text", sizeof(inputType));
        interactive = true;
    } else if (strcmp(tag, "button") == 0) {
        extractAttr(attrs, "type", inputType, sizeof(inputType));
        toLowerInPlace(inputType);
        if (!inputType[0]) strlcpy(inputType, "submit", sizeof(inputType));
        interactive = true;
    } else if (strcmp(tag, "textarea") == 0 || strcmp(tag, "select") == 0 || strcmp(tag, "option") == 0) {
        interactive = true;
    }

    if (!interactive) return false;

    FormControl& c = formControls_[formControlCount_];
    c.used = true;
    c.blockIndex = blockIndex;
    c.formScope = activeFormScope_;
    copyTrim(c.tag, sizeof(c.tag), tag, sizeof(c.tag) - 1);
    copyTrim(c.inputType, sizeof(c.inputType), inputType[0] ? inputType : tag, sizeof(c.inputType) - 1);
    extractAttr(attrs, "name", c.name, sizeof(c.name));
    extractAttr(attrs, "value", c.value, sizeof(c.value));
    c.disabled = hasAttrFlag(attrs, "disabled");
    c.readonly = hasAttrFlag(attrs, "readonly");
    c.required = hasAttrFlag(attrs, "required");
    c.checked = hasAttrFlag(attrs, "checked") || hasAttrFlag(attrs, "selected");
    extractAttr(attrs, "onclick", c.onclick, sizeof(c.onclick));
    extractAttr(attrs, "onchange", c.onchange, sizeof(c.onchange));

    c.action[0] = '\0';
    char formaction[kUrlMaxLen] = "";
    if (extractAttr(attrs, "formaction", formaction, sizeof(formaction))) {
        copyTrim(c.action, sizeof(c.action), formaction, sizeof(c.action) - 1);
    } else if (activeFormAction_[0]) {
        copyTrim(c.action, sizeof(c.action), activeFormAction_, sizeof(c.action) - 1);
    }

    strlcpy(c.method, activeFormMethod_, sizeof(c.method));
    char formmethod[12] = "";
    if (extractAttr(attrs, "formmethod", formmethod, sizeof(formmethod))) {
        toLowerInPlace(formmethod);
        if (strcmp(formmethod, "post") == 0) {
            strlcpy(c.method, "post", sizeof(c.method));
        } else {
            strlcpy(c.method, "get", sizeof(c.method));
        }
    }

    char token[32] = "";
    snprintf(token, sizeof(token), "ctrl:%u", static_cast<unsigned>(formControlCount_));
    layout_.setBlockMeta(blockIndex, 6, token, "");
    ++formControlCount_;
    return true;
}

bool BrowserApp::openControlEditor(uint8_t controlIndex) {
    if (controlIndex >= formControlCount_) return false;
    FormControl& c = formControls_[controlIndex];
    if (c.disabled || c.readonly) return false;

    const bool textLike =
        strcmp(c.tag, "textarea") == 0 || strcmp(c.tag, "select") == 0 || strcmp(c.tag, "option") == 0 ||
        strcmp(c.inputType, "text") == 0 || strcmp(c.inputType, "search") == 0 || strcmp(c.inputType, "email") == 0 ||
        strcmp(c.inputType, "url") == 0 || strcmp(c.inputType, "tel") == 0 || strcmp(c.inputType, "password") == 0 ||
        strcmp(c.inputType, "number") == 0 || strcmp(c.inputType, "date") == 0 || strcmp(c.inputType, "time") == 0 ||
        strcmp(c.inputType, "datetime-local") == 0 || strcmp(c.inputType, "month") == 0 || strcmp(c.inputType, "week") == 0 ||
        strcmp(c.inputType, "color") == 0 || strcmp(c.inputType, "file") == 0;

    if (!textLike) return false;

    copyTrim(formKeyboardInitial_, sizeof(formKeyboardInitial_), c.value, sizeof(formKeyboardInitial_) - 1);
    formKeyboardControlIndex_ = controlIndex;
    formKeyboardPending_ = true;
    return true;
}

bool BrowserApp::submitControl(uint8_t controlIndex) {
    if (controlIndex >= formControlCount_) return false;
    const FormControl& trigger = formControls_[controlIndex];

    char action[kUrlMaxLen] = "";
    if (trigger.action[0]) {
        copyTrim(action, sizeof(action), trigger.action, sizeof(action) - 1);
    } else {
        copyTrim(action, sizeof(action), url_, sizeof(action) - 1);
    }

    char query[1400] = "";
    for (uint8_t i = 0; i < formControlCount_; ++i) {
        const FormControl& c = formControls_[i];
        if (!c.used || c.disabled) continue;
        if (trigger.formScope != c.formScope) continue;
        if (!c.name[0]) continue;
        if (strcmp(c.inputType, "submit") == 0 || strcmp(c.inputType, "button") == 0 || strcmp(c.inputType, "reset") == 0) continue;
        if ((strcmp(c.inputType, "checkbox") == 0 || strcmp(c.inputType, "radio") == 0) && !c.checked) continue;

        if (query[0]) strlcat(query, "&", sizeof(query));
        strlcat(query, c.name, sizeof(query));
        strlcat(query, "=", sizeof(query));
        strlcat(query, c.value[0] ? c.value : "on", sizeof(query));
    }

    if (strcmp(trigger.method, "post") == 0) {
        snprintf(status_, sizeof(status_), "POST %s (%uB)", action, static_cast<unsigned>(strlen(query)));
        dirtyProgress_ = true;
        return true;
    }

    char resolved[kUrlMaxLen] = "";
    if (resolveUrl(url_, action, resolved, sizeof(resolved))) {
        copyTrim(action, sizeof(action), resolved, sizeof(action) - 1);
    }

    if (query[0]) {
        const bool hasQuery = strchr(action, '?') != nullptr;
        strlcat(action, hasQuery ? "&" : "?", sizeof(action));
        strlcat(action, query, sizeof(action));
    }

    openUrl(action, true);
    return true;
}

bool BrowserApp::consumeFormKeyboardRequest(char* outInitial, size_t outLen) {
    if (!outInitial || outLen == 0 || !formKeyboardPending_ || formKeyboardControlIndex_ == 0xFF) return false;
    copyTrim(outInitial, outLen, formKeyboardInitial_, outLen - 1);
    formKeyboardPending_ = false;
    return true;
}

void BrowserApp::applyFormKeyboardValue(const char* value, bool accepted) {
    if (!accepted || formKeyboardControlIndex_ == 0xFF || formKeyboardControlIndex_ >= formControlCount_) {
        formKeyboardControlIndex_ = 0xFF;
        formKeyboardInitial_[0] = '\0';
        return;
    }

    FormControl& c = formControls_[formKeyboardControlIndex_];
    copyTrim(c.value, sizeof(c.value), value ? value : "", sizeof(c.value) - 1);
    if (c.onchange[0]) {
        executeJavaScript(c.onchange, "form-onchange");
    }
    snprintf(status_, sizeof(status_), "%s=%s", c.name[0] ? c.name : c.tag, c.value);
    dirtyProgress_ = true;
    dirtyContent_ = true;
    formKeyboardControlIndex_ = 0xFF;
    formKeyboardInitial_[0] = '\0';
}

bool BrowserApp::extractJsStringLiteral(const char* src, char* out, size_t outLen) const {
    if (!src || !out || outLen == 0) return false;
    out[0] = '\0';

    const char* q = src;
    while (*q && *q != '\'' && *q != '"') ++q;
    if (!*q) return false;

    const char quote = *q;
    ++q;
    size_t w = 0;
    bool escaped = false;
    while (*q && w + 1 < outLen) {
        if (escaped) {
            out[w++] = *q;
            escaped = false;
            ++q;
            continue;
        }
        if (*q == '\\') {
            escaped = true;
            ++q;
            continue;
        }
        if (*q == quote) break;
        out[w++] = *q;
        ++q;
    }
    out[w] = '\0';
    decodeEntities(out);
    return out[0] != '\0';
}

bool BrowserApp::applyJsNavigation(const char* value) {
    if (!value || !value[0]) return false;
    char href[kUrlMaxLen] = "";
    copyTrim(href, sizeof(href), value, sizeof(href) - 1);
    decodeEntities(href);

    size_t start = 0;
    while (href[start] == ' ' || href[start] == '\t' || href[start] == '\n' || href[start] == '\r') ++start;
    if (start > 0) {
        memmove(href, href + start, strlen(href + start) + 1);
    }
    size_t len = strlen(href);
    while (len > 0 && (href[len - 1] == ' ' || href[len - 1] == '\t' || href[len - 1] == '\n' || href[len - 1] == '\r')) {
        href[len - 1] = '\0';
        --len;
    }
    if (!href[0]) return false;

    char lower[kUrlMaxLen] = "";
    copyTrim(lower, sizeof(lower), href, sizeof(lower) - 1);
    toLowerInPlace(lower);
    if (startsWith(lower, "javascript:")) {
        executeJavaScript(href + 11, "javascript-url");
        return true;
    }

    char resolved[kUrlMaxLen] = "";
    if (resolveUrl(url_, href, resolved, sizeof(resolved))) {
        openUrl(resolved, true);
    } else {
        openUrl(href, true);
    }
    return true;
}

void BrowserApp::applyJsAlert(const char* value) {
    if (!value || !value[0]) return;
    char msg[44] = "";
    copyTrim(msg, sizeof(msg), value, sizeof(msg) - 1);
    decodeEntities(msg);
    copyTrim(status_, sizeof(status_), msg, sizeof(status_) - 1);
    dirtyProgress_ = true;
}

bool BrowserApp::executeJsStatement(const char* stmt, uint8_t depth) {
    if (!stmt || !stmt[0]) return false;

    char work[220] = "";
    copyTrim(work, sizeof(work), stmt, sizeof(work) - 1);
    size_t start = 0;
    while (work[start] == ' ' || work[start] == '\t' || work[start] == '\n' || work[start] == '\r') ++start;
    if (start > 0) {
        memmove(work, work + start, strlen(work + start) + 1);
    }
    size_t len = strlen(work);
    while (len > 0 && (work[len - 1] == ' ' || work[len - 1] == '\t' || work[len - 1] == '\n' || work[len - 1] == '\r')) {
        work[len - 1] = '\0';
        --len;
    }
    if (!work[0]) return false;

    char lower[220] = "";
    copyTrim(lower, sizeof(lower), work, sizeof(lower) - 1);
    toLowerInPlace(lower);

    if (strstr(lower, "while(") || strstr(lower, "for(") || strstr(lower, "do{") || strstr(lower, "function*")) {
        strlcpy(status_, "JS loop blocked", sizeof(status_));
        dirtyProgress_ = true;
        return true;
    }

    if (startsWith(lower, "alert(")) {
        char value[64] = "";
        if (extractJsStringLiteral(work, value, sizeof(value))) {
            applyJsAlert(value);
        }
        return true;
    }

    if (startsWith(lower, "console.log(") || startsWith(lower, "console.error(") || startsWith(lower, "console.warn(")) {
        strlcpy(status_, "JS console", sizeof(status_));
        dirtyProgress_ = true;
        return true;
    }

    if (startsWith(lower, "document.title") || startsWith(lower, "window.document.title")) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char title[44] = "";
            if (extractJsStringLiteral(eq + 1, title, sizeof(title))) {
                copyTrim(title_, sizeof(title_), title, sizeof(title_) - 1);
                copyTrim(status_, sizeof(status_), title, sizeof(status_) - 1);
                dirtyChrome_ = true;
                dirtyProgress_ = true;
            }
        }
        return true;
    }

    if (startsWith(lower, "document.write(") || startsWith(lower, "document.writeln(")) {
        char value[96] = "";
        if (extractJsStringLiteral(work, value, sizeof(value))) {
            CssStyle style = styleStack_[styleDepth_ > 0 ? static_cast<uint8_t>(styleDepth_ - 1) : 0];
            if (layout_.appendText(value, style, activeHref_, 150)) {
                dirtyContent_ = true;
            }
        }
        return true;
    }

    if (startsWith(lower, "history.back(")) {
        navigateHistory(-1);
        return true;
    }

    if (startsWith(lower, "history.forward(")) {
        navigateHistory(1);
        return true;
    }

    if (startsWith(lower, "scrollto(") || startsWith(lower, "window.scrollto(")) {
        const char* p = strchr(work, '(');
        if (p) {
            int x = 0;
            int y = 0;
            if (sscanf(p + 1, "%d,%d", &x, &y) == 2) {
                (void)x;
                const int16_t docH = layout_.docHeight();
                const int16_t maxScroll = docH > lastViewportH_ ? static_cast<int16_t>(docH - lastViewportH_) : 0;
                scrollY_ = clamp16(static_cast<int16_t>(y), 0, maxScroll);
                dirtyContent_ = true;
            }
        }
        return true;
    }

    if (startsWith(lower, "scrollby(") || startsWith(lower, "window.scrollby(")) {
        const char* p = strchr(work, '(');
        if (p) {
            int x = 0;
            int y = 0;
            if (sscanf(p + 1, "%d,%d", &x, &y) == 2) {
                (void)x;
                scroll(static_cast<int16_t>(y));
            }
        }
        return true;
    }

    if (startsWith(lower, "location.href") || startsWith(lower, "window.location.href") || startsWith(lower, "window.location=") || startsWith(lower, "location=")) {
        const char* eq = strchr(work, '=');
        if (eq) {
            char href[kUrlMaxLen] = "";
            if (extractJsStringLiteral(eq + 1, href, sizeof(href))) {
                applyJsNavigation(href);
            }
        }
        return true;
    }

    if (startsWith(lower, "location.assign(") || startsWith(lower, "location.replace(") || startsWith(lower, "window.open(")) {
        char href[kUrlMaxLen] = "";
        if (extractJsStringLiteral(work, href, sizeof(href))) {
            applyJsNavigation(href);
        }
        return true;
    }

    if (startsWith(lower, "settimeout(") || startsWith(lower, "setinterval(") || startsWith(lower, "eval(")) {
        char nested[220] = "";
        if (extractJsStringLiteral(work, nested, sizeof(nested))) {
            if (depth < 5) {
                executeJavaScript(nested, "js-nested");
            } else {
                strlcpy(status_, "JS depth limit", sizeof(status_));
                dirtyProgress_ = true;
            }
        }
        return true;
    }

    if (startsWith(lower, "fetch(") || startsWith(lower, "xmlhttprequest") || startsWith(lower, "websocket")) {
        strlcpy(status_, "JS net limited", sizeof(status_));
        dirtyProgress_ = true;
        return true;
    }

    if (startsWith(lower, "document.body.style") || startsWith(lower, "document.documentelement.style")) {
        strlcpy(status_, "JS style partial", sizeof(status_));
        dirtyProgress_ = true;
        return true;
    }

    return false;
}

void BrowserApp::executeJavaScript(const char* script, const char* source) {
    (void)source;
    if (!script || !script[0]) return;

    if (jsExecDepth_ >= 5) {
        strlcpy(status_, "JS depth blocked", sizeof(status_));
        dirtyProgress_ = true;
        return;
    }

    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 22000U) {
        strlcpy(status_, "JS low memory", sizeof(status_));
        dirtyProgress_ = true;
        return;
    }

    ++jsExecDepth_;

    char scriptWork[700] = "";
    copyTrim(scriptWork, sizeof(scriptWork), script, sizeof(scriptWork) - 1);
    if (strlen(script) >= sizeof(scriptWork) - 1) {
        strlcpy(status_, "JS truncated", sizeof(status_));
        dirtyProgress_ = true;
    }

    for (size_t i = 0; scriptWork[i]; ++i) {
        if (scriptWork[i] == '\n' || scriptWork[i] == '\r') scriptWork[i] = ';';
    }

    uint8_t ops = 0;
    char* save = nullptr;
    char* stmt = strtok_r(scriptWork, ";", &save);
    while (stmt && ops < 42) {
        executeJsStatement(stmt, jsExecDepth_);
        ++ops;
        stmt = strtok_r(nullptr, ";", &save);
    }

    if (stmt) {
        strlcpy(status_, "JS op limit", sizeof(status_));
        dirtyProgress_ = true;
    }

    if (jsExecDepth_ > 0) --jsExecDepth_;
}

bool BrowserApp::enqueueExternalResource(const char* url, bool script) {
    if (!url || !url[0] || externalQueueCount_ >= kExternalQueueMax) return false;

    char normalized[kUrlMaxLen] = "";
    copyTrim(normalized, sizeof(normalized), url, sizeof(normalized) - 1);
    for (uint8_t i = 0; i < externalQueueCount_; ++i) {
        if (externalQueue_[i].used && strcmp(externalQueue_[i].url, normalized) == 0 && externalQueue_[i].script == script) {
            return true;
        }
    }

    ExternalResource& item = externalQueue_[externalQueueCount_++];
    item.used = true;
    item.script = script;
    item.retries = 0;
    copyTrim(item.url, sizeof(item.url), normalized, sizeof(item.url) - 1);

    const char* mode = script ? "Queue JS" : "Queue CSS";
    strlcpy(status_, mode, sizeof(status_));
    dirtyProgress_ = true;
    return true;
}

bool BrowserApp::fetchExternalText(const char* url, char* out, size_t outLen, uint16_t timeoutMs, size_t bodyCap) const {
    if (!url || !url[0] || !out || outLen < 2) return false;
    out[0] = '\0';
    if (!isHttpUrl(url)) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 22000U) return false;

    size_t cap = bodyCap;
    if (cap + 1 > outLen) cap = outLen - 1;
    if (freeHeap < 32000U && cap > 360) cap = 360;
    else if (freeHeap < 52000U && cap > 620) cap = 620;
    else if (cap > 900) cap = 900;

    HTTPClient http;
    http.setTimeout(timeoutMs);
    if (!http.begin(url)) return false;

    const int code = http.GET();
    if (code < 200 || code >= 300) {
        http.end();
        return false;
    }

    Stream* stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return false;
    }

    const uint32_t started = millis();
    size_t w = 0;
    while (w < cap && millis() - started < static_cast<uint32_t>(timeoutMs + 300U)) {
        if (!http.connected() && stream->available() == 0) break;
        if (stream->available() == 0) {
            delay(1);
            continue;
        }

        const int c = stream->read();
        if (c < 0) continue;
        if (c == 0) continue;
        out[w++] = static_cast<char>(c);
    }
    out[w] = '\0';
    http.end();
    return w > 0;
}

void BrowserApp::cacheUnparsedJs(const char* src) {
    if (!src || !src[0] || deferredJsCount_ >= kDeferredCacheMax) return;
    copyTrim(deferredJsCache_[deferredJsCount_], sizeof(deferredJsCache_[0]), src, sizeof(deferredJsCache_[0]) - 1);
    ++deferredJsCount_;
}

void BrowserApp::cacheUnparsedCss(const char* src) {
    if (!src || !src[0] || deferredCssCount_ >= kDeferredCacheMax) return;
    copyTrim(deferredCssCache_[deferredCssCount_], sizeof(deferredCssCache_[0]), src, sizeof(deferredCssCache_[0]) - 1);
    ++deferredCssCount_;
}

void BrowserApp::flushDeferredCaches(uint32_t nowMs) {
    if (nowMs - deferredLastFlushMs_ < 220U) return;
    deferredLastFlushMs_ = nowMs;

    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 32000U) return;

    if (deferredCssCount_ > 0) {
        parseStylesheet(deferredCssCache_[0]);
        for (uint8_t i = 1; i < deferredCssCount_; ++i) {
            strlcpy(deferredCssCache_[i - 1], deferredCssCache_[i], sizeof(deferredCssCache_[0]));
        }
        if (deferredCssCount_ > 0) --deferredCssCount_;
        deferredCssCache_[deferredCssCount_][0] = '\0';
        dirtyContent_ = true;
        return;
    }

    if (deferredJsCount_ > 0) {
        executeJavaScript(deferredJsCache_[0], "deferred-js");
        for (uint8_t i = 1; i < deferredJsCount_; ++i) {
            strlcpy(deferredJsCache_[i - 1], deferredJsCache_[i], sizeof(deferredJsCache_[0]));
        }
        if (deferredJsCount_ > 0) --deferredJsCount_;
        deferredJsCache_[deferredJsCount_][0] = '\0';
    }
}

void BrowserApp::processExternalResourceQueue(uint32_t nowMs) {
    if (loading_) return;
    if (externalQueueCount_ == 0) {
        externalLoading_ = false;
        externalLoadingUrl_[0] = '\0';
        return;
    }
    if (nowMs - externalLastAttemptMs_ < 120U) return;

    ExternalResource current = externalQueue_[0];
    for (uint8_t i = 1; i < externalQueueCount_; ++i) {
        externalQueue_[i - 1] = externalQueue_[i];
    }
    if (externalQueueCount_ > 0) --externalQueueCount_;
    if (externalQueueCount_ < kExternalQueueMax) {
        externalQueue_[externalQueueCount_] = ExternalResource{};
    }

    externalLoading_ = true;
    externalLastAttemptMs_ = nowMs;
    copyTrim(externalLoadingUrl_, sizeof(externalLoadingUrl_), current.url, sizeof(externalLoadingUrl_) - 1);

    char shortUrl[32] = "";
    copyTrim(shortUrl, sizeof(shortUrl), externalLoadingUrl_, sizeof(shortUrl) - 1);
    snprintf(status_, sizeof(status_), "%s %s", current.script ? "Loading JS" : "Loading CSS", shortUrl);
    dirtyProgress_ = true;

    char payload[960] = "";
    const bool ok = fetchExternalText(current.url, payload, sizeof(payload), 1600, 820);
    if (!ok) {
        if (current.retries < 1 && externalQueueCount_ < kExternalQueueMax) {
            ++current.retries;
            externalQueue_[externalQueueCount_++] = current;
            strlcpy(status_, current.script ? "Retry JS" : "Retry CSS", sizeof(status_));
        } else {
            strlcpy(status_, current.script ? "JS src failed" : "CSS href failed", sizeof(status_));
            if (current.script) {
                cacheUnparsedJs(current.url);
            } else {
                cacheUnparsedCss(current.url);
            }
        }
        dirtyProgress_ = true;
        externalLoading_ = false;
        return;
    }

    if (current.script) {
        executeJavaScript(payload, current.url);
    } else {
        parseStylesheet(payload);
        dirtyContent_ = true;
    }

    externalLoading_ = false;
    if (externalQueueCount_ == 0) {
        externalLoadingUrl_[0] = '\0';
    }
}

void BrowserApp::processToken(const Token& tok) {
    if (styleDepth_ == 0) {
        styleStack_[0] = CssStyle{};
        styleDepth_ = 1;
    }

    if (tok.type == TokenType::Text) {
        if (inScriptTag_) {
            const size_t chunkLen = strlen(tok.text);
            if (scriptBufferLen_ + chunkLen + 1 < sizeof(scriptBuffer_)) {
                strlcat(scriptBuffer_, tok.text, sizeof(scriptBuffer_));
                scriptBufferLen_ = static_cast<uint16_t>(strlen(scriptBuffer_));
            } else {
                cacheUnparsedJs(tok.text);
                strlcpy(status_, "JS script cached", sizeof(status_));
                dirtyProgress_ = true;
            }
            return;
        }

        if (inStyleTag_) {
            if (cssBufferLen_ + strlen(tok.text) + 1 < sizeof(cssBuffer_)) {
                strlcat(cssBuffer_, tok.text, sizeof(cssBuffer_));
                cssBufferLen_ = static_cast<uint16_t>(strlen(cssBuffer_));
            } else {
                cacheUnparsedCss(tok.text);
                strlcpy(status_, "CSS cached", sizeof(status_));
                dirtyProgress_ = true;
            }
            return;
        }

        if (inTextareaTag_ && activeTextareaControl_ != 0xFF && activeTextareaControl_ < formControlCount_) {
            char text[220] = "";
            copyTrim(text, sizeof(text), tok.text, sizeof(text) - 1);
            decodeEntities(text);
            if (text[0]) {
                FormControl& c = formControls_[activeTextareaControl_];
                if (c.value[0]) strlcat(c.value, "\n", sizeof(c.value));
                strlcat(c.value, text, sizeof(c.value));
            }
            return;
        }

        char text[96] = "";
        copyTrim(text, sizeof(text), tok.text, sizeof(text) - 1);
        const bool preserveWs = styleStack_[styleDepth_ - 1].pre || inTitleTag_;
        if (!preserveWs) {
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
        }

        decodeEntities(text);
        if (!text[0]) return;

        if (inTitleTag_) {
            char merged[44] = "";
            snprintf(merged, sizeof(merged), "%s%s%s", titleBuffer_, titleBuffer_[0] ? " " : "", text);
            copyTrim(titleBuffer_, sizeof(titleBuffer_), merged, sizeof(titleBuffer_) - 1);
            return;
        }

        if (layout_.appendText(text, styleStack_[styleDepth_ - 1], activeHref_, 150)) {
            dirtyContent_ = true;
        }
        return;
    }

    if (tok.type == TokenType::EndTag) {
        if (strcmp(tok.name, "style") == 0) {
            inStyleTag_ = false;
            if (cssBuffer_[0]) {
                parseStylesheet(cssBuffer_);
                cssBuffer_[0] = '\0';
                cssBufferLen_ = 0;
            }
            return;
        }

        if (strcmp(tok.name, "script") == 0) {
            inScriptTag_ = false;
            if (scriptBuffer_[0]) {
                executeJavaScript(scriptBuffer_, "script-tag");
                scriptBuffer_[0] = '\0';
                scriptBufferLen_ = 0;
            }
            return;
        }

        if (strcmp(tok.name, "title") == 0) {
            inTitleTag_ = false;
            if (titleBuffer_[0]) {
                copyTrim(title_, sizeof(title_), titleBuffer_, sizeof(title_) - 1);
                copyTrim(status_, sizeof(status_), titleBuffer_, sizeof(status_) - 1);
                dirtyChrome_ = true;
                dirtyProgress_ = true;
            }
            return;
        }

        if (strcmp(tok.name, "a") == 0) {
            activeHref_[0] = '\0';
        }

        if (strcmp(tok.name, "ul") == 0 || strcmp(tok.name, "ol") == 0) {
            if (listDepth_ > 0) --listDepth_;
        }

        if (strcmp(tok.name, "form") == 0) {
            endFormScope();
        }

        if (strcmp(tok.name, "textarea") == 0) {
            inTextareaTag_ = false;
            activeTextareaControl_ = 0xFF;
        }

        if (isBlockTag(tok.name) || strcmp(tok.name, "hr") == 0) {
            if (layout_.appendBreak()) dirtyContent_ = true;
        }

        const bool styleCarrierEnd = isInlineTag(tok.name) || isBlockTag(tok.name);
        if (styleDepth_ > 1 && styleCarrierEnd && !isVoidTag(tok.name)) {
            --styleDepth_;
        }
        return;
    }

    if (tok.type != TokenType::StartTag && tok.type != TokenType::SelfClosingTag) {
        return;
    }

    if (strcmp(tok.name, "script") == 0) {
        scriptBuffer_[0] = '\0';
        scriptBufferLen_ = 0;
        char src[100] = "";
        if (extractAttr(tok.attrs, "src", src, sizeof(src))) {
            char resolved[kUrlMaxLen] = "";
            if (resolveUrl(url_, src, resolved, sizeof(resolved))) {
                enqueueExternalResource(resolved, true);
            } else if (isHttpUrl(src)) {
                enqueueExternalResource(src, true);
            } else {
                cacheUnparsedJs(src);
                strlcpy(status_, "JS src cached", sizeof(status_));
                dirtyProgress_ = true;
            }
            inScriptTag_ = false;
            return;
        }
        inScriptTag_ = true;
        return;
    }

    if (strcmp(tok.name, "style") == 0) {
        inStyleTag_ = true;
        cssBuffer_[0] = '\0';
        cssBufferLen_ = 0;
        return;
    }

    if (strcmp(tok.name, "title") == 0) {
        inTitleTag_ = true;
        titleBuffer_[0] = '\0';
        return;
    }

    if (strcmp(tok.name, "base") == 0) {
        char href[kUrlMaxLen] = "";
        if (extractAttr(tok.attrs, "href", href, sizeof(href))) {
            char resolved[kUrlMaxLen] = "";
            if (resolveUrl(url_, href, resolved, sizeof(resolved))) {
                copyTrim(baseHref_, sizeof(baseHref_), resolved, sizeof(baseHref_) - 1);
            } else {
                copyTrim(baseHref_, sizeof(baseHref_), href, sizeof(baseHref_) - 1);
            }
        }
        return;
    }

    if (strcmp(tok.name, "link") == 0) {
        char rel[40] = "";
        char href[kUrlMaxLen] = "";
        extractAttr(tok.attrs, "rel", rel, sizeof(rel));
        extractAttr(tok.attrs, "href", href, sizeof(href));
        toLowerInPlace(rel);
        if (href[0] && strstr(rel, "stylesheet")) {
            char resolved[kUrlMaxLen] = "";
            if (resolveUrl(url_, href, resolved, sizeof(resolved))) {
                enqueueExternalResource(resolved, false);
            } else if (isHttpUrl(href)) {
                enqueueExternalResource(href, false);
            } else {
                cacheUnparsedCss(href);
            }
        }
        return;
    }

    if (inScriptTag_ || isIgnoredNonVisualTag(tok.name)) {
        return;
    }

    char nodeId[20] = "";
    char nodeCls[24] = "";
    if (cssRuleCount_ > 0) {
        extractAttr(tok.attrs, "id", nodeId, sizeof(nodeId));
        extractAttr(tok.attrs, "class", nodeCls, sizeof(nodeCls));
        toLowerInPlace(nodeId);
        toLowerInPlace(nodeCls);
    }

    CssStyle style = css_.inherit(styleStack_[styleDepth_ - 1]);
    css_.applyTagDefaults(tok.name, style);
    if (cssRuleCount_ > 0) {
        applyStylesheet(tok.name, nodeId, nodeCls, style);
    }
    if (hasAttrFlag(tok.attrs, "hidden") || hasAttrFlag(tok.attrs, "aria-hidden")) {
        style.display = DisplayMode::Hidden;
    }

    char inlineStyle[100] = "";
    if (extractAttr(tok.attrs, "style", inlineStyle, sizeof(inlineStyle))) {
        css_.applyInline(inlineStyle, style);
    }

    char onLoadJs[120] = "";
    char onErrorJs[120] = "";
    char onChangeJs[120] = "";
    if (extractAttr(tok.attrs, "onload", onLoadJs, sizeof(onLoadJs))) {
        executeJavaScript(onLoadJs, "onload");
    }
    if (extractAttr(tok.attrs, "onerror", onErrorJs, sizeof(onErrorJs))) {
        executeJavaScript(onErrorJs, "onerror");
    }
    if (extractAttr(tok.attrs, "onchange", onChangeJs, sizeof(onChangeJs))) {
        executeJavaScript(onChangeJs, "onchange");
    }

    if (strcmp(tok.name, "form") == 0) {
        beginFormScope(tok.attrs);
        char onSubmitJs[120] = "";
        if (extractAttr(tok.attrs, "onsubmit", onSubmitJs, sizeof(onSubmitJs))) {
            executeJavaScript(onSubmitJs, "onsubmit");
        }
    }

    if (strcmp(tok.name, "a") == 0) {
        extractAttr(tok.attrs, "href", activeHref_, sizeof(activeHref_));
    }

    if (strcmp(tok.name, "ul") == 0 || strcmp(tok.name, "ol") == 0) {
        if (layout_.appendBreak()) dirtyContent_ = true;
        if (listDepth_ < 4) {
            listOrdered_[listDepth_] = strcmp(tok.name, "ol") == 0;
            listCounter_[listDepth_] = 0;
            ++listDepth_;
        }
    } else if (strcmp(tok.name, "li") == 0) {
        if (layout_.appendBreak()) dirtyContent_ = true;
        appendListPrefix(style);
        dirtyContent_ = true;
    } else if (strcmp(tok.name, "br") == 0) {
        if (layout_.appendBreak()) dirtyContent_ = true;
    } else if (strcmp(tok.name, "hr") == 0) {
        if (layout_.appendText("----------------", style, "", 150)) dirtyContent_ = true;
        if (layout_.appendBreak()) dirtyContent_ = true;
    } else if (strcmp(tok.name, "table") == 0 || strcmp(tok.name, "tr") == 0 || strcmp(tok.name, "dl") == 0) {
        if (layout_.appendBreak()) dirtyContent_ = true;
    } else if (strcmp(tok.name, "td") == 0 || strcmp(tok.name, "th") == 0 || strcmp(tok.name, "dd") == 0) {
        if (layout_.appendText("|", style, "", 150)) dirtyContent_ = true;
    } else if (strcmp(tok.name, "dt") == 0) {
        CssStyle dtStyle = style;
        dtStyle.bold = true;
        if (layout_.appendBreak()) dirtyContent_ = true;
        if (layout_.appendText("-", dtStyle, "", 150)) dirtyContent_ = true;
    } else if (strcmp(tok.name, "img") == 0) {
        char src[100] = "";
        extractAttr(tok.attrs, "src", src, sizeof(src));
        if (!src[0]) {
            char srcset[100] = "";
            if (extractAttr(tok.attrs, "srcset", srcset, sizeof(srcset))) {
                const char* comma = strchr(srcset, ',');
                if (comma) {
                    char first[100] = "";
                    size_t n = static_cast<size_t>(comma - srcset);
                    if (n >= sizeof(first)) n = sizeof(first) - 1;
                    memcpy(first, srcset, n);
                    first[n] = '\0';
                    char* sp = strchr(first, ' ');
                    if (sp) *sp = '\0';
                    copyTrim(src, sizeof(src), first, sizeof(src) - 1);
                } else {
                    char* sp = strchr(srcset, ' ');
                    if (sp) *sp = '\0';
                    copyTrim(src, sizeof(src), srcset, sizeof(src) - 1);
                }
            }
        }

        char ws[12] = "";
        char hs[12] = "";
        extractAttr(tok.attrs, "width", ws, sizeof(ws));
        extractAttr(tok.attrs, "height", hs, sizeof(hs));
        uint16_t iw = ws[0] ? static_cast<uint16_t>(atoi(ws)) : 96;
        uint16_t ih = hs[0] ? static_cast<uint16_t>(atoi(hs)) : 56;
        if (iw < 12) iw = 12;
        if (ih < 12) ih = 12;
        image_.fitToViewport(iw, ih, 150, 90);
        if (!image_.validateDimensions(iw, ih, 150, 90)) {
            iw = 80;
            ih = 40;
        }

        if (src[0]) {
            if (layout_.appendImage(src, iw, ih, style, 150)) {
                dirtyContent_ = true;
            }
        } else {
            char alt[64] = "";
            extractAttr(tok.attrs, "alt", alt, sizeof(alt));
            if (!alt[0]) strlcpy(alt, "[image]", sizeof(alt));
            if (layout_.appendText(alt, style, "", 150)) dirtyContent_ = true;
        }
    } else if (strcmp(tok.name, "form") == 0 || strcmp(tok.name, "fieldset") == 0 || strcmp(tok.name, "legend") == 0 ||
               strcmp(tok.name, "label") == 0 || strcmp(tok.name, "input") == 0 || strcmp(tok.name, "button") == 0 ||
               strcmp(tok.name, "select") == 0 || strcmp(tok.name, "optgroup") == 0 || strcmp(tok.name, "option") == 0 ||
               strcmp(tok.name, "textarea") == 0 || strcmp(tok.name, "video") == 0 || strcmp(tok.name, "audio") == 0 ||
               strcmp(tok.name, "canvas") == 0 || strcmp(tok.name, "progress") == 0 || strcmp(tok.name, "meter") == 0 ||
               strcmp(tok.name, "iframe") == 0 || strcmp(tok.name, "svg") == 0 || strcmp(tok.name, "math") == 0 ||
               strcmp(tok.name, "dialog") == 0 || strcmp(tok.name, "output") == 0 || strcmp(tok.name, "datalist") == 0 ||
               strcmp(tok.name, "source") == 0 || strcmp(tok.name, "track") == 0 || strcmp(tok.name, "embed") == 0 ||
               strcmp(tok.name, "object") == 0) {
        const uint8_t beforeControls = formControlCount_;
        appendControlBlock(tok.name, tok.attrs, style);
        if (strcmp(tok.name, "textarea") == 0 && tok.type == TokenType::StartTag) {
            inTextareaTag_ = true;
            if (formControlCount_ > beforeControls) {
                activeTextareaControl_ = static_cast<uint8_t>(formControlCount_ - 1U);
            } else {
                activeTextareaControl_ = 0xFF;
            }
        }
    } else if (isBlockTag(tok.name) && strcmp(tok.name, "html") != 0 && strcmp(tok.name, "body") != 0) {
        if (layout_.appendBreak()) dirtyContent_ = true;
    }

    const bool styleCarrier = isInlineTag(tok.name) || isBlockTag(tok.name);
    if (tok.type == TokenType::StartTag && styleCarrier && !isVoidTag(tok.name) && strcmp(tok.name, "style") != 0 && strcmp(tok.name, "script") != 0 &&
        strcmp(tok.name, "title") != 0 && strcmp(tok.name, "head") != 0 && strcmp(tok.name, "meta") != 0 && strcmp(tok.name, "link") != 0 &&
        strcmp(tok.name, "base") != 0 && strcmp(tok.name, "noscript") != 0 && strcmp(tok.name, "template") != 0) {
        if (styleDepth_ < 8) {
            styleStack_[styleDepth_++] = style;
        } else {
            styleStack_[7] = style;
        }
    }
}

void BrowserApp::refreshStatusFromNet() {
    if (network_.state() == NetState::Error) {
        strlcpy(status_, network_.statusText(), sizeof(status_));
    } else if (network_.state() == NetState::Done) {
        const int16_t code = network_.statusCode();
        if (code >= 100) {
            snprintf(status_, sizeof(status_), "HTTP %d", static_cast<int>(code));
        } else {
            const char* type = network_.contentType();
            if (type && type[0]) {
                copyTrim(status_, sizeof(status_), type, sizeof(status_) - 1);
            } else {
                strlcpy(status_, "Loaded", sizeof(status_));
            }
        }
    } else {
        copyTrim(status_, sizeof(status_), network_.statusText(), sizeof(status_) - 1);
    }
}

bool BrowserApp::statusBarVisibleNow(uint32_t nowMs) const {
    if (loading_ || externalLoading_ || externalQueueCount_ > 0) return true;
    if (status_[0] == '\0') return false;
    return nowMs < statusActivityUntilMs_;
}

void BrowserApp::buildStatusLine(char* out, size_t outLen, uint16_t pm) const {
    if (!out || outLen == 0) return;
    out[0] = '\0';

    if (externalLoading_ || externalQueueCount_ > 0) {
        char shortUrl[18] = "";
        const char* src = externalLoadingUrl_[0] ? externalLoadingUrl_ : status_;
        copyTrim(shortUrl, sizeof(shortUrl), src, sizeof(shortUrl) - 1);
        snprintf(out, outLen, "Assets %u %s", static_cast<unsigned>(externalQueueCount_ + (externalLoading_ ? 1U : 0U)), shortUrl);
        return;
    }

    if (loading_) {
        snprintf(out, outLen, "%s %u%%", status_, static_cast<unsigned>(pm / 10U));
        return;
    }

    copyTrim(out, outLen, status_, outLen - 1);
}

void BrowserApp::tick(uint32_t nowMs) {
    updateSmoothScroll(nowMs);

    const bool wasLoading = loading_;
    const bool wasLoaded = loaded_;
    const bool wasExternalLoading = externalLoading_ || externalQueueCount_ > 0;
    char prevStatus[44] = "";
    copyTrim(prevStatus, sizeof(prevStatus), status_, sizeof(prevStatus) - 1);

    if (loading_) {
        char chunk[220] = "";
        size_t read = 0;
        uint8_t loops = 0;
        while (loops < 6) {
            ++loops;
            if (!network_.readBodyChunk(chunk, sizeof(chunk), read)) {
                break;
            }
            Token tok{};
            for (size_t i = 0; i < read; ++i) {
                if (tokenizer_.feedChar(chunk[i], tok)) {
                    processToken(tok);
                }
            }
        }

        if (network_.state() == NetState::Done) {
            Token tail{};
            while (tokenizer_.flushPending(tail)) {
                processToken(tail);
            }

            if (network_.isRedirect()) {
                char nextUrl[kUrlMaxLen] = "";
                if (redirectDepth_ < 4 && resolveUrl(url_, network_.redirectLocation(), nextUrl, sizeof(nextUrl))) {
                    ++redirectDepth_;
                    copyTrim(url_, sizeof(url_), nextUrl, sizeof(url_) - 1);
                    dirtyChrome_ = true;
                    clearDocument();
                    copyTrim(baseHref_, sizeof(baseHref_), url_, sizeof(baseHref_) - 1);
                    schemeFallbackTried_ = false;
                    if (network_.open(url_)) {
                        loading_ = true;
                        loaded_ = false;
                        strlcpy(status_, "Redirecting", sizeof(status_));
                        return;
                    }
                    if (tryAlternateScheme()) {
                        return;
                    }
                }
                loading_ = false;
                loaded_ = false;
                strlcpy(status_, "Redirect failed", sizeof(status_));
                updateProgressAnimation(nowMs);
                return;
            }

            const int16_t code = network_.statusCode();
            if (code >= 400 && layout_.blockCount() == 0) {
                appendHttpErrorPage(code);
            }
            loading_ = false;
            loaded_ = true;
        } else if (network_.state() == NetState::Error) {
            if (tryAlternateScheme()) {
                return;
            }
            loading_ = false;
            loaded_ = layout_.blockCount() > 0;
        }
        refreshStatusFromNet();
    }

    processExternalResourceQueue(nowMs);
    flushDeferredCaches(nowMs);

    if (!loading_ && (externalLoading_ || externalQueueCount_ > 0) && status_[0] == '\0') {
        strlcpy(status_, "Loading assets", sizeof(status_));
    }

    const bool isExternalLoading = externalLoading_ || externalQueueCount_ > 0;
    if (loading_ || isExternalLoading || strcmp(prevStatus, status_) != 0) {
        statusActivityUntilMs_ = nowMs + 2800U;
    }
    if (wasLoading != loading_ || wasExternalLoading != isExternalLoading) {
        dirtyChrome_ = true;
    }

    updateProgressAnimation(nowMs);

    const bool statusVisible = statusBarVisibleNow(nowMs);
    if (strcmp(prevStatus, status_) != 0 || wasLoading != loading_ || wasLoaded != loaded_ || wasExternalLoading != isExternalLoading ||
        lastStatusBarVisible_ != statusVisible) {
        dirtyProgress_ = true;
    }
}

bool BrowserApp::handleClick(int16_t x, int16_t y, const Rect& body) {
    const BrowserRenderLayout chrome = buildBrowserRenderLayout(body, statusBarVisibleNow(millis()));
    const bool loadingVisual = loading_ || externalLoading_ || externalQueueCount_ > 0;

    if (x >= chrome.favBtn.x && y >= chrome.favBtn.y && x < chrome.favBtn.x + chrome.favBtn.w && y < chrome.favBtn.y + chrome.favBtn.h) {
        toggleFavorite();
        return true;
    }
    if (x >= chrome.backBtn.x && y >= chrome.backBtn.y && x < chrome.backBtn.x + chrome.backBtn.w && y < chrome.backBtn.y + chrome.backBtn.h) {
        navigateHistory(-1);
        return true;
    }
    if (x >= chrome.fwdBtn.x && y >= chrome.fwdBtn.y && x < chrome.fwdBtn.x + chrome.fwdBtn.w && y < chrome.fwdBtn.y + chrome.fwdBtn.h) {
        navigateHistory(1);
        return true;
    }
    if (x >= chrome.prevBtn.x && y >= chrome.prevBtn.y && x < chrome.prevBtn.x + chrome.prevBtn.w && y < chrome.prevBtn.y + chrome.prevBtn.h) {
        reload();
        return true;
    }
    if (x >= chrome.nextBtn.x && y >= chrome.nextBtn.y && x < chrome.nextBtn.x + chrome.nextBtn.w && y < chrome.nextBtn.y + chrome.nextBtn.h) {
        openHome();
        return true;
    }
    if (x >= chrome.openBtn.x && y >= chrome.openBtn.y && x < chrome.openBtn.x + chrome.openBtn.w && y < chrome.openBtn.y + chrome.openBtn.h) {
        if (loadingVisual) {
            stopLoading();
        } else {
            openUrl(url_, true);
        }
        return true;
    }
    if (x >= chrome.upBtn.x && y >= chrome.upBtn.y && x < chrome.upBtn.x + chrome.upBtn.w && y < chrome.upBtn.y + chrome.upBtn.h) {
        scroll(-14);
        return true;
    }
    if (x >= chrome.downBtn.x && y >= chrome.downBtn.y && x < chrome.downBtn.x + chrome.downBtn.w && y < chrome.downBtn.y + chrome.downBtn.h) {
        scroll(14);
        return true;
    }
    if (x >= chrome.scrollUp.x && y >= chrome.scrollUp.y && x < chrome.scrollUp.x + chrome.scrollUp.w && y < chrome.scrollUp.y + chrome.scrollUp.h) {
        scroll(-20);
        return true;
    }
    if (x >= chrome.scrollDown.x && y >= chrome.scrollDown.y && x < chrome.scrollDown.x + chrome.scrollDown.w && y < chrome.scrollDown.y + chrome.scrollDown.h) {
        scroll(20);
        return true;
    }

    int8_t hit = renderer_.hit(x, y);
    if (hit < 0) return false;
    const Renderer::ClickTarget& t = renderer_.targets()[hit];
    if (t.kind == 1) {
        const LayoutEngine::Block* blocks = layout_.blocks();
        if (t.index < layout_.blockCount() && blocks[t.index].href[0]) {
            char lowerHref[kUrlMaxLen] = "";
            copyTrim(lowerHref, sizeof(lowerHref), blocks[t.index].href, sizeof(lowerHref) - 1);
            toLowerInPlace(lowerHref);
            if (startsWith(lowerHref, "javascript:")) {
                executeJavaScript(blocks[t.index].href + 11, "anchor-js");
            } else {
                char resolved[kUrlMaxLen] = "";
                if (resolveUrl(url_, blocks[t.index].href, resolved, sizeof(resolved))) {
                    openUrl(resolved, true);
                } else {
                    strlcpy(status_, "Unsupported link", sizeof(status_));
                    dirtyProgress_ = true;
                }
            }
            return true;
        }
    }
    if (t.kind == 2) {
        const LayoutEngine::Block* blocks = layout_.blocks();
        if (t.index < layout_.blockCount() && blocks[t.index].src[0]) {
            char resolved[kUrlMaxLen] = "";
            if (resolveUrl(url_, blocks[t.index].src, resolved, sizeof(resolved))) {
                copyTrim(pendingImageSrc_, sizeof(pendingImageSrc_), resolved, sizeof(pendingImageSrc_) - 1);
            } else {
                copyTrim(pendingImageSrc_, sizeof(pendingImageSrc_), blocks[t.index].src, sizeof(pendingImageSrc_) - 1);
            }
            openImagePending_ = pendingImageSrc_[0] != '\0';
        }
        return true;
    }
    if (t.kind == 6) {
        const LayoutEngine::Block* blocks = layout_.blocks();
        if (t.index < layout_.blockCount() && startsWith(blocks[t.index].href, "ctrl:")) {
            const uint8_t idx = static_cast<uint8_t>(atoi(blocks[t.index].href + 5));
            if (idx < formControlCount_) {
                FormControl& c = formControls_[idx];
                if (c.disabled) return true;
                if (c.onclick[0]) {
                    executeJavaScript(c.onclick, "form-onclick");
                }

                if (strcmp(c.inputType, "checkbox") == 0) {
                    c.checked = !c.checked;
                    strlcpy(status_, c.checked ? "Checkbox on" : "Checkbox off", sizeof(status_));
                    dirtyProgress_ = true;
                    dirtyContent_ = true;
                    return true;
                }
                if (strcmp(c.inputType, "radio") == 0) {
                    for (uint8_t i = 0; i < formControlCount_; ++i) {
                        if (i == idx) continue;
                        if (formControls_[i].formScope != c.formScope) continue;
                        if (strcmp(formControls_[i].inputType, "radio") != 0) continue;
                        if (strcmp(formControls_[i].name, c.name) != 0) continue;
                        formControls_[i].checked = false;
                    }
                    c.checked = true;
                    strlcpy(status_, "Radio selected", sizeof(status_));
                    dirtyProgress_ = true;
                    dirtyContent_ = true;
                    return true;
                }

                const bool submitLike = strcmp(c.inputType, "submit") == 0 ||
                                        (strcmp(c.tag, "button") == 0 && (c.inputType[0] == '\0' || strcmp(c.inputType, "submit") == 0)) ||
                                        strcmp(c.inputType, "image") == 0;
                if (submitLike) {
                    return submitControl(idx);
                }
                if (strcmp(c.inputType, "reset") == 0) {
                    for (uint8_t i = 0; i < formControlCount_; ++i) {
                        if (formControls_[i].formScope != c.formScope) continue;
                        if (strcmp(formControls_[i].inputType, "checkbox") == 0 || strcmp(formControls_[i].inputType, "radio") == 0) {
                            formControls_[i].checked = false;
                        } else {
                            formControls_[i].value[0] = '\0';
                        }
                    }
                    strlcpy(status_, "Form reset", sizeof(status_));
                    dirtyProgress_ = true;
                    dirtyContent_ = true;
                    return true;
                }

                if (openControlEditor(idx)) {
                    strlcpy(status_, "Edit field", sizeof(status_));
                    dirtyProgress_ = true;
                    return true;
                }
            }
        }
        return true;
    }
    if (t.kind == 4 && t.index < favoriteCount_) {
        openUrl(favorites_[t.index], true);
        return true;
    }
    if (t.kind == 5 && t.index < historyCount_) {
        uint8_t idx = static_cast<uint8_t>(historyCount_ - 1U - t.index);
        historyIndex_ = static_cast<int8_t>(idx);
        openUrl(history_[idx], false);
        return true;
    }
    return false;
}

bool BrowserApp::consumeOpenImageRequest(char* outSrc, size_t outLen) {
    if (!outSrc || outLen == 0 || !openImagePending_ || !pendingImageSrc_[0]) return false;
    copyTrim(outSrc, outLen, pendingImageSrc_, outLen - 1);
    openImagePending_ = false;
    pendingImageSrc_[0] = '\0';
    return outSrc[0] != '\0';
}

void BrowserApp::rebuildTargets(const Rect& body, uint32_t nowMs) {
    const BrowserRenderLayout layout = buildBrowserRenderLayout(body, statusBarVisibleNow(nowMs));
    renderer_.clearTargets();

    for (uint8_t i = 0; i < favoriteCount_ && i < 3; ++i) {
        const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 11 + i * 9),
                        static_cast<int16_t>(layout.sidePanel.w - 4), 8};
        renderer_.addTarget(chip, 4, i);
    }

    for (uint8_t i = 0; i < historyCount_ && i < 6; ++i) {
        const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 39 + i * 6),
                        static_cast<int16_t>(layout.sidePanel.w - 4), 5};
        renderer_.addTarget(chip, 5, i);
    }

    const int16_t docH = layout_.docHeight();
    const int16_t maxScroll = docH > layout.visibleH ? static_cast<int16_t>(docH - layout.visibleH) : 0;
    scrollY_ = clamp16(scrollY_, 0, maxScroll);

    const LayoutEngine::Block* blocks = layout_.blocks();
    const uint8_t n = layout_.blockCount();
    for (uint8_t i = 0; i < n; ++i) {
        const LayoutEngine::Block& b = blocks[i];
        const int16_t y = static_cast<int16_t>(layout.contentTop + b.y - scrollY_);
        if (y + b.h < layout.contentTop || y > layout.contentBottom) continue;

        if (b.kind == 2) {
            Rect ir{static_cast<int16_t>(layout.viewport.x + 3), y, static_cast<int16_t>(b.imageW), static_cast<int16_t>(b.imageH)};
            if (ir.w > layout.viewport.w - 8) ir.w = static_cast<int16_t>(layout.viewport.w - 8);
            if (ir.h > layout.contentBottom - ir.y) ir.h = static_cast<int16_t>(layout.contentBottom - ir.y);
            if (ir.w > 0 && ir.h > 0) {
                renderer_.addTarget(ir, 2, i);
            }
            continue;
        }

        if (b.kind != 1 && b.kind != 6) continue;
        const int16_t rowH = b.style.fontSize == FontSize::Large ? 12 : (b.style.fontSize == FontSize::Small ? 8 : 10);
        const Rect row{static_cast<int16_t>(layout.viewport.x + 3), y, static_cast<int16_t>(layout.viewport.w - 6), rowH};
        renderer_.addTarget(row, b.kind, i);
    }
}

uint8_t BrowserApp::consumeDirtyRegions(Rect* out, uint8_t maxItems, const Rect& body) {
    if (!out || maxItems == 0) return 0;

    uint8_t count = 0;

    const uint32_t nowMs = millis();
    const bool statusVisible = statusBarVisibleNow(nowMs);
    const BrowserRenderLayout layout = buildBrowserRenderLayout(body, statusVisible);
    const Rect& viewport = layout.viewport;
    const Rect& statusBar = layout.statusBar;
    const int16_t contentTop = layout.contentTop;
    const int16_t contentBottom = layout.contentBottom;

    const bool statusVisibilityChanged = (lastStatusBarVisible_ != statusVisible);
    const uint16_t currentProgress = progressPermille();
    const uint8_t currentBlockCount = layout_.blockCount();

    if (dirtyChrome_) {
        bool committedChrome = true;
        if (count < maxItems) {
            out[count++] = layout.headerSection;
        } else {
            committedChrome = false;
        }
        if (count < maxItems) {
            out[count++] = layout.sidePanel;
        } else {
            committedChrome = false;
        }
        if (committedChrome) {
            dirtyChrome_ = false;
        }
    }

    char statusLine[40] = "";
    buildStatusLine(statusLine, sizeof(statusLine), currentProgress);

    const int16_t barW = static_cast<int16_t>(statusBar.w - 4);
    int16_t currentFill = static_cast<int16_t>((static_cast<int32_t>(barW) * currentProgress) / 1000);
    if (currentFill < 1) currentFill = 1;
    if (currentFill > barW) currentFill = barW;

    const bool loadingVisual = loading_ || externalLoading_ || externalQueueCount_ > 0;
    const int16_t glowW = 14;

    if (dirtyProgress_ || lastProgress_ != currentProgress || strcmp(lastStatusLine_, statusLine) != 0 || statusVisibilityChanged) {
        if (statusVisibilityChanged && count < maxItems) {
            out[count++] = Rect{viewport.x, static_cast<int16_t>(viewport.y + viewport.h - 12), viewport.w, 12};
        }

        if (statusVisible && statusBar.h > 0) {
            const Rect textBand{static_cast<int16_t>(statusBar.x + 2), static_cast<int16_t>(statusBar.y + 1), static_cast<int16_t>(statusBar.w - 4), 8};

            if (strcmp(lastStatusLine_, statusLine) != 0 || statusVisibilityChanged) {
                uint8_t firstDiff = 0;
                while (lastStatusLine_[firstDiff] && statusLine[firstDiff] && lastStatusLine_[firstDiff] == statusLine[firstDiff]) {
                    ++firstDiff;
                }

                int16_t x = static_cast<int16_t>(textBand.x + static_cast<int16_t>(firstDiff * 6));
                int16_t w = static_cast<int16_t>((textBand.x + textBand.w) - x);
                if (w < 6) w = 6;
                Rect textDelta{x, textBand.y, w, textBand.h};
                Rect textClip = rectIntersection(textDelta, textBand);
                if (textClip.w > 0 && textClip.h > 0 && count < maxItems) {
                    out[count++] = textClip;
                }
            }

            const Rect barBand{static_cast<int16_t>(statusBar.x + 2), static_cast<int16_t>(statusBar.y + 9), barW, 2};
            if (currentFill >= lastProgressFill_) {
                const int16_t deltaW = static_cast<int16_t>(currentFill - lastProgressFill_);
                if (deltaW > 0 && count < maxItems) {
                    Rect addRect{static_cast<int16_t>(barBand.x + lastProgressFill_), barBand.y, deltaW, barBand.h};
                    Rect barClip = rectIntersection(addRect, barBand);
                    if (barClip.w > 0 && barClip.h > 0) {
                        out[count++] = barClip;
                    }
                }
            } else if (count < maxItems) {
                out[count++] = barBand;
            }

            const Rect fillBand{barBand.x, barBand.y, currentFill, barBand.h};
            int16_t glowX = -32768;
            int16_t glowVisibleW = 0;
            if (loadingVisual && currentFill > 1) {
                const int16_t glowRange = static_cast<int16_t>(currentFill + glowW);
                glowX = static_cast<int16_t>(fillBand.x - glowW + static_cast<int16_t>((static_cast<int32_t>(glowRange) * progressGlowPermille_) / 1000));
                const Rect glowRect{glowX, fillBand.y, glowW, fillBand.h};
                const Rect glowClip = rectIntersection(glowRect, fillBand);
                glowX = glowClip.x;
                glowVisibleW = glowClip.w;
                if (glowClip.w > 0 && glowClip.h > 0 && count < maxItems) {
                    out[count++] = glowClip;
                }
            }

            if (lastProgressGlowW_ > 0) {
                const Rect previousGlow{lastProgressGlowX_, fillBand.y, lastProgressGlowW_, fillBand.h};
                const Rect prevClip = rectIntersection(previousGlow, fillBand);
                if (prevClip.w > 0 && prevClip.h > 0 && count < maxItems) {
                    out[count++] = prevClip;
                }
            }

            lastProgressGlowX_ = glowX;
            lastProgressGlowW_ = glowVisibleW;

            if (count == 0 && count < maxItems) {
                out[count++] = statusBar;
            }
        } else {
            lastProgressGlowX_ = -32768;
            lastProgressGlowW_ = 0;
        }

        copyTrim(lastStatusLine_, sizeof(lastStatusLine_), statusLine, sizeof(lastStatusLine_) - 1);
        lastProgressFill_ = currentFill;
        lastProgress_ = currentProgress;
        lastStatusBarVisible_ = statusVisible;
        dirtyProgress_ = false;
    }

    const int16_t currentDocHeight = layout_.docHeight();
    if (dirtyContent_ || lastBlockCount_ != currentBlockCount || lastScrollY_ != scrollY_ || statusVisibilityChanged) {
        uint8_t visibleCount = 0;

        const LayoutEngine::Block* blocks = layout_.blocks();
        for (uint8_t i = 0; i < currentBlockCount; ++i) {
            const LayoutEngine::Block& b = blocks[i];
            const int16_t y = static_cast<int16_t>(contentTop + b.y - scrollY_);
            if (y + b.h < contentTop || y > contentBottom) continue;
            ++visibleCount;
        }

        const int16_t visibleH = static_cast<int16_t>(contentBottom - contentTop);
        const int16_t visibleTopDoc = scrollY_;
        const int16_t visibleBottomDoc = static_cast<int16_t>(scrollY_ + visibleH);
        const int16_t growthStartDoc = currentDocHeight >= lastDocHeight_ ? static_cast<int16_t>(lastDocHeight_ > 10 ? lastDocHeight_ - 10 : 0) : 0;

        const bool scrollChanged = (lastScrollY_ != scrollY_);
        const bool docShrank = currentDocHeight < lastDocHeight_;
        const bool blockCountShrank = currentBlockCount < lastBlockCount_;
        const bool appendOnlyGrowth = dirtyContent_ && !scrollChanged && !statusVisibilityChanged && !docShrank && !blockCountShrank;
        const bool visibleChanged = scrollChanged || statusVisibilityChanged || (!appendOnlyGrowth && visibleCount != lastVisibleBlockCount_);
        bool contentNeedsRepaint = visibleChanged;
        if (!contentNeedsRepaint && dirtyContent_) {
            const bool dirtyTouchesVisible = !(currentDocHeight < visibleTopDoc || growthStartDoc > visibleBottomDoc);
            contentNeedsRepaint = dirtyTouchesVisible;
        }

        bool committedState = true;
        if (contentNeedsRepaint) {
            const Rect contentRegion{viewport.x, contentTop, viewport.w, static_cast<int16_t>(contentBottom - contentTop)};
            Rect repaint = contentRegion;

            if (appendOnlyGrowth && dirtyContent_ && currentDocHeight >= lastDocHeight_) {
                const int16_t dirtyTopPx = static_cast<int16_t>(contentTop + growthStartDoc - scrollY_);
                repaint = rectIntersection(contentRegion, Rect{viewport.x, dirtyTopPx, viewport.w, static_cast<int16_t>(contentBottom - dirtyTopPx)});
            }

            if (repaint.w > 0 && repaint.h > 0) {
                if (count < maxItems) {
                    out[count++] = repaint;
                } else {
                    committedState = false;
                }
            }
        }

        if (committedState) {
            lastVisibleBlockCount_ = visibleCount;
            lastScrollY_ = scrollY_;
            lastBlockCount_ = currentBlockCount;
            lastDocHeight_ = currentDocHeight;
            dirtyContent_ = false;
        }
    }

    return count;
}

void BrowserApp::render(katux::graphics::Renderer& renderer, const Rect& body, uint32_t nowMs, const Rect* clip) {
    const bool statusVisible = statusBarVisibleNow(nowMs);
    const BrowserRenderLayout layout = buildBrowserRenderLayout(body, statusVisible);
    const bool renderHeader = !clip || rectsIntersect(layout.headerSection, *clip);
    const bool renderSidePanel = !clip || rectsIntersect(layout.sidePanel, *clip);
    const bool renderViewport = !clip || rectsIntersect(layout.viewport, *clip);
    const bool renderContent = !clip || rectsIntersect(layout.contentRect, *clip);
    const bool renderProgress = statusVisible && (!clip || rectsIntersect(layout.statusBar, *clip));

    if (!clip || renderSidePanel || renderContent) {
        rebuildTargets(body, nowMs);
    }

    if (!clip) {
        renderer.fillRect(body, 0xD69A);
    }

    if (renderHeader) {
        const bool favorite = isFavorite(url_);
        const bool canGoBack = historyIndex_ > 0;
        const bool canGoForward = historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int8_t>(historyCount_);
        const bool loadingVisual = loading_ || externalLoading_ || externalQueueCount_ > 0;
        const uint16_t navBg = 0x7BEF;
        const uint16_t disabledBg = 0xC618;
        const uint16_t reloadBg = 0x867F;
        const uint16_t homeBg = 0xFD20;
        const uint16_t actionBg = loadingVisual ? 0xF800 : 0x39E7;
        fillRectClipped(renderer, layout.urlBar, 0xBDF7, clip);
        drawFrameClipped(renderer, layout.urlBar, 0x7BEF, clip);

        const uint16_t favBg = favorite ? 0xFFE0 : 0xC618;
        fillRectClipped(renderer, layout.favBtn, favBg, clip);
        drawFrameClipped(renderer, layout.favBtn, 0x7BEF, clip);
        fillRectClipped(renderer, layout.backBtn, canGoBack ? navBg : disabledBg, clip);
        fillRectClipped(renderer, layout.fwdBtn, canGoForward ? navBg : disabledBg, clip);
        fillRectClipped(renderer, layout.prevBtn, reloadBg, clip);
        fillRectClipped(renderer, layout.nextBtn, homeBg, clip);
        fillRectClipped(renderer, layout.openBtn, actionBg, clip);
        fillRectClipped(renderer, layout.upBtn, navBg, clip);
        fillRectClipped(renderer, layout.downBtn, navBg, clip);

        drawTextClipped(renderer, static_cast<int16_t>(layout.backBtn.x + 5), static_cast<int16_t>(layout.backBtn.y + 1), "<", 0x0000, canGoBack ? navBg : disabledBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.fwdBtn.x + 5), static_cast<int16_t>(layout.fwdBtn.y + 1), ">", 0x0000, canGoForward ? navBg : disabledBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.prevBtn.x + 6), static_cast<int16_t>(layout.prevBtn.y + 1), "R", 0x0000, reloadBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.nextBtn.x + 6), static_cast<int16_t>(layout.nextBtn.y + 1), "H", 0x0000, homeBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.openBtn.x + (loadingVisual ? 2 : 3)), static_cast<int16_t>(layout.openBtn.y + 1), loadingVisual ? "Stop" : "Go", 0x0000,
                        actionBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.upBtn.x + 4), static_cast<int16_t>(layout.upBtn.y + 1), "^", 0x0000, navBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.downBtn.x + 4), static_cast<int16_t>(layout.downBtn.y + 1), "v", 0x0000, navBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.favBtn.x + 4), static_cast<int16_t>(layout.favBtn.y + 1), "*", 0x0000, favBg, clip);

        const bool secure = startsWith(url_, "https://");
        const uint16_t schemeBg = secure ? 0x07E0 : 0xC618;
        fillRectClipped(renderer, layout.schemeBadge, schemeBg, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.schemeBadge.x + 2), static_cast<int16_t>(layout.schemeBadge.y + 1), secure ? "S" : "H", 0x0000,
                        schemeBg, clip);

        char urlLine[96] = "";
        const char* displayUrl = startsWith(url_, "http://") ? (url_ + 7) : (startsWith(url_, "https://") ? (url_ + 8) : url_);
        const int16_t textX = static_cast<int16_t>(layout.urlBar.x + 13);
        int16_t textW = static_cast<int16_t>(layout.urlBar.x + layout.urlBar.w - 2 - textX);
        if (textW < 6) textW = 6;
        size_t maxChars = static_cast<size_t>(textW / 6);
        if (maxChars < 4) maxChars = 4;
        if (maxChars >= sizeof(urlLine)) maxChars = sizeof(urlLine) - 1;

        const size_t fullLen = strlen(displayUrl);
        if (fullLen > maxChars) {
            const size_t head = maxChars > 3 ? maxChars - 3 : maxChars;
            copyTrim(urlLine, sizeof(urlLine), displayUrl, head);
            if (maxChars > 3) {
                strlcat(urlLine, "...", sizeof(urlLine));
            }
        } else {
            copyTrim(urlLine, sizeof(urlLine), displayUrl, maxChars);
        }
        drawTextClipped(renderer, textX, static_cast<int16_t>(layout.urlBar.y + 2), urlLine, 0x0000, 0xBDF7, clip);
    }

    if (renderSidePanel) {
        fillRectClipped(renderer, layout.sidePanel, 0xE75D, clip);
        drawFrameClipped(renderer, layout.sidePanel, 0xA534, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 2), "Fav", 0x2104, 0xE75D, clip);
        drawTextClipped(renderer, static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 30), "Hist", 0x2104, 0xE75D, clip);

        for (uint8_t i = 0; i < favoriteCount_ && i < 3; ++i) {
            const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 11 + i * 9),
                            static_cast<int16_t>(layout.sidePanel.w - 4), 8};
            fillRectClipped(renderer, chip, 0xFFF4, clip);
            char favLine[20] = "";
            const char* s = startsWith(favorites_[i], "http://") ? (favorites_[i] + 7) : (startsWith(favorites_[i], "https://") ? (favorites_[i] + 8) : favorites_[i]);
            copyTrim(favLine, sizeof(favLine), s, 14);
            drawTextClipped(renderer, static_cast<int16_t>(chip.x + 1), static_cast<int16_t>(chip.y + 1), favLine, 0x001F, 0xFFF4, clip);
        }

        for (uint8_t i = 0; i < historyCount_ && i < 6; ++i) {
            const uint8_t histIdx = static_cast<uint8_t>(historyCount_ - 1U - i);
            const Rect chip{static_cast<int16_t>(layout.sidePanel.x + 2), static_cast<int16_t>(layout.sidePanel.y + 39 + i * 6),
                            static_cast<int16_t>(layout.sidePanel.w - 4), 5};
            fillRectClipped(renderer, chip, 0xD6FF, clip);
            char histLine[20] = "";
            const char* s = startsWith(history_[histIdx], "http://") ? (history_[histIdx] + 7)
                                                                      : (startsWith(history_[histIdx], "https://") ? (history_[histIdx] + 8) : history_[histIdx]);
            copyTrim(histLine, sizeof(histLine), s, 14);
            drawTextClipped(renderer, static_cast<int16_t>(chip.x + 1), chip.y, histLine, 0x2104, 0xD6FF, clip);
        }
    }

    if (renderViewport) {
        const Rect contentBg{layout.viewport.x, layout.viewport.y, layout.viewport.w, static_cast<int16_t>(layout.contentBottom - layout.viewport.y)};
        fillRectClipped(renderer, contentBg, 0xFFFF, clip);
        drawFrameClipped(renderer, layout.viewport, 0x7BEF, clip);
    }

    if (renderProgress) {
        fillRectClipped(renderer, layout.statusBar, 0xC618, clip);

        const uint16_t pm = progressPermille();
        char statusLine[40] = "";
        buildStatusLine(statusLine, sizeof(statusLine), pm);

        const Rect textBand{static_cast<int16_t>(layout.statusBar.x + 2), static_cast<int16_t>(layout.statusBar.y + 1), static_cast<int16_t>(layout.statusBar.w - 4), 8};
        const Rect textPaint = clipRect(textBand, clip);
        if (textPaint.w > 0 && textPaint.h > 0) {
            const int16_t charStartPx = static_cast<int16_t>(textPaint.x - textBand.x);
            int16_t firstChar = static_cast<int16_t>(charStartPx / 6);
            if (firstChar < 0) firstChar = 0;

            int16_t lastChar = static_cast<int16_t>((textPaint.x + textPaint.w - textBand.x + 5) / 6);
            const int16_t len = static_cast<int16_t>(strlen(statusLine));
            if (lastChar > len) lastChar = len;

            if (lastChar > firstChar) {
                char partial[40] = "";
                uint8_t wi = 0;
                for (int16_t i = firstChar; i < lastChar && wi + 1 < sizeof(partial); ++i) {
                    partial[wi++] = statusLine[i];
                }
                partial[wi] = '\0';
                drawTextClipped(renderer, static_cast<int16_t>(textBand.x + firstChar * 6), static_cast<int16_t>(layout.statusBar.y + 2), partial, 0x0000,
                                0xC618, clip);
            }
        }

        const int16_t barW = static_cast<int16_t>(layout.statusBar.w - 4);
        int16_t fill = static_cast<int16_t>((static_cast<int32_t>(barW) * pm) / 1000);
        if (fill < 1) fill = 1;
        if (fill > barW) fill = barW;

        const bool loadingVisual = loading_ || externalLoading_ || externalQueueCount_ > 0;
        const int16_t glowW = 14;
        const Rect barBand{static_cast<int16_t>(layout.statusBar.x + 2), static_cast<int16_t>(layout.statusBar.y + 9), barW, 2};
        const Rect barPaint = clipRect(barBand, clip);
        if (barPaint.w > 0 && barPaint.h > 0) {
            renderer.fillRect(barPaint, 0x7BEF);
        }

        if (fill > 0) {
            const Rect fillArea{barBand.x, barBand.y, fill, barBand.h};
            const Rect fillPaint = rectIntersection(fillArea, barPaint);
            if (fillPaint.w > 0 && fillPaint.h > 0) {
                renderer.fillRect(fillPaint, 0x07E0);
            }

            if (loadingVisual) {
                const int16_t glowRange = static_cast<int16_t>(fill + glowW);
                const int16_t glowX = static_cast<int16_t>(fillArea.x - glowW + static_cast<int16_t>((static_cast<int32_t>(glowRange) * progressGlowPermille_) / 1000));
                const Rect glowArea{glowX, fillArea.y, glowW, fillArea.h};
                const Rect glowPaint = rectIntersection(rectIntersection(glowArea, fillArea), barPaint);
                if (glowPaint.w > 0 && glowPaint.h > 0) {
                    renderer.fillRect(glowPaint, 0x9FF2);
                }
            }
        }
    }

    if (renderContent) {
        lastViewportH_ = layout.visibleH;

        const int16_t docH = layout_.docHeight();
        const int16_t maxScroll = docH > layout.visibleH ? static_cast<int16_t>(docH - layout.visibleH) : 0;
        scrollY_ = clamp16(scrollY_, 0, maxScroll);

        if (!loaded_ && !loading_) {
            drawTextClipped(renderer, static_cast<int16_t>(layout.viewport.x + 6), static_cast<int16_t>(layout.viewport.y + 20), "Open a page", 0x2104, 0xFFFF,
                            clip);
            return;
        }

        const LayoutEngine::Block* blocks = layout_.blocks();
        const uint8_t n = layout_.blockCount();
        for (uint8_t i = 0; i < n; ++i) {
            const LayoutEngine::Block& b = blocks[i];
            const int16_t y = static_cast<int16_t>(layout.contentTop + b.y - scrollY_);
            if (y + b.h < layout.contentTop || y > layout.contentBottom) continue;

            if (b.kind == 2) {
                Rect ir{static_cast<int16_t>(layout.viewport.x + 3), y, static_cast<int16_t>(b.imageW), static_cast<int16_t>(b.imageH)};
                if (ir.w > layout.viewport.w - 8) ir.w = static_cast<int16_t>(layout.viewport.w - 8);
                if (ir.h > layout.contentBottom - ir.y) ir.h = static_cast<int16_t>(layout.contentBottom - ir.y);
                if (ir.w > 0 && ir.h > 0 && (!clip || rectsIntersect(ir, *clip))) {
                    image_.render(renderer, ir, b.src, nowMs);
                }
                continue;
            }

            const int16_t rowH = b.style.fontSize == FontSize::Large ? 12 : (b.style.fontSize == FontSize::Small ? 8 : 10);
            const Rect row{static_cast<int16_t>(layout.viewport.x + 3), y, static_cast<int16_t>(layout.viewport.w - 6), rowH};
            const Rect rowPaint = clipRect(row, clip);
            if (rowPaint.w <= 0 || rowPaint.h <= 0) continue;

            const uint16_t bg = b.kind == 1 ? 0xE71C : b.style.bg;
            renderer.fillRect(rowPaint, bg);

            const int16_t glyphW = b.style.fontSize == FontSize::Large ? 7 : 6;
            int16_t txtX = static_cast<int16_t>(row.x + 1);
            const int16_t textW = static_cast<int16_t>(strlen(b.text) * glyphW);
            if (b.style.align == TextAlign::Center) {
                txtX = static_cast<int16_t>(row.x + (row.w - textW) / 2);
            } else if (b.style.align == TextAlign::Right) {
                txtX = static_cast<int16_t>(row.x + row.w - textW - 1);
            }
            if (txtX < row.x + 1) txtX = static_cast<int16_t>(row.x + 1);

            drawTextClipped(renderer, txtX, static_cast<int16_t>(row.y + 1), b.text, b.style.fg, bg, clip, glyphW, static_cast<int16_t>(rowH - 1));
        }

        if (scrollY_ > 0) {
            fillRectClipped(renderer, layout.scrollUp, 0xBDF7, clip);
            drawFrameClipped(renderer, layout.scrollUp, 0x2104, clip);
            drawTextClipped(renderer, static_cast<int16_t>(layout.scrollUp.x + 2), static_cast<int16_t>(layout.scrollUp.y + 1), "^", 0x0000, 0xBDF7, clip);
        }
        if (scrollY_ < maxScroll) {
            fillRectClipped(renderer, layout.scrollDown, 0xBDF7, clip);
            drawFrameClipped(renderer, layout.scrollDown, 0x2104, clip);
            drawTextClipped(renderer, static_cast<int16_t>(layout.scrollDown.x + 2), static_cast<int16_t>(layout.scrollDown.y + 1), "v", 0x0000, 0xBDF7,
                            clip);
        }
    }
}

}