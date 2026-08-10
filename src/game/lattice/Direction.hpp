/* Direction.hpp — the four cardinal tile faces.
 *
 * SINGLE RESPONSIBILITY: name the directions and the pure arithmetic over
 * them (delta, opposite, perpendicular pair, boundary-walk rotation).
 *
 * N = +y, E = +x, S = -y, W = -x.
 */
#pragma once

#include <array>

namespace game {


enum class Dir : int { North = 0, East, South, West };

inline constexpr int kDirCount = 4;

/* All four directions, for range-for over faces. */
inline constexpr std::array<Dir, kDirCount> kAllDirs = {
    Dir::North, Dir::East, Dir::South, Dir::West
};

inline constexpr int toIndex(Dir d) { return static_cast<int>(d); }
inline constexpr Dir fromIndex(int i) { return static_cast<Dir>(i); }

inline constexpr int dx(Dir d)
{
    constexpr int kDx[kDirCount] = { 0, 1, 0, -1 };
    return kDx[toIndex(d)];
}

inline constexpr int dy(Dir d)
{
    constexpr int kDy[kDirCount] = { 1, 0, -1, 0 };
    return kDy[toIndex(d)];
}

inline constexpr Dir opposite(Dir d)
{
    constexpr Dir kOpp[kDirCount] = { Dir::South, Dir::West, Dir::North, Dir::East };
    return kOpp[toIndex(d)];
}

/* The two directions square to `d`. Used by peek-finding (step out sideways
 * from cover) and by ramp side-stepping onto a parallel flight. */
inline constexpr std::array<Dir, 2> perpendicular(Dir d)
{
    constexpr std::array<Dir, 2> kPerp[kDirCount] = {
        { Dir::East,  Dir::West  },   /* N */
        { Dir::North, Dir::South },   /* E */
        { Dir::East,  Dir::West  },   /* S */
        { Dir::North, Dir::South },   /* W */
    };
    return kPerp[toIndex(d)];
}

/* Travel heading of a boundary edge, band interior on its LEFT: edge (T,N)
 * runs -x, so its heading is turnLeft(N) = W. Rotating the EDGE label rotates
 * the heading the same way, which is why these are just +-1 mod 4. */
inline constexpr Dir turnLeft(Dir d)  { return fromIndex((toIndex(d) + 3) % kDirCount); }
inline constexpr Dir turnRight(Dir d) { return fromIndex((toIndex(d) + 1) % kDirCount); }

}  // namespace game
