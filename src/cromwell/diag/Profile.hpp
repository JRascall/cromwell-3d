/* Profile.hpp — name a scope so it can be timed.
 *
 * SINGLE RESPONSIBILITY: one set of instrumentation macros, feeding whichever
 * profilers are present.
 *
 * TWO CONSUMERS, ONE SET OF ZONES. A zone always feeds cromwell's own
 * Profiler, which is what the in-game dev panel draws and what F9 captures. It
 * ADDITIONALLY feeds Tracy when built with -DXC_TRACY=ON.
 *
 * That split is deliberate. The question asked most often is "is it slow right
 * now, and which pass" — and answering it should not need a second application,
 * a socket and a rebuild. It should be a panel while you fly the camera. Tracy
 * is better at the deeper question, and worse at that one. So the cheap
 * always-on profiler answers the common case and Tracy is there for the hard
 * case, from the same instrumentation, because instrumentation written twice
 * drifts.
 *
 * A NEW SYSTEM GETS A ZONE IN THE SAME COMMIT. Not because it is tidy — because
 * an uninstrumented system does not appear in the panel as a zero, it does not
 * appear AT ALL, and its cost silently inflates whatever encloses it. The
 * `frame` row is the truth and the rows beneath it are what has been accounted
 * for; the difference between them is time nobody can see. That gap widens
 * every time something is added without a zone, and a spike hiding inside it is
 * precisely the debugging session this exists to prevent.
 *
 * ONE ZONE PER SYSTEM, AND GRANULARITY IS EARNED BY COST. Sub-zones are for
 * things that are a large share of the frame, not for things that have a lot of
 * source files. The failure is mirroring the module tree: navigation gets a zone
 * for the representation, one for the planner, one for steering, one for the
 * flow field, and now twenty rows describe 0.3 ms. Zone names are not a table of
 * contents for the code — they answer "where did the frame go", and a system
 * that is not going anywhere needs one row saying so.
 *
 * `render` earns shadow map / ssao / lit scene because it is most of the frame.
 * `steam` does not, because knowing which part of a 0.02 ms callback pump was
 * slow changes nothing.
 *
 * So add a system's zone with the system, and sub-zones only once a measurement
 * points at it — the split will follow whatever line the measurement revealed,
 * which is rarely the line the module structure suggested. Ten temporary zones
 * to hunt a spike is exactly right; delete them and keep the one that explained
 * it.
 *
 * WHERE TO PUT ZONES. Whole passes, whole systems, whole frames — anything
 * running once or a few dozen times a frame. A zone costs ~40 ns, which is
 * nothing around a render pass and ruinous inside a ray step: one in
 * RayCaster's DDA loop would cost more than the loop it measures and would
 * produce tens of millions of events per bake. Instrument the CALLER. If you
 * want a number for something running thousands of times, what you want is a
 * benchmark — see tests/PerfMain.cpp.
 *
 *   CW_PROFILE_ZONE_N("shadow map");   a named scope
 *   CW_PROFILE_FRAME();                once per presented frame
 *   CW_PROFILE_MESSAGE("bake done");   a marker on the timeline
 *
 * GPU zones are the other half and live in cromwell/gpu/GpuProfiler.hpp,
 * because they need a GL context and this header must stay usable from the
 * headless simulation.
 */
#pragma once

#include "cromwell/diag/Profiler.hpp"

/* Paste the line number in, so two zones in one scope cannot collide. */
#define CW_PROFILE_JOIN2(a, b) a##b
#define CW_PROFILE_JOIN(a, b)  CW_PROFILE_JOIN2(a, b)

#if XC_HAVE_TRACY

#include <tracy/Tracy.hpp>

/* Tracy's macros declare their own scoped objects, so these pair the two
 * rather than choosing between them. */
#define CW_PROFILE_ZONE_N(name)                                               \
    ::cromwell::ProfileScope CW_PROFILE_JOIN(cwZone_, __LINE__)(name);        \
    ZoneScopedN(name)
#define CW_PROFILE_FRAME()       do { ::cromwell::Profiler::instance().endFrame(); FrameMark; } while (0)
#define CW_PROFILE_MESSAGE(text) TracyMessageL(text)
#define CW_PROFILE_THREAD(name)  ::tracy::SetThreadName(name)
#define CW_PROFILE_PLOT(n, v)    TracyPlot(n, v)

#else

#define CW_PROFILE_ZONE_N(name)                                               \
    ::cromwell::ProfileScope CW_PROFILE_JOIN(cwZone_, __LINE__)(name)
#define CW_PROFILE_FRAME()       ::cromwell::Profiler::instance().endFrame()
#define CW_PROFILE_MESSAGE(text) ((void)sizeof(text))
#define CW_PROFILE_THREAD(name)  ((void)sizeof(name))
#define CW_PROFILE_PLOT(n, v)    ((void)sizeof(n))

#endif

/* Named after the enclosing function. __func__ is a static array with the
 * lifetime the profiler needs, which a std::string would not be. */
#define CW_PROFILE_ZONE() CW_PROFILE_ZONE_N(__func__)
