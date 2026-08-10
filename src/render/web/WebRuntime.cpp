#include "render/web/WebRuntime.hpp"

#include "render/web/WebBrowserHost.hpp"

#include "include/cef_app.h"
#include "include/cef_version.h"

#include <windows.h>

#include <filesystem>

namespace xcom {
namespace {

/* Where the executable lives, which is also where the CEF runtime was staged
 * by cmake/copy_cef_runtime.cmake. Everything CEF needs is addressed from here
 * rather than from the working directory: the app is routinely launched from
 * the project root by one person and from build2/Release by another, and the
 * .pak files must be found either way. */
std::filesystem::path executableDirectory()
{
    wchar_t buffer[MAX_PATH] = { 0 };
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
}

CefString toCef(const std::filesystem::path& path)
{
    return CefString(path.wstring());
}

}  // namespace

struct WebRuntime::Impl {
    /* Nothing yet. CefApp is only needed to customise command lines or
     * register schemes, and phase one does neither — CefInitialize accepts a
     * null application. The struct stays so that adding one later does not
     * change the header. */
};

WebRuntime::WebRuntime() : impl_(std::make_unique<Impl>()) {}

WebRuntime::~WebRuntime() { stop(); }

bool WebRuntime::start()
{
    if (started_) return true;

    const std::filesystem::path root   = executableDirectory();
    const std::filesystem::path helper = root / "xcom_web_helper.exe";

    if (!std::filesystem::exists(helper)) {
        reason_ = "xcom_web_helper.exe not found beside the executable";
        return false;
    }
    if (!std::filesystem::exists(root / "libcef.dll")) {
        reason_ = "libcef.dll not found beside the executable";
        return false;
    }

    CefMainArgs args(GetModuleHandleW(nullptr));

    CefSettings settings;
    settings.no_sandbox = 1;

    /* THE PUMP IS OURS. multi_threaded_message_loop would put Chromium on its
     * own thread and make OnPaint arrive off the main thread, which the whole
     * of WebSurface is written to assume it does not. external_message_pump
     * would be the more precise option — it asks to be told when work is
     * pending rather than being polled — but it needs OnScheduleMessagePumpWork
     * and a timer, and at 60 fps a poll per frame is the same thing for less
     * machinery. Revisit when the frame budget says so. */
    settings.multi_threaded_message_loop = 0;
    settings.external_message_pump       = 0;

    settings.windowless_rendering_enabled = 1;

    CefString(&settings.browser_subprocess_path) = toCef(helper);
    CefString(&settings.resources_dir_path)      = toCef(root);
    CefString(&settings.locales_dir_path)        = toCef(root / "locales");

    /* Chromium wants somewhere to put its profile. Under the build tree rather
     * than %LOCALAPPDATA% so that deleting the project takes the cache with it,
     * and so two checkouts do not fight over one profile. */
    const std::filesystem::path cache = root / "webcache";
    std::error_code ignored;
    std::filesystem::create_directories(cache, ignored);
    CefString(&settings.root_cache_path) = toCef(cache);
    CefString(&settings.cache_path)      = toCef(cache);

    settings.log_severity = LOGSEVERITY_WARNING;

    /* Beside the executable rather than in the working directory, so a crash
     * report lands somewhere findable whichever directory the app was started
     * from. Chromium writes renderer and GPU process failures here, which is
     * the only account of them a windowed build ever gets - there is no
     * console attached to a WIN32 executable. */
    CefString(&settings.log_file) = toCef(root / "cef.log");

    if (!CefInitialize(args, settings, nullptr, nullptr)) {
        reason_ = "CefInitialize failed (exit code "
                + std::to_string(CefGetExitCode()) + ")";
        return false;
    }

    started_ = true;
    reason_.clear();
    return true;
}

void WebRuntime::tick()
{
    if (!started_) return;
    CefDoMessageLoopWork();
}

void WebRuntime::stop()
{
    if (!started_) return;

    /* CefShutdown expects every browser to be gone, and closing one is
     * asynchronous: CEF posts the teardown and finishes it several loop turns
     * later, once the render and GPU processes have actually exited.
     *
     * SLEEPING IS THE POINT, NOT THE PUMPING. This used to be ten bare calls to
     * CefDoMessageLoopWork, which returns immediately when there is nothing
     * queued — so the whole loop finished in microseconds, gave Chromium no
     * wall-clock time at all, and CefShutdown ran while browsers were still
     * alive. The visible symptom was nine orphaned xcom_web_helper.exe
     * processes surviving a clean exit, each holding ~50 MB and a file lock on
     * the executable that the next build then could not overwrite.
     *
     * Waits on CEF's own count of live browsers rather than on a fixed delay,
     * so a normal exit costs a few milliseconds and only a stuck teardown pays
     * the full second. */
    constexpr int kMaxWaitMilliseconds = 1000;
    constexpr int kStepMilliseconds    = 5;
    for (int waited = 0; waited < kMaxWaitMilliseconds; waited += kStepMilliseconds) {
        CefDoMessageLoopWork();
        if (liveBrowserCount() <= 0) break;
        Sleep(kStepMilliseconds);
    }

    /* A few more turns after the last browser is gone: the close completing is
     * not the same as the teardown tasks it queued having run. */
    for (int i = 0; i < 10; ++i) {
        CefDoMessageLoopWork();
        Sleep(kStepMilliseconds);
    }

    CefShutdown();
    started_ = false;
}

}  // namespace xcom
