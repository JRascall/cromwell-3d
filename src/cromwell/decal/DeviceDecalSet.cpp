#include "cromwell/decal/DeviceDecalSet.hpp"

namespace cromwell {

namespace {

/* THE ONE HANDED BACK FOR AN ID NOTHING REGISTERED. Every handle invalid, so a
 * caller that ignores the id and asks anyway gets a material the decal pass
 * will skip — which is the safe direction. See DeviceDecalSet::Material on why
 * a missing albedo must abandon the decal rather than substitute white. */
const DeviceDecalSet::Material kNoMaterial{};

}  // namespace

DeviceDecalMaterialId DeviceDecalSet::addMaterial(const Material& material)
{
    /* REFUSED AT REGISTRATION RATHER THAN AT DRAW TIME. A material with no
     * albedo can never ink anything, so handing back an id for it would mean
     * every projector naming it is a draw that is set up, submitted and
     * discarded — and a decal tool offering it in a list of materials that
     * silently place nothing. Failing here is one check instead of two. */
    if (!material.albedo.valid()) return kInvalidDeviceDecalMaterial;

    materials_.push_back(material);
    return static_cast<DeviceDecalMaterialId>(materials_.size() - 1);
}

bool DeviceDecalSet::hasMaterial(DeviceDecalMaterialId id) const
{
    return id >= 0 && id < static_cast<DeviceDecalMaterialId>(materials_.size());
}

const DeviceDecalSet::Material& DeviceDecalSet::material(DeviceDecalMaterialId id) const
{
    if (!hasMaterial(id)) return kNoMaterial;
    return materials_[static_cast<std::size_t>(id)];
}

}  // namespace cromwell
