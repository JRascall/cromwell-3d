/* BlockedMask.hpp — cells a particular mover may not enter.
 *
 * SINGLE RESPONSIBILITY: store one bit per cell and answer "is this blocked".
 * It knows nothing about units — OccupancyMaskBuilder fills it.
 *
 * An empty mask means pure terrain, which is what a headless search wants.
 */
#pragma once

#include <cstddef>
#include <vector>

namespace xcom {

class BlockedMask {
public:
    BlockedMask() = default;
    explicit BlockedMask(int cellCount) : bits_(static_cast<std::size_t>(cellCount), 0) {}

    void reset(int cellCount)
    {
        bits_.assign(static_cast<std::size_t>(cellCount), 0);
    }

    void block(int index) { bits_[static_cast<std::size_t>(index)] = 1; }

    bool isBlocked(int index) const
    {
        return !bits_.empty() && bits_[static_cast<std::size_t>(index)] != 0;
    }

    bool empty() const { return bits_.empty(); }
    int  size() const { return static_cast<int>(bits_.size()); }

private:
    std::vector<unsigned char> bits_;
};

}  // namespace xcom
