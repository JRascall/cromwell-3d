/* VisibilityField.hpp — what a unit can see, per cell.
 *
 * SINGLE RESPONSIBILITY: store one visibility grade per cell. Filling it is
 * VisibilityComputer's job.
 */
#pragma once

#include <cstddef>
#include <vector>

namespace game {


enum class Visibility : unsigned char { None = 0, Direct, PeekOnly };

class VisibilityField {
public:
    VisibilityField() = default;
    explicit VisibilityField(int cellCount) { reset(cellCount); }

    void reset(int cellCount)
    {
        grades_.assign(static_cast<std::size_t>(cellCount), Visibility::None);
    }

    Visibility at(int index) const { return grades_[static_cast<std::size_t>(index)]; }
    void set(int index, Visibility grade) { grades_[static_cast<std::size_t>(index)] = grade; }

    bool isVisible(int index) const { return at(index) != Visibility::None; }
    int  size() const { return static_cast<int>(grades_.size()); }

    int countVisible() const;
    int countPeekOnly() const;

private:
    std::vector<Visibility> grades_;
};

}  // namespace game
