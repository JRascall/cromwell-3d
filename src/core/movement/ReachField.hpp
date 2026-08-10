/* ReachField.hpp — the result of one search.
 *
 * SINGLE RESPONSIBILITY: store per-cell cost, predecessor and arrival kind,
 * and answer questions about them. It does not know how it was filled.
 */
#pragma once

#include "core/movement/Move.hpp"

#include <limits>
#include <vector>

namespace xcom {

class ReachField {
public:
    static constexpr float kUnreachable = std::numeric_limits<float>::infinity();

    ReachField() = default;
    explicit ReachField(int cellCount) { reset(cellCount); }

    void reset(int cellCount);

    int size() const { return static_cast<int>(cost_.size()); }

    float    cost(int index) const { return cost_[static_cast<std::size_t>(index)]; }
    int      previous(int index) const { return previous_[static_cast<std::size_t>(index)]; }
    MoveKind arrivalKind(int index) const { return arrivalKind_[static_cast<std::size_t>(index)]; }

    bool isReachable(int index) const { return cost(index) != kUnreachable; }
    bool isWithin(int index, float budget) const { return cost(index) <= budget + 1e-6f; }

    /* Number of cells reachable at or under `budget`. */
    int countWithin(float budget) const;

    void setStart(int index) { cost_[static_cast<std::size_t>(index)] = 0.0f; }
    void setArrival(int index, float cost, int previous, MoveKind kind);

private:
    std::vector<float>    cost_;
    std::vector<int>      previous_;
    std::vector<MoveKind> arrivalKind_;
};

}  // namespace xcom
