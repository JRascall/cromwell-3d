/* PerfMain.cpp — the perf benchmark's entry point.
 *
 * Its own binary rather than a flag on the app, for the same reason the tile
 * tests are: it needs no window, and a measurement that has to open one is a
 * measurement of the window as well.
 */
#include "game/light/SunBakeBenchmark.hpp"
#include "game/perf/PerfBenchmark.hpp"

#include <cstring>

/* `--bake` also runs the sun bake, which is the heaviest RayCaster consumer in
 * the tree and takes long enough that it is opt-in rather than part of every
 * run. It is the same benchmark the app exposes as --bake-benchmark, reachable
 * here without linking a window. */
int main(int argc, char** argv)
{
    const int status = game::runPerfBenchmark();
    if (status) return status;

    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--bake") == 0) return game::runSunBakeBenchmark();

    return 0;
}
