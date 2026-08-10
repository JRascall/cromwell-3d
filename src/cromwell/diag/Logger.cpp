/* Logger.cpp — see Logger.hpp. */
#include "cromwell/diag/Logger.hpp"

#include <chrono>
#include <cstring>
#include <ctime>

namespace cromwell {
namespace {

constexpr int kMessageLimit = 4096;

/* The clock the elapsed column counts from. Steady, not wall clock: the
 * question a log answers is "how long after startup", and a wall clock can
 * step sideways mid-run. */
std::chrono::steady_clock::time_point& startTime()
{
    static std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    return start;
}

const char* levelName(LogLevel level)
{
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "?????";
    }
}

/* __FILE__ is whatever path the compiler was handed — an absolute one under
 * MSVC — and a full path per line would bury the message. Only the leaf is
 * ambiguous in theory; in this tree no two source files share a name. */
const char* leafOf(const char* path)
{
    if (!path) return "";
    const char* leaf = path;
    for (const char* at = path; *at; ++at)
        if (*at == '/' || *at == '\\') leaf = at + 1;
    return leaf;
}

/* HH:MM:SS.mmm of the local wall clock. Cheap enough per record — this is one
 * localtime call against a value the C library caches the zone for. */
void formatClock(char* out, std::size_t size)
{
    using namespace std::chrono;

    const auto now   = system_clock::now();
    const auto stamp = system_clock::to_time_t(now);
    const auto milli = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &stamp);
#else
    localtime_r(&stamp, &local);
#endif

    std::snprintf(out, size, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min,
                  local.tm_sec, static_cast<int>(milli));
}

double elapsedSeconds()
{
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now() - startTime()).count();
}

/* Everything up to the last separator, "" if there is none. */
std::string directoryOf(const std::string& path)
{
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut + 1);
}

}  // namespace

Logger::~Logger() { close(); }

bool Logger::open(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }

    /* Rotate. The run worth reading is usually the one that just died, and by
     * the time anyone opens the file they have already relaunched over it. One
     * generation only — a crash you have reproduced twice does not need the
     * third-oldest log. */
    const std::size_t dot  = path.find_last_of('.');
    const std::size_t sep  = path.find_last_of("/\\");
    const bool hasStem     = dot != std::string::npos && (sep == std::string::npos || dot > sep);
    const std::string prev = (hasStem ? path.substr(0, dot) : path) + ".prev.log";

    std::remove(prev.c_str());
    std::rename(path.c_str(), prev.c_str());   /* fails harmlessly on first run */

    file_ = std::fopen(path.c_str(), "w");
    path_ = path;
    if (!file_) return false;

    startTime() = std::chrono::steady_clock::now();

    char clock[32];
    formatClock(clock, sizeof clock);

    const auto stamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &stamp);
#else
    localtime_r(&stamp, &local);
#endif

    /* ASCII only in the records themselves. The source is UTF-8 and Notepad
     * still opens a .log as ANSI, so an em dash here reaches the reader as
     * mojibake on the very first line. */
    std::fprintf(file_, "=== cromwell log %04d-%02d-%02d %s ===\n",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, clock);
    std::fflush(file_);
    return true;
}

bool Logger::openBeside(const char* argv0, const std::string& filename)
{
    const std::string directory = argv0 ? directoryOf(argv0) : std::string();
    return open(directory + filename);
}

void Logger::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;
    std::fprintf(file_, "=== closed after %.3f s ===\n", elapsedSeconds());
    std::fclose(file_);
    file_ = nullptr;
}

bool Logger::isOpen() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

void Logger::write(LogLevel level, const char* file, int line, const char* text)
{
    if (level < minLevel_) return;
    emit(level, file, line, text ? text : "(null)");
}

void Logger::vwritef(LogLevel level, const char* file, int line, const char* format,
                     std::va_list args)
{
    if (level < minLevel_) return;

    char message[kMessageLimit];
    /* vsnprintf always terminates and never writes past the buffer; a message
     * longer than the limit is truncated, which is the right failure for a
     * diagnostic. */
    const int written = std::vsnprintf(message, sizeof message, format ? format : "", args);
    if (written < 0) std::snprintf(message, sizeof message, "(bad log format)");

    emit(level, file, line, message);
}

void Logger::emit(LogLevel level, const char* file, int line, const char* text)
{
    char clock[32];
    formatClock(clock, sizeof clock);
    const double elapsed = elapsedSeconds();

    /* The lock covers the whole record, not each fputs: two threads logging at
     * once should produce two lines, not one line of both. */
    std::lock_guard<std::mutex> lock(mutex_);

    const auto record = [&](std::FILE* stream) {
        if (file && *file)
            std::fprintf(stream, "%s %8.3f %s %s:%d  %s\n", clock, elapsed,
                         levelName(level), leafOf(file), line, text);
        else
            std::fprintf(stream, "%s %8.3f %s  %s\n", clock, elapsed,
                         levelName(level), text);
    };

    if (file_) {
        record(file_);
        /* RULE 1. Without this the last few seconds before a crash — exactly
         * the interesting part — die in the stdio buffer with the process. */
        std::fflush(file_);
    }
    if (mirror_) {
        record(stderr);
        std::fflush(stderr);
    }
}

void Logger::writeCrash(const char* text)
{
    /* No lock: see the header. The process is already going down. */
    char clock[32];
    formatClock(clock, sizeof clock);

    if (file_) {
        std::fprintf(file_, "%s %8.3f FATAL  %s\n", clock, elapsedSeconds(),
                     text ? text : "(null)");
        std::fflush(file_);
    }
    std::fprintf(stderr, "%s FATAL  %s\n", clock, text ? text : "(null)");
    std::fflush(stderr);
}

void Logger::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) std::fflush(file_);
}

LogLevel parseLogLevel(const std::string& name)
{
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "warn")  return LogLevel::Warn;
    if (name == "error") return LogLevel::Error;
    if (name == "fatal") return LogLevel::Fatal;
    if (name == "off")   return LogLevel::Off;
    return LogLevel::Info;
}

Logger* logger()
{
    /* Function-local static: constructed on first use, so a log call from the
     * constructor of another static cannot reach a Logger that has not been
     * built yet. It is deliberately never destroyed — the destructor would run
     * during static teardown, and something logging on the way out would then
     * write through a closed file. Leaking one FILE* at exit is free; the OS
     * closes it. */
    static Logger* instance = new Logger();
    return instance;
}

}  // namespace cromwell
