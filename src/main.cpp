/* main.cpp — xcom-c: an XCOM 2-style tactical tile prototype on cromwell.
 *
 * SINGLE RESPONSIBILITY: parse the command line, arm the diagnostics and hand
 * control to Application. It is the one file that sees both halves of the
 * project, which is why the two namespaces are spelled out here rather than
 * being blurred by a using-directive:
 *
 *   cromwell::  the engine — src/cromwell. Renderer, GPU, camera rig, logging,
 *               event bus. Knows nothing about tiles, soldiers or cover, and
 *               is built by `cmake --build . --target cromwell` with no game
 *               source in the graph at all.
 *   game::      this game — src/game. The tactical simulation (pure C++ and
 *               headless-testable, see tests/TileCoreTests.cpp) and the
 *               renderers, rules and app shell built on top of cromwell.
 *
 * The dependency runs one way only. If that ever stops being true the engine
 * target stops building, which is the point of it being a separate target.
 *
 * The diagnostics come up FIRST and in this order — log, crash handler,
 * raylib bridge — because each one is only useful if the one before it is
 * already running, and because everything worth catching happens after.
 */
#include "game/Application.hpp"
#include "game/cli/CliOptions.hpp"
#include "cromwell/steam/SteamClient.hpp"
#include "cromwell/diag/CrashHandler.hpp"
#include "cromwell/diag/RaylibLogBridge.hpp"
#include "cromwell/diag/Logger.hpp"
#include "game/light/SunBakeBenchmark.hpp"

#include <string>

int main(int argc, char** argv)
{
    /* BEFORE EVERYTHING, including the log. Steam may decide this process was
     * launched the wrong way and relaunch it through the client, in which case
     * this one must exit immediately and leave nothing behind - a log file
     * opened here would be truncated by the relaunched process a moment later,
     * and the crash handler would be installed in a process about to vanish.
     *
     * A no-op while the app id is Spacewar's 480, which every account owns. See
     * cromwell/steam/SteamClient.hpp. */
    if (cromwell::steamRestartIfNecessary()) return 0;

    const game::CliOptions options = game::CliOptions::parse(argc, argv);

    /* Beside the executable, not in the working directory: this is launched
     * from the project root, from builds/win and from Explorer, and a log you
     * have to go looking for is a log nobody reads. */
    /* The game names its own log. cromwell's default is deliberately generic —
     * an engine has no business deciding what the product is called — so the
     * name has to be supplied from here or the file turns up as cromwell.log. */
    if (options.logPath.empty()) cromwell::logger()->openBeside(argv[0], "xcom.log");
    else                         cromwell::logger()->open(options.logPath);
    cromwell::logger()->setMinLevel(cromwell::parseLogLevel(options.logLevel));

    cromwell::installCrashHandler();
    cromwell::installRaylibLogBridge();

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
        const int status = game::runSunBakeBenchmark();
        cromwell::logger()->close();
        return status;
    }

    game::Application application(options);
    const int status = application.run();

    /* The footer this writes is the point: a log that ends without one ended
     * because the process did, and that is the first thing to look for. */
    LOGGER->info("exiting with status %d", status);
    cromwell::logger()->close();
    return status;
}
