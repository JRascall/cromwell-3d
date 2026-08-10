/* EdgeId.hpp — a directed boundary edge as one integer.
 *
 * SINGLE RESPONSIBILITY: pack and unpack (cell, direction).
 *
 * That is what the JS version wanted to be all along — it had to key corners
 * by "x,y" strings and then spend a hundred lines undoing the ambiguity that
 * created. Because every query names a specific CELL rather than asking "what
 * is at this corner", two boundaries sharing a 2D corner on different storeys
 * are never confused.
 */
#pragma once

#include "core/lattice/Direction.hpp"

namespace xcom {

class EdgeId {
public:
    EdgeId() = default;
    EdgeId(int cellIndex, Dir d) : raw_(cellIndex * kDirCount + toIndex(d)) {}
    static EdgeId fromRaw(int raw) { EdgeId e; e.raw_ = raw; return e; }

    int cell() const { return raw_ / kDirCount; }
    Dir dir()  const { return fromIndex(raw_ % kDirCount); }
    int raw()  const { return raw_; }

    friend bool operator==(EdgeId a, EdgeId b) { return a.raw_ == b.raw_; }

private:
    int raw_ = 0;
};

/* The corner points of a directed boundary edge, band interior on its LEFT. */
struct EdgeSegment {
    float fromX = 0.0f, fromY = 0.0f;
    float toX   = 0.0f, toY   = 0.0f;
};

inline EdgeSegment edgeCorners(int x, int y, Dir d)
{
    const float X = static_cast<float>(x);
    const float Y = static_cast<float>(y);
    switch (d) {
        case Dir::North: return { X + 1, Y + 1, X,     Y + 1 };  /* -x, interior below */
        case Dir::South: return { X,     Y,     X + 1, Y     };  /* +x, interior above */
        case Dir::East:  return { X + 1, Y,     X + 1, Y + 1 };  /* +y, interior -x    */
        default:         return { X,     Y + 1, X,     Y     };  /* W: -y, interior +x */
    }
}

}  // namespace xcom
