/* RenderScene.hpp — everything in one world that can be drawn, and the engine's
 * ownership of the list.
 *
 * SINGLE RESPONSIBILITY: hold the renderables a world contains, keep their
 * world-space bounds current, and answer "what should THIS view draw, in what
 * order". It opens no pass, binds nothing and issues no draw.
 *
 * =========================================================================
 *                        PUBLIC API. See Renderable.hpp.
 * =========================================================================
 *
 * ============================ WHY THIS EXISTS ============================
 *
 * It replaces `IGeometrySource`, where the engine called back and the GAME
 * issued the draws — deciding per pass whether it was the whole lattice or the
 * player's cutaway, casters-only, opaque or translucent. That seam was always a
 * migration scaffold and never met the bar this engine is for. What it cost, in
 * the order the costs actually bite:
 *
 *   - THE ENGINE COULD NOT CULL. It did not know what existed, so frustum
 *     culling had to be the game's job, in every game.
 *   - THE ENGINE COULD NOT SORT. The transparent pass drew in bucket order
 *     rather than back to front, so two overlapping panes blended wrongly — and
 *     nothing in the architecture could fix it, because the engine did not own
 *     the draws.
 *   - THE ENGINE COULD NOT BATCH OR INSTANCE draws it does not issue.
 *   - PASS SEMANTICS LEAKED INTO EVERY GAME. A second project had to implement
 *     `submit()` AND know that `GeometryPass::Shadow` means casters-only over
 *     the whole world rather than the cutaway.
 *
 * Unreal, Unity and Godot all do the opposite: the game registers renderables
 * and the engine owns the list, culls it, sorts it, batches it and draws it.
 * This is that. See rhi/MIGRATION.md §4.12 for the whole argument.
 *
 * ===================== ONE PER WORLD. NEVER A SINGLETON. ==================
 *
 * This is the first decision and it is the one Source gets wrong for us. Its
 * client leaf system is `EXPOSE_SINGLE_INTERFACE_GLOBALVAR` — one instance, one
 * world — and split-screen renders several VIEWS of that single world. Four
 * players in four DIFFERENT worlds cannot be expressed at all.
 *
 * cromwell has to carry both cases: single player is one world, one scene, one
 * view; four-player co-op in different worlds is four scenes, four views and
 * potentially four pipelines at different quality settings. So this is an
 * ordinary object a world owns, and nothing anywhere is global.
 *
 * ============ WHAT THIS OWNS, AND THE MESH LIFETIME RULE =================
 *
 * **A SCENE REFERENCES DEVICE RESOURCES. IT NEVER OWNS OR DESTROYS ONE.**
 *
 * That sentence is the rule, and it needs stating because the failure it
 * prevents is a use-after-free across two worlds that never mention each other.
 * Two worlds sharing the unit cube means tearing one down would dangle the
 * other's renderables — and the symptom is a crash, or worse a wrong mesh, in
 * the world that did not change.
 *
 *   - **Shared geometry belongs to the ASSET LAYER**, whose lifetime spans
 *     every world. See RenderAssets: it is device-scoped on purpose, and it is
 *     where a mesh two worlds both draw has to live.
 *   - **A world owns only what it BUILT from its own data** — its chunked
 *     terrain, its own baked meshes — and destroys them only after removing
 *     every renderable naming them from this scene.
 *   - **Destroying this scene destroys no GPU resource.** It drops references.
 *     Whoever created a mesh destroys it.
 *
 * WHAT THIS SCENE DOES OWN is the reflection probes, and that is a correction
 * to where they used to live. `DeviceProbeSet` sat on `ScenePipeline`, which is
 * a VIEWPOINT — but a probe set describes a WORLD: where the rooms are and what
 * they reflect. Two players in one world would otherwise each capture and
 * prefilter the same probes, wasting the work and letting the two copies drift.
 * They move here, one set per world, captured once and read by every view of it.
 *
 * ================ WHAT WAS READ OUT OF SOURCE AND NOT COPIED ==============
 *
 * study/topics/rendering/render_scene_architecture.md is Valve's client leaf
 * system read from the shipped SDK, and two of its findings are recorded here
 * as deliberate NON-adoptions. Both look like omissions otherwise.
 *
 * **THE PER-VIEW TRANSLUCENCY CACHE — not needed, and the reason is structural.**
 * `RenderableInfo_t::m_TranslucencyCalculatedView` records which view a cached
 * sort answer belongs to, and split-screen is exactly why it exists: two panes
 * see the same pane of glass at two different depths, so a single cached
 * distance on the renderable is correct until the day a second view exists.
 * We have no such cache to invalidate, because the distance is computed during
 * collection and lives in the per-view SceneDrawList — a subtract and a dot
 * product, cheaper than the lookup that would avoid it. **Source needs the cache
 * because it asks the OBJECT (`GetRenderOrigin` is a virtual call into game
 * code); we hold the centre as data.** That is the departure in §4.12 paying for
 * itself somewhere it was not aimed. The bug the field guards against is
 * unreachable here rather than fixed.
 *
 * **THE FRAME STAMP — deliberately absent, and here is when to add it.**
 * Source carries `m_RenderFrame`/`m_RenderFrame2` because a renderable
 * reachable through several visible BSP leaves would otherwise be gathered once
 * per leaf. Collection here walks a flat array exactly once per view, so
 * nothing can be gathered twice and a stamp would be a store that no path can
 * read. It becomes necessary the moment collection goes through a SPATIAL INDEX
 * and a chunk can span several visible cells — which is a real prospect, since
 * chunking the statics is what made culling possible in the first place.
 *
 * WHEN IT ARRIVES IT BELONGS ON THE COLLECTOR, NOT ON THE RENDERABLE, and
 * Source's own shape says why: it has TWO int fields because it supports two
 * split-screen views, which is a player count hardcoded into a data structure.
 * The general form is a stamp per (view, frame), which means an array beside
 * the collection rather than a field beside the mesh. Writing that down is the
 * whole reason the field is not here: adding it later is a small change to one
 * function, and un-hardcoding it after copying Source's two-field shape would
 * not be.
 *
 * ========================== IT FAILS LOUD, ON PURPOSE =====================
 *
 * Source's default render group is `RENDER_GROUP_OTHER` — "unclassified, won't
 * get drawn". A renderable that never says what it is silently vanishes, which
 * is the wrong failure: nothing is on screen and nothing is wrong anywhere.
 * Ours does the opposite. Every default draws, and anything that cannot
 * possibly be seen — no mesh, no material, empty bounds, a viewer mask no view
 * carries — is complained about once, by count, at the first collection.
 */
#pragma once

#include "cromwell/decal/DeviceDecalSet.hpp"
#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/render/Renderable.hpp"
#include "cromwell/render/SceneDrawList.hpp"
#include "cromwell/render/View.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

class IMaterialQuery;
namespace rhi { class IRenderDevice; }

class RenderScene {
public:
    /* THE DEVICE, for the probe set this scene owns — nothing else here touches
     * one. The MATERIAL QUERY is borrowed and must outlive the scene: it is the
     * asset layer's, shared by every world, and asking it what a material is
     * happens when a renderable is REGISTERED rather than when it is drawn.
     *
     * See RenderAssets, which is what a caller normally has both of. */
    RenderScene(rhi::IRenderDevice& device, const IMaterialQuery& materials);
    ~RenderScene();

    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;

    /* BRINGS UP WHAT NEEDS THE DEVICE, which today is the reflection probe
     * array. Renderables can be registered without it; only the probes cannot.
     *
     * FALSE IS NOT FATAL AND THAT IS DELIBERATE. A device with no cubemap
     * arrays — macOS's capped GL, a future software backend — draws every
     * surface against the analytic sky, which is a flatter frame and not a
     * broken one. It is the same fallback a world with no probes placed on it
     * already takes, so there is exactly one code path for both. Callers should
     * log and carry on rather than refuse to draw. */
    bool initialise();

    /* ---- what is in the world -------------------------------------------
     *
     * Returns an id that stays valid until `remove` or `clear`. An invalid id
     * comes back only when the description could never draw anything at all —
     * see the "fails loud" note above; the reason is logged. */
    RenderableId add(const RenderableDesc& desc);

    void remove(RenderableId id);

    /* EVERY RENDERABLE, GONE. Destroys no GPU resource — see the lifetime rule.
     * Ids issued before this are invalidated, which is the point: a world
     * rebuild that left stale ids in the game's own tables would move the wrong
     * things. */
    void clear();

    /* ---- what changed about one of them ---------------------------------
     *
     * All of these ignore a stale or unknown id rather than asserting. A
     * renderable removed on a frame boundary while something else still holds
     * its id is ordinary — a unit dies and its overlay is updated one call
     * later — and the generation in the id is precisely what makes ignoring it
     * safe rather than silently addressing whatever took the slot. */
    void setTransform(RenderableId id, const Mat4& transform);
    void setVisible(RenderableId id, bool visible);
    void setFilterFlags(RenderableId id, FilterFlags flags);
    void setViewers(RenderableId id, ViewerMask viewers);
    void setTint(RenderableId id, Vec4 tint);
    void setMaterial(RenderableId id, MaterialId material);

    /* WHAT THIS OBJECT IS, for the custom depth pass — see
     * RenderableDesc::withCustomStencil. A setter as well as a descriptor field
     * because selection changes every time a player clicks, and re-adding a
     * renderable to change one byte would recycle its slot and invalidate the
     * id its owner is holding. */
    void setCustomStencil(RenderableId id, std::uint8_t value);

    /* NEW GEOMETRY UNDER THE SAME ID, AND IT EARNS ITS PLACE. §4.12 left this
     * open — "either setMesh exists or an overlay rebuild is remove-and-re-add"
     * — and the overlays settle it: a visibility field or a path preview is ONE
     * renderable whose mesh is rebuilt whenever the field changes, which is the
     * normal path rather than an edge case. Remove-and-re-add would recycle the
     * slot, invalidate the id its owner is holding, and turn a mesh swap into
     * bookkeeping at every call site that does it.
     *
     * THE BOUNDS COME WITH IT, for the reason RenderableDesc::withMesh states:
     * new geometry with the old extent is a renderable that culls against a box
     * it no longer fills. */
    void setMesh(RenderableId id, rhi::MeshHandle mesh, const Aabb& localBounds);

    /* ---- what the engine asks ------------------------------------------- */

    /* WHAT THIS VIEW SHOULD DRAW, culled and sorted, into a list the caller
     * owns. Clears the list first. See SceneDrawList on why the storage is the
     * caller's. */
    void collect(const View& view, SceneDrawList& out) const;

    /* THE EXTENT OF EVERYTHING REGISTERED, so the engine can frame the sun's
     * orthographic box without knowing what a lattice is.
     *
     * IT IS THE UNION OF THE RENDERABLES rather than a number the game states,
     * and that is a small improvement over what `IGeometrySource::worldBounds`
     * did: the game's answer was the lattice plus a margin, which includes the
     * empty air above an untouched map, and the sun's fit spends resolution on
     * every unit of it. This box is what is actually there.
     *
     * An empty scene reports an empty box, and the shadow pass skips rather
     * than dividing by zero on the way to a NaN matrix. */
    const Aabb& worldBounds() const;

    /* ---- the reflection probes, which describe this WORLD ---------------
     *
     * Non-const because WHICH volumes exist is the game's answer — a flooded
     * cell partition here, a portal graph or hand-placed volumes elsewhere —
     * and the engine never learns why. The array, the schedule and the capture
     * pass stay the engine's; see DeviceProbeSet and game/render/scene/ProbePlacement.hpp.
     *
     * HERE RATHER THAN ON ScenePipeline, which is the fix described in the
     * ownership note at the top of this header. */
    DeviceProbeSet&       probes() { return probes_; }
    const DeviceProbeSet& probes() const { return probes_; }

    /* ---- and the decals, which describe this WORLD for the same reason ----
     *
     * A scorch mark is on the floor of a building, not in a viewpoint. Two
     * players in one room see the same marks and a second pipeline at a
     * different quality setting must not mean a second set of them — the
     * identical argument the probes make one accessor up, and the reason both
     * are here rather than on ScenePipeline.
     *
     * Non-const because WHICH decals exist is the game's answer: it places
     * them, and the engine draws whatever it finds without learning what a
     * scorch is. See DeviceDecalSet. */
    DeviceDecalSet&       decals() { return decals_; }
    const DeviceDecalSet& decals() const { return decals_; }

    /* ---- diagnostics ----------------------------------------------------- */
    int renderableCount() const { return liveCount_; }

private:
    /* WHAT THE ENGINE KEEPS PER RENDERABLE. Private and never published: it is
     * the descriptor plus the bookkeeping the game must not see, which is
     * exactly the split Source draws around `RenderableInfo_t`. */
    struct Record {
        Mat4 transform;
        Aabb localBounds{ Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 0.0f } };

        /* THE LOCAL BOX PUT THROUGH THE TRANSFORM, kept rather than recomputed.
         * Culling reads it once per view per frame and it changes only when the
         * transform or the mesh does — which for the static world is never and
         * for a unit is once a frame. Recomputing it inside the cull loop would
         * be eight matrix-vector products per renderable per view to answer a
         * question whose input did not change. */
        Aabb worldBounds{ Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 0.0f } };

        Vec3 worldCentre;

        Vec4            tint = Vec4::one();
        rhi::MeshHandle mesh;
        MaterialId      material;

        FilterFlags filterFlags = kNoFilterFlags;
        ViewerMask  viewers = kAllViewers;

        /* WHICH GENERATION THIS SLOT IS ON. Bumped when the slot is freed, so
         * an id issued before that stops matching. Starts at one because
         * generation zero is the invalid id — see RenderableId. */
        std::uint32_t generation = 1;

        /* HOW BIG IT IS, AS A SORT BUCKET. Source's render groups begin
         * `OPAQUE_STATIC_HUGE` and `OPAQUE_ENTITY_HUGE` so the biggest things
         * draw first, occlude, and let the depth test reject more of what
         * follows — a free early-z win that costs one comparison in a sort that
         * was happening anyway. It is not something this design would have
         * thought of; see the study note §3.
         *
         * Zero is the biggest, so ascending sort order draws big things first.
         * RenderScene.cpp derives it. */
        std::uint8_t sizeRank = 0;

        /* ASKED OF THE MATERIAL ONCE, WHEN IT IS SET, rather than per view per
         * frame. It decides which of the two buckets the renderable lands in,
         * and the answer changes only when someone edits a material — which is
         * a startup or a dev-panel event, not a frame one. See
         * IMaterialQuery.hpp on why the scene asks for this one bit and nothing
         * else. */
        bool translucent = false;

        bool castsShadow = true;

        /* 0 means "not in the custom depth pass" — see RenderableDesc. */
        std::uint8_t customStencil = 0;
        bool visibleInReflections = true;
        bool visible = true;

        /* WHETHER THIS SLOT HOLDS ANYTHING. A freed slot stays in the array and
         * goes on the free list — indices are stable and ids stay meaningful,
         * which a compacting array cannot offer. */
        bool alive = false;
    };

    /* THE WORLD BOX, RECOMPUTED WHEN SOMETHING MOVED. A union has no cheap
     * incremental update: growing is trivial and SHRINKING is not, because
     * removing the renderable that defined an extreme means finding the next
     * one. So it is marked dirty and rebuilt at most once per frame, on the
     * first ask — a walk over a few hundred boxes, in the pass setup rather
     * than in a per-pixel or per-cell loop. */
    void rebuildWorldBounds() const;

    /* Whether a record could be seen by this view AT ALL, before the frustum is
     * consulted. Everything here is an integer test; the box arithmetic comes
     * after, because that is the expensive half — see CLAUDE.md on culling
     * cheaply before testing expensively. */
    static bool relevantTo(const Record& record, const View& view);

    Record*       find(RenderableId id);
    const Record* find(RenderableId id) const;

    /* Fills in everything derived from the mesh and the transform: the world
     * box, its centre and the size rank. One function because getting a subset
     * of them updated is the bug — a renderable whose transform moved but whose
     * cached centre did not sorts against where it used to be. */
    static void refreshDerived(Record& record);

    const IMaterialQuery& materials_;

    std::vector<Record>        records_;
    std::vector<std::uint32_t> freeSlots_;
    int                        liveCount_ = 0;

    /* Mutable because worldBounds() is const and lazily rebuilds. The
     * alternative is a non-const accessor on a scene the pipeline holds by
     * const reference, which would make every pass able to edit the world it is
     * drawing. */
    mutable Aabb worldBounds_{ Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 0.0f } };
    mutable bool boundsDirty_ = true;

    /* SAID ONCE PER SCENE, not once per frame. A renderable that cannot be seen
     * is a registration mistake, and a registration mistake repeated at sixty
     * hertz is a log nobody reads. */
    mutable bool reportedUndrawable_ = false;

    DeviceProbeSet probes_;
    DeviceDecalSet decals_;
};

}  // namespace cromwell
