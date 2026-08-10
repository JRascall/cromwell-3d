/* RaylibLogBridge.cpp — see RaylibLogBridge.hpp. */
#include "app/diag/RaylibLogBridge.hpp"

#include "core/diag/Logger.hpp"

#include "raylib.h"

#include <cstdarg>
#include <cstdio>

namespace xcom {
namespace {

/* RAYLIB'S "INFO" IS NOT OUR INFO, and it is demoted by one step here.
 *
 * What raylib calls info is a narration — every texture id, every framebuffer,
 * every module it loaded — and about three hundred lines of it arrive before
 * the first frame. Left at Info it buries our own records, which is the
 * failure mode a log has no defence against: the reader stops reading.
 *
 * So its narration lands at Debug and is dropped by the default floor. What is
 * NOT demoted is anything raylib itself calls a warning or worse — the shader
 * compile and link errors, the framebuffer failures, the GL refusals. Those
 * were the reason for bridging this channel in the first place, and they stay
 * visible at every level anyone would run at. */
LogLevel levelOf(int raylibLevel)
{
    switch (raylibLevel) {
        case LOG_TRACE:
        case LOG_DEBUG:   return LogLevel::Trace;
        case LOG_INFO:    return LogLevel::Debug;
        case LOG_WARNING: return LogLevel::Warn;
        case LOG_ERROR:   return LogLevel::Error;
        case LOG_FATAL:   return LogLevel::Fatal;
        default:          return LogLevel::Debug;
    }
}

/* raylib hands over the format string and the va_list unformatted, which is
 * exactly what vwritef wants — nothing is formatted twice and nothing is
 * copied on the way through.
 *
 * The prefix names the CHANNEL, not the author, and that is deliberate: this
 * project's own render code calls TraceLog as well, and the callback cannot
 * tell a raylib message from one of ours. Calling it "raylib:" would put
 * raylib's name on our messages. Anything logged through LOGGER carries its
 * file and line instead, which is the reason to prefer it in new code. */
void onTraceLog(int raylibLevel, const char* text, std::va_list args)
{
    const LogLevel level = levelOf(raylibLevel);

    /* Tested here as well as inside the logger, because the prefixing below
     * costs a copy of the format string and the dropped records outnumber the
     * kept ones by two orders of magnitude at the default floor. */
    if (level < logger()->minLevel()) return;

    char prefixed[512];
    std::snprintf(prefixed, sizeof prefixed, "tracelog: %s", text ? text : "");

    logger()->vwritef(level, nullptr, 0, prefixed, args);
}

}  // namespace

void installRaylibLogBridge()
{
    SetTraceLogCallback(onTraceLog);

    /* raylib filters before it ever calls us, so asking for trace on our side
     * has to be passed through or the loudest level would be quieter than the
     * one below it. Anything above trace leaves raylib at its own default,
     * which already withholds the per-call chatter. */
    SetTraceLogLevel(logger()->minLevel() <= LogLevel::Trace ? LOG_ALL : LOG_INFO);

    LOGGER->info("TraceLog bridged into this file "
                 "(narration at debug, warnings and errors at their own level)");
}

}  // namespace xcom
