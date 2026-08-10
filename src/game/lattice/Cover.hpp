/* Cover.hpp — the cover grades and the cosmetic surface tags.
 *
 * SINGLE RESPONSIBILITY: name the two small enumerations tiles are classified
 * by. Cover ordering matters — Edge resolution takes the MAXIMUM of the two
 * sides, so None < Half < Full must hold.
 */
#pragma once

namespace game {


enum class Cover : int { None = 0, Half, Full };

/* Purely cosmetic surface variants — see Tile::artTag. Nothing in the
 * simulation reads this; it picks the render material and nothing else. */
enum class Art : int { Plain = 0, Road, Grass };

inline constexpr bool operator<(Cover a, Cover b)
{
    return static_cast<int>(a) < static_cast<int>(b);
}

inline constexpr Cover strongest(Cover a, Cover b) { return a < b ? b : a; }

}  // namespace game
