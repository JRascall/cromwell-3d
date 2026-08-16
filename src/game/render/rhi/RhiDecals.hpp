/* RhiDecals.hpp — this game's decals, mirrored into the device scene.
 *
 * SINGLE RESPONSIBILITY: convert the game's `cromwell::DecalSet` — which speaks
 * raylib — into the scene's `DeviceDecalSet`, and load each material's maps
 * through the asset layer once. It draws nothing and owns no texture.
 *
 * ================== WHY A MIRROR AND NOT A SECOND SOURCE ==================
 *
 * There is ONE authoritative list of decals in the process and it is the game's.
 * `PlayerController` places into it, the dev panel's brush previews against it,
 * `Application` clears it — every one of those is gameplay reaching for game
 * state, and none of them should have to know which renderer is drawing.
 *
 * So this is a converter, not an owner. A second authoritative set would be a
 * second answer to "where are the scorch marks", discovered later as the two
 * renderers disagreeing about a board — which is exactly the shape of the two
 * SunLights RhiFrameRenderer's header refuses.
 *
 * =================== WHY THE CONVERSION EXISTS AT ALL =====================
 *
 * `Decal` carries a raylib `Matrix` and `Color`, and `DecalSet` owns raylib
 * `Texture2D`s. `ScenePipeline` may not name raylib — that is the whole point
 * of the port — so the data has to cross a boundary, and it crosses it here.
 * Same bargain `toVec3` makes for the sun: one function at one seam, rather
 * than the type leaking into the engine's frame description.
 *
 * The two sets are deleted into one at parity (§4.13), at which point this file
 * goes with them.
 *
 * ======================= REBUILT WHOLESALE, AND WHY =======================
 *
 * Every frame, from scratch, rather than diffing. A board carries tens of
 * decals — the projectors are POD and the vector keeps its capacity, so the
 * rebuild is a memcpy-shaped loop over a few dozen structs. Diffing a mirror
 * against its source needs an identity for each entry, and `Decal` has none;
 * inventing one would be the "cache key made of the things that change it"
 * trap §5 records, where the key goes stale the first time a new cause appears.
 *
 * THE MATERIALS ARE NOT REBUILT, because they are textures. Registered once per
 * NAME on first sight and cached by the asset layer for the life of the device.
 */
#pragma once

#include "cromwell/decal/DeviceDecalSet.hpp"

#include <optional>
#include <vector>

namespace cromwell {
class DecalSet;
class RenderAssets;
class RenderScene;
struct Decal;
}  // namespace cromwell

namespace game {

class RhiDecals {
public:
    RhiDecals() = default;

    RhiDecals(const RhiDecals&) = delete;
    RhiDecals& operator=(const RhiDecals&) = delete;

    /* ONE FRAME'S WORTH, into the scene's decal set.
     *
     * `preview` is the dev tool's ghost and is appended LAST, so it composites
     * over every committed decal — which is what makes it a preview of what you
     * will get rather than of what you would have got. Null when the brush is
     * disarmed. It goes through the identical conversion for the same reason
     * the raylib path draws it through the identical pass: a preview that took
     * a different route would be predicting a different thing.
     *
     * `assets` is non-const because a material seen for the first time loads
     * its maps, which is a device operation. That happens on the frame a decal
     * material is first used and never again. */
    void sync(cromwell::RenderScene& scene, const cromwell::DecalSet& decals,
              cromwell::RenderAssets& assets, const cromwell::Decal* preview);

    /* How many decals reached the scene last sync, and how many were dropped
     * for want of an albedo. The second is what the dev panel reports: a decal
     * tool that places marks nobody can see is indistinguishable from a broken
     * pass, and this is the number that tells them apart. */
    int placedCount() const { return placedCount_; }
    int droppedCount() const { return droppedCount_; }

private:
    /* Registers a material's maps on the device set the first time its name is
     * seen, and hands back the DEVICE's id for it. */
    cromwell::DeviceDecalMaterialId materialFor(cromwell::DeviceDecalSet& set,
                                                cromwell::RenderAssets& assets,
                                                const cromwell::DecalSet& decals,
                                                int id);

    /* ---- THE GAME'S MATERIAL ID -> THE DEVICE'S, STORED RATHER THAN ASSUMED
     *
     * The first version of this relied on the two tables lining up, on the
     * reasoning that both are appended to in the same order from the same
     * source. THAT IS FALSE THE MOMENT ONE MATERIAL IS REFUSED, and one is
     * routinely refused: a material with no albedo file gets no device id —
     * DecalDemo builds three of its four procedurally, and a mirror keyed by
     * name cannot follow a texture that never had a file.
     *
     * With one refusal the device's indices shift by one against the game's, so
     * every material after it draws with its NEIGHBOUR'S texture. That is a
     * wrong picture rather than an error, and it would read as an authoring
     * mistake in whichever mark happened to look wrong.
     *
     * ---- AND WHY IT IS AN OPTIONAL RATHER THAN A SENTINEL ----------------
     *
     * Three states, not two: NEVER ASKED, asked and answered, asked and
     * REFUSED. The first version used `kInvalidDeviceDecalMaterial` for both of
     * the last two and filled gaps with it — which quietly made "not asked yet"
     * mean "refused".
     *
     * That is not a theoretical distinction. Decals are converted in DRAW
     * ORDER, which is sorted by `sortOrder` and not by material id, so the
     * first decal seen routinely names material 1 or 2. Filling ids 0..N-1 with
     * the refusal value marked the material at 0 as refused BEFORE ANYTHING
     * TRIED TO LOAD IT — and material 0 is `example`, the only authored one and
     * the dev tool's default. Every decal on the board vanished, with no
     * warning for the one that mattered, because refusals are cached and it was
     * never asked.
     *
     * An empty optional cannot be confused with a refusal, so a gap is filled
     * and still answered properly the moment something asks. */
    std::vector<std::optional<cromwell::DeviceDecalMaterialId>> deviceIds_;

    int placedCount_ = 0;
    int droppedCount_ = 0;
};

}  // namespace game
