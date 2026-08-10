/* main.cpp — xcom-c: an XCOM 2-style tactical tile prototype on raylib.
 *
 * SINGLE RESPONSIBILITY: parse the command line, arm the diagnostics and hand
 * control to Application. Everything else lives behind one of three layers:
 *
 *   core/    the simulation. Pure C++, no raylib, headless-testable — see
 *            tests/TileCoreTests.cpp.
 *   render/  the only place that touches the GPU.
 *   app/     input, state and the frame loop that joins the two.
 *
 * The diagnostics come up FIRST and in this order — log, crash handler,
 * raylib bridge — because each one is only useful if the one before it is
 * already running, and because everything worth catching happens after.
 */
#include "app/Application.hpp"
#include "app/cli/CliOptions.hpp"
#include "app/diag/CrashHandler.hpp"
#include "app/diag/RaylibLogBridge.hpp"
#include "core/diag/Logger.hpp"
#include "core/light/SunBakeBenchmark.hpp"

#include <string>

int main(int argc, char** argv)
{
    const xcom::CliOptions options = xcom::CliOptions::parse(argc, argv);

    /* Beside the executable, not in the working directory: this is launched
     * from the project root, from builds/win and from Explorer, and a log you
     * have to go looking for is a log nobody reads. */
    if (options.logPath.empty()) xcom::logger()->openBeside(argv[0]);
    else                         xcom::logger()->open(options.logPath);
    xcom::logger()->setMinLevel(xcom::parseLogLevel(options.logLevel));

    xcom::installCrashHandler();
    xcom::installRaylibLogBridge();

    /* The exact invocation, on one line. Half of reproducing a crash is
     * knowing which flags produced it. */
    std::string commandLine;
    for (int i = 0; i < argc; i++) {
        commandLine += argv[i];
        commandLine += ' ';
    }
    LOGGER->info("command line: " + commandLine);

    /* Headless, and before anything opens a window — the bake is core work and
     * has no business needing a GPU to be measured. */
    if (options.bakeBenchmark) {
        const int status = xcom::runSunBakeBenchmark();
        xcom::logger()->close();
        return status;
    }

    xcom::Application application(options);
    const int status = application.run();

    /* The footer this writes is the point: a log that ends without one ended
     * because the process did, and that is the first thing to look for. */
    LOGGER->info("exiting with status %d", status);
    xcom::logger()->close();
    return status;
}
