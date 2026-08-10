/* SteamClient.hpp — the Steam connection.
 *
 * SINGLE RESPONSIBILITY: own the Steamworks session — bring it up, pump its
 * callbacks once a frame, take it down — and report who is logged in. It
 * decides nothing about any game; the app reads personaName() and does what it
 * likes with it.
 *
 * ENGINE, NOT GAME. Identity, the overlay and the friends list are true of any
 * product shipped on Steam, and nothing here names a tile, a soldier or an app
 * id — the id arrives as XC_STEAM_APPID from the build. It is in cromwell
 * rather than cromwell_base for a link-time reason and not a conceptual one:
 * steam_api64.lib is a load-time import, so a target that links it needs
 * steam_api64.dll beside it, and putting that requirement in _base would drag
 * it into the headless test binaries — which run out of the build tree, where
 * no DLL is staged. See the app target in CMakeLists.txt.
 *
 * NO STEAM HEADER IS INCLUDED HERE, and none may be. Everything the SDK drags
 * in stays inside SteamClient.cpp, which is what lets the rest of the project
 * be compiled by someone who has never unzipped the SDK. Same rule the web
 * module follows for CEF, for the same reason.
 *
 * DEGRADES, NEVER FAILS. Steam not installed, Steam not running, SDK not
 * present at build time — all three cost the Steam features and nothing else,
 * and reason() says which one happened. A prototype must still run on a machine
 * that has never heard of Steam.
 */
#pragma once

#include <cstdint>
#include <string>

namespace cromwell {

/* The app id this build is stamped with — XC_STEAM_APPID from the build, 480
 * (Spacewar) unless someone changed it. Zero when built without the SDK. */
unsigned int steamAppId();

/* True when the process was launched outside Steam and Steam has been asked to
 * relaunch it properly, in which case the caller must exit IMMEDIATELY and draw
 * nothing.
 *
 * Must be the first thing main does. It is also a no-op today: the check is
 * skipped for Spacewar — every account owns app 480, so there is nothing for
 * Steam to install or own-check, and letting it fire would bounce every
 * double-clicked debug run through the client. It goes live the moment
 * XC_STEAM_APPID stops being 480, which is exactly when it should. */
bool steamRestartIfNecessary();

class SteamClient {
public:
    SteamClient() = default;
    ~SteamClient();

    /* Holds a process-wide connection, so there is exactly one of these. */
    SteamClient(const SteamClient&) = delete;
    SteamClient& operator=(const SteamClient&) = delete;

    /* Connects to the running Steam client. False is an ordinary outcome, not
     * an error — see reason(). Call BEFORE the window exists: the overlay hooks
     * itself into the graphics context on the first frame presented, and a
     * context created before Steam was initialised never gets hooked. */
    bool start();

    /* Steam's slice of the frame: dispatches the callbacks queued since the
     * last call. Cheap, and skipped entirely when not running. Miss it and the
     * overlay stops responding while nothing else obviously breaks, which is
     * what makes it easy to lose. */
    void tick();

    /* Idempotent, and called by the destructor. Explicit so the shutdown can be
     * ORDERED against the window rather than left to member teardown. */
    void stop();

    bool running() const { return running_; }

    /* Why Steam is not running, when it is not. Empty while connected. */
    const std::string& reason() const { return reason_; }

    /* The logged-in account. Empty unless running. */
    const std::string& personaName() const { return personaName_; }
    uint64_t steamId() const { return steamId_; }

    /* True while the Steam overlay is up. The game keeps rendering underneath
     * it, but the overlay has the keyboard and the mouse — anything that acts
     * on input, or a single-player clock, should stop while this is set.
     * Nothing reads it yet; it exists because the callback has to be registered
     * anyway and the flag is the cheap half. */
    bool overlayActive() const;

private:
    bool        running_ = false;
    std::string reason_  = "not started";
    std::string personaName_;
    uint64_t    steamId_ = 0;
};

}  // namespace cromwell
