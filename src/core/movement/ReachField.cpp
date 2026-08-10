#include "core/movement/ReachField.hpp"

namespace xcom {

void ReachField::reset(int cellCount)
{
    const std::size_t n = static_cast<std::size_t>(cellCount);
    cost_.assign(n, kUnreachable);
    previous_.assign(n, -1);
    arrivalKind_.assign(n, MoveKind::Walk);
}

void ReachField::setArrival(int index, float cost, int previous, MoveKind kind)
{
    const std::size_t i = static_cast<std::size_t>(index);
    cost_[i]        = cost;
    previous_[i]    = previous;
    arrivalKind_[i] = kind;
}

int ReachField::countWithin(float budget) const
{
    int n = 0;
    for (float c : cost_)
        if (c <= budget + 1e-6f) n++;
    return n;
}

}  // namespace xcom
