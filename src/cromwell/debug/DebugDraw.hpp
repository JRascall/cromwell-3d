/* DebugDraw.hpp — drawing a line in the world from anywhere, including from
 * code that cannot draw.
 *
 * SINGLE RESPONSIBILITY: collect debug geometry with lifetimes. It rasterises
 * nothing; DebugRenderer does that. Same split as the UI kit's draw list and
 * painter, for the same reason and with a sharper edge to it.
 *
 * ===================== WHY THE SPLIT MATTERS MORE HERE =====================
 *
 * THE CODE THAT MOST NEEDS TO DRAW IS THE CODE THAT CANNOT. A pathfinder, a
 * trace, a cover scorer, a steering behaviour — every one of them lives in the
 * headless half and has no renderer, no camera and no GL context. That is
 * exactly why they are hard to debug, and printing numbers is a poor substitute
 * for seeing where the ray went.
 *
 * So the queue is in cromwell_base with no raylib anywhere near it. A sweep can
 * call `debugLine(...)` from inside its inner loop; the frame draws whatever
 * accumulated. Nothing in the calling code learns what a renderer is.
 *
 * ========================= WHY THERE IS ONE, GLOBALLY ======================
 *
 * BECAUSE A DEBUG LINE HAS TO BE ONE LINE OF CODE OR IT WILL NOT BE WRITTEN.
 * The entire value of this facility is that you can drop a call into the middle
 * of a function you are staring at, see the answer, and delete it — and
 * threading a DebugDraw& down six call levels to do that is enough friction
 * that people print numbers instead. Unreal and Unity both made it global for
 * this reason, and both were right.
 *
 * It follows the LOGGER precedent already in this tree: one process-wide sink,
 * reached by a free function. It is NOT in Services, because a system that has
 * to be registered before it works is a system that is absent in exactly the
 * throwaway situations it exists for.
 *
 * NOT THREAD-SAFE, and deliberately not — a mutex on every debug line would
 * change the timing of the thing being debugged, which is the one thing a
 * diagnostic must not do. Call it from the frame thread.
 *
 * ======================= EVERYTHING BECOMES A SEGMENT ======================
 *
 * A sphere is three circles, a box is twelve edges, an arrow is a shaft and
 * four barbs. All of them are TESSELLATED WHEN THEY ARE ADDED, so what the
 * queue holds is one flat array of line segments and the renderer is a single
 * loop with no shape switch in it.
 *
 * That costs a little memory for a persistent sphere and buys three things: the
 * renderer cannot grow a per-shape code path, the headless half can be tested by
 * counting segments, and a caller can add a shape this file has never heard of
 * by adding its lines directly.
 *
 * ========================== LIFETIME, AND THE DEFAULT ======================
 *
 * `seconds = 0` means ONE FRAME, which is the common case: call it every frame
 * from the system you are watching and it tracks. A positive lifetime is for
 * one-shot events — a trace that happened once, a hit that was scored — and is
 * what Unreal's persistent lines are for.
 *
 * COLD CODE, but it is a per-frame system, so the RENDERER gets a profiler zone
 * (see DebugRenderer.hpp). This half is a vector push and has no zone of its
 * own; it runs inside whatever zone its caller already has, which is the point
 * — a diagnostic that showed up as its own row would be measuring the person
 * looking at it.
 */
#pragma once

#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/ui/core/UiColor.hpp"

#include <vector>

namespace cromwell {

/* Debug geometry is coloured with the UI's colour type.
 *
 * NOT BECAUSE IT IS UI — it plainly is not — but because UiColor is already the
 * engine's headless linear-RGBA value and a second identical struct would be
 * duplication that drifts. That the type lives under ui/ is a misfiling worth
 * correcting one day; inventing DebugColour to avoid the include would be the
 * more expensive mistake. */
using DebugColour = ui::UiColor;

/* Named colours, so a call site reads as intent. Deliberately few and
 * deliberately saturated: debug geometry has to be legible over an arbitrary
 * scene, and the moment there is a palette people start colour-coding fifteen
 * things and nobody can remember which is which. */
namespace debugColour {
DebugColour red();
DebugColour green();
DebugColour blue();
DebugColour yellow();
DebugColour cyan();
DebugColour magenta();
DebugColour white();
DebugColour orange();
}  // namespace debugColour

/* One line, which is all the queue holds. */
struct DebugSegment {
    Vec3 from;
    Vec3 to;
    DebugColour colour;

    /* Counts down; removed at or below zero on the next advance(). */
    float secondsRemaining = 0.0f;

    /* False draws it THROUGH geometry — an x-ray line. Off by default, because
     * a debug line hidden behind the wall you are investigating tells you
     * nothing, and the first thing anyone does with a depth-tested one is turn
     * the depth test off. Pass true when the occlusion is the information. */
    bool depthTested = false;
};

class DebugDraw {
public:
    /* The process-wide queue. See the header on why this is global. */
    static DebugDraw& get();

    /* ---- primitives ------------------------------------------------------
     * `seconds` of 0 means one frame. Every shape below reduces to line()
     * before it is stored. */

    void line(Vec3 from, Vec3 to, DebugColour colour, float seconds = 0.0f,
              bool depthTested = false);

    /* A line with a head at `to`. `headLength` of 0 sizes the head at a fifth
     * of the shaft, which reads correctly at any scale — a fixed head is
     * invisible on a long arrow and swamps a short one. */
    void arrow(Vec3 from, Vec3 to, DebugColour colour, float headLength = 0.0f,
               float seconds = 0.0f, bool depthTested = false);

    /* Three great circles, in the three axis planes. `segments` is per circle. */
    void sphere(Vec3 centre, float radius, DebugColour colour, float seconds = 0.0f,
                int segments = 24, bool depthTested = false);

    /* An axis-aligned box, as its twelve edges. */
    void box(Vec3 centre, Vec3 halfExtents, DebugColour colour, float seconds = 0.0f,
             bool depthTested = false);
    void bounds(Vec3 min, Vec3 max, DebugColour colour, float seconds = 0.0f,
                bool depthTested = false);

    /* An UPRIGHT capsule, matching TraceShape::capsule — `halfHeight` measured
     * to the outside of the caps. The shape a character controller sweeps, so
     * being able to draw exactly what was swept is most of the point. */
    void capsule(Vec3 centre, float radius, float halfHeight, DebugColour colour,
                 float seconds = 0.0f, int segments = 16, bool depthTested = false);

    /* A small three-axis cross. For marking a position where a sphere would be
     * too heavy — a path node, a spawn point, a contact. */
    void point(Vec3 at, float size, DebugColour colour, float seconds = 0.0f,
               bool depthTested = false);

    /* The red/green/blue gizmo for an orientation: X red, Y green, Z blue, the
     * convention every tool uses. Takes no colour, because the colours ARE the
     * information. */
    void axes(Vec3 origin, Quat rotation, float length, float seconds = 0.0f,
              bool depthTested = false);

    /* A contact: a point, and the normal coming out of it. The shape a trace
     * result wants, and having it as one call is what makes checking a sweep a
     * one-line edit. */
    void normal(Vec3 at, Vec3 direction, float length, DebugColour colour,
                float seconds = 0.0f, bool depthTested = false);

    /* ---- the frame ------------------------------------------------------ */

    /* Ages everything and drops what has expired. Call once per frame, BEFORE
     * anything adds to it — a one-frame line added during frame N is drawn at
     * the end of N and removed by frame N+1's advance. */
    void advance(float deltaSeconds);

    void clear();

    /* ---- what the renderer reads ---------------------------------------- */
    const std::vector<DebugSegment>& segments() const { return segments_; }
    bool empty() const { return segments_.empty(); }

    /* Segments discarded because the queue was full, since the last clear.
     * NOT SILENT: a debug facility that quietly stopped drawing would be
     * misread as "the thing I am debugging stopped happening", which is the
     * worst possible failure for a diagnostic. The renderer surfaces this. */
    int dropped() const { return dropped_; }

    /* The cap. Generous — a few thousand lines is a busy debug frame — but
     * finite, because the usual way this facility goes wrong is a call inside a
     * loop that runs far more often than its author thought. */
    static constexpr int kMaxSegments = 65536;

private:
    DebugDraw() = default;

    std::vector<DebugSegment> segments_;
    int dropped_ = 0;
};

/* ---- free functions, because a debug call must be short --------------------
 * The whole facility fails if using it is a chore. See the header. */

void debugLine(Vec3 from, Vec3 to, DebugColour colour, float seconds = 0.0f);
void debugArrow(Vec3 from, Vec3 to, DebugColour colour, float seconds = 0.0f);
void debugSphere(Vec3 centre, float radius, DebugColour colour, float seconds = 0.0f);
void debugBox(Vec3 centre, Vec3 halfExtents, DebugColour colour, float seconds = 0.0f);
void debugCapsule(Vec3 centre, float radius, float halfHeight, DebugColour colour,
                  float seconds = 0.0f);
void debugPoint(Vec3 at, float size, DebugColour colour, float seconds = 0.0f);
void debugAxes(Vec3 origin, Quat rotation, float length, float seconds = 0.0f);
void debugNormal(Vec3 at, Vec3 direction, float length, DebugColour colour,
                 float seconds = 0.0f);

}  // namespace cromwell
