/* StripSet.hpp — the built ribbon strips.
 *
 * SINGLE RESPONSIBILITY: own strip meshes and their per-strip draw metadata.
 * Building them is somebody else's job; drawing them is RibbonRenderer's.
 *
 * RAII: the destructor unloads every mesh.
 *
 * WHAT IS AND IS NOT IN HERE. A strip is a mesh, a colour, a ring channel and
 * the storey it starts on — four things the renderer needs to decide whether
 * and how to draw it. Where the strip CAME from is not one of them. This game
 * builds them by polylining the border loops of a reach field over a cell
 * lattice; that is game/render/RibbonMeshSet's business, and it fills one of
 * these. Another game could stroke a spline into the same container and the
 * renderer would not know the difference.
 *
 * `storey` rather than a cell z on purpose: it is compared against the
 * cutaway's storey and nothing else, so the lattice's cells-per-storey
 * conversion happens once, where the lattice is, instead of being a thing the
 * renderer has to include a world model to perform.
 */
#pragma once

#include "raylib.h"

#include "cromwell/ribbon/Ring.hpp"

#include <vector>

namespace cromwell {

class StripSet {
public:
    struct Strip {
        Mesh  mesh = { 0 };
        int   storey = 0;        /* culled when it starts above the cutaway */
        Color colour = WHITE;
        Ring  ring = Ring::Move;
    };

    StripSet() = default;
    ~StripSet() { clear(); }

    /* Meshes are GPU handles this object owns; copying one would double-free
     * them on the second destructor. */
    StripSet(const StripSet&) = delete;
    StripSet& operator=(const StripSet&) = delete;

    void clear();

    /* Takes ownership of `mesh`. */
    void add(Mesh mesh, int storey, Color colour, Ring ring);

    const std::vector<Strip>& strips() const { return strips_; }
    bool empty() const { return strips_.empty(); }

private:
    std::vector<Strip> strips_;
};

}  // namespace cromwell
