/* SunBakeBenchmark.hpp — the `--bake-benchmark` entry point.
 *
 * SINGLE RESPONSIBILITY: time SunBaker over the demo map and print the result.
 * Lives in core because it needs no window and no GPU.
 */
#pragma once

namespace game {


/* Returns a process exit code. */
int runSunBakeBenchmark();

}  // namespace game
