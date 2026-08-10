/* WebHelperMain.cpp — the whole of cromwell_web_helper.exe.
 *
 * SINGLE RESPONSIBILITY: be the process Chromium re-launches, and answer the
 * one question that can only be answered from inside it.
 *
 * Chromium is not one process. It spawns a renderer per site, a GPU process, a
 * network service and assorted utilities, and it spawns them by running an
 * executable again with different command-line switches. Point that at
 * xcom.exe and every one of those processes runs InitWindow, loads the shaders
 * and builds the world before CefExecuteProcess gets a look in.
 *
 * So the subprocess is this instead: no raylib, no game, no window.
 *
 * IT IS ALSO THE ONLY PLACE THE DOM EXISTS. Whether the caret is in a text
 * field is a fact about the page, and the page lives here, in the render
 * process — the browser process where the game runs cannot see it. That answer
 * is what decides whether keystrokes belong to the web view or to the game
 * (see WebSurface::wantsKeyboard), so it is computed here and sent across.
 */
#include "include/cef_app.h"
#include "include/cef_dom.h"
#include "include/cef_process_message.h"

#include <windows.h>

namespace {

/* Must match the name the browser side listens for in WebBrowserHost.cpp. */
constexpr char kEditableMessage[] = "cromwell.editable";

class HelperApp : public CefApp, public CefRenderProcessHandler {
public:
    /* Spelled out because DISALLOW_COPY_AND_ASSIGN below declares a copy
     * constructor, and any user-declared constructor suppresses the implicit
     * default one. */
    HelperApp() = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    /* Fires on every focus change inside the page, with a null node when focus
     * is cleared. This is the signal CEF actually guarantees for OSR;
     * CefRenderHandler::OnVirtualKeyboardRequested looks like it should do the
     * same job from the browser process, but it is tied to platforms that put
     * a keyboard on the screen and cannot be relied on here. */
    void OnFocusedNodeChanged(CefRefPtr<CefBrowser>,
                              CefRefPtr<CefFrame> frame,
                              CefRefPtr<CefDOMNode> node) override
    {
        const bool editable = node && node->IsEditable();

        /* Edge-triggered: focus changes fire constantly on a busy page and
         * most of them move between non-editable nodes. */
        if (editable == lastEditable_) return;
        lastEditable_ = editable;

        if (!frame) return;
        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kEditableMessage);
        message->GetArgumentList()->SetBool(0, editable);
        frame->SendProcessMessage(PID_BROWSER, message);
    }

private:
    bool lastEditable_ = false;

    IMPLEMENT_REFCOUNTING(HelperApp);
    DISALLOW_COPY_AND_ASSIGN(HelperApp);
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    CefMainArgs args(instance);
    CefRefPtr<HelperApp> app(new HelperApp);

    /* Does not return until this subprocess is finished; its result is the
     * process exit code. */
    return CefExecuteProcess(args, app, nullptr);
}
