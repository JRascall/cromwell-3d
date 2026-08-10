/* SteamAvatar.hpp — fetch a Steam account's avatar over HTTPS.
 *
 * SINGLE RESPONSIBILITY: turn a steamID64 into the bytes of that account's
 * largest avatar image, without blocking the frame.
 *
 * WHY HTTP AND NOT THE SDK. The Steamworks call (GetLargeFriendAvatar) returns
 * the same 184x184 image as raw RGBA and needs no network code at all — but it
 * only answers for accounts Steam has cached, which in practice means the local
 * user and current friends, and it answers asynchronously through a handle that
 * reads -1 while the download is in flight. Going to the community site works
 * for ANY account, cached or not, friend or not, which is what a lobby list
 * needs. The cost is everything below: a TLS client, a JPEG decode and a
 * thread.
 *
 * TWO REQUESTS, NOT ONE. The profile page is asked for as XML
 * (/profiles/<id>?xml=1, no API key needed) purely to learn the avatar's URL,
 * which is content-hashed and therefore unguessable; the second request fetches
 * the image itself. The alternative is the Web API's GetPlayerSummaries, which
 * would mean shipping a publisher key inside the client.
 *
 * POLL, DO NOT WAIT. start() puts both requests on a worker; poll() moves a
 * finished result into place and is cheap enough to call every frame. Nothing
 * here touches a GPU — the caller decodes and uploads, because this half is
 * headless and knows nothing about textures.
 */
#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace cromwell {

class SteamAvatar {
public:
    enum class State {
        Idle,       /* nothing asked for                      */
        Fetching,   /* in flight on the worker                */
        Ready,      /* bytes() holds an image                 */
        Failed,     /* error() says why                       */
    };

    SteamAvatar() = default;
    ~SteamAvatar();

    SteamAvatar(const SteamAvatar&) = delete;
    SteamAvatar& operator=(const SteamAvatar&) = delete;

    /* Begins a fetch. A second call while one is in flight is ignored, so this
     * is safe to call from a frame that does not track whether it already
     * asked. */
    void start(uint64_t steamId64);

    /* Moves a finished worker result into place. Call once a frame; returns
     * true on the frame the state changed, so a caller can upload a texture
     * exactly once rather than testing for it. */
    bool poll();

    State state() const { return state_; }
    bool  isReady() const { return state_ == State::Ready; }

    /* The image FILE bytes — a JPEG, as served. Decoding is the caller's, so
     * that this stays free of an image library and of a GPU. */
    const std::vector<unsigned char>& bytes() const { return bytes_; }

    /* Where it came from, and why it failed. Both for the dev panel. */
    const std::string& url() const { return url_; }
    const std::string& error() const { return error_; }

private:
    struct Result {
        bool ok = false;
        std::string url;
        std::string error;
        std::vector<unsigned char> bytes;
    };

    /* Runs on the worker: profile XML, then the image. */
    static Result fetch(uint64_t steamId64);

    /* Pulls the largest avatar URL out of the profile XML. Public-by-name so
     * it can be tested without a network. */
public:
    static std::string parseAvatarUrl(const std::string& profileXml);

private:
    State state_ = State::Idle;
    std::string url_;
    std::string error_;
    std::vector<unsigned char> bytes_;

    std::future<Result> pending_;
};

}  // namespace cromwell
