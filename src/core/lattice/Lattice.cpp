#include "core/lattice/Lattice.hpp"

namespace xcom {

Lattice::Lattice(int width, int height, int storeys)
    : width_(width),
      height_(height),
      storeys_(storeys),
      depth_(storeys * kCellsPerStorey),
      cellCount_(width * height * storeys * kCellsPerStorey)
{
}

int Lattice::cellOfHeight(float height, float* offsetOut) const
{
    int z = static_cast<int>(std::floor(height / kCellHeight + 1e-6f));
    if (z < 0) z = 0;
    if (z > depth_ - 1) z = depth_ - 1;
    if (offsetOut) *offsetOut = height - cellBaseHeight(z);
    return z;
}

}  // namespace xcom
