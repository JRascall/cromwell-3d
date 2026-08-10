/* PerfBenchmark.hpp — how much does the simulation cost, and how does that
 * cost GROW.
 *
 * SINGLE RESPONSIBILITY: time the queries the AI will lean on, at several
 * roster sizes, and print the numbers.
 *
 * WHY ROSTER SIZES RATHER THAN ONE RUN. A benchmark that reports a single
 * millisecond figure answers "is it fast today", which is the less useful
 * question. The one that decides how many bodies this game can hold is "what
 * happens to that figure when the roster triples" — a query that scans every
 * unit looks fine at four and is a wall at a hundred, and the only way to see
 * the difference is to measure both. Every row below is therefore repeated at
 * 4, 16, 64 and 256 bodies.
 *
 * HEADLESS, like SunBakeBenchmark beside it. These are questions about the
 * simulation, so no window is opened and no GPU is touched.
 */
#pragma once

namespace game {

/* Runs every case and prints a report. Returns 0. */
int runPerfBenchmark();

}  // namespace game
