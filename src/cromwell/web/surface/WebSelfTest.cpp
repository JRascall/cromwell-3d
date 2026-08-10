/* The raylib side of the module — no CEF here, same rule as WebSurface.cpp. */
#include "cromwell/web/surface/WebSelfTest.hpp"

#include "cromwell/web/cef/WebRuntime.hpp"
#include "cromwell/web/surface/WebSurface.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace cromwell {
namespace {

constexpr int kSurfaceWidth  = 640;
constexpr int kSurfaceHeight = 300;

/* One text field, filling a known rectangle at the top so the click below can
 * be aimed at it without measuring anything. The oninput handler is the return
 * channel: whatever lands in the field is copied to the title, which arrives
 * back in the browser process through CefDisplayHandler::OnTitleChange.
 *
 * A data: URL so the test needs no network and no files. */
const char* kPage =
    "data:text/html,"
    "<html><head><title>START</title></head><body style='margin:0'>"
    "<input id='t' style='position:absolute;left:0;top:0;width:600px;height:80px;font-size:32px'"
    " oninput='document.title=\"VALUE:\"+this.value'>"
    "</body></html>";

/* Google's search field is a textarea on the desktop layout and an input on
 * some others, so both are tried. The active element's tag is reported too:
 * "the field lost focus" and "the field never had it" need telling apart. */
const char* kFocusSearch =
    "(function(){var q=document.querySelector('textarea[name=q]')"
    "||document.querySelector('input[name=q]');"
    "if(q){q.focus();document.title='FOCUSED:'+document.activeElement.tagName;}"
    "else{document.title='NO-SEARCH-FIELD';}})();";

const char* kReadSearch =
    "(function(){var q=document.querySelector('textarea[name=q]')"
    "||document.querySelector('input[name=q]');"
    "document.title='VAL:['+(q?q.value:'?')+'] active:'+document.activeElement.tagName;})();";

/* Whatever this site calls its search box. YouTube's is input[name=search_query],
 * Google's is a textarea; the generic type=search catches most of the rest. */
const char* kFocusAnyField =
    "(function(){var q=document.querySelector('input[name=search_query]')"
    "||document.querySelector('textarea[name=q]')"
    "||document.querySelector('input[name=q]')"
    "||document.querySelector('input[type=search]')"
    "||document.querySelector('input[type=text]');"
    "if(q){q.focus();document.title='FOCUSED:'+q.name;}else{document.title='NO-FIELD';}})();";

/* What the field ended up containing, so a run that typed nothing can be told
 * from one that typed and did not navigate. */
const char* kReadAnyField =
    "(function(){var q=document.querySelector('input[name=search_query]')"
    "||document.querySelector('textarea[name=q]')||document.querySelector('input[name=q]');"
    "document.title='VAL:['+(q?q.value:'?')+']';})();";

/* Submits the search the way the page's own button would, rather than relying
 * on the enter key reaching it. Without this the test is not comparable across
 * CEF versions: a build where the key events do not land simply never
 * navigates, and then "it did not crash" means nothing at all. */
const char* kSubmitSearch =
    "(function(){var b=document.querySelector('button#search-icon-legacy')"
    "||document.querySelector('button[aria-label=Search]')"
    "||document.querySelector('#search-icon-legacy');"
    "if(b){b.click();document.title='SUBMITTED';return;}"
    "var q=document.querySelector('input[name=search_query]');"
    "if(q&&q.form){q.form.submit();document.title='FORM-SUBMITTED';return;}"
    "document.title='NO-SUBMIT';})();";

/* Chromium is asynchronous in every direction: a navigation, a focus change
 * and a keystroke each need several turns of the message loop before their
 * effect is observable. Everything here is therefore "pump, then look".
 *
 * THE WAIT IS NOT PADDING. CefDoMessageLoopWork returns immediately when there
 * is nothing to do, so a bare loop of it spins through a thousand iterations
 * in a millisecond and hands Chromium no wall-clock time at all — which is
 * exactly how the first version of this test managed to report that a page had
 * failed to load when it had never been given a chance to start. Real time has
 * to pass between turns, at roughly the rate the game would have pumped it. */
constexpr double kFrameSeconds = 1.0 / 60.0;

void pump(WebRuntime& runtime, WebSurface& page, int frames)
{
    for (int i = 0; i < frames; ++i) {
        runtime.tick();
        page.upload();
        WaitTime(kFrameSeconds);
    }
}

void record(std::string& report, const char* stage, const WebSurface& page)
{
    char line[512];
    /* The url and the paint size are here so that "nothing worked" can be told
     * apart from "the page never loaded", which is a different bug entirely
     * and was the first thing this test got wrong about itself. */
    std::snprintf(line, sizeof(line),
                  "%-22s focus=%d editable=%d signals=%d paint=%dx%d title=\"%s\" url=%.60s\n",
                  stage,
                  page.pageFocused() ? 1 : 0,
                  page.editableFocused() ? 1 : 0,
                  page.editableSignals(),
                  page.width(), page.height(),
                  page.title().c_str(),
                  page.currentUrl().c_str());
    report += line;
}

}  // namespace

std::string runWebSelfTest(WebRuntime& runtime, const std::string& logPath,
                           const std::string& soakUrl,
                           const std::string& typeText)
{
    std::string report = "web self test\n=============\n";

    /* SOAK MODE. Not a test of anything in particular — it loads one page and
     * stays on it, flushing the report after every step. That is the whole
     * trick: if the process dies partway through, the log still names the last
     * step that completed, which is the only thing a crash with no console and
     * no debugger leaves behind. */
    if (!soakUrl.empty()) {
        report += "soak: " + soakUrl + "\n";

        WebSurface soak(runtime, 1024, 720, soakUrl);
        if (!soak.valid()) {
            report += "FAIL: the surface did not start\n";
            std::ofstream(logPath) << report;
            return report;
        }

        /* The reported sequence, reproduced: focus the site's search box, type,
         * press enter. Each step is flushed, so if the process dies the log
         * says which one it died on. */
        if (!typeText.empty()) {
            pump(runtime, soak, 300);
            record(report, "soak loaded", soak);
            std::ofstream(logPath) << report;

            soak.setFocused(true);
            soak.runJavaScript(kFocusAnyField);
            pump(runtime, soak, 60);
            record(report, "field focused", soak);
            std::ofstream(logPath) << report;

            for (const char c : typeText) {
                soak.character(c);
                pump(runtime, soak, 10);
            }
            record(report, "typed", soak);
            std::ofstream(logPath) << report;

            soak.runJavaScript(kReadAnyField);
            pump(runtime, soak, 30);
            record(report, "field contents", soak);
            std::ofstream(logPath) << report;

            soak.key(KEY_ENTER, true);
            soak.key(KEY_ENTER, false);
            pump(runtime, soak, 120);
            record(report, "enter sent", soak);
            std::ofstream(logPath) << report;

            /* Belt and braces: if enter did not move the page, click the
             * search button so the navigation happens either way. */
            soak.runJavaScript(kSubmitSearch);
            pump(runtime, soak, 60);
            record(report, "search submitted", soak);
            std::ofstream(logPath) << report;
        }

        for (int second = 1; second <= 30; ++second) {
            pump(runtime, soak, 60);

            char stage[64];
            std::snprintf(stage, sizeof(stage), "t+%02ds", second);
            record(report, stage, soak);

            /* Rewritten from scratch each time rather than appended to, so a
             * kill mid-write cannot leave a truncated last line. */
            std::ofstream(logPath) << report;
        }

        report += "\nverdict: SURVIVED 30s\n";
        std::ofstream(logPath) << report;
        return report;
    }

    WebSurface page(runtime, kSurfaceWidth, kSurfaceHeight, kPage);
    if (!page.valid()) {
        report += "FAIL: the surface did not start; CEF is not running\n";
        std::ofstream(logPath) << report;
        return report;
    }

    /* 1. Let the page load. */
    pump(runtime, page, 120);
    record(report, "after load", page);

    /* 2. Click the field, exactly as a user would. This is the path that was
     *    reported broken, so it is tested before the scripted shortcut. */
    page.setFocused(true);
    page.mouseMove(300, 40);
    page.mouseButton(300, 40, 0 /* MOUSE_BUTTON_LEFT */, true, 1);
    page.mouseButton(300, 40, 0, false, 1);
    pump(runtime, page, 60);
    record(report, "after click", page);

    /* 3. Type. If the click above did focus the field, this is the real thing
     *    end to end. */
    for (const char c : std::string("abc")) page.character(c);
    pump(runtime, page, 60);
    record(report, "after typing abc", page);

    /* 4. Focus the field from script instead, which cannot fail for want of a
     *    correctly aimed or correctly ordered click. If typing works after
     *    this but not after step 3, the fault is in the click; if it fails
     *    here too, the fault is in the key events. */
    page.runJavaScript("document.getElementById('t').focus();");
    pump(runtime, page, 60);
    record(report, "after script focus", page);

    for (const char c : std::string("xyz")) page.character(c);
    pump(runtime, page, 60);
    record(report, "after typing xyz", page);

    /* 5. Ask the page directly rather than trusting the oninput handler to
     *    have fired, so an empty field is distinguishable from a handler that
     *    never ran. */
    page.runJavaScript("document.title='FINAL:'+document.getElementById('t').value"
                       "+' active:'+document.activeElement.id;");
    pump(runtime, page, 60);
    record(report, "final readback", page);

    /* The verdict, spelled out, so the log does not need interpreting. */
    const std::string title = page.title();
    report += "\nverdict: ";
    if (title.find("FINAL:abcxyz") != std::string::npos)
        report += "PASS - click focus and typing both work\n";
    else if (title.find("FINAL:xyz") != std::string::npos)
        report += "PARTIAL - typing works, but the CLICK did not focus the field\n";
    else if (title.find("FINAL:abc") != std::string::npos)
        report += "PARTIAL - the click worked, but script focus did not\n";
    else if (title.find("active:t") != std::string::npos)
        report += "FAIL - the field IS focused, so the KEY EVENTS are being dropped\n";
    else
        report += "FAIL - the field never took focus at all\n";

    /* ---- phase two: the real thing ------------------------------------
     *
     * A bare <input> on a data URL proves the mechanism and nothing else. The
     * bug being chased only shows on a real site — typing that works for a
     * character or two and then dies — and a page like Google differs in every
     * way that could cause it: a suggestion list that appears after the first
     * keystroke, iframes in other render processes, and script that moves
     * focus around. So the same sequence runs again against the live page,
     * recording after EVERY character rather than after the batch, because
     * "it stopped at the third one" is the whole shape of the report. */
    report += "\nphase two: live page\n--------------------\n";

    page.loadUrl("https://www.google.com");
    pump(runtime, page, 240);
    record(report, "google loaded", page);

    /* Focused by script rather than by coordinate: where the box sits depends
     * on the layout, and a click that misses would look exactly like a click
     * that failed. */
    page.runJavaScript(kFocusSearch);
    pump(runtime, page, 60);
    record(report, "google focused", page);

    const std::string typed = "hello";
    for (size_t i = 0; i < typed.size(); ++i) {
        page.character(typed[i]);
        pump(runtime, page, 30);
        page.runJavaScript(kReadSearch);
        pump(runtime, page, 20);

        char stage[64];
        std::snprintf(stage, sizeof(stage), "google char %d '%c'",
                      static_cast<int>(i + 1), typed[i]);
        record(report, stage, page);
    }

    std::ofstream out(logPath);
    out << report;
    return report;
}

}  // namespace cromwell
