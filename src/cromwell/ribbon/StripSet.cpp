#include "cromwell/ribbon/StripSet.hpp"

namespace cromwell {

void StripSet::clear()
{
    for (Strip& strip : strips_) UnloadMesh(strip.mesh);
    strips_.clear();
}

void StripSet::add(Mesh mesh, int storey, Color colour, Ring ring)
{
    strips_.push_back(Strip{ mesh, storey, colour, ring });
}

}  // namespace cromwell
