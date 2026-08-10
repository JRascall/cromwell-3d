/* Logger.hpp — the process-wide log file.
 *
 * SINGLE RESPONSIBILITY: turn a call site and a message into one timestamped
 * line in a text file that is already on disk before the next line runs.
 *
 * Use it through the macro, which is the whole point of the design:
 *
 *     LOGGER->info("map loaded");
 *     LOGGER->warn("probe %d has no room", index);
 *     LOGGER->error("shader %s failed to compile", name.c_str());
 *
 * LOGGER expands to a temporary carrying __FILE__ and __LINE__, so every
 * record names the line that wrote it without the caller passing anything.
 * That is what makes the file readable after a crash: the last line in it is a
 * source location, not just a sentence.
 *
 * THREE RULES, all of them about surviving a crash:
 *
 *   1. EVERY RECORD IS FLUSHED. A crash kills the process, not the file cache,
 *      so a record that has reached the OS survives and a record sitting in
 *      this process's stdio buffer does not. The cost is a write syscall per
 *      line; logging happens at interaction rate, not per triangle.
 *   2. THE PREVIOUS RUN IS KEPT. open() renames an existing log to
 *      "<stem>.prev.log" before truncating, because the run you want to read is
 *      almost always the one that just died — and by then you have relaunched.
 *   3. NEVER THROWS, NEVER ASSERTS. A logger that can take the process down is
 *      worse than no logger. An unopened Logger silently drops records.
 *
 * Formatting is printf-style, and a call with NO variadic arguments is written
 * VERBATIM — LOGGER->info("50% done") logs "50% done" rather than reading %d
 * off the stack. Passing a std::string to a format call is a compile error;
 * pass .c_str(), or use the single-argument overload which takes std::string
 * directly.
 *
 * Lives in core/ so the headless tests and the pure-C++ simulation can log
 * without linking raylib. Application installs the raylib TraceLog bridge and
 * the crash handler on top of it; see app/diag/.
 */
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>

namespace xcom {

/* Ordered by severity — setMinLevel drops everything below it, and Off drops
 * everything full stop. */
enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

class Logger {
public:
    ~Logger();

    /* Opens `path`, truncating it and rotating any existing file to
     * "<stem>.prev.log". Returns false if the file could not be opened, in
     * which case every later record is dropped rather than crashing. */
    bool open(const std::string& path);

    /* open() against the directory of the running executable rather than the
     * working directory. `argv0` is the only portable handle on that
     * directory, and the working directory here varies — the app is launched
     * both from the project root and from builds/win. */
    bool openBeside(const char* argv0, const std::string& filename = "xcom.log");

    void close();
    bool isOpen() const;

    /* Where the records are going. Set once by open() and not written again,
     * so it is safe to read from any thread. */
    const std::string& path() const { return path_; }

    /* Atomic rather than mutex-guarded, so the level test in front of every
     * call site costs a load and not a lock — and so raising the level from
     * one thread while another logs is defined behaviour rather than a race. */
    void setMinLevel(LogLevel level) { minLevel_ = level; }
    LogLevel minLevel() const { return minLevel_; }

    /* Also copy records to stderr. On a Release build the app has no console
     * so this goes nowhere; on a Debug build it is the running commentary. */
    void setMirrorToStderr(bool on) { mirror_ = on; }

    /* The three entry points. `file` and `line` may be nullptr/0 for a record
     * with no call site — that is how the raylib bridge logs. */
    void write(LogLevel level, const char* file, int line, const char* text);
    void writef(LogLevel level, const char* file, int line, const char* format, ...);
    void vwritef(LogLevel level, const char* file, int line, const char* format,
                 std::va_list args);

    /* LOCK-FREE, for the crash handler only. The faulting thread may already
     * hold the mutex — it may have faulted inside a log call — and a handler
     * that deadlocks writes nothing at all. Interleaved bytes are a better
     * outcome than a hung process, and by this point the process is over. */
    void writeCrash(const char* text);

    void flush();

private:
    void emit(LogLevel level, const char* file, int line, const char* text);

    mutable std::mutex mutex_;
    std::FILE*  file_ = nullptr;
    std::string path_;

    std::atomic<LogLevel> minLevel_{ LogLevel::Trace };
    std::atomic<bool>     mirror_{ true };
};

/* The process-wide logger. A pointer, not a reference, so the LOGGER macro
 * reads the way logging macros are expected to read. */
Logger* logger();

/* "trace".."fatal", or "off". Anything else is Info — a typo in a command line
 * flag should not silence the log. */
LogLevel parseLogLevel(const std::string& name);

/* One call site. Constructed fresh at every LOGGER, carries the location the
 * macro captured, and forwards to logger(). operator-> returns itself, which
 * is what lets `LOGGER->info(...)` work on a temporary: the temporary lives
 * until the end of the full expression, comfortably past the call. */
class LogSite {
public:
    constexpr LogSite(const char* file, int line) : file_(file), line_(line) {}

    const LogSite* operator->() const { return this; }

/* Two overloads per level: a format one that only formats when there is
 * something to format, and a std::string one so the common case needs no
 * .c_str() and cannot misread a stray % as a conversion. */
#define XC_DECLARE_LOG_LEVEL(name, level)                                          \
    template <class... Args>                                                       \
    void name(const char* format, Args... args) const                              \
    {                                                                              \
        static_assert((std::is_trivially_copyable_v<Args> && ...),                 \
                      "log arguments go through varargs: pass str.c_str(), "       \
                      "not a std::string");                                        \
        if constexpr (sizeof...(Args) == 0)                                        \
            logger()->write(level, file_, line_, format);                          \
        else                                                                       \
            logger()->writef(level, file_, line_, format, args...);                \
    }                                                                              \
    void name(const std::string& text) const                                       \
    {                                                                              \
        logger()->write(level, file_, line_, text.c_str());                        \
    }

    XC_DECLARE_LOG_LEVEL(trace, LogLevel::Trace)
    XC_DECLARE_LOG_LEVEL(debug, LogLevel::Debug)
    XC_DECLARE_LOG_LEVEL(info,  LogLevel::Info)
    XC_DECLARE_LOG_LEVEL(warn,  LogLevel::Warn)
    XC_DECLARE_LOG_LEVEL(error, LogLevel::Error)
    XC_DECLARE_LOG_LEVEL(fatal, LogLevel::Fatal)

#undef XC_DECLARE_LOG_LEVEL

    /* For the rare caller that wants the file on disk right now and cannot
     * wait for the next record — there is no such caller today, since every
     * record flushes, but a batched mode would need it. */
    void flush() const { logger()->flush(); }

private:
    const char* file_;
    int         line_;
};

}  // namespace xcom

/* Deliberately unqualified and deliberately shouty: this is the one name in
 * the project that every translation unit is expected to reach for. */
#define LOGGER (::xcom::LogSite{ __FILE__, __LINE__ })
