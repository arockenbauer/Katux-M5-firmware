#pragma once

#include <stdint.h>
#include <stddef.h>

#include "renderer.h"

namespace katux::graphics::browser {

static constexpr size_t kBrowserUrlMaxLen = 192; // further shrink urls to save RAM


enum class NetState : uint8_t {
    Idle = 0,
    Connecting,
    Headers,
    StreamingBody,
    Done,
    Error
};

enum class TokenType : uint8_t {
    None = 0,
    Text,
    StartTag,
    EndTag,
    SelfClosingTag
};

enum class FontSize : uint8_t {
    Small = 0,
    Medium,
    Large
};

enum class TextAlign : uint8_t {
    Left = 0,
    Center,
    Right
};

enum class DisplayMode : uint8_t {
    Block = 0,
    Inline,
    Hidden
};

enum class ImageFormat : uint8_t {
    Unknown = 0,
    Jpeg,
    Png,
    Bmp,
    Qoi,
    WebP
};

struct DomNode {
    char tag[12] = "";
    char id[20] = "";
    char cls[24] = "";
    char href[100] = "";
    char src[100] = "";
    char style[100] = "";
    char text[96] = "";
    bool isText = false;
    bool isClosing = false;
};

struct CssStyle {
    uint16_t fg = 0x2104;
    uint16_t bg = 0xFFFF;
    FontSize fontSize = FontSize::Medium;
    TextAlign align = TextAlign::Left;
    DisplayMode display = DisplayMode::Block;
    uint8_t marginV = 2;
    bool bold = false;
    bool italic = false;
    bool pre = false;
};

struct Token {
    TokenType type = TokenType::None;
    char name[12] = "";
    char attrs[120] = "";
    char text[96] = "";
};

class NetworkManager {
   public:
    NetworkManager();
    ~NetworkManager();
    bool open(const char* url);
    void reset();
    NetState state() const;
    const char* statusText() const;
    const char* contentType() const;
    uint32_t bytesReceived() const;
    int32_t contentLength() const;
    int16_t statusCode() const;
    bool isRedirect() const;
    const char* redirectLocation() const;
    float progress() const;
    bool readBodyChunk(char* out, size_t outLen, size_t& outRead);
    bool hasError() const;

   private:
    bool parseUrl(const char* url);
    bool connectStep();
    bool parseHeadersStep();

    NetState state_ = NetState::Idle;
    bool secure_ = false;
    char host_[64] = "";
    char* path_ = nullptr;            // allocated at runtime
    char statusText_[40] = "Idle";
    char contentType_[40] = "";
    int32_t contentLength_ = -1;
    int16_t statusCode_ = 0;
    char* redirectLocation_ = nullptr; // allocated at runtime
    bool headerStatusParsed_ = false;
    uint32_t bytesReceived_ = 0;
    // header parsing rarely needs more than 120 chars
    char headerLine_[120] = "";
    uint16_t headerLineLen_ = 0;
};

class HtmlTokenizer {
   public:
    void reset();
    bool feedChar(char c, Token& outToken);
    bool flushPending(Token& outToken);

   private:
    bool flushText(Token& outToken);
    bool flushTag(Token& outToken);

    bool inTag_ = false;
    bool inQuote_ = false;
    char quoteChar_ = 0;
    char textBuf_[120] = "";
    uint16_t textLen_ = 0;
    char tagBuf_[160] = "";
    uint16_t tagLen_ = 0;
};

class CssEngine {
   public:
    CssStyle inherit(const CssStyle& parent) const;
    void applyTagDefaults(const char* tag, CssStyle& style) const;
    void applyInline(const char* styleText, CssStyle& style) const;
    uint16_t parseColor(const char* value, uint16_t fallback) const;
};

class LayoutEngine {
   public:
    struct Block {
        uint8_t kind = 0;
        CssStyle style{};
        int16_t y = 0;
        int16_t h = 0;
        char text[64] = "";
        char href[64] = "";
        char src[64] = "";
        uint16_t imageW = 0;
        uint16_t imageH = 0;
    };

    static constexpr uint8_t kMaxBlocks = 64; // reduce memory further

    LayoutEngine();
    ~LayoutEngine();

    void reset();
    bool appendText(const char* text, const CssStyle& style, const char* href, int16_t maxWidth);
    bool appendImage(const char* src, uint16_t w, uint16_t h, const CssStyle& style, int16_t maxWidth);
    bool appendBreak();
    bool setBlockMeta(uint8_t index, uint8_t kind, const char* href, const char* src);
    int16_t docHeight() const;
    uint8_t blockCount() const;
    const Block* blocks() const;

   private:
    void pushLine(const char* line, const CssStyle& style, const char* href);

    Block* blocks_ = nullptr; // allocated dynamically
    uint8_t count_ = 0;
    int16_t cursorY_ = 2;
};

class CacheManager {
   public:
    static constexpr uint8_t kEntryMax = 10;

    struct Entry {
        bool valid = false;
        char key[100] = "";
        ImageFormat format = ImageFormat::Unknown;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t lastUse = 0;
    };

    void begin();
    bool find(const char* key, Entry& out, uint32_t nowMs);
    void put(const char* key, ImageFormat format, uint16_t width, uint16_t height, uint32_t nowMs);

   private:
    Entry* entries_ = nullptr;
    Entry local_[kEntryMax]{};
};

class ImageRenderer {
   public:
    void begin(CacheManager* cache);
    ImageFormat detectFormat(const char* src) const;
    bool validateDimensions(uint16_t w, uint16_t h, int16_t viewportW, int16_t viewportH) const;
    void fitToViewport(uint16_t& w, uint16_t& h, int16_t viewportW, int16_t viewportH) const;
    void render(katux::graphics::Renderer& renderer, const Rect& rect, const char* src, uint32_t nowMs);

   private:
    CacheManager* cache_ = nullptr;
};

class Renderer {
   public:
    static constexpr uint8_t kTargetMax = 40;

    struct ClickTarget {
        Rect rect{0, 0, 0, 0};
        uint8_t kind = 0;
        uint8_t index = 0;
    };

    void clearTargets();
    bool addTarget(const Rect& r, uint8_t kind, uint8_t index);
    int8_t hit(int16_t x, int16_t y) const;
    const ClickTarget* targets() const;
    uint8_t targetCount() const;

   private:
    ClickTarget targets_[kTargetMax]{};
    uint8_t count_ = 0;
};

class BrowserApp {
   public:
    // keep the history/favorites buffers small; typical use rarely exceeds a few entries
    static constexpr uint8_t kHistoryMax = 4;   // reduced from 8 to save ~2KB
    static constexpr uint8_t kFavoriteMax = 4;  // reduced from 8 to save ~2KB
    static constexpr size_t kUrlMaxLen = kBrowserUrlMaxLen;

    void begin(const char* initialUrl, const char* const* presets, uint8_t presetCount);
    void tick(uint32_t nowMs);
    void openUrl(const char* url, bool addToHistory);
    void setCurrentUrlText(const char* url);
    void openPreset(int8_t delta);
    void navigateHistory(int8_t step);
    void scroll(int16_t delta);
    void toggleFavorite();
    bool isFavorite(const char* url) const;
    bool isLoading() const;
    bool isLoaded() const;
    const char* currentUrl() const;
    const char* status() const;
    uint16_t progressPermille() const;
    void clearDocument();
    bool handleClick(int16_t x, int16_t y, const Rect& body);
    bool consumeOpenImageRequest(char* outSrc, size_t outLen);
    bool consumeFormKeyboardRequest(char* outInitial, size_t outLen);
    void applyFormKeyboardValue(const char* value, bool accepted);
    uint8_t consumeDirtyRegions(Rect* out, uint8_t maxItems, const Rect& body);
    void render(katux::graphics::Renderer& renderer, const Rect& body, uint32_t nowMs, const Rect* clip = nullptr);

   private:
    struct CssRule {
        bool valid = false;
        bool mediaMatched = true;
        uint16_t specificity = 0;
        uint16_t order = 0;
        char tag[12] = "";
        char id[20] = "";
        char cls[24] = "";
        char declarations[180] = "";
        CssStyle style{};
    };

    static constexpr uint8_t kCssRuleMax = 28;
    static constexpr uint32_t kScrollInputComboMs = 120U;
    static constexpr float kScrollImpulseBase = 0.34f;
    static constexpr float kScrollImpulseBoost = 0.07f;
    static constexpr float kScrollMaxVelocityBase = 24.0f;
    static constexpr float kScrollMaxVelocityBoost = 2.5f;
    static constexpr float kScrollDecayFree = 0.82f;
    static constexpr float kScrollDecayHeld = 0.90f;

    void processToken(const Token& tok);
    void pushHistory(const char* url);
    bool extractAttr(const char* attrs, const char* attr, char* out, size_t outLen) const;
    void decodeEntities(char* text) const;
    void refreshStatusFromNet();
    bool resolveUrl(const char* baseUrl, const char* href, char* out, size_t outLen) const;
    void appendHttpErrorPage(int16_t code);
    void parseStylesheet(const char* cssText);
    void parseCssRule(const char* selector, const char* declarations, bool mediaMatched);
    bool parseCssMediaMatch(const char* mediaExpr) const;
    bool selectorMatches(const CssRule& rule, const char* tag, const char* id, const char* cls) const;
    void applyStylesheet(const char* tag, const char* id, const char* cls, CssStyle& style) const;
    uint16_t computeProgressTargetPermille(uint32_t nowMs) const;
    void updateProgressAnimation(uint32_t nowMs);
    void updateSmoothScroll(uint32_t nowMs);
    bool isBlockTag(const char* tag) const;
    bool isInlineTag(const char* tag) const;
    bool isVoidTag(const char* tag) const;
    bool isIgnoredNonVisualTag(const char* tag) const;
    bool hasAttrFlag(const char* attrs, const char* attr) const;
    void appendListPrefix(const CssStyle& style);
    void appendControlBlock(const char* tag, const char* attrs, const CssStyle& style);
    bool extractJsStringLiteral(const char* src, char* out, size_t outLen) const;
    bool applyJsNavigation(const char* value);
    void applyJsAlert(const char* value);
    bool executeJsStatement(const char* stmt, uint8_t depth);
    void executeJavaScript(const char* script, const char* source);
    bool enqueueExternalResource(const char* url, bool script);
    bool fetchExternalText(const char* url, char* out, size_t outLen, uint16_t timeoutMs, size_t bodyCap) const;
    void processExternalResourceQueue(uint32_t nowMs);
    void cacheUnparsedJs(const char* src);
    void cacheUnparsedCss(const char* src);
    void flushDeferredCaches(uint32_t nowMs);
    void beginFormScope(const char* attrs);
    void endFormScope();
    bool registerInteractiveControl(uint8_t blockIndex, const char* tag, const char* attrs);
    bool openControlEditor(uint8_t controlIndex);
    bool submitControl(uint8_t controlIndex);
    bool statusBarVisibleNow(uint32_t nowMs) const;
    void buildStatusLine(char* out, size_t outLen, uint16_t pm) const;

    const char* const* presets_ = nullptr;
    uint8_t presetCount_ = 0;
    uint8_t presetIndex_ = 0;

    char url_[kUrlMaxLen] = "http://example.com/";
    char baseHref_[kUrlMaxLen] = "";
    char title_[44] = "Navigator";
    char status_[44] = "Ready";

    char history_[kHistoryMax][kUrlMaxLen]{};
    uint8_t historyCount_ = 0;
    int8_t historyIndex_ = -1;

    char favorites_[kFavoriteMax][kUrlMaxLen]{};
    uint8_t favoriteCount_ = 0;

    bool loading_ = false;
    bool loaded_ = false;
    uint8_t redirectDepth_ = 0;
    uint32_t loadStartMs_ = 0;
    int16_t scrollY_ = 0;
    int16_t lastViewportH_ = 0;
    float scrollVelocity_ = 0.0f;
    int8_t scrollInputDir_ = 0;
    uint8_t scrollInputBoost_ = 0;
    uint32_t lastScrollInputMs_ = 0;
    uint32_t lastScrollAnimMs_ = 0;

    NetworkManager network_{};
    HtmlTokenizer tokenizer_{};
    CssEngine css_{};
    LayoutEngine layout_{};
    CacheManager cache_{};
    ImageRenderer image_{};
    Renderer renderer_{};

    struct FormControl {
        bool used = false;
        uint8_t blockIndex = 0;
        uint8_t formScope = 0;
        char tag[12] = "";
        char inputType[20] = "";
        char name[32] = "";
        char value[256] = "";              // most fields are short
        char action[kUrlMaxLen] = "";
        char method[8] = "get";
        char onclick[100] = "";
        char onchange[100] = "";
        bool checked = false;
        bool disabled = false;
        bool readonly = false;
        bool required = false;
    };

    // limit form inputs even further – 16 entries are almost always enough
    // dropped from 32 (≈30 KB) to cut another ~15 KB of BSS
    static constexpr uint8_t kFormControlMax = 16;
    FormControl formControls_[kFormControlMax]{};
    uint8_t formControlCount_ = 0;
    uint8_t formScopeCounter_ = 0;
    uint8_t activeFormScope_ = 0;
    char activeFormAction_[kUrlMaxLen] = "";
    char activeFormMethod_[8] = "get";
    bool formKeyboardPending_ = false;
    uint8_t formKeyboardControlIndex_ = 0xFF;
    char formKeyboardInitial_[256] = "";    // keyboard initial value buffer
    bool inTextareaTag_ = false;
    uint8_t activeTextareaControl_ = 0xFF;

    CssStyle styleStack_[8]{};
    uint8_t styleDepth_ = 0;
    char activeHref_[kUrlMaxLen] = "";
    bool openImagePending_ = false;
    char pendingImageSrc_[kUrlMaxLen] = "";
    bool dirtyProgress_ = true;
    bool dirtyContent_ = true;
    uint16_t lastProgress_ = 0;
    uint16_t progressDisplayed_ = 0;
    uint16_t progressTarget_ = 0;
    uint32_t lastProgressAnimMs_ = 0;
    uint8_t lastBlockCount_ = 0;
    int16_t lastProgressFill_ = 0;
    uint16_t progressGlowPermille_ = 0;
    int16_t lastProgressGlowX_ = -32768;
    int16_t lastProgressGlowW_ = 0;
    int16_t lastScrollY_ = 0;
    uint8_t lastVisibleBlockCount_ = 0;
    int16_t lastDocHeight_ = 0;
    char lastStatusLine_[40] = "";

    bool inStyleTag_ = false;
    bool inScriptTag_ = false;
    bool inTitleTag_ = false;
    char titleBuffer_[44] = "";
    uint8_t listDepth_ = 0;
    bool listOrdered_[4]{};
    uint16_t listCounter_[4]{};
    uint16_t cssOrder_ = 0;
    // used while accumulating a stylesheet; 256 bytes is sufficient
    char cssBuffer_[256] = "";
    uint16_t cssBufferLen_ = 0;
    CssRule cssRules_[kCssRuleMax]{};
    uint8_t cssRuleCount_ = 0;
    // javascript snippets are short; trim to 256
    char scriptBuffer_[256] = "";
    uint16_t scriptBufferLen_ = 0;
    uint8_t jsExecDepth_ = 0;

    static constexpr uint8_t kExternalQueueMax = 6;
    struct ExternalResource {
        bool used = false;
        bool script = true;
        uint8_t retries = 0;
        char url[kUrlMaxLen] = "";
    };
    ExternalResource externalQueue_[kExternalQueueMax]{};
    uint8_t externalQueueCount_ = 0;
    bool externalLoading_ = false;
    uint32_t externalLastAttemptMs_ = 0;
    char externalLoadingUrl_[kUrlMaxLen] = "";
    uint32_t statusActivityUntilMs_ = 0;
    bool lastStatusBarVisible_ = true;

    // cache for deferred scripts/styles; two entries suffice
    static constexpr uint8_t kDeferredCacheMax = 2;
    char deferredJsCache_[kDeferredCacheMax][120]{};
    char deferredCssCache_[kDeferredCacheMax][120]{};
    uint8_t deferredJsCount_ = 0;
    uint8_t deferredCssCount_ = 0;
    uint32_t deferredLastFlushMs_ = 0;
};

}