/* WebRuntime.hpp — Chromium's lifetime, expressed as one object.
 *
 * SINGLE RESPONSIBILITY: bring CEF up, give it a slice of every frame, take it
 * down again.
 *
 * NOT ONE CEF HEADER IN THIS FILE, and that is the point. CEF requires
 * /std:c++20 and compiles its own translation units at a warning bar this
 * project does not meet; the rest of the project is C++17. Keeping every CEF type
 * behind a forward-declared Impl is what lets both be true at once, and it is
 * why Application.cpp can include this without inheriting any of it.
 *
 * ONE PER PROCESS. CefInitialize is not re-entrant and CefShutdown is final:
 * once it has run, no browser can be created again for the life of the
 * process. Construct this next to the window and let it outlive every surface.
 *
 * THE PUMP IS NOT OPTIONAL. CEF is configured here without its own message
 * loop, so nothing in Chromium advances — not layout, not network, not the
 * paint callback — unless tick() is called. Once per frame, outside
 * BeginDrawing, is where it belongs.
 */
#pragma once

#include <memory>
#include <string>

namespace cromwell {

class WebRuntime {
public:
    WebRuntime();
    ~WebRuntime();

    WebRuntime(const WebRuntime&) = delete;
    WebRuntime& operator=(const WebRuntime&) = delete;

    /* Returns false when CEF could not start — a missing libcef.dll, a helper
     * exe that is not beside the app, a second call. The caller should carry on
     * without web surfaces rather than treat it as fatal; everything else in
     * the renderer works without them. reason() then says what went wrong. */
    bool start();

    bool valid() const { return started_; }
    const std::string& reason() const { return reason_; }

    /* One slice of Chromium's message loop. Call once per frame. */
    void tick();

    /* Idempotent, and also run by the destructor. Separated because shutdown
     * has to happen while the GL context is still alive — surfaces hold
     * textures — and the destructor may not run that early. */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool        started_ = false;
    std::string reason_;
};

}  // namespace cromwell
