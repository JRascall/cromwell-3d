/* The raylib half of the module. NO CEF IN THIS FILE, and therefore no
 * windows.h — see WebBrowserHost.hpp for why the two cannot share a
 * translation unit. Everything that needs a browser goes through
 * WebBrowserHost, which names neither library. */
#include "cromwell/web/surface/WebSurface.hpp"

#include "cromwell/web/cef/WebBrowserHost.hpp"
#include "cromwell/web/cef/WebRuntime.hpp"

namespace cromwell {
namespace {

/* Windows virtual-key codes. Fixed by the platform ABI since Windows 3, and
 * written out here rather than included, because winuser.h is exactly the
 * header this file cannot have. */
constexpr int kVkBack   = 0x08;
constexpr int kVkTab    = 0x09;
constexpr int kVkReturn = 0x0D;
constexpr int kVkEscape = 0x1B;
constexpr int kVkPrior  = 0x21;  // page up
constexpr int kVkNext   = 0x22;  // page down
constexpr int kVkEnd    = 0x23;
constexpr int kVkHome   = 0x24;
constexpr int kVkLeft   = 0x25;
constexpr int kVkUp     = 0x26;
constexpr int kVkRight  = 0x27;
constexpr int kVkDown   = 0x28;
constexpr int kVkDelete = 0x2E;
constexpr int kVkF5     = 0x74;

/* raylib's key codes are GLFW's, which are neither ASCII nor virtual-key codes
 * once you leave the printable range. Only the keys a text field actually
 * needs are mapped; everything else arrives as a character event via
 * GetCharPressed and never needs a code at all. */
int virtualKeyFor(int rayKey)
{
    switch (rayKey) {
        case KEY_BACKSPACE:  return kVkBack;
        case KEY_TAB:        return kVkTab;
        case KEY_ENTER:      return kVkReturn;
        case KEY_KP_ENTER:   return kVkReturn;
        case KEY_ESCAPE:     return kVkEscape;
        case KEY_DELETE:     return kVkDelete;
        case KEY_LEFT:       return kVkLeft;
        case KEY_RIGHT:      return kVkRight;
        case KEY_UP:         return kVkUp;
        case KEY_DOWN:       return kVkDown;
        case KEY_HOME:       return kVkHome;
        case KEY_END:        return kVkEnd;
        case KEY_PAGE_UP:    return kVkPrior;
        case KEY_PAGE_DOWN:  return kVkNext;
        case KEY_A:          return 'A';
        case KEY_C:          return 'C';
        case KEY_V:          return 'V';
        case KEY_X:          return 'X';
        case KEY_Z:          return 'Z';
        case KEY_L:          return 'L';
        case KEY_F5:         return kVkF5;
        default:             return 0;
    }
}

/* Sampled fresh for every event rather than cached: a modifier released
 * between two events in the same frame is a real thing that happens, and a
 * stale control flag turns a click into a ctrl-click. */
uint32_t currentModifiers()
{
    uint32_t flags = 0;
    if (IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT))   flags |= WebModShift;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) flags |= WebModControl;
    if (IsKeyDown(KEY_LEFT_ALT)     || IsKeyDown(KEY_RIGHT_ALT))     flags |= WebModAlt;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))   flags |= WebModLeftButton;
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) flags |= WebModMiddleButton;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))  flags |= WebModRightButton;
    return flags;
}

WebMouseButton buttonFor(int rayButton)
{
    if (rayButton == MOUSE_BUTTON_RIGHT)  return WebMouseButton::Right;
    if (rayButton == MOUSE_BUTTON_MIDDLE) return WebMouseButton::Middle;
    return WebMouseButton::Left;
}

}  // namespace

/* -------------------------------------------------------------------- impl */

struct WebSurface::Impl {
    std::unique_ptr<WebBrowserHost> host;
    Texture2D texture = { 0 };
    int  textureWidth  = 0;
    int  textureHeight = 0;
    bool focused = false;

    void rebuildTexture(int width, int height)
    {
        if (texture.id != 0) UnloadTexture(texture);

        Image blank = GenImageColor(width, height, BLANK);
        ImageFormat(&blank, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        texture = LoadTextureFromImage(blank);
        UnloadImage(blank);

        /* POINT, NOT BILINEAR, AND IT MATTERS MORE THAN IT USUALLY WOULD. The
         * contents are antialiased text that Chromium has already rendered for
         * a specific pixel grid; every glyph stem is one or two pixels wide and
         * its edges carry the coverage that makes it readable. Interpolating
         * that a second time smears exactly the detail the first pass was
         * computing. With the surface sized to the panel there is nothing to
         * interpolate anyway — this only guarantees that a half-pixel offset
         * cannot quietly reintroduce a blur.
         *
         * No mipmaps: this is a screen-space panel, and the diegetic case that
         * needs them is not built yet. */
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);

        textureWidth  = width;
        textureHeight = height;
    }
};

/* ---------------------------------------------------------------- lifetime */

WebSurface::WebSurface(WebRuntime& runtime, int width, int height, const std::string& url)
    : impl_(std::make_unique<Impl>())
{
    impl_->host = WebBrowserHost::create(runtime, width, height, url);
    if (impl_->host) impl_->rebuildTexture(width, height);
}

WebSurface::~WebSurface()
{
    if (impl_->host) impl_->host->close();
    if (impl_->texture.id != 0) UnloadTexture(impl_->texture);
}

bool WebSurface::valid() const { return impl_->host != nullptr; }

/* ------------------------------------------------------------------ upload */

void WebSurface::upload()
{
    if (!impl_->host) return;

    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    if (!impl_->host->takeFrame(&pixels, &width, &height)) return;

    if (width != impl_->textureWidth || height != impl_->textureHeight)
        impl_->rebuildTexture(width, height);

    UpdateTexture(impl_->texture, pixels);
}

Texture2D WebSurface::texture() const { return impl_->texture; }
int WebSurface::width()  const { return impl_->textureWidth; }
int WebSurface::height() const { return impl_->textureHeight; }

/* ---------------------------------------------------------------- browsing */

void WebSurface::loadUrl(const std::string& url)
{
    if (impl_->host) impl_->host->loadUrl(url);
}

void WebSurface::reload()
{
    if (impl_->host) impl_->host->reload();
}

void WebSurface::goBack()    { if (impl_->host) impl_->host->goBack(); }
void WebSurface::goForward() { if (impl_->host) impl_->host->goForward(); }

bool WebSurface::canGoBack() const    { return impl_->host && impl_->host->canGoBack(); }
bool WebSurface::canGoForward() const { return impl_->host && impl_->host->canGoForward(); }
bool WebSurface::loading() const      { return impl_->host && impl_->host->loading(); }

std::string WebSurface::currentUrl() const
{
    return impl_->host ? impl_->host->currentUrl() : std::string{};
}

bool WebSurface::wantsKeyboard() const
{
    /* Deliberately not && editableFocused() — see the header. That version
     * flapped, because the signals behind it fire unreliably in both
     * directions. */
    return impl_->host && impl_->focused;
}

bool WebSurface::pageFocused() const { return impl_->focused; }

bool WebSurface::editableFocused() const
{
    return impl_->host && impl_->host->editableFocused();
}

int WebSurface::editableSignals() const
{
    return impl_->host ? impl_->host->editableSignals() : 0;
}

void WebSurface::runJavaScript(const std::string& script)
{
    if (impl_->host) impl_->host->runJavaScript(script);
}

std::string WebSurface::title() const
{
    return impl_->host ? impl_->host->title() : std::string();
}

void WebSurface::resize(int width, int height)
{
    if (impl_->host) impl_->host->resize(width, height);
}

/* ------------------------------------------------------------------- input */

void WebSurface::mouseMove(int x, int y)
{
    if (impl_->host) impl_->host->mouseMove(x, y, currentModifiers());
}

void WebSurface::mouseButton(int x, int y, int button, bool down, int clickCount)
{
    if (impl_->host)
        impl_->host->mouseButton(x, y, buttonFor(button), down, clickCount, currentModifiers());
}

void WebSurface::mouseWheel(int x, int y, float deltaX, float deltaY)
{
    if (impl_->host)
        impl_->host->mouseWheel(x, y, static_cast<int>(deltaX), static_cast<int>(deltaY),
                                currentModifiers());
}

void WebSurface::mouseLeave()
{
    if (impl_->host) impl_->host->mouseLeave(currentModifiers());
}

void WebSurface::key(int rayKey, bool down)
{
    if (impl_->host) impl_->host->keyEvent(virtualKeyFor(rayKey), down, currentModifiers());
}

void WebSurface::character(int codepoint)
{
    if (impl_->host) impl_->host->charEvent(codepoint, currentModifiers());
}

void WebSurface::setFocused(bool focused)
{
    if (!impl_->host || impl_->focused == focused) return;
    impl_->focused = focused;
    impl_->host->setFocus(focused);
}

}  // namespace cromwell
