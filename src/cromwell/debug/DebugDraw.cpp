#include "cromwell/debug/DebugDraw.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/* A circle in the plane spanned by `axisU` and `axisV`, as a closed loop of
 * segments. Every round shape here is one or more of these. */
void addCircle(DebugDraw& draw, Vec3 centre, Vec3 axisU, Vec3 axisV, float radius,
               DebugColour colour, int segments, float seconds, bool depthTested)
{
    const int count = std::max(segments, 3);

    Vec3 previous = centre + axisU * radius;
    for (int step = 1; step <= count; ++step) {
        const float angle = kTwoPi * static_cast<float>(step) / static_cast<float>(count);
        const Vec3 next = centre + axisU * (std::cos(angle) * radius)
                          + axisV * (std::sin(angle) * radius);
        draw.line(previous, next, colour, seconds, depthTested);
        previous = next;
    }
}

/* Half a circle, from `axisU` swinging toward `axisV`. The dome on a capsule's
 * cap — drawing a full circle there would put half of it inside the body. */
void addArc(DebugDraw& draw, Vec3 centre, Vec3 axisU, Vec3 axisV, float radius,
            DebugColour colour, int segments, float seconds, bool depthTested)
{
    const int count = std::max(segments, 2);

    Vec3 previous = centre + axisU * radius;
    for (int step = 1; step <= count; ++step) {
        const float angle = 3.14159265358979323846f * static_cast<float>(step)
                            / static_cast<float>(count);
        const Vec3 next = centre + axisU * (std::cos(angle) * radius)
                          + axisV * (std::sin(angle) * radius);
        draw.line(previous, next, colour, seconds, depthTested);
        previous = next;
    }
}

}  // namespace

namespace debugColour {

/* Quoted as sRGB bytes and decoded to linear by UiColor, so what is written
 * here is what a colour picker would show — the scene is lit and tone-mapped,
 * and a literal 1.0 in linear is not the red anyone pictured. */
DebugColour red()     { return ui::UiColor::fromSrgb8(255, 64, 64); }
DebugColour green()   { return ui::UiColor::fromSrgb8(64, 255, 64); }
DebugColour blue()    { return ui::UiColor::fromSrgb8(80, 140, 255); }
DebugColour yellow()  { return ui::UiColor::fromSrgb8(255, 230, 64); }
DebugColour cyan()    { return ui::UiColor::fromSrgb8(64, 240, 255); }
DebugColour magenta() { return ui::UiColor::fromSrgb8(255, 80, 220); }
DebugColour white()   { return ui::UiColor::fromSrgb8(255, 255, 255); }
DebugColour orange()  { return ui::UiColor::fromSrgb8(255, 150, 40); }

}  // namespace debugColour

DebugDraw& DebugDraw::get()
{
    /* Function-local static: constructed on first use, so a debug call from a
     * static initialiser somewhere else cannot race an uninitialised queue. */
    static DebugDraw instance;
    return instance;
}

void DebugDraw::line(Vec3 from, Vec3 to, DebugColour colour, float seconds, bool depthTested)
{
    if (static_cast<int>(segments_.size()) >= kMaxSegments) {
        ++dropped_;
        return;
    }
    segments_.push_back(DebugSegment{ from, to, colour, seconds, depthTested });
}

void DebugDraw::arrow(Vec3 from, Vec3 to, DebugColour colour, float headLength,
                      float seconds, bool depthTested)
{
    line(from, to, colour, seconds, depthTested);

    const Vec3 along = to - from;
    const float length = along.length();
    if (length < 1e-6f) return;

    const Vec3 direction = along / length;

    /* PROPORTIONAL BY DEFAULT. A fixed head length is invisible on a
     * twenty-metre arrow and larger than the shaft on a ten-centimetre one, and
     * debug arrows are drawn at both scales in the same frame. */
    const float head = headLength > 0.0f ? std::min(headLength, length) : length * 0.2f;

    /* Any two axes across the shaft. Which two does not matter — the barbs are
     * a visual cue, not a measurement — but they must be well-conditioned for
     * ANY shaft direction, which is what anyPerpendicular guarantees and a
     * fixed cross-against-up does not. */
    const Vec3 sideA = anyPerpendicular(direction);
    const Vec3 sideB = cross(direction, sideA);

    const Vec3 base = to - direction * head;
    const float spread = head * 0.4f;

    line(to, base + sideA * spread, colour, seconds, depthTested);
    line(to, base - sideA * spread, colour, seconds, depthTested);
    line(to, base + sideB * spread, colour, seconds, depthTested);
    line(to, base - sideB * spread, colour, seconds, depthTested);
}

void DebugDraw::sphere(Vec3 centre, float radius, DebugColour colour, float seconds,
                       int segments, bool depthTested)
{
    /* Three great circles rather than a wire mesh. Enough to read as a sphere
     * from any angle and a fraction of the segments — a debug sphere is a
     * position and a radius, not a surface. */
    addCircle(*this, centre, Vec3::right(), Vec3::up(), radius, colour, segments, seconds,
              depthTested);
    addCircle(*this, centre, Vec3::up(), Vec3::forward(), radius, colour, segments, seconds,
              depthTested);
    addCircle(*this, centre, Vec3::forward(), Vec3::right(), radius, colour, segments,
              seconds, depthTested);
}

void DebugDraw::box(Vec3 centre, Vec3 halfExtents, DebugColour colour, float seconds,
                    bool depthTested)
{
    bounds(centre - halfExtents, centre + halfExtents, colour, seconds, depthTested);
}

void DebugDraw::bounds(Vec3 min, Vec3 max, DebugColour colour, float seconds,
                       bool depthTested)
{
    const Vec3 corner[8] = {
        { min.x, min.y, min.z }, { max.x, min.y, min.z },
        { max.x, min.y, max.z }, { min.x, min.y, max.z },
        { min.x, max.y, min.z }, { max.x, max.y, min.z },
        { max.x, max.y, max.z }, { min.x, max.y, max.z },
    };

    for (int index = 0; index < 4; ++index) {
        const int next = (index + 1) % 4;
        line(corner[index], corner[next], colour, seconds, depthTested);            /* bottom */
        line(corner[index + 4], corner[next + 4], colour, seconds, depthTested);    /* top    */
        line(corner[index], corner[index + 4], colour, seconds, depthTested);       /* upright */
    }
}

void DebugDraw::capsule(Vec3 centre, float radius, float halfHeight, DebugColour colour,
                        float seconds, int segments, bool depthTested)
{
    const float r = std::max(radius, 0.0f);
    const float half = std::max(halfHeight, r);

    /* The cap centres — the ends of the inner segment, the same quantity
     * TraceShape::segmentHalf reports. Drawing to `half` instead would put the
     * domes a radius too far apart, which is exactly the mistake the sweep
     * tests guard against and would make the drawing lie about the shape. */
    const float segmentHalf = half - r;
    const Vec3 top = centre + Vec3{ 0.0f, segmentHalf, 0.0f };
    const Vec3 bottom = centre - Vec3{ 0.0f, segmentHalf, 0.0f };

    addCircle(*this, top, Vec3::right(), Vec3::forward(), r, colour, segments, seconds,
              depthTested);
    addCircle(*this, bottom, Vec3::right(), Vec3::forward(), r, colour, segments, seconds,
              depthTested);

    /* The four uprights joining them. */
    for (const Vec3 side : { Vec3::right(), Vec3::left(), Vec3::forward(), Vec3::back() }) {
        line(bottom + side * r, top + side * r, colour, seconds, depthTested);
    }

    /* Two arcs per cap, at right angles, which is what makes the ends read as
     * domes rather than as flat discs. */
    addArc(*this, top, Vec3::right(), Vec3::up(), r, colour, segments / 2, seconds,
           depthTested);
    addArc(*this, top, Vec3::forward(), Vec3::up(), r, colour, segments / 2, seconds,
           depthTested);
    addArc(*this, bottom, Vec3::right(), Vec3::down(), r, colour, segments / 2, seconds,
           depthTested);
    addArc(*this, bottom, Vec3::forward(), Vec3::down(), r, colour, segments / 2, seconds,
           depthTested);
}

void DebugDraw::point(Vec3 at, float size, DebugColour colour, float seconds,
                      bool depthTested)
{
    const float half = size * 0.5f;
    line(at - Vec3{ half, 0.0f, 0.0f }, at + Vec3{ half, 0.0f, 0.0f }, colour, seconds,
         depthTested);
    line(at - Vec3{ 0.0f, half, 0.0f }, at + Vec3{ 0.0f, half, 0.0f }, colour, seconds,
         depthTested);
    line(at - Vec3{ 0.0f, 0.0f, half }, at + Vec3{ 0.0f, 0.0f, half }, colour, seconds,
         depthTested);
}

void DebugDraw::axes(Vec3 origin, Quat rotation, float length, float seconds,
                     bool depthTested)
{
    /* X red, Y green, Z blue — the convention every DCC tool and both engines
     * use, so it needs no legend. */
    arrow(origin, origin + rightOf(rotation) * length, debugColour::red(), 0.0f, seconds,
          depthTested);
    arrow(origin, origin + upOf(rotation) * length, debugColour::green(), 0.0f, seconds,
          depthTested);
    arrow(origin, origin + forwardOf(rotation) * length, debugColour::blue(), 0.0f, seconds,
          depthTested);
}

void DebugDraw::normal(Vec3 at, Vec3 direction, float length, DebugColour colour,
                       float seconds, bool depthTested)
{
    point(at, length * 0.25f, colour, seconds, depthTested);
    arrow(at, at + direction.normalised() * length, colour, 0.0f, seconds, depthTested);
}

void DebugDraw::advance(float deltaSeconds)
{
    /* Aged first, then swept. A segment added with seconds = 0 therefore
     * survives exactly the frame it was added in — it is drawn at the end of
     * that frame and removed by the next advance. See the header. */
    for (DebugSegment& segment : segments_) {
        segment.secondsRemaining -= deltaSeconds;
    }

    std::erase_if(segments_, [](const DebugSegment& segment) {
        return segment.secondsRemaining <= 0.0f;
    });

    /* Reset with the frame: `dropped` answers "did THIS frame overflow", which
     * is the actionable question. A running total since process start would
     * keep reporting an overflow long after the loop that caused it was
     * deleted. */
    dropped_ = 0;
}

void DebugDraw::clear()
{
    segments_.clear();
    dropped_ = 0;
}

/* ---- the free functions ------------------------------------------------- */

void debugLine(Vec3 from, Vec3 to, DebugColour colour, float seconds)
{
    DebugDraw::get().line(from, to, colour, seconds);
}

void debugArrow(Vec3 from, Vec3 to, DebugColour colour, float seconds)
{
    DebugDraw::get().arrow(from, to, colour, 0.0f, seconds);
}

void debugSphere(Vec3 centre, float radius, DebugColour colour, float seconds)
{
    DebugDraw::get().sphere(centre, radius, colour, seconds);
}

void debugBox(Vec3 centre, Vec3 halfExtents, DebugColour colour, float seconds)
{
    DebugDraw::get().box(centre, halfExtents, colour, seconds);
}

void debugCapsule(Vec3 centre, float radius, float halfHeight, DebugColour colour,
                  float seconds)
{
    DebugDraw::get().capsule(centre, radius, halfHeight, colour, seconds);
}

void debugPoint(Vec3 at, float size, DebugColour colour, float seconds)
{
    DebugDraw::get().point(at, size, colour, seconds);
}

void debugAxes(Vec3 origin, Quat rotation, float length, float seconds)
{
    DebugDraw::get().axes(origin, rotation, length, seconds);
}

void debugNormal(Vec3 at, Vec3 direction, float length, DebugColour colour, float seconds)
{
    DebugDraw::get().normal(at, direction, length, colour, seconds);
}

}  // namespace cromwell
