#include "cromwell/steam/SteamClient.hpp"

#include "cromwell/diag/Logger.hpp"

#include <cstdlib>

/* THE ONLY FILE IN THE PROJECT THAT SEES A STEAM HEADER. When XC_HAVE_STEAM is
 * 0 — no SDK under third_party/steamworks — everything below compiles to a stub
 * that reports why. That is why this file is in the source list
 * unconditionally: no #if leaks into main.cpp or Application, and building
 * without the SDK changes what the app can DO, never what compiles. */
#if XC_HAVE_STEAM
#include "steam/steam_api.h"
#endif

namespace cromwell {

#if XC_HAVE_STEAM
namespace {

/* The overlay listener.
 *
 * A free object rather than a SteamClient member because STEAM_CALLBACK
 * generates a CCallback that registers itself in ITS constructor — so the
 * listener cannot exist before SteamAPI_Init has run, while SteamClient is
 * constructed with Application, long before. Hence a pointer, made in start()
 * and destroyed in stop(). */
class OverlayListener {
public:
    bool active = false;

private:
    STEAM_CALLBACK(OverlayListener, onOverlay, GameOverlayActivated_t);
};

void OverlayListener::onOverlay(GameOverlayActivated_t* event)
{
    active = event->m_bActive != 0;
    LOGGER->info("STEAM: overlay {}", active ? "opened" : "closed");
}

OverlayListener* g_overlay = nullptr;

/* Tells the SDK which app this is, WITHOUT the steam_appid.txt file Valve's
 * samples use. That file has to sit beside the exe, and shipping it by accident
 * makes a retail build read its own app id off the disk, where anyone can edit
 * it. An environment variable dies with the process and cannot be shipped at
 * all. Both are read by SteamAPI_Init, so this has to come first.
 *
 * Same approach as the Unreal project next door, which sets SteamAppId and
 * SteamGameId this way in its own subsystem. */
void announceAppId()
{
    const char* appId = XC_STEAM_APPID_STRING;
#ifdef _WIN32
    _putenv_s("SteamAppId",  appId);
    _putenv_s("SteamGameId", appId);
#else
    setenv("SteamAppId",  appId, 1);
    setenv("SteamGameId", appId, 1);
#endif
}

}  // namespace
#endif  // XC_HAVE_STEAM

unsigned int steamAppId()
{
#if XC_HAVE_STEAM
    return XC_STEAM_APPID;
#else
    return 0;
#endif
}

bool steamRestartIfNecessary()
{
#if XC_HAVE_STEAM
    /* Skipped for Spacewar — see the header. Every account owns app 480, so
     * there is nothing to own-check, and the relaunch would do nothing but
     * bounce every debug run through the Steam client. */
    if (XC_STEAM_APPID == 480) return false;

    announceAppId();
    if (!SteamAPI_RestartAppIfNecessary(XC_STEAM_APPID)) return false;

    LOGGER->info("STEAM: relaunching through the client for app {}",
                 static_cast<unsigned>(XC_STEAM_APPID));
    return true;
#else
    return false;
#endif
}

bool SteamClient::start()
{
#if !XC_HAVE_STEAM
    reason_ = "built without the Steamworks SDK "
              "(see third_party/steamworks/README.md)";
    LOGGER->info("STEAM: disabled - {}", reason_);
    return false;
#else
    if (running_) return true;

    announceAppId();

    /* InitEx rather than Init: the plain form returns a bare bool, and "Steam
     * is not running" and "the client is older than this SDK" are worth telling
     * apart when somebody asks why their name is missing from the title bar. */
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK) {
        reason_ = error[0] ? error : "SteamAPI_InitEx failed";
        LOGGER->warn("STEAM: disabled - {}", reason_);
        return false;
    }

    running_ = true;
    reason_.clear();

    if (ISteamFriends* friends = SteamFriends())
        personaName_ = friends->GetPersonaName();
    if (ISteamUser* user = SteamUser())
        steamId_ = user->GetSteamID().ConvertToUint64();

    g_overlay = new OverlayListener();

    LOGGER->info("STEAM: app {}, signed in as {} ({})",
                 static_cast<unsigned>(XC_STEAM_APPID), personaName_, steamId_);
    return true;
#endif
}

void SteamClient::tick()
{
#if XC_HAVE_STEAM
    if (running_) SteamAPI_RunCallbacks();
#endif
}

void SteamClient::stop()
{
#if XC_HAVE_STEAM
    if (!running_) return;

    /* The listener first: ~CCallback unregisters with the dispatcher, and doing
     * that after SteamAPI_Shutdown has torn the dispatcher down is a
     * use-after-free rather than a no-op. */
    delete g_overlay;
    g_overlay = nullptr;

    SteamAPI_Shutdown();
    LOGGER->info("STEAM: shut down");
#endif
    running_ = false;
    reason_  = "stopped";
    personaName_.clear();
    steamId_ = 0;
}

bool SteamClient::overlayActive() const
{
#if XC_HAVE_STEAM
    return g_overlay && g_overlay->active;
#else
    return false;
#endif
}

SteamClient::~SteamClient()
{
    stop();
}

}  // namespace cromwell
