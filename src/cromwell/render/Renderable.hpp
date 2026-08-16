/* Renderable.hpp — one thing in a world that can be drawn.
 *
 * SINGLE RESPONSIBILITY: describe a drawable — its geometry, where it is, what
 * it is made of, and which views are allowed to see it — as DATA, with no way
 * to express how or when it is drawn.
 *
 * =========================================================================
 *                        PUBLIC API. READ THIS FIRST.
 * =========================================================================
 *
 * This header, View.hpp, RenderScene.hpp and IScenePass.hpp are the FOUR files
 * a game is written against. Everything else under cromwell/render is internal
 * and will not be published — see rhi/MIGRATION.md §4.12 on licensing, which is
 * the reason the line is being drawn now rather than after somebody has built
 * against the wrong side of it.
 *
 * The practical consequence, and it governs every choice below: **once a studio
 * ships against this, it cannot change.** So the fields are private and reached
 * through accessors, not because a descriptor has an invariant worth defending
 * — it mostly does not — but because a public data member is a layout contract
 * forever, and an accessor is not. Adding a field to a struct with public
 * members changes its ABI; adding one behind accessors does not change any
 * caller's code.
 *
 * ====================== WHAT THE GAME REGISTERS, AND WHY DATA =============
 *
 * A mesh, a material, a transform, a tint, bounds, and some flags about which
 * views may see it. That is all. The game hands this over and never sees an
 * encoder, a pass or a pipeline again.
 *
 * SOURCE DOES THE OPPOSITE AND IT IS WORTH KNOWING WHY WE DO NOT. Valve's
 * client leaf system registers an INTERFACE — `AddRenderable(IClientRenderable*,
 * RenderGroup_t)` — and calls `DrawModel(flags)` back on it once the engine has
 * decided what is visible. That buys enormous freedom: a rope, a beam, a sprite
 * and a portal are all renderables, and the engine never learns what a rope is.
 *
 * We register data instead, and the cost is real and should be stated rather
 * than discovered. Anything that cannot be expressed as mesh-plus-material-plus-
 * transform needs the ENGINE to grow a component for it — ropes, beams, trails,
 * particles — where Source lets the game invent one.
 *
 * The reason is the whole point of this port: a `DrawModel` callback puts game
 * code inside a render pass holding a command encoder, which is precisely what
 * IGeometrySource already was and what this replaces. Source could afford it
 * because it has one backend and the game and the renderer are one binary.
 * cromwell has to reach Vulkan, Metal and two console APIs, where "the game
 * issues draws" means the game knows what a command buffer is on four targets.
 *
 * It is the same trade Unity's SRP and Godot's RenderingServer make, and the
 * same reason both ship a fixed vocabulary of renderer components. It is the
 * right trade for a multi-backend engine and the wrong one for a single-backend
 * game. See study/topics/rendering/render_scene_architecture.md §1 and §6.
 *
 * ============ THE TWO FILTERS, AND WHY THEY HAVE OPPOSITE SENSES ==========
 *
 * They look inconsistent until you see what each one's DEFAULT has to be, and
 * then only one arrangement works. Read this before using either.
 *
 * `filterFlags` — WHAT THIS RENDERABLE IS, in the game's own vocabulary. A view
 *   carries `hiddenFlags`, and a renderable is skipped when the two intersect.
 *   The default is ZERO: a renderable that claims nothing is hidden by nothing,
 *   so it draws in every view. **The engine never learns what a bit means.**
 *
 *   THE SENSE IS "HIDE IF ANY MATCH" AND THAT IS NOT A PREFERENCE. The obvious
 *   design — the renderable's key AND the view's mask, draw where non-zero — is
 *   what §4.12 originally proposed, and it is WRONG for more than one axis. This
 *   game's cutaway has two: a storey and a wall facing, and a surface must pass
 *   BOTH. Under show-if-any-match a wall on a hidden storey whose facing is
 *   shown still ANDs to non-zero and draws, so the storey cut leaks the moment
 *   the facing cut is used. Under hide-if-any-match the two axes compose
 *   correctly, and so does a third and a fourth, in one AND per renderable.
 *
 *   The defaults fall out right as a bonus. A view that sets no hidden flags
 *   sees everything, so the SUN'S VIEW AND A PROBE CAPTURE ARE CORRECT BY
 *   CONSTRUCTION rather than by a caller remembering to pass "the whole world".
 *   That exact bug has already been paid for once here — see CutawayView.hpp,
 *   where letting the camera's cut reach the shadow pass made the lighting
 *   change when the player changed floor.
 *
 * `viewers` — WHICH VIEWERS MAY SEE THIS, one bit per viewer, ANDed against the
 *   view's `viewerMask` and drawn where non-zero. Default: all of them.
 *
 *   THE OPPOSITE SENSE, because ownership is naturally a positive statement.
 *   Two players in ONE shared world each have their own selection, so each has
 *   their own visibility overlay and cover markers; those renderables sit in the
 *   one scene and must be visible to exactly one view. Expressing that as "show
 *   to viewer 2" is one bit set. Expressing it in the hide sense would be
 *   "hidden from every viewer except 2" — every bit but one, restated whenever
 *   the player count changes, and wrong by default. It is a SEPARATE FIELD
 *   rather than a slice of `filterFlags` for the same reason: §4.12's bit budget
 *   spent all 32 on storeys and facings and left nothing for this, and a field
 *   the game cannot accidentally spend is the only way to reserve it.
 *
 * ================== WHAT IS ENGINE VOCABULARY AND WHAT IS NOT ==============
 *
 * `castsShadow` and `visibleInReflections` are the engine's, not the game's,
 * and they are deliberately NOT filter bits. A game should not have to know
 * that a shadow pass wants casters only, or that a probe capture wants the
 * whole world — that knowledge leaking into every project is one of the four
 * things §4.12 lists as the cost of the interface this replaces. They are the
 * same two knobs Unreal ships (`bCastShadow`, `bVisibleInReflectionCaptures`),
 * and they are named after what they mean rather than after the pass that reads
 * them.
 */
#pragma once

#include "cromwell/collision/Shape.hpp"
#include "cromwell/material/MaterialId.hpp"
#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec4.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>

namespace cromwell {

/* ONE BIT PER VIEWER. Thirty-two is far past any split-screen anybody ships —
 * four is the requirement, eight is generous — and the width is stated here so
 * a game can static_assert its own player count against it rather than
 * discovering the ceiling as a shift that quietly did nothing. */
using ViewerMask = std::uint32_t;

inline constexpr int kViewerCount = 32;

/* Every viewer, which is what a renderable that belongs to the WORLD rather
 * than to a player wants: the terrain, the buildings, the units. */
inline constexpr ViewerMask kAllViewers = ~ViewerMask{ 0 };

/* THE BIT FOR ONE VIEWER. A helper rather than `1u << i` at the call site,
 * because `1u << 32` is undefined behaviour and reads as perfectly ordinary
 * code — this is the same class of silent limit §4.12 flags for the storey
 * bits, and the answer is the same: make the budget checkable. */
inline constexpr ViewerMask viewerBit(int viewer)
{
    return (viewer >= 0 && viewer < kViewerCount) ? (ViewerMask{ 1 } << viewer)
                                                  : ViewerMask{ 0 };
}

/* HOW MANY BITS A GAME HAS TO SPEND ON ITS OWN CATEGORIES. Published so that a
 * game can write
 *
 *     static_assert(kMaxStoreys + kSurfaceFacingCount <= cromwell::kFilterFlagBits);
 *
 * and get a BUILD ERROR when a taller map would push a storey bit off the end
 * of the field. `1u << storey` past the width is undefined and silently
 * misbehaves — see rhi/MIGRATION.md §4.12, which lists this as one of the five
 * problems the design review found and which this constant exists to close. */
inline constexpr int kFilterFlagBits = 32;

using FilterFlags = std::uint32_t;

/* CLAIMS NOTHING, SO NOTHING HIDES IT. The default, and the value a renderable
 * that does not participate in any cutaway should keep. */
inline constexpr FilterFlags kNoFilterFlags = 0;

/* WHICH REGISTRATION THIS IS. Handed back by RenderScene::add and required by
 * every later call about the same renderable.
 *
 * TWO SEPARATE FIELDS, NOT A PACKED INTEGER, and the reason is a trap this
 * project has already paid for: the framebuffer cache key packed a layer number
 * into three bits, which was injective right up until the probe array arrived
 * with ninety-six slices, and then a pass rendered into somebody else's target.
 * See rhi/MIGRATION.md §5. Packing an index and a generation into one word has
 * exactly that shape — it works until one field outgrows its allowance, and
 * then two unrelated renderables become the same id. Eight bytes is not memory
 * worth economising for the privilege of being wrong later.
 *
 * WHY A GENERATION AT ALL. Slots are recycled: remove a renderable, add
 * another, and the new one lands in the freed slot. A caller holding the old id
 * would then move, hide or re-mesh a completely different object — silently,
 * because the index is perfectly valid. The generation makes a stale id
 * detectable, and the scene ignores one rather than corrupting a live
 * renderable. This is the same lesson as the glyph cache keyed by an atlas
 * ADDRESS, also in §5: borrowing an identity means borrowing its invalidation,
 * and the owner has to be asked for it explicitly. */
struct RenderableId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    /* GENERATION ZERO IS NEVER ISSUED — the scene starts every slot at one — so
     * a default-constructed id is invalid the way a default-constructed handle
     * is, and `if (id)` reads correctly in a struct nobody remembered to fill. */
    constexpr bool valid() const { return generation != 0; }
    constexpr explicit operator bool() const { return valid(); }

    friend constexpr bool operator==(RenderableId a, RenderableId b)
    {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(RenderableId a, RenderableId b) { return !(a == b); }
};

/* =========================================================================
 *  THE DESCRIPTION, FILLED AT A CALL SITE AND HANDED TO RenderScene::add
 * =========================================================================
 *
 * CHAINED, per CLAUDE.md: it has more than three optional knobs and is
 * configured immediately before use, which is exactly the case that rule names.
 *
 *     const auto id = scene.add(RenderableDesc()
 *         .withMesh(chunk.mesh, chunk.localBounds)
 *         .withMaterial(materials.idOf(SurfaceKind::Wall))
 *         .withTransform(Mat4::identity())
 *         .withFilterFlags(storeyBit(3) | facingBit(SurfaceFacing::North)));
 *
 * It is NOT one of the aggregates CLAUDE.md exempts — HttpRequest, FrameView,
 * the UI specs — and the difference is worth stating because the exemption
 * looks like it should apply. Those are one-shot carriers read by one callee in
 * the same breath. This one is COPIED INTO A SCENE AND OUTLIVES ITS CALLER by
 * the lifetime of the world, it is published to licensees, and two of its
 * fields have a relationship the type should enforce — see withMesh. */
class RenderableDesc {
public:
    /* ---- geometry -------------------------------------------------------
     *
     * THE MESH AND ITS LOCAL BOUNDS TOGETHER, IN ONE CALL, and that is the one
     * place this class insists on something.
     *
     * They are one fact. Bounds that do not enclose the mesh are not a slightly
     * wrong number; they are a renderable that vanishes when the camera turns,
     * or one that is never culled and costs a draw from everywhere. Two setters
     * would make "set the mesh and forget the bounds" expressible, and the
     * result of that mistake is a hole in the world that appears only at
     * certain angles — the hardest kind of rendering bug to attribute, because
     * nothing is wrong with the thing that disappeared.
     *
     * LOCAL, not world. The scene transforms them on add and on every
     * setTransform, so a mesh instanced at fifty places carries one box. */
    RenderableDesc& withMesh(rhi::MeshHandle mesh, const Aabb& localBounds)
    {
        mesh_ = mesh;
        localBounds_ = localBounds;
        return *this;
    }

    RenderableDesc& withMaterial(MaterialId material)
    {
        material_ = material;
        return *this;
    }

    /* WORLD SPACE, and there is no parent. §4.12 rules out a scene graph
     * deliberately: parents and local transforms are an animation and
     * attachment problem, and nothing in this engine has one yet. When
     * skeletal attachment arrives it composes ABOVE this — whatever owns the
     * skeleton writes a world transform per frame — rather than turning the
     * flat array into a tree the culler has to walk. */
    RenderableDesc& withTransform(const Mat4& transform)
    {
        transform_ = transform;
        return *this;
    }

    /* MULTIPLIES the mesh's own vertex colours. White leaves them alone, which
     * is what the static world wants; a unit's body is a white cube and takes
     * its whole colour from here. Alpha rides along for a material that reads
     * it — it is not a blend mode, which is the material's to say. */
    RenderableDesc& withTint(Vec4 tint)
    {
        tint_ = tint;
        return *this;
    }

    /* ---- who may see it — read the header on the two senses ------------- */

    RenderableDesc& withFilterFlags(FilterFlags flags)
    {
        filterFlags_ = flags;
        return *this;
    }

    RenderableDesc& withViewers(ViewerMask viewers)
    {
        viewers_ = viewers;
        return *this;
    }

    /* ---- engine vocabulary ---------------------------------------------
     *
     * WHAT THE SUN'S DEPTH PASS TAKES. False for anything that transmits light
     * rather than blocking it — glass belongs in the transmission plane, not in
     * the depth one — and for anything that is not physically there at all: a
     * gameplay marker lying flush on the floor does not intercept light, and a
     * selection overlay certainly does not.
     *
     * THE GAME SAYS "THIS DOES NOT CAST", NOT "SKIP ME IN THE SHADOW PASS",
     * and the difference is the whole point. The second phrasing requires every
     * project to know what a shadow pass is; the first is a property of the
     * object that stays true if the engine grows cascades, ray-traced shadows
     * or none at all. */
    RenderableDesc& withCastsShadow(bool casts)
    {
        castsShadow_ = casts;
        return *this;
    }

    /* WHETHER A REFLECTION PROBE CAPTURES IT. Default yes, because a room's
     * cubemap should contain the room.
     *
     * OFF IS FOR THINGS THAT ARE NOT PART OF THE WORLD: a selection ring, a
     * path preview, a hover plate. Those belong to one player's interface, and
     * a probe is captured once and shared by every view — so an overlay that
     * got into one would appear, mirrored and stale, in every player's
     * reflections including the players who do not own it. That is the same
     * class of mistake as the cutaway reaching the capture, which §4.12 and
     * CutawayView.hpp both describe at length: the capture is a statement about
     * the WORLD, and anything view-owned that reaches it is wrong in a place
     * nobody looks first. */
    RenderableDesc& withVisibleInReflections(bool visible)
    {
        visibleInReflections_ = visible;
        return *this;
    }

    /* ---- WHAT THIS OBJECT IS, FOR EFFECTS THAT NEED TO SINGLE IT OUT -----
     *
     * An integer 0-255. Zero — the default — means the custom depth pass does
     * not draw it; anything else is written into that buffer along with the
     * object's depth, for a later pass to find.
     *
     * A PORT OF UNREAL'S bRenderCustomDepth AND CustomDepthStencilValue, and
     * deliberately as general as theirs: selection outlines, x-ray silhouettes,
     * masking a post effect to particular actors, depth-of-field and blur masks
     * are all the same buffer with different consumers. That generality is the
     * reason to copy it rather than build a "selection outline" feature.
     *
     * AN INTEGER PER OBJECT, NOT A CATEGORY PER CHANNEL. Four channels holding
     * four fixed categories is the obvious design and it is the rigid one: two
     * soldiers cannot be told apart, and every new kind of view wants a new
     * channel. An arbitrary value subsumes it — a consumer asks for the value it
     * cares about, "all units" is a range test, and nothing in the renderer has
     * to agree in advance about what the categories mean. The engine never
     * learns what a value IS, exactly as it never learns what a filter bit is.
     *
     * ASKED OF THE RENDERABLE, NOT OF THE PASS, like castsShadow above. */
    RenderableDesc& withCustomStencil(std::uint8_t value)
    {
        customStencil_ = value;
        return *this;
    }

    /* SWITCHED OFF WITHOUT BEING REMOVED, for something that comes and goes on
     * a timer or a selection. Cheaper than a remove-and-re-add, which recycles
     * a slot and invalidates the id its owner is holding. */
    RenderableDesc& withVisible(bool visible)
    {
        visible_ = visible;
        return *this;
    }

    /* ---- what the scene reads ------------------------------------------ */
    rhi::MeshHandle mesh() const { return mesh_; }
    const Aabb&     localBounds() const { return localBounds_; }
    MaterialId      material() const { return material_; }
    const Mat4&     transform() const { return transform_; }
    Vec4            tint() const { return tint_; }
    FilterFlags     filterFlags() const { return filterFlags_; }
    ViewerMask      viewers() const { return viewers_; }
    bool            castsShadow() const { return castsShadow_; }
    bool            visibleInReflections() const { return visibleInReflections_; }
    std::uint8_t    customStencil() const { return customStencil_; }
    bool            visible() const { return visible_; }

private:
    rhi::MeshHandle mesh_;
    Aabb            localBounds_{ Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 0.0f } };
    MaterialId      material_;
    Mat4            transform_;
    Vec4            tint_ = Vec4::one();

    FilterFlags filterFlags_ = kNoFilterFlags;
    ViewerMask  viewers_ = kAllViewers;

    bool castsShadow_ = true;
    bool visibleInReflections_ = true;

    /* ZERO MEANS "NOT IN THE CUSTOM DEPTH PASS", which is why it is the default
     * — the overwhelming majority of a world is not tagged, and a buffer that
     * collected everything would be the depth prepass again. See the setter. */
    std::uint8_t customStencil_ = 0;
    bool visible_ = true;
};

}  // namespace cromwell
