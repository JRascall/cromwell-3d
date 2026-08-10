/* CrashHandler.cpp — see CrashHandler.hpp.
 *
 * NOTHING IN HERE MAY INCLUDE raylib. The Windows headers below declare
 * CloseWindow and ShowCursor with C linkage and different signatures than
 * raylib's, so a translation unit that sees both does not compile — the same
 * split WebBrowserHost.hpp documents.
 */
#include "cromwell/diag/CrashHandler.hpp"

#include "cromwell/diag/Logger.hpp"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

namespace cromwell {
namespace {

bool installed = false;

/* Set the moment a handler starts. A fault raised while reporting a fault —
 * and the symbol machinery below is not itself crash-proof — must not recurse
 * into the same reporting path. */
volatile LONG reporting = 0;

const char* exceptionName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
        default:                              return "UNKNOWN";
    }
}

void report(const char* format, ...)
{
    char line[2048];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);

    logger()->writeCrash(line);
}

/* The log path with its extension swapped for .dmp, so the two halves of a
 * crash report sit next to each other under one name. */
std::string dumpPath()
{
    std::string path = logger()->path();
    if (path.empty()) return "crash.dmp";

    const std::size_t dot = path.find_last_of('.');
    const std::size_t sep = path.find_last_of("/\\");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        path.erase(dot);
    return path + ".dmp";
}

/* Which module owns an address, and how far into it — the one form of an
 * address that still means something in a log read on another machine, or
 * after the module has been rebased. */
void describeAddress(char* out, std::size_t size, DWORD64 address)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module) {
        char file[MAX_PATH] = {};
        GetModuleFileNameA(module, file, MAX_PATH);

        const char* leaf = file;
        for (const char* at = file; *at; ++at)
            if (*at == '\\' || *at == '/') leaf = at + 1;

        const DWORD64 base = reinterpret_cast<DWORD64>(module);
        std::snprintf(out, size, "%s+0x%llx", leaf,
                      static_cast<unsigned long long>(address - base));
    } else {
        std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
    }
}

/* Walks the stack of the thread that FAULTED, which is why it takes the
 * exception's own CONTEXT rather than capturing here: by the time this runs,
 * the handler's frames have already replaced the interesting ones. StackWalk64
 * writes through the context, so it gets a copy. */
void writeStackTrace(const CONTEXT& faultContext)
{
#ifdef _M_X64
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread  = GetCurrentThread();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE)) {
        report("  (no symbols: SymInitialize failed, %lu)", GetLastError());
    }

    CONTEXT context = faultContext;

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset    = context.Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    /* Bounded: a stack overflow crash has a stack thousands of frames deep and
     * the top thirty of them say everything the bottom ones would. */
    for (int depth = 0; depth < 40; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;

        char where[MAX_PATH + 64];
        describeAddress(where, sizeof where, frame.AddrPC.Offset);

        /* SYMBOL_INFO is a header with the name written off its end, so the
         * buffer has to be oversized by hand and MaxNameLen set to match. */
        char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        SYMBOL_INFO* symbol   = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct  = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen    = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct    = sizeof(IMAGEHLP_LINE64);
            DWORD column         = 0;

            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &column, &line))
                report("  #%02d %s+0x%llx  (%s:%lu)  [%s]", depth, symbol->Name,
                       static_cast<unsigned long long>(displacement),
                       line.FileName, line.LineNumber, where);
            else
                report("  #%02d %s+0x%llx  [%s]", depth, symbol->Name,
                       static_cast<unsigned long long>(displacement), where);
        } else {
            report("  #%02d %s", depth, where);
        }
    }

    SymCleanup(process);
#else
    (void)faultContext;
    report("  (no stack trace: 64-bit build only)");
#endif
}

void writeMinidump(EXCEPTION_POINTERS* exception)
{
    const std::string path = dumpPath();

    const HANDLE dump = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) {
        report("could not open %s for the minidump (%lu)", path.c_str(), GetLastError());
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION information = {};
    information.ThreadId          = GetCurrentThreadId();
    information.ExceptionPointers = exception;
    information.ClientPointers    = FALSE;

    /* WithIndirectlyReferencedMemory costs a few MB over a plain dump and buys
     * the values of the locals a debugger will be asked about first. */
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
        MiniDumpWithThreadInfo);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                      type, exception ? &information : nullptr,
                                      nullptr, nullptr);
    CloseHandle(dump);

    report(ok ? "minidump written to %s" : "minidump FAILED for %s", path.c_str());
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exception)
{
    if (InterlockedExchange(&reporting, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;

    const EXCEPTION_RECORD* record = exception ? exception->ExceptionRecord : nullptr;

    report("================ CRASH ================");
    if (record) {
        char where[MAX_PATH + 64];
        describeAddress(where, sizeof where, reinterpret_cast<DWORD64>(record->ExceptionAddress));

        report("%s (0x%08lx) at %s", exceptionName(record->ExceptionCode),
               record->ExceptionCode, where);

        /* For an access violation the second parameter is the address that was
         * touched, and the first says how. A near-null one is a null
         * dereference; a huge one is usually a dangling pointer. */
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2) {
            const ULONG_PTR kind = record->ExceptionInformation[0];
            report("  %s address 0x%llx",
                   kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                   static_cast<unsigned long long>(record->ExceptionInformation[1]));
        }
    } else {
        report("no exception record");
    }

    if (exception && exception->ContextRecord) writeStackTrace(*exception->ContextRecord);
    writeMinidump(exception);
    report("=======================================");

    /* EXECUTE_HANDLER, not CONTINUE_SEARCH: the report is written, and letting
     * it fall through would only hand the same crash to the Windows error
     * dialog on top of it. */
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate()
{
    if (InterlockedExchange(&reporting, 1) == 0) {
        report("================ TERMINATE ================");

        /* Rethrowing inside the terminate handler is the only way to see what
         * the uncaught exception actually was. */
        if (std::exception_ptr active = std::current_exception()) {
            try {
                std::rethrow_exception(active);
            } catch (const std::exception& error) {
                report("uncaught exception: %s", error.what());
            } catch (...) {
                report("uncaught exception of a non-std type");
            }
        } else {
            report("std::terminate with no active exception");
        }
        report("===========================================");
    }

    /* _Exit, not abort: abort would raise SIGABRT into the handler below and
     * report the same death a second time. */
    std::_Exit(3);
}

void onAbort(int)
{
    if (InterlockedExchange(&reporting, 1) == 0)
        /* ASCII only, like every other record: see the header line in
         * Logger::open. */
        report("abort() - an assertion or a CRT check failed");
    std::_Exit(3);
}

}  // namespace

void installCrashHandler()
{
    if (installed) return;
    installed = true;

    SetUnhandledExceptionFilter(onUnhandledException);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);

    /* Otherwise the CRT pops its own "abort() has been called" dialog and the
     * process hangs on a message box nobody is there to click. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    LOGGER->info("crash handler installed");
}

}  // namespace cromwell

#else  /* not Windows */

namespace cromwell {

void installCrashHandler()
{
    LOGGER->info("crash handler not available on this platform");
}

}  // namespace cromwell

#endif
