/* Ring.hpp — which movement ring a ribbon belongs to.
 *
 * SINGLE RESPONSIBILITY: name the rings and combine them into a mask.
 *
 * Blue is always up; the amber sprint ring joins it only while the cursor is
 * over a tile that costs more than one move. Both are built at recompute and
 * masked at draw time.
 *
 * The two are never both SOLID, though — that is what BorderRelevance is for.
 * The ring the cursor is inside draws the profile's standing channel (solid);
 * every other ring draws the scrolling one (dashed). So hovering into the
 * sprint band does not hide blue, it demotes it to a dashed blue outline with
 * solid amber outside it.
 */
#pragma once

namespace xcom {

enum class Ring : int { Move = 1, Sprint = 2 };

class RingMask {
public:
    RingMask() = default;
    RingMask(Ring ring) : bits_(static_cast<int>(ring)) {}
    static RingMask none() { return RingMask(0); }
    static RingMask both() { return RingMask(static_cast<int>(Ring::Move) |
                                             static_cast<int>(Ring::Sprint)); }

    bool contains(Ring ring) const { return (bits_ & static_cast<int>(ring)) != 0; }
    bool isEmpty() const { return bits_ == 0; }
    int  raw() const { return bits_; }

    RingMask operator|(Ring ring) const { return RingMask(bits_ | static_cast<int>(ring)); }

    friend bool operator==(RingMask a, RingMask b) { return a.bits_ == b.bits_; }

private:
    explicit RingMask(int bits) : bits_(bits) {}
    int bits_ = 0;
};

}  // namespace xcom
