/* HttpClient.hpp — fetch a URL over HTTPS.
 *
 * SINGLE RESPONSIBILITY: perform one blocking GET and hand back the bytes.
 *
 * BLOCKING, AND THAT IS THE POINT. A network round trip is tens to hundreds of
 * milliseconds — ten frames or more — so this must never be called from the
 * frame. Callers put it on a worker and poll the result; see SteamAvatar.hpp
 * for the pattern. Making it blocking keeps the transport simple and puts the
 * threading decision where the caller can see it, rather than hiding a thread
 * inside something that looks like a function call.
 *
 * NO DEPENDENCY. The implementation is WinHTTP, which ships with Windows —
 * chosen over libcurl because the alternative to "no new dependency" here is
 * vendoring a TLS stack to download a 184-pixel picture. The interface names no
 * platform type, so a POSIX implementation drops in beside WinHttpClient.cpp
 * without touching a caller.
 *
 * HTTPS ONLY. Everything this fetches is a public CDN over TLS, and an http://
 * URL would be a downgrade nobody asked for.
 *
 * PUBLIC FIELDS, deliberately. This is a one-shot data carrier: no invariant
 * spans its fields, so no setter could validate anything; it is filled by one
 * caller, read by one callee, and does not outlive the call that made it. The
 * project's rule is private members behind accessors — see any component — and
 * this is the documented exception to it, not an oversight.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cromwell {

struct HttpResponse {
    /* True only for a 2xx with a body. A 404 is `ok == false` with status 404
     * — an outcome, not an exception, because a missing avatar is ordinary. */
    bool ok = false;
    int  status = 0;

    /* Empty when ok. Names the transport failure — DNS, TLS, timeout — for a
     * log, since those are indistinguishable from the status code alone. */
    std::string error;

    std::vector<unsigned char> body;
};

/* Follows redirects, which the Steam CDN uses. `timeoutMs` covers each phase
 * (resolve, connect, send, receive) rather than the whole request. */
HttpResponse httpGet(const std::string& url, int timeoutMs = 10000);

}  // namespace cromwell
