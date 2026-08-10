/* WebBrowserHost.hpp — a browser, described without naming CEF or raylib.
 *
 * SINGLE RESPONSIBILITY: be the seam between the two halves of this module.
 *
 * THIS EXISTS BECAUSE raylib.h AND windows.h CANNOT SHARE A TRANSLATION UNIT.
 * Both declare CloseWindow and ShowCursor with C linkage and incompatible
 * signatures — raylib's `void CloseWindow(void)` against winuser's
 * `BOOL CloseWindow(HWND)` — so a file that sees both fails with C2733 and no
 * include order fixes it, because the clash is the declarations themselves.
 * Every CEF header pulls in windows.h, so "include CEF" and "include raylib"
 * are mutually exclusive per file.
 *
 * So the module is split down that line and this interface is the join:
 *
 *   WebSurface.cpp      raylib, no CEF   — the texture, the upload, the
 *                                          translation of raylib input
 *   WebBrowserHost.cpp  CEF, no raylib   — the browser, the paint callback,
 *                                          the BGRA buffer
 *
 * Which means nothing below may name a type from either side. Coordinates are
 * ints, pixels are bytes, buttons and modifiers are the plain enums declared
 * here, and key codes are Windows virtual-key numbers — chosen because CEF
 * wants them anyway and because they are just integers.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace cromwell {

class WebRuntime;

enum WebModifier : uint32_t {
    WebModShift        = 1u << 0,
    WebModControl      = 1u << 1,
    WebModAlt          = 1u << 2,
    WebModLeftButton   = 1u << 3,
    WebModMiddleButton = 1u << 4,
    WebModRightButton  = 1u << 5,
};

enum class WebMouseButton { Left, Middle, Right };

/* How many browsers exist right now, counted from CEF's own created/closed
 * callbacks. WebRuntime::stop waits on this: CefShutdown must not run while a
 * browser is still alive, and closing one is asynchronous. */
int liveBrowserCount();

class WebBrowserHost {
public:
    /* Null when the runtime is not up. */
    static std::unique_ptr<WebBrowserHost> create(WebRuntime& runtime,
                                                  int width, int height,
                                                  const std::string& url);

    virtual ~WebBrowserHost() = default;

    /* The most recent frame, or nothing. Returns false when the page has not
     * repainted since the last call, which is the common case and the reason
     * the caller does not upload a texture every frame.
     *
     * The buffer is BGRA, top-left origin, width*height*4 bytes, and it is
     * owned by the host — valid until the next call into it. */
    virtual bool takeFrame(const uint8_t** pixels, int* width, int* height) = 0;

    virtual void loadUrl(const std::string& url) = 0;
    virtual void reload() = 0;
    virtual void resize(int width, int height) = 0;

    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual bool canGoBack() const = 0;
    virtual bool canGoForward() const = 0;
    virtual bool loading() const = 0;

    /* Where the main frame actually is, which is not where it was asked to go
     * — a redirect, a search submission or a link click all move it. Empty
     * before the first navigation commits. */
    virtual std::string currentUrl() const = 0;

    /* Runs a script in the main frame. Fire and forget - anything the script
     * needs to report comes back through document.title, which arrives as
     * title() below. Used by the self-test to focus a field and read a value
     * back without a human at the keyboard. */
    virtual void runJavaScript(const std::string& script) = 0;
    virtual std::string title() const = 0;

    virtual int  width() const = 0;
    virtual int  height() const = 0;

    /* Is an editable node focused inside the page. See WebSurface::wantsKeyboard. */
    virtual bool editableFocused() const = 0;

    /* How many focus messages the render process has sent. Zero after clicking
     * a text field means the IPC is broken, not that the field is unfocused. */
    virtual int  editableSignals() const = 0;

    virtual void mouseMove(int x, int y, uint32_t modifiers) = 0;
    virtual void mouseButton(int x, int y, WebMouseButton button, bool down,
                             int clickCount, uint32_t modifiers) = 0;
    virtual void mouseWheel(int x, int y, int deltaX, int deltaY, uint32_t modifiers) = 0;
    virtual void mouseLeave(uint32_t modifiers) = 0;

    /* `virtualKey` is a Windows virtual-key code; see the note above. */
    virtual void keyEvent(int virtualKey, bool down, uint32_t modifiers) = 0;
    virtual void charEvent(int codepoint, uint32_t modifiers) = 0;

    virtual void setFocus(bool focused) = 0;

    /* Asks the browser to close and returns once it has been told. Completing
     * the close needs WebRuntime::stop to pump the loop. */
    virtual void close() = 0;
};

}  // namespace cromwell
