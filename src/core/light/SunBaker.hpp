/* SunBaker.hpp — direct sunlight, baked per lightmap texel.
 *
 * SINGLE RESPONSIBILITY: decide how much of the sun's disc reaches each texel
 * of every static surface, and say how long that took.
 *
 * WHY BAKE AT ALL, WHEN THERE IS ALREADY A SHADOW MAP. Source 2's best-looking
 * shadows are not shadow maps: static geometry casting onto static geometry is
 * resolved offline by a path tracer, and only dynamic objects use a depth
 * buffer (see study/source2_rendering.md). A bake has no texel grid to
 * staircase against, no depth bias to tune, and gets real area-light softness
 * with contact hardening for free, because "how much of the sun can I see" is
 * literally what it measures.
 *
 * WHAT ABOUT DESTRUCTION. A bake goes stale the moment a wall is blown out:
 * its shadow stays on the floor, and the light that should now pour through
 * the hole does not. That is what bakeRegion is for. Destruction here is a
 * discrete, bounded event, and the set of texels it can possibly affect is
 * computable rather than guessed:
 *
 *   - texels near the blast, whose local occlusion changed, and
 *   - texels in the SHADOW SHAFT of the changed cells — march each changed
 *     cell along the sun direction and collect what it was shading.
 *
 * With exactly one sun that second set is a line, not a hemisphere, which is
 * the whole reason this is cheap.
 *
 * PARAMETERISATION IS PER CELL FACE. An axis-aligned lattice does not need an
 * atlas packer: every surface is a unit quad belonging to one cell and one
 * direction, so a patch index is (cell, face) and the UVs are stable across a
 * rebuild. That also means a re-bake can address exactly the patches it needs.
 *
 * PURE CORE — no raylib, no GPU. It answers a question about the world, and is
 * headless-testable like the rest of core/.
 */
#pragma once

#include "core/lattice/Direction.hpp"
#include "core/world/World.hpp"

#include <cstdint>
#include <vector>

namespace xcom {

struct SunBakeStats {
    int           patches = 0;       /* cell faces that carry a lightmap patch */
    int           texels = 0;
    std::int64_t  rays = 0;
    double        milliseconds = 0.0;

    double raysPerSecond() const
    {
        return milliseconds > 0.0 ? static_cast<double>(rays) / (milliseconds / 1000.0) : 0.0;
    }
};

/* Where the sun is and how big it looks. `angularRadius` is Source 2's
 * SunSpreadAngle by another name: it is what makes a shadow soften with
 * distance from its caster instead of staying razor sharp. */
struct SunSample {
    float directionX = 0.0f;   /* the direction light TRAVELS, normalised */
    float directionY = -1.0f;
    float directionZ = 0.0f;
    float angularRadius = 0.03f;   /* radians; the real sun is ~0.0047 */
};

/* HOW THE LIGHTMAP IS LAID OUT.
 *
 * DENSE, PLUS AN INDEX TEXTURE. Patches are packed back to back — patch n goes
 * to slot n — and a small lookup texture maps (cell, face) to a slot. The
 * shader still derives cell and face from world position and surface normal,
 * so there is still no second UV channel and nothing for the geometry emitter
 * to assign; it just takes one extra fetch to find out where the patch lives.
 *
 * THE FIRST VERSION LAID THE ATLAS OUT TO MATCH THE WORLD, one page per
 * (z, face), which made the address pure arithmetic and needed no index at
 * all. That was simpler and it was the wrong trade: it has to allocate a block
 * for every cell in the lattice whether or not anything stands there, and on
 * this map only 1120 of 25920 possible patches exist. Twenty-three parts in
 * twenty-four of the texture were empty, which capped the affordable
 * resolution at 16 texels per tile — against the 4096 shadow map's effective
 * 109 per tile. It looked worse than what it replaced, and being cheap to
 * implement is no consolation for that.
 *
 * Packing spends the same memory on real surfaces, so the same budget buys
 * ~64 texels per tile instead.
 *
 * The cost of packing is that atlas neighbours are no longer world
 * neighbours, so bilinear must not run off the edge of a patch. The shader
 * clamps to the patch's inner half-texel, which costs a little blending across
 * cell seams and is invisible at this density. */
struct SunLightmapLayout {
    int texelsPerTile = 8;
    int gridWidth = 0;
    int gridHeight = 0;
    int gridDepth = 0;
    int patchesPerRow = 1;
    int width = 0;      /* atlas texture size, texels */
    int height = 0;

    /* (cell, face) -> slot. Width is gridWidth * facesPerCell, height is
     * gridHeight * gridDepth. Slots are 16 bit, split across R and G. */
    int indexWidth = 0;
    int indexHeight = 0;
};

class SunBaker {
public:
    /* `texelsPerTile` is per axis — 16 gives a 16x16 patch per cell face,
     * about 9cm texels at XCOM's 96uu tile. `raysPerTexel` samples the sun's
     * disc; 1 gives hard shadows, more gives a penumbra. */
    SunBaker(const World& world, int texelsPerTile, int raysPerTexel);

    /* Re-derives the patch set after the world's geometry changed, CARRYING
     * SURVIVING TEXELS ACROSS. Destruction removes surfaces, so the patch list
     * shifts; without a stable (cell, face) key every index would move and the
     * whole map would need re-baking to avoid garbage. Call this on the same
     * event that rebuilds the render mesh, then bakeRegion. */
    void refreshGeometry();

    /* Every static surface in the map. */
    SunBakeStats bakeAll(const SunSample& sun);

    /* Only what a change at `centre` could have altered: the blast itself,
     * plus everything its cells were shading. */
    SunBakeStats bakeRegion(const SunSample& sun, const Cell& centre, float radiusTiles);

    /* 0 = fully shadowed, 1 = fully lit. Empty until a bake has run. */
    const std::vector<float>& visibility() const { return visibility_; }

    int patchCount() const { return static_cast<int>(patches_.size()); }
    int texelsPerPatch() const { return texelsPerPatch_; }

    /* ---- the uploadable lightmap ---------------------------------------- */
    const SunLightmapLayout& layout() const { return layout_; }

    /* Single-channel, one byte per texel, row-major. Rebuilt from visibility_
     * by refreshAtlas(); cells with no surface stay fully lit so nothing ever
     * samples an uninitialised black. */
    const std::vector<unsigned char>& atlas() const { return atlas_; }

    /* FOUR bytes per entry — slot low byte in R, high byte in G, B and A
     * unused. Two would do, but raylib swizzles a two-channel texture to
     * (R,R,R,G), so .rg in the shader reads the low byte twice and silently
     * corrupts every slot number. RGBA8 has no such surprise, and this texture
     * is a hundred kilobytes.
     *
     * 0xFFFF means "no patch here", which the shader reads as "not baked". */
    const std::vector<unsigned char>& indexMap() const { return indexMap_; }

    /* Call after a bake to push the results into the atlas image. */
    void refreshAtlas();

    /* The stable address of a surface: (cell, face) -> patch slot, or -1 when
     * that surface does not exist. Patch slots move when geometry changes;
     * this key does not, which is what lets a caller compare a bake before and
     * after a demolition. */
    static constexpr int kFacesPerCell = 5;   /* 0 = floor, 1..4 = walls */
    int slotOf(const Cell& cell, int face) const;
    const std::vector<int>& slotTable() const { return slotOf_; }

private:
    /* All triples below are (latticeX, latticeY, height) — the coordinate
     * RayCaster::cast speaks, where the lattice's y is the world's z and
     * height is up. Mixing that up with the renderer's y-up world coordinates
     * is the easiest mistake to make in this file. */
    struct Patch {
        Cell  cell;
        int   face = 0;
        float originX = 0.0f, originY = 0.0f, originH = 0.0f;   /* texel (0,0) corner */
        float uX = 0.0f, uY = 0.0f, uH = 0.0f;                  /* one texel step, u  */
        float vX = 0.0f, vY = 0.0f, vH = 0.0f;                  /* one texel step, v  */
        float normalX = 0.0f, normalY = 0.0f, normalH = 1.0f;
    };

    /* Rebuilt whenever the geometry changes, which is the same event that
     * rebuilds the render mesh. */
    void collectPatches();

    /* Bakes the listed patches across every core, returns rays cast. */
    std::int64_t bakeSlots(const std::vector<std::size_t>& slots, const SunSample& sun);

    /* Bakes one patch into visibility_, returns rays cast. */
    std::int64_t bakePatch(const class RayCaster& caster, const Patch& patch,
                           std::size_t firstTexel, const SunSample& sun);

    /* Marks every cell the changed set could have been shading. */
    void collectShadowShaft(const SunSample& sun, const Cell& centre, float radiusTiles,
                            std::vector<std::uint8_t>& affected) const;

    const World& world_;
    int texelsPerTile_;
    int raysPerTexel_;
    int texelsPerPatch_;

    std::vector<Patch> patches_;
    std::vector<float> visibility_;
    std::vector<int>   slotOf_;   /* cellIndex * kFacesPerCell + face -> slot */
    bool patchesValid_ = false;

    SunLightmapLayout          layout_;
    std::vector<unsigned char> atlas_;
    std::vector<unsigned char> indexMap_;
};

}  // namespace xcom
