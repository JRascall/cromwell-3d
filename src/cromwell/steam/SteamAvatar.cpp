#include "cromwell/steam/SteamAvatar.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/net/HttpClient.hpp"

#include <chrono>

namespace cromwell {
namespace {

/* The keyless profile endpoint. /profiles/<id> rather than /id/<vanity>
 * because a vanity URL is something the account owner can change and may not
 * have set at all, whereas the 64-bit id is what SteamClient already knows. */
std::string profileUrl(uint64_t steamId64)
{
    return "https://steamcommunity.com/profiles/" + std::to_string(steamId64) + "?xml=1";
}

/* Returns the text between the first <tag> and its closing </tag>, or empty.
 *
 * A hand-rolled extraction rather than an XML parser, deliberately: the whole
 * requirement is one element from a document Valve controls, and pulling in a
 * parser to read a single tag would be the larger liability. It is strict
 * about the closing tag so a truncated response yields nothing rather than the
 * rest of the file. */
std::string between(const std::string& document, const std::string& tag)
{
    const std::string open  = "<" + tag + ">";
    const std::string close = "</" + tag + ">";

    const std::size_t from = document.find(open);
    if (from == std::string::npos) return {};

    const std::size_t start = from + open.size();
    const std::size_t end = document.find(close, start);
    if (end == std::string::npos) return {};

    return document.substr(start, end - start);
}

/* Valve wraps the URLs in CDATA. */
std::string stripCdata(std::string text)
{
    const std::string open = "<![CDATA[";
    const std::string close = "]]>";

    const std::size_t from = text.find(open);
    if (from == std::string::npos) return text;

    const std::size_t start = from + open.size();
    const std::size_t end = text.find(close, start);
    if (end == std::string::npos) return text;

    return text.substr(start, end - start);
}

}  // namespace

SteamAvatar::~SteamAvatar()
{
    /* The worker holds no reference to this object, but its future does own the
     * thread — waiting here rather than detaching means a destructor during
     * shutdown cannot outlive the process's networking stack. */
    if (pending_.valid()) pending_.wait();
}

std::string SteamAvatar::parseAvatarUrl(const std::string& profileXml)
{
    /* Largest first. avatarFull is 184x184, which is the biggest Valve serves
     * through any route — the SDK's "large" avatar is the same image. */
    for (const char* tag : { "avatarFull", "avatarMedium", "avatarIcon" }) {
        const std::string found = stripCdata(between(profileXml, tag));
        if (!found.empty()) return found;
    }
    return {};
}

SteamAvatar::Result SteamAvatar::fetch(uint64_t steamId64)
{
    Result result;

    const HttpResponse profile = httpGet(profileUrl(steamId64));
    if (!profile.ok) {
        result.error = "profile fetch failed: " + profile.error;
        return result;
    }

    const std::string xml(profile.body.begin(), profile.body.end());
    result.url = parseAvatarUrl(xml);
    if (result.url.empty()) {
        /* A private profile answers 200 with a document that has no avatar in
         * it, so this is an ordinary outcome rather than a transport error. */
        result.error = "no avatar in profile (private, or no such account)";
        return result;
    }

    const HttpResponse image = httpGet(result.url);
    if (!image.ok) {
        result.error = "image fetch failed: " + image.error;
        return result;
    }

    result.bytes = image.body;
    result.ok = true;
    return result;
}

void SteamAvatar::start(uint64_t steamId64)
{
    if (state_ == State::Fetching) return;
    if (steamId64 == 0) {
        state_ = State::Failed;
        error_ = "no steam id";
        return;
    }

    state_ = State::Fetching;
    error_.clear();
    url_.clear();
    bytes_.clear();

    pending_ = std::async(std::launch::async, &SteamAvatar::fetch, steamId64);
}

bool SteamAvatar::poll()
{
    if (state_ != State::Fetching || !pending_.valid()) return false;

    /* Zero wait: this is called from the frame, so it asks whether the worker
     * has finished and never blocks on the answer. */
    if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;

    const Result result = pending_.get();

    url_ = result.url;
    if (result.ok) {
        bytes_ = result.bytes;
        state_ = State::Ready;
        LOGGER->info("STEAM: avatar fetched, {} bytes from {}", bytes_.size(), url_);
    } else {
        error_ = result.error;
        state_ = State::Failed;
        LOGGER->warn("STEAM: avatar unavailable - {}", error_);
    }

    return true;
}

}  // namespace cromwell
