/* CrashHandler.hpp — the last thing that runs.
 *
 * SINGLE RESPONSIBILITY: when the process is going down uninvited, write WHY
 * into the log before it goes, and leave a minidump beside it.
 *
 * A log alone answers "how far did it get". This answers "what killed it":
 * the exception code, the faulting address, and a symbolised call stack —
 * function, file and line, as long as the .pdb is next to the .exe, which the
 * Release build now emits (see the MSVC block in CMakeLists.txt).
 *
 * Install it as early as main() can — BEFORE CEF starts, since Chromium
 * installs handlers of its own and the last one in wins for the processes it
 * owns. Windows only; on other platforms installCrashHandler() does nothing,
 * and the logger alone still works.
 */
#pragma once

namespace cromwell {

/* Routes unhandled SEH exceptions, uncaught C++ exceptions and abort() into
 * the logger. Safe to call more than once; only the first call installs. */
void installCrashHandler();

}  // namespace cromwell
