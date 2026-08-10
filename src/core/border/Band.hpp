/* Band.hpp — the set of cells a movement ring covers.
 *
 * SINGLE RESPONSIBILITY: membership. One bit per cell, nothing else.
 *
 * BOUNDARY IS DECIDED BY SET MEMBERSHIP, NOT CONNECTIVITY. The line outlines
 * where you can GO; it does not trace geometry. A wall standing between two
 * reachable tiles is inside the band and gets no line at all — which is why an
 * interior wall never makes the border run down both its faces and hairpin
 * around its tip. Height is what separates one part of the band from another,
 * not walls.
 */
#pragma once

#include <cstddef>
#include <vector>

namespace xcom {

class Band {
public:
    Band() = default;
    explicit Band(int cellCount) { reset(cellCount); }

    void reset(int cellCount)
    {
        members_.assign(static_cast<std::size_t>(cellCount), 0);
    }

    void mark(int index) { members_[static_cast<std::size_t>(index)] = 1; }

    bool contains(int index) const { return members_[static_cast<std::size_t>(index)] != 0; }

    int size() const { return static_cast<int>(members_.size()); }
    int count() const;

private:
    std::vector<unsigned char> members_;
};

}  // namespace xcom
