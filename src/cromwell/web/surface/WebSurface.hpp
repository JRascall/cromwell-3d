/* WebSurface.hpp — one browser view, as a texture this renderer can sample.
 *
 * SINGLE RESPONSIBILITY: hand out a Texture2D holding the latest browser frame.
 *
 * THE ACQUISITION IS PLATFORM-SPECIFIC AND THE RESULT IS NOT. This is the CPU
 * path — CefRenderHandler::OnPaint, a BGRA buffer, one UpdateTexture per dirty
 * frame. It is the slow path and it is deliberately first: it is byte-identical
 * on Windows, macOS and Linux, and it is the fallback the GPU path needs
 * anyway. Chromium falls back to software compositing under --disable-gpu,
 * SwiftShader, RDP and blacklisted drivers, and when it does, OnAcceleratedPaint
 * is simply never called. A build without this path has no answer for that.
 *
 * THE TEXTURE IS RGBA AND sRGB. RGBA because ImGui draws it through its own
 * shader and there is no substituting one per image, so Chromium's BGRA is
 * exchanged during the copy that was happening anyway — see the note in
 * WebBrowserHost.cpp. sRGB rather than linear, because Chromium has no concept
 * of this renderer's exposure: a caller drawing inside HdrTarget::Scope must
 * linearise and scale it, and a caller on the backbuffer, after ToneMapPass,
 * must not.
 *
 * SINGLE-THREADED BY CONSTRUCTION. WebRuntime runs CEF without its own message
 * loop, so the CEF UI thread IS the thread that called CefInitialize, which is
 * the thread that calls tick(), which is the main thread. OnPaint therefore
 * arrives inside WebRuntime::tick() on the main thread, and the buffer below
 * needs no lock. Give CEF its own message loop and that stops being true.
 */
#pragma once

#include "raylib.h"

#include <memory>
#include <string>

namespace cromwell {

class WebRuntime;

class WebSurface {
public:
    /* Sized in browser pixels, which are also texture texels. A page authored
     * at 1280 wide in a 512-wide surface is unreadable; pick the size against
     * the content, not against the quad it lands on. */
    WebSurface(WebRuntime& runtime, int width, int height, const std::string& url);
    ~WebSurface();

    WebSurface(const WebSurface&) = delete;
    WebSurface& operator=(const WebSurface&) = delete;

    bool valid() const;

    /* Uploads the most recent OnPaint into the texture if one has arrived since
     * the last call. Cheap and safe to call every frame; does nothing when the
     * page has not repainted. Must run with a current GL context and outside
     * BeginTextureMode. */
    void upload();

    /* Zero until the first paint lands. */
    Texture2D texture() const;
    int  width() const;
    int  height() const;

    void loadUrl(const std::string& url);
    void reload();
    void goBack();
    void goForward();
    bool canGoBack() const;
    bool canGoForward() const;
    bool loading() const;
    std::string currentUrl() const;

    /* RESIZE IT TO THE PIXELS IT WILL OCCUPY, ALWAYS. This is a page of text,
     * and text survives no resampling at all: a 1280-wide surface shown in a
     * 900-wide panel is a 0.7 scale factor across every glyph stem, and no
     * filter setting rescues that. The only sharp configuration is one texel
     * per pixel, which means the browser has to be told the panel's real size
     * rather than laid out at some nominal width and scaled to fit.
     *
     * Cheap to call every frame — it returns immediately when the size has not
     * changed — but not free when it has: Chromium relayouts the page and
     * repaints it, so a caller dragging a window edge should settle before
     * calling rather than resize on every frame of the drag. */
    void resize(int width, int height);

    /* Should keystrokes go to the page. True from the click that focuses it
     * until the click, or the escape, that lets it go.
     *
     * IT USED TO ASK CHROMIUM WHETHER A TEXT FIELD WAS FOCUSED, which is a
     * better rule and does not work. Neither signal CEF offers is dependable
     * for offscreen rendering: OnVirtualKeyboardRequested is tied to platforms
     * that put a keyboard on screen and mostly never fires, and
     * OnFocusedNodeChanged — measured against a live Google page by
     * runWebSelfTest — did not fire at all when script focused the search box.
     * Between them they left the gate flapping: typing would work for a
     * character or two and then stop, which is worse than any honest rule.
     *
     * So the rule is now one this code owns outright and can therefore
     * guarantee. The cost is that game hotkeys are suppressed for as long as
     * the page holds focus rather than only while a caret is in a field. The
     * gain is that it is predictable, and that letting go is always one click
     * outside or one press of escape.
     *
     * editableFocused below is still reported, because it is useful to see —
     * it is just no longer trusted to decide anything. */
    bool wantsKeyboard() const;

    bool pageFocused() const;
    bool editableFocused() const;
    int  editableSignals() const;

    /* Script in, one string out. The return channel is document.title, which
     * is enough for the self-test and costs no IPC of our own. */
    void runJavaScript(const std::string& script);
    std::string title() const;

    /* Input. Coordinates are in surface texels with the origin top-left, which
     * is what CEF wants and what a UV multiplied by size() gives you. */
    void mouseMove(int x, int y);
    void mouseButton(int x, int y, int button, bool down, int clickCount = 1);
    void mouseWheel(int x, int y, float deltaX, float deltaY);
    void mouseLeave();
    void key(int rayKey, bool down);
    void character(int codepoint);
    void setFocused(bool focused);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cromwell
