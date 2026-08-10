/* The WinHTTP implementation of HttpClient.hpp.
 *
 * WINDOWS ONLY, and the only file in the engine that knows it. Everything
 * platform-specific is behind httpGet's signature, so a POSIX version is a
 * sibling file rather than an edit to any caller.
 *
 * WinHTTP rather than WinINet: WinINet is documented as unsupported from a
 * service and carries the user's IE proxy state and cookie jar, neither of
 * which a game download wants.
 */
#include "cromwell/net/HttpClient.hpp"

#if defined(_WIN32)

/* WIN32_LEAN_AND_MEAN before windows.h: winsock.h and winsock2.h both define
 * the same symbols, and something else in this process includes the latter. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <memory>

namespace cromwell {
namespace {

std::wstring widen(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), size);
    return wide;
}

/* Every WinHTTP handle needs closing on every path out, including the error
 * ones — which is most of them here. */
struct HandleCloser {
    void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

HttpResponse failure(std::string reason)
{
    HttpResponse response;
    response.error = std::move(reason);
    return response;
}

}  // namespace

HttpResponse httpGet(const std::string& url, int timeoutMs)
{
    const std::wstring wideUrl = widen(url);

    /* Split the URL rather than parsing it by hand: WinHttpCrackUrl handles
     * ports, escaping and the query string, all of which a substring search
     * gets wrong on the first unusual input. */
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts))
        return failure("malformed url");

    if (parts.nScheme != INTERNET_SCHEME_HTTPS)
        return failure("only https is allowed");

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Handle session(WinHttpOpen(L"cromwell/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return failure("WinHttpOpen failed");

    WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) return failure("connect failed");

    Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE));
    if (!request) return failure("open request failed");

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return failure("send failed");

    if (!WinHttpReceiveResponse(request.get(), nullptr))
        return failure("no response");

    HttpResponse response;

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    /* Read regardless of status: an error body is often the only explanation of
     * why the status is what it is. */
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            response.error = "read failed";
            return response;
        }
        if (available == 0) break;

        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);

        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
            response.error = "read failed";
            return response;
        }
        response.body.resize(offset + read);
    }

    response.ok = response.status >= 200 && response.status < 300 && !response.body.empty();
    if (!response.ok && response.error.empty())
        response.error = "http status " + std::to_string(response.status);

    return response;
}

}  // namespace cromwell

#else

namespace cromwell {

HttpResponse httpGet(const std::string&, int)
{
    HttpResponse response;
    response.error = "no http implementation on this platform";
    return response;
}

}  // namespace cromwell

#endif
