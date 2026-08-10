/* RaylibLogBridge.hpp — raylib's TraceLog, redirected into the logger.
 *
 * SINGLE RESPONSIBILITY: make raylib's own diagnostics land in the same file
 * as ours, in the same order.
 *
 * Worth doing because of WHAT raylib says on that channel: shader compile and
 * link errors, texture and framebuffer failures, GL version refusals. Those
 * are the messages immediately before most of this renderer's hard failures,
 * and by default they go to a console a Release build does not have.
 *
 * Not worth reading is the rest of it — the several hundred lines of asset
 * narration before the first frame — so THAT half is demoted to debug and only
 * appears under `--log-level debug`. The failures are not demoted. See
 * levelOf() in the .cpp for where the line is drawn.
 *
 * Install before InitWindow — raylib logs during initialisation, and that is
 * exactly the part worth having.
 */
#pragma once

namespace xcom {

void installRaylibLogBridge();

}  // namespace xcom
