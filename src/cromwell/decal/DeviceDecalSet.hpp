/* DeviceDecalSet.hpp — where the decals are, and what they wear, on the device.
 *
 * SINGLE RESPONSIBILITY: hold this world's decal projectors and the material
 * table they name, in terms no graphics API owns. It draws nothing and owns no
 * texture.
 *
 * ================== WHY IT IS THE SCENE'S AND NOT THE PIPELINE'S ===========
 *
 * Because decals describe a WORLD. A scorch mark is on the floor of a building,
 * not in a viewpoint — two players looking at the same room see the same marks,
 * and a second pipeline at a different quality setting must not mean a second
 * set of them. That is the same argument DeviceProbeSet makes, and it is
 * rhi/MIGRATION.md §4.12's first open problem: every ownership question this
 * renderer has had was an object filed under the wrong one of DEVICE, WORLD and
 * VIEW. See RenderAssets.hpp, which names the three.
 *
 * ================ IT REFERENCES TEXTURES AND OWNS NONE =====================
 *
 * The maps are RenderAssets' — device lifetime, shared by every world, cached
 * by name. This holds handles to them, exactly as a renderable holds a handle
 * to a mesh it does not own, and under exactly the same rule: whoever built the
 * resource destroys it, after removing every reference. A decal material table
 * that owned its textures would load the same scorch mark once per world.
 *
 * ================= WHY NOT JUST USE THE EXISTING DecalSet ==================
 *
 * `cromwell/decal/DecalSet` is the raylib path's and holds `Texture2D` and
 * raylib `Matrix`. `ScenePipeline` may not name raylib at all — that is the
 * whole point of the port — so the data has to cross a boundary, and it crosses
 * it as this. The game converts at the seam, one function, the same bargain
 * `toVec3` makes for the sun.
 *
 * The two are deleted into one at parity (§4.13).
 *
 * ====================== WHAT A PROJECTOR ACTUALLY IS =======================
 *
 * A BOX, NOT A QUAD, and everything good about the technique follows from that.
 * The pass rasterises the box, unprojects the depth buffer inside it, and inks
 * whatever real surface it finds — so a decal lands on a kerb, a stair nose and
 * a rubble pile with no clipping, no per-receiver geometry and no z-fighting,
 * because no coplanar triangle is ever submitted.
 *
 * LOCAL SPACE, matching Decal.hpp so the two cannot drift:
 *   local +Z   out of the receiving surface; the box projects along -Z
 *   local  X   the decal's U, left to right
 *   local  Y   the decal's V, bottom to top
 * So `transform`'s three columns are the decal's tangent, bitangent and normal
 * scaled to its size, and its fourth is the centre of the box.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

/* Index into the material table below. */
using DeviceDecalMaterialId = int;
constexpr DeviceDecalMaterialId kInvalidDeviceDecalMaterial = -1;

class DeviceDecalSet {
public:
    /* ONE MATERIAL'S MAPS. Handles into RenderAssets; this owns none of them.
     *
     * THE ALBEDO IS THE ONLY ONE THAT CANNOT FALL BACK, because its ALPHA is
     * the decal's shape. A white stand-in would ink the whole projector box as
     * a solid rectangle over the world — which is why the pass skips a decal
     * whose albedo is invalid rather than substituting one. The other two fall
     * back to RenderAssets' 1x1 stand-ins and cost a bind. */
    struct Material {
        rhi::TextureHandle albedo;
        rhi::TextureHandle normal;
        rhi::TextureHandle packed;   /* metal R, rough G, emissive mask B */
    };

    /* ONE PROJECTOR. An aggregate filled by one caller and read by one callee,
     * with no invariant spanning its fields — the same one-shot carrier
     * SceneFrame and the rhi descriptors are, and for the same reason.
     *
     * Every default here is Decal.hpp's, deliberately: a projector converted
     * from one and a projector built here must mean the same thing, and two
     * sets of defaults is how they stop. */
    struct Projector {
        Mat4 transform;                 /* unit cube -> world */

        DeviceDecalMaterialId material = kInvalidDeviceDecalMaterial;

        /* WHO THIS DECAL IS, ACROSS FRAMES, and it exists for exactly one
         * consumer: the visibility capture.
         *
         * The pass renders the world from each decal's own position once, when
         * the decal is placed, and keeps it — so it needs to know that the
         * projector it is looking at this frame is the same one it captured
         * last frame. The set is REBUILT WHOLESALE every frame (see RhiDecals
         * on why), so an index into it is not that: place one decal at the
         * front of the list and every id behind it shifts, invalidating every
         * capture in the world for a change that moved nothing.
         *
         * So the id comes from whoever owns the authoritative list and must be
         * stable for the life of a decal. NEGATIVE MEANS TRANSIENT: no cached
         * capture, re-rendered every frame it is seen, which is what a dev
         * tool's preview wants because it moves with the cursor. */
        int id = -1;

        /* Multiplies the albedo map. */
        float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        /* Master coverage; everything the decal computes multiplies this. */
        float opacity = 1.0f;

        /* Multiplies the packed map's blue channel to give the emissive mask.
         * THE GLOW TAKES ITS COLOUR FROM THE ALBEDO, which is why one scalar
         * suffices — one channel instead of a fourth attachment. */
        float emissive = 0.0f;

        /* Factors on the packed map. METALNESS DEFAULTS TO ZERO, breaking
         * glTF's factor-times-white rule on purpose: most decals have an albedo
         * and no packed map, and every one of them would otherwise arrive fully
         * CONDUCTIVE — a printed label rendering as coloured chrome that
         * vanishes wherever there is nothing to reflect. */
        float roughness = 0.9f;
        float metalness = 0.0f;

        /* Scales the tangent-space normal's xy, applied in the RECEIVER's
         * frame — so a flat decal normal leaves the surface exactly as it was,
         * at any angle. */
        float normalStrength = 1.0f;

        /* WHETHER THE DECAL GOES ROUND CORNERS. Off is one fixed projection
         * axis and the decal stops dead at the edge of the surface it was
         * placed on, which is right for a poster or a road marking. On resolves
         * the projection per pixel and unwraps across the fold, which is right
         * for anything thrown at the world. See the fragment shader. */
        bool wrap = true;

        /* HOW SQUARELY A SURFACE MUST FACE THE PROJECTOR TO TAKE INK, as the
         * cosine to the projection axis. NOT POLISH: a box projection smears
         * its texture down every surface roughly parallel to the axis, because
         * those occupy almost no area in the projected UV, and no filter turns
         * a one-texel streak back into a decal. Rejecting them is the only
         * cure. */
        float angleFadeStart = 0.40f;
        float angleFadeEnd   = 0.70f;

        /* How much of the box's depth is spent fading at its near and far
         * faces. Without it a decal on a surface the box only just reaches ends
         * in a straight cut, which reads as a rectangle rather than a stain. */
        float depthFade = 0.15f;
    };

    DeviceDecalSet() = default;

    DeviceDecalSet(const DeviceDecalSet&) = delete;
    DeviceDecalSet& operator=(const DeviceDecalSet&) = delete;

    /* ---- the material table ---------------------------------------------
     *
     * Registered by the game, which is the only side that knows what a decal
     * material is called and where its maps live. Returns the id a projector
     * names, or the invalid one when the albedo is missing — see Material. */
    DeviceDecalMaterialId addMaterial(const Material& material);

    const Material& material(DeviceDecalMaterialId id) const;
    bool            hasMaterial(DeviceDecalMaterialId id) const;
    int             materialCount() const { return static_cast<int>(materials_.size()); }

    /* ---- the projectors --------------------------------------------------
     *
     * REPLACED WHOLESALE RATHER THAN ADDED TO ONE AT A TIME, and that is the
     * shape the game's side wants: the authoritative list is the game's
     * `DecalSet`, this is a converted mirror of it, and syncing a mirror by
     * diffing it against its source is more machinery than rebuilding a vector
     * of a few dozen POD structs. The overlays make the same call for the same
     * reason — see RhiOverlays on hashing rather than keying.
     *
     * The vector keeps its capacity across clears, so a steady state allocates
     * nothing. */
    void clear() { projectors_.clear(); }
    void add(const Projector& projector) { projectors_.push_back(projector); }

    const std::vector<Projector>& projectors() const { return projectors_; }
    bool empty() const { return projectors_.empty(); }

private:
    std::vector<Material>  materials_;
    std::vector<Projector> projectors_;
};

}  // namespace cromwell
