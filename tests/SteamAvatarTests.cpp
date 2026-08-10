/* SteamAvatarTests.cpp — the avatar fetch, split into the half that can be
 * tested offline and the half that cannot.
 *
 * BY DEFAULT THIS TOUCHES NO NETWORK. ctest runs it with no arguments, which
 * exercises only parseAvatarUrl against captured documents — a test that needed
 * steamcommunity.com to be up would fail for reasons that have nothing to do
 * with this code, and a suite that goes red when the wi-fi drops is a suite
 * people learn to ignore.
 *
 * Run it with `--live <steamid64>` by hand to check the transport end to end:
 * TLS, the redirect the CDN issues, and whether the document still has the
 * shape parseAvatarUrl expects. That last one is the real risk — the ?xml=1
 * endpoint is undocumented, and Valve owes us nothing.
 */
#include "cromwell/net/HttpClient.hpp"
#include "cromwell/steam/SteamAvatar.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* Trimmed from a real response, CDATA and all. */
const char* kProfileXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<profile>\n"
    "  <steamID64>76561197986084937</steamID64>\n"
    "  <avatarIcon><![CDATA[https://avatars.fastly.steamstatic.com/abc123.jpg]]></avatarIcon>\n"
    "  <avatarMedium><![CDATA[https://avatars.fastly.steamstatic.com/abc123_medium.jpg]]></avatarMedium>\n"
    "  <avatarFull><![CDATA[https://avatars.fastly.steamstatic.com/abc123_full.jpg]]></avatarFull>\n"
    "</profile>\n";

void picksTheLargestAvatar()
{
    const std::string url = SteamAvatar::parseAvatarUrl(kProfileXml);
    CHECK(url == "https://avatars.fastly.steamstatic.com/abc123_full.jpg",
          "should pick avatarFull (184x184), got '%s'", url.c_str());
}

void fallsBackWhenFullIsAbsent()
{
    const std::string onlyMedium =
        "<profile><avatarMedium><![CDATA[https://x/y_medium.jpg]]></avatarMedium></profile>";
    CHECK(SteamAvatar::parseAvatarUrl(onlyMedium) == "https://x/y_medium.jpg",
          "should fall back to the medium avatar");
}

void privateProfileYieldsNothing()
{
    /* A private profile answers 200 with a document carrying no avatar — an
     * ordinary outcome, not a transport failure. */
    const std::string privateProfile =
        "<profile><privacyMessage>This profile is private.</privacyMessage></profile>";
    CHECK(SteamAvatar::parseAvatarUrl(privateProfile).empty(),
          "a private profile should yield no url");
}

void malformedDocumentsYieldNothing()
{
    CHECK(SteamAvatar::parseAvatarUrl("").empty(), "empty document");
    CHECK(SteamAvatar::parseAvatarUrl("not xml at all").empty(), "garbage document");

    /* Truncated mid-element: must yield nothing rather than the rest of the
     * file, which is why the closing tag is required. */
    CHECK(SteamAvatar::parseAvatarUrl("<profile><avatarFull><![CDATA[https://x/y.jpg")
              .empty(),
          "a truncated document should yield no url");
}

int runOffline()
{
    picksTheLargestAvatar();
    fallsBackWhenFullIsAbsent();
    privateProfileYieldsNothing();
    malformedDocumentsYieldNothing();

    if (g_failures == 0) std::printf("steam avatar tests passed (offline)\n");
    else                 std::printf("%d steam avatar check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

/* Not run by ctest. Proves the transport against the real service. */
int runLive(const char* steamId)
{
    std::printf("live fetch for %s\n", steamId);

    const std::string profile =
        "https://steamcommunity.com/profiles/" + std::string(steamId) + "?xml=1";

    const HttpResponse xml = httpGet(profile);
    std::printf("  profile: status=%d bytes=%zu %s\n",
                xml.status, xml.body.size(), xml.error.c_str());
    if (!xml.ok) return 1;

    const std::string url =
        SteamAvatar::parseAvatarUrl(std::string(xml.body.begin(), xml.body.end()));
    std::printf("  avatar url: %s\n", url.empty() ? "(none)" : url.c_str());
    if (url.empty()) return 1;

    const HttpResponse image = httpGet(url);
    std::printf("  image: status=%d bytes=%zu %s\n",
                image.status, image.body.size(), image.error.c_str());
    if (!image.ok) return 1;

    /* JPEG starts FF D8 FF. Confirms we fetched an image rather than an error
     * page served with a 200. */
    const bool jpeg = image.body.size() > 3 && image.body[0] == 0xFF &&
                      image.body[1] == 0xD8 && image.body[2] == 0xFF;
    std::printf("  looks like a jpeg: %s\n", jpeg ? "yes" : "NO");
    return jpeg ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--live") == 0) return runLive(argv[2]);
    return runOffline();
}
