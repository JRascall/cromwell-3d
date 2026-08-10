/* The CEF half of the module. NO raylib IN THIS FILE — see WebBrowserHost.hpp
 * for why the line is drawn here and what crosses it. */
#include "render/web/WebBrowserHost.hpp"

#include "render/web/WebRuntime.hpp"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_permission_handler.h"
#include "include/cef_render_handler.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <cstring>
#include <string>
#include <vector>

namespace xcom {
namespace {

/* Must match the name the render process sends in WebHelperMain.cpp. */
constexpr char kEditableMessage[] = "xcom.editable";

/* Plain int, not an atomic: both callbacks that touch it run on the CEF UI
 * thread, which is the main thread here. */
int gLiveBrowsers = 0;

uint32_t toCefModifiers(uint32_t modifiers)
{
    uint32_t flags = 0;
    if (modifiers & WebModShift)        flags |= EVENTFLAG_SHIFT_DOWN;
    if (modifiers & WebModControl)      flags |= EVENTFLAG_CONTROL_DOWN;
    if (modifiers & WebModAlt)          flags |= EVENTFLAG_ALT_DOWN;
    if (modifiers & WebModLeftButton)   flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (modifiers & WebModMiddleButton) flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (modifiers & WebModRightButton)  flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return flags;
}

/* The virtual key a character would have arrived on, on a US layout.
 *
 * NEEDED BECAUSE A CHARACTER ON ITS OWN IS NOT A KEYSTROKE. Windows sends
 * WM_KEYDOWN then WM_CHAR then WM_KEYUP, and Chromium's input pipeline is
 * written against that: several paths through RenderWidgetHostImpl drop a char
 * event that did not follow a key-down. Synthesising the whole trio is what
 * every working offscreen integration does, and it needs a key code to put on
 * the two outer events.
 *
 * Only has to be plausible, not correct for the user's actual layout — the
 * character itself rides on the middle event, which is what gets inserted.
 * Zero means "no sensible key", and the trio collapses back to a lone char. */
int virtualKeyForCharacter(int codepoint)
{
    if (codepoint >= 'a' && codepoint <= 'z') return codepoint - 'a' + 'A';
    if (codepoint >= 'A' && codepoint <= 'Z') return codepoint;
    if (codepoint >= '0' && codepoint <= '9') return codepoint;
    if (codepoint == ' ') return 0x20;  // VK_SPACE

    /* The OEM keys, by the character they carry unshifted or shifted on a US
     * keyboard. */
    switch (codepoint) {
        case ';': case ':':  return 0xBA;  // VK_OEM_1
        case '=': case '+':  return 0xBB;  // VK_OEM_PLUS
        case ',': case '<':  return 0xBC;  // VK_OEM_COMMA
        case '-': case '_':  return 0xBD;  // VK_OEM_MINUS
        case '.': case '>':  return 0xBE;  // VK_OEM_PERIOD
        case '/': case '?':  return 0xBF;  // VK_OEM_2
        case '`': case '~':  return 0xC0;  // VK_OEM_3
        case '[': case '{':  return 0xDB;  // VK_OEM_4
        case '\\': case '|': return 0xDC;  // VK_OEM_5
        case ']': case '}':  return 0xDD;  // VK_OEM_6
        case '\'': case '"': return 0xDE;  // VK_OEM_7
        default: break;
    }

    /* The shifted digits, which share their key with the digit. */
    switch (codepoint) {
        case '!': return '1';  case '@': return '2';  case '#': return '3';
        case '$': return '4';  case '%': return '5';  case '^': return '6';
        case '&': return '7';  case '*': return '8';  case '(': return '9';
        case ')': return '0';
        default: return 0;
    }
}

CefMouseEvent mouseEventAt(int x, int y, uint32_t modifiers)
{
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = toCefModifiers(modifiers);
    return event;
}

}  // namespace

/* ------------------------------------------------------------------ client */

/* One object implementing every handler this surface needs. CEF's interfaces
 * are separate so a client can split them across objects; nothing here wants
 * that, and one object means one reference count to reason about. */
class SurfaceClient : public CefClient,
                      public CefRenderHandler,
                      public CefDisplayHandler,
                      public CefPermissionHandler,
                      public CefLifeSpanHandler {
public:
    SurfaceClient(int width, int height) : width_(width), height_(height)
    {
        pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
    }

    CefRefPtr<CefRenderHandler>   GetRenderHandler() override   { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override  { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }

    /* -- CefPermissionHandler --
     *
     * ANSWERING THESE IS NOT OPTIONAL FOR A WINDOWLESS BROWSER. A permission
     * prompt is a piece of browser chrome, and an offscreen Alloy-style
     * browser has none to put it in. Leave them unimplemented and Chromium
     * goes looking for UI that does not exist; on YouTube, which queries
     * permissions while building a results page, that ends in a null
     * dereference deep inside libcef.dll and takes the game down with it. See
     * chromiumembedded/cef#3643, which is the same fault reached from the
     * storage-access check.
     *
     * Everything is refused rather than dismissed. Returning true is the part
     * that matters: it tells CEF the client has dealt with the prompt and that
     * no window has to be opened for it. */
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>,
                                uint64_t,
                                const CefString&,
                                uint32_t,
                                CefRefPtr<CefPermissionPromptCallback> callback) override
    {
        if (callback) callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return true;
    }

    /* The camera and microphone, asked for separately from everything else. A
     * tactics game's browser panel has no business granting either. */
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>,
                                        CefRefPtr<CefFrame>,
                                        const CefString&,
                                        uint32_t,
                                        CefRefPtr<CefMediaAccessCallback> callback) override
    {
        if (callback) callback->Continue(CEF_MEDIA_PERMISSION_NONE);
        return true;
    }

    /* -- CefDisplayHandler -- */

    /* The title is the cheapest channel out of a page: one string, delivered
     * to the browser process without an IPC of our own. WebSelfTest writes an
     * answer into document.title and reads it back here. */
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override
    {
        title_ = title.ToString();
    }

    /* -- CefRenderHandler -- */

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        rect.Set(0, 0, width_, height_);
    }

    void OnPaint(CefRefPtr<CefBrowser>,
                 PaintElementType type,
                 const RectList&,
                 const void* buffer,
                 int width,
                 int height) override
    {
        /* PET_POPUP is the separate layer Chromium composites a native <select>
         * dropdown into, with its own rect from OnPopupSize. Phase one does not
         * composite it, so those paints are dropped and a dropdown renders as
         * nothing. Everything that is an ordinary DOM element — including
         * Google's search suggestions — comes through PET_VIEW and is
         * unaffected. */
        if (type != PET_VIEW) return;

        /* The buffer is the whole view every time, not just the dirty rects, so
         * this is a whole-surface copy rather than a per-rect blit. */
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        if (bytes == 0) return;

        if (width != width_ || height != height_) {
            /* A paint that raced ahead of a resize. Take the new size and let
             * the raylib side rebuild its texture to match. */
            width_  = width;
            height_ = height;
        }
        if (pixels_.size() != bytes) pixels_.resize(bytes);

        /* B AND R SWAPPED HERE, NOT IN A SHADER. Chromium composites BGRA and
         * raylib has no BGRA texture format, so the channels have to be
         * exchanged somewhere. A fragment shader did it for free while this
         * surface was drawn by hand — but it is drawn by ImGui now, through
         * ImGui's own shader, and there is no supported way to substitute one
         * for a single image.
         *
         * The cost is close to nothing: this replaces a memcpy of the same
         * buffer rather than adding a pass over it, so the memory traffic is
         * unchanged and only a byte shuffle is new. It vectorises. */
        const uint32_t* source = static_cast<const uint32_t*>(buffer);
        uint32_t* destination  = reinterpret_cast<uint32_t*>(pixels_.data());
        const size_t count     = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t bgra = source[i];
            destination[i] = (bgra & 0xFF00FF00u)          // G and A stay put
                           | ((bgra & 0x00FF0000u) >> 16)  // R down to byte 0
                           | ((bgra & 0x000000FFu) << 16); // B up to byte 2
        }
        dirty_ = true;
    }

    /* Chromium raises this when it wants an on-screen keyboard shown or
     * hidden. It reads like the right signal for "is the caret in a field",
     * and on a touch platform it is — but on desktop offscreen rendering it
     * does not reliably fire at all, which is why clicking a search box used
     * to focus it without ever letting anything be typed into it.
     *
     * Kept as a second opinion rather than deleted: when it does fire it is
     * correct, and both signals clear themselves, so OR-ing them cannot leave
     * the keyboard stuck. The one that actually does the work is the render
     * process message below. */
    void OnVirtualKeyboardRequested(CefRefPtr<CefBrowser>, TextInputMode mode) override
    {
        keyboardRequested_ = (mode != CEF_TEXT_INPUT_MODE_NONE);
    }

    /* -- CefClient -- */

    /* The answer to "is an editable node focused", sent from the render
     * process by HelperApp::OnFocusedNodeChanged — the only place the DOM is
     * visible. See WebHelperMain.cpp. */
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>,
                                  CefRefPtr<CefFrame>,
                                  CefProcessId,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        if (message->GetName() != kEditableMessage) return false;
        nodeEditable_ = message->GetArgumentList()->GetBool(0);
        /* Counted, not just stored. "The caret is not in a field" and "the
         * render process has never told us anything" look identical from the
         * browser side and need completely different fixes, so the panel shows
         * this and the difference stops being a guess. */
        ++editableSignals_;
        return true;
    }

    /* -- CefLifeSpanHandler -- */

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        CEF_REQUIRE_UI_THREAD();
        browser_ = browser;
        ++gLiveBrowsers;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        CEF_REQUIRE_UI_THREAD();
        browser_ = nullptr;
        --gLiveBrowsers;
    }

    /* A windowless browser has nowhere to put a popup window, so a
     * target=_blank would open a browser nothing ever renders. Navigating the
     * surface itself is what a game UI wants anyway. */
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame>,
                       int,
                       const CefString& target_url,
                       const CefString&,
                       WindowOpenDisposition,
                       bool,
                       const CefPopupFeatures&,
                       CefWindowInfo&,
                       CefRefPtr<CefClient>&,
                       CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&,
                       bool*) override
    {
        /* POSTED, NOT CALLED. Navigating from inside Chromium's own
         * popup-creation callback re-enters it mid-decision; the task queue is
         * the documented way to say "do this next turn instead". This was not
         * the cause of the YouTube crash — that was ruled out by testing — but
         * it is the kind of re-entrancy that produces one. */
        if (browser) {
            const CefString url = target_url;
            CefRefPtr<CefBrowser> target = browser;
            CefPostTask(TID_UI, base::BindOnce(
                [](CefRefPtr<CefBrowser> b, CefString u) {
                    if (b && b->GetMainFrame()) b->GetMainFrame()->LoadURL(u);
                },
                target, url));
        }
        return true;  // block the popup; we navigate instead, next turn
    }

    /* -- used by the host -- */

    CefRefPtr<CefBrowser> browser() const { return browser_; }
    bool editable() const { return nodeEditable_ || keyboardRequested_; }
    const std::string& title() const { return title_; }
    int  editableSignals() const { return editableSignals_; }
    bool dirty() const    { return dirty_; }
    void clearDirty()     { dirty_ = false; }
    const uint8_t* pixels() const { return pixels_.data(); }
    int  width() const  { return width_; }
    int  height() const { return height_; }

    void setSize(int width, int height)
    {
        width_  = width;
        height_ = height;
        pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
    }

private:
    CefRefPtr<CefBrowser> browser_;
    std::vector<uint8_t>  pixels_;
    int  width_  = 0;
    int  height_ = 0;
    bool dirty_             = false;
    bool nodeEditable_      = false;  // from the render process, authoritative
    bool keyboardRequested_ = false;  // from OnVirtualKeyboardRequested, if it fires
    int  editableSignals_   = 0;      // how many render-process messages arrived
    std::string title_;

    IMPLEMENT_REFCOUNTING(SurfaceClient);
    DISALLOW_COPY_AND_ASSIGN(SurfaceClient);
};

/* -------------------------------------------------------------------- host */

class CefBrowserHostImpl final : public WebBrowserHost {
public:
    CefBrowserHostImpl(int width, int height, const std::string& url)
    {
        client_ = new SurfaceClient(width, height);

        CefWindowInfo windowInfo;
        /* No parent window. CEF uses it only to pick a monitor and to parent
         * dialogs; handing over the game's HWND would make Chromium's own
         * dialogs modal to it, which is not wanted while this is a panel. */
        windowInfo.SetAsWindowless(nullptr);
        windowInfo.shared_texture_enabled       = 0;  // phase one is the CPU path
        windowInfo.external_begin_frame_enabled = 0;

        CefBrowserSettings browserSettings;
        browserSettings.windowless_frame_rate = 60;

        /* Opaque on purpose. A transparent background makes Chromium emit
         * premultiplied alpha, which then has to be drawn with
         * BLEND_ALPHA_PREMULTIPLY or every glyph edge picks up a dark halo.
         * That is the right thing for a HUD that overlays the world and the
         * wrong complication for a first page load. */
        browserSettings.background_color = CefColorSetARGB(255, 255, 255, 255);

        CefBrowserHost::CreateBrowser(windowInfo, client_, CefString(url),
                                      browserSettings, nullptr, nullptr);
    }

    ~CefBrowserHostImpl() override { close(); }

    bool takeFrame(const uint8_t** pixels, int* width, int* height) override
    {
        if (!client_ || !client_->dirty()) return false;
        *pixels = client_->pixels();
        *width  = client_->width();
        *height = client_->height();
        client_->clearDirty();
        return true;
    }

    void loadUrl(const std::string& url) override
    {
        if (CefRefPtr<CefBrowser> browser = current())
            browser->GetMainFrame()->LoadURL(CefString(url));
    }

    void reload() override
    {
        if (CefRefPtr<CefBrowser> browser = current()) browser->Reload();
    }

    void resize(int width, int height) override
    {
        if (!client_) return;
        if (width == client_->width() && height == client_->height()) return;
        client_->setSize(width, height);
        if (CefRefPtr<CefBrowser> browser = current()) browser->GetHost()->WasResized();
    }

    void goBack() override
    {
        if (CefRefPtr<CefBrowser> browser = current()) browser->GoBack();
    }

    void goForward() override
    {
        if (CefRefPtr<CefBrowser> browser = current()) browser->GoForward();
    }

    bool canGoBack() const override
    {
        CefRefPtr<CefBrowser> browser = current();
        return browser && browser->CanGoBack();
    }

    bool canGoForward() const override
    {
        CefRefPtr<CefBrowser> browser = current();
        return browser && browser->CanGoForward();
    }

    bool loading() const override
    {
        CefRefPtr<CefBrowser> browser = current();
        return browser && browser->IsLoading();
    }

    void runJavaScript(const std::string& script) override
    {
        CefRefPtr<CefBrowser> browser = current();
        if (!browser) return;
        CefRefPtr<CefFrame> frame = browser->GetMainFrame();
        if (frame) frame->ExecuteJavaScript(CefString(script), frame->GetURL(), 0);
    }

    std::string title() const override { return client_ ? client_->title() : std::string(); }

    std::string currentUrl() const override
    {
        CefRefPtr<CefBrowser> browser = current();
        if (!browser) return {};
        CefRefPtr<CefFrame> frame = browser->GetMainFrame();
        return frame ? frame->GetURL().ToString() : std::string{};
    }

    int  width() const override  { return client_ ? client_->width() : 0; }
    int  height() const override { return client_ ? client_->height() : 0; }
    bool editableFocused() const override { return client_ && client_->editable(); }
    int  editableSignals() const override { return client_ ? client_->editableSignals() : 0; }

    void mouseMove(int x, int y, uint32_t modifiers) override
    {
        if (CefRefPtr<CefBrowser> browser = current())
            browser->GetHost()->SendMouseMoveEvent(mouseEventAt(x, y, modifiers), false);
    }

    void mouseButton(int x, int y, WebMouseButton button, bool down,
                     int clickCount, uint32_t modifiers) override
    {
        CefRefPtr<CefBrowser> browser = current();
        if (!browser) return;

        cef_mouse_button_type_t type = MBT_LEFT;
        if (button == WebMouseButton::Right)  type = MBT_RIGHT;
        if (button == WebMouseButton::Middle) type = MBT_MIDDLE;

        browser->GetHost()->SendMouseClickEvent(mouseEventAt(x, y, modifiers),
                                                type, !down, clickCount);
    }

    void mouseWheel(int x, int y, int deltaX, int deltaY, uint32_t modifiers) override
    {
        if (CefRefPtr<CefBrowser> browser = current())
            browser->GetHost()->SendMouseWheelEvent(mouseEventAt(x, y, modifiers),
                                                    deltaX, deltaY);
    }

    void mouseLeave(uint32_t modifiers) override
    {
        if (CefRefPtr<CefBrowser> browser = current())
            browser->GetHost()->SendMouseMoveEvent(mouseEventAt(-1, -1, modifiers), true);
    }

    void keyEvent(int virtualKey, bool down, uint32_t modifiers) override
    {
        CefRefPtr<CefBrowser> browser = current();
        if (!browser || virtualKey == 0) return;

        CefKeyEvent event;
        event.type             = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
        event.windows_key_code = virtualKey;
        /* Zero rather than the lParam a real WM_KEYDOWN would carry. Chromium
         * uses it to derive the physical-key half of a DOM event
         * (KeyboardEvent.code), which nothing in a game panel reads; the
         * logical key it does read comes from windows_key_code above. */
        event.native_key_code  = 0;
        event.modifiers        = toCefModifiers(modifiers);
        event.is_system_key    = false;

        browser->GetHost()->SendKeyEvent(event);
    }

    void charEvent(int codepoint, uint32_t modifiers) override
    {
        CefRefPtr<CefBrowser> browser = current();
        if (!browser) return;

        /* THE WHOLE TRIO, NOT JUST THE CHARACTER. See virtualKeyForCharacter:
         * a lone KEYEVENT_CHAR is not what a keyboard produces and Chromium is
         * entitled to ignore it. Down, character, up — the same three messages
         * Windows would have delivered. */
        const int virtualKey = virtualKeyForCharacter(codepoint);

        CefKeyEvent event;
        event.modifiers       = toCefModifiers(modifiers);
        event.is_system_key   = false;
        event.native_key_code = 0;

        if (virtualKey != 0) {
            event.type                 = KEYEVENT_RAWKEYDOWN;
            event.windows_key_code     = virtualKey;
            event.character            = 0;
            event.unmodified_character = 0;
            browser->GetHost()->SendKeyEvent(event);
        }

        /* THE CHARACTER, NOT THE KEY, and the difference is not cosmetic.
         * Windows puts the character in WM_CHAR's wParam and Chromium reads
         * windows_key_code the same way for a char event — so putting the
         * virtual key here instead types 'A' when 'a' was pressed, for every
         * letter. The outer two events are the ones that want the key code. */
        event.type                 = KEYEVENT_CHAR;
        event.windows_key_code     = codepoint;
        event.character            = static_cast<char16_t>(codepoint);
        event.unmodified_character = static_cast<char16_t>(codepoint);
        browser->GetHost()->SendKeyEvent(event);

        if (virtualKey != 0) {
            event.type                 = KEYEVENT_KEYUP;
            event.windows_key_code     = virtualKey;
            event.character            = 0;
            event.unmodified_character = 0;
            browser->GetHost()->SendKeyEvent(event);
        }
    }

    void setFocus(bool focused) override
    {
        if (focused_ == focused) return;
        focused_ = focused;
        if (CefRefPtr<CefBrowser> browser = current()) browser->GetHost()->SetFocus(focused);
    }

    void close() override
    {
        if (!client_) return;
        if (CefRefPtr<CefBrowser> browser = current()) {
            /* force_close: there is no user to prompt and no beforeunload
             * dialog anyone could answer. WebRuntime::stop pumps the loop
             * afterwards, which is what actually completes this. */
            browser->GetHost()->CloseBrowser(true);
        }
        client_ = nullptr;
    }

private:
    CefRefPtr<CefBrowser> current() const
    {
        return client_ ? client_->browser() : nullptr;
    }

    CefRefPtr<SurfaceClient> client_;
    bool focused_ = false;
};

int liveBrowserCount() { return gLiveBrowsers; }

std::unique_ptr<WebBrowserHost> WebBrowserHost::create(WebRuntime& runtime,
                                                       int width, int height,
                                                       const std::string& url)
{
    if (!runtime.valid()) return nullptr;
    return std::make_unique<CefBrowserHostImpl>(width, height, url);
}

}  // namespace xcom
