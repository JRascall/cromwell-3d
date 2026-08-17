/* DeviceProbeSet.hpp — one reflection probe per room, in a cubemap array, on the RHI.
 *
 * SINGLE RESPONSIBILITY: own the array texture, the per-probe volumes, the
 * uniform block the shader reads them from, and the round-robin schedule that
 * keeps the faces current. WHICH rooms exist is the game's business; WHAT a
 * probe means to a surface is the shader's; WHO renders a face is
 * ScenePipeline's — this hands out the matrices and the slice to draw into.
 *
 * ===================== THE RAYLIB SET, WITHOUT THE RAW GL ==================
 *
 * ReflectionProbeSet.hpp is this same design against rlgl, and its header
 * carries the argument for the design at length — read it first. Everything it
 * says about two volumes per probe, per-pixel selection and parallax
 * correction is equally true here and is not repeated.
 *
 * WHAT IS DIFFERENT IS THE MACHINERY UNDERNEATH. That file is the one place in
 * the engine that calls GL directly, because rlgl has no cubemap-array anything
 * — no creation, no per-layer framebuffer attach. The RHI has both:
 * `TextureDesc::cube` with `6 * probes` layers, and `ColourAttachment::layer`
 * naming the slice a pass renders into. So this file names no graphics API at
 * all, and the console backends get probes for free.
 *
 * IT ALSO DOES NOT RENDER. ReflectionProbeSet::capture takes a callback and
 * drives the draw itself, because with rlgl the framebuffer bind, the viewport
 * and the matrices are global state somebody has to set. Here the pass IS the
 * unit of work — ScenePipeline::drawProbeCapture opens it, and all this has to
 * supply is which slice, from where, through what matrix. That inverts the
 * ownership in the right direction: the object that owns every other render
 * target owns this one's passes too.
 *
 * ======================= TWO VOLUMES, NOT ONE BOX ==========================
 *
 * Repeated here because it is the one thing a reader must not skim:
 *
 *   influence — where this probe APPLIES. A fragment picks its probe by asking
 *               which influence volumes contain it, and the transition band is
 *               what makes a doorway a crossfade rather than a pop.
 *   parallax  — what its reflection ray is CORRECTED against. The room's own
 *               walls, hard-bounded, so a reflection lands on the surface that
 *               produced it rather than sliding as the camera moves.
 *
 * They are usually similar and conflating them produces two different bugs
 * that each look like the other one.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

namespace rhi { class IRenderDevice; }

class DeviceProbeSet {
public:
    /* 128 per face. There are many of these and they are cubemap-array LAYERS,
     * so the memory is multiplied: 128^2 RGBA16F x 6 faces is 786 KB per probe,
     * and a 16-probe board is 12 MB. At 256 the same board would be 50 MB to
     * describe reflections that blend back to the analytic sky by roughness
     * 0.55 anyway. The number is deliberately the raylib set's, so an A/B
     * between the two renderers compares the same reflection. */
    static constexpr int kFaceSize = 128;

    /* THE PREFILTERED CHAIN: 128, 64, 32, 16, 8, 4. Level L holds the probe
     * convolved for roughness L/(kMipLevels-1), so a shader reads its roughness
     * straight off as a LOD instead of fading the whole term out to a flat sky.
     *
     * SIX RATHER THAN THE FULL EIGHT. The last two levels of a 128 chain are
     * 2x2 and 1x1, and a GGX lobe at roughness 1 is already indistinguishable
     * from the 4x4 level's average — the two extra levels cost a dispatch each
     * to describe a constant. Memory over the whole array is 12 MB to 16 MB. */
    static constexpr int kMipLevels = 6;

    /* The hard ceiling on probes, and therefore on array layers. The shader
     * carries volume arrays of this length, so raising it costs uniform space
     * in every fragment — 16 rooms is a generous tactical board. Rooms beyond
     * this are dropped smallest-first by the placement pass, with a warning. */
    static constexpr int kMaxProbes = 16;

    /* ONE PROBE'S PLACEMENT, IN WORLD SPACE. Everything here is what the shader
     * needs; nothing here knows about cells or rooms.
     *
     * AN AGGREGATE, filled by one caller and read by one callee, with no
     * invariant spanning its fields — the same one-shot carrier SceneFrame and
     * the rhi descriptors are, and for the same reason. */
    struct Volume {
        Vec3 capture;            /* where the cubemap is rendered from        */

        Vec3 parallaxMin;        /* hard bounds for the reflection ray        */
        Vec3 parallaxMax;

        Vec3 influenceMin;       /* where this probe applies, plus a fade band */
        Vec3 influenceMax;

        /* How far inside the influence box the probe reaches full strength.
         * Source 2 calls it the edge fade distance. Zero makes a doorway a hard
         * switch between two completely different reflections, which reads as a
         * flicker rather than as a room change. */
        float transition = 0.5f;

        /* WHICH VOLUME WINS WHERE TWO CONTAIN THE SAME POINT. HIGH WINS, and
         * ties break on how far inside its box the fragment is.
         *
         * EXPLICIT, NOT DERIVED. This used to be the influence box's VOLUME, on
         * the rule that a tighter box describes a point better than a looser
         * one. That is a fine default and it cannot say the two things a level
         * designer needs to say: "this volume overrides that one" between boxes
         * of equal size, and — because equal volumes tie, and the tie fell to
         * array order — it forbade OVERLAPPING volumes altogether.
         *
         * Overlapping volumes with a designer-set priority is exactly what
         * Source 2's env_cubemap_box offers, and it is what this now is. The
         * placement pass supplies defaults (see ProbePlacement.cpp); a map
         * format that carries authored volumes sets them directly. */
        float priority = 0.0f;

        /* False for the room that IS the outdoors, which is the fallback every
         * fragment outside every interior lands on. Carried for the log line
         * and for a caller that wants to treat the two differently; the shader
         * does not read it. */
        bool interior = true;

        /* ============ WHETHER THE BOX IS A GOOD ENOUGH PROXY TO AIM AT =======
         *
         * Parallax correction re-aims the reflection ray from the CAPTURE POINT
         * at where it leaves the box. That is only valid while the capture
         * point and the shading point share an environment — true inside a
         * room, where both are in the same small box and the box really is the
         * walls.
         *
         * IT IS FALSE ACROSS A BOARD, and the failure is spectacular rather
         * than subtle. Give a probe a board-sized box and the ray travels
         * twenty tiles to the far edge before it is re-aimed; re-aiming that
         * from a capture point in the middle of the map, when the surface is
         * ten tiles off to one side, can point at the opposite half of the
         * world. Geometry then appears in a window on the WRONG SIDE — which
         * reads as a mirrored cubemap, and is not: the faces are correct and
         * the lookup into them is aimed wrong.
         *
         * SO THE OUTDOOR VOLUME TURNS IT OFF and is sampled as an environment
         * at infinity, which is what a board-sized capture from one point
         * actually is. ReflectionProbeSet.hpp already stated the principle for
         * the interior case and it is the same one: "uncorrected, the same leak
         * is a distant blur nobody would question." An uncorrected outdoor
         * reflection is imprecise and slides slightly as the camera moves; a
         * corrected one is confidently, visibly wrong.
         *
         * A SEPARATE FLAG FROM `interior`, though today they are the same
         * boolean inverted. They are not the same IDEA — this asks whether the
         * bounds describe the geometry, and a hand-placed exterior probe round
         * a courtyard would be exterior and still worth correcting. Keying
         * behaviour off `interior` would make that a rewrite instead of a
         * value. */
        bool parallax = true;
    };

    /* ONE (PROBE, FACE) PAIR OF WORK, which is what the schedule hands out.
     *
     * The EYE is here rather than left for the caller to look up, and that is
     * the same lesson ReflectionProbeSet's callback signature learned: every
     * view-dependent term — specular, Fresnel, the glass ramp — has to be
     * evaluated where the reflection is being GATHERED. With one probe a stale
     * eye position was a subtly wrong highlight; with sixteen it is fifteen
     * wrong ones, and none of them are on screen to be noticed. */
    struct Face {
        int  probe = 0;
        int  index = 0;                /* 0..5, GL's face order                  */
        int  slice = 0;                /* probe * 6 + index, the array layer     */
        Vec3 eye;
        Mat4 viewProjection;
    };

    /* THE MATRIX ONE CUBE FACE IS CAPTURED THROUGH, and it is public and static
     * for exactly one reason: the device self-test's cube-orientation stage has
     * to use THE SAME face table this class captures with, or it proves nothing
     * about the renderer.
     *
     * A test that retyped the six forward and up vectors would pass against its
     * own copy while the real one was mirrored — and a mirrored cubemap is the
     * failure this whole area is most prone to, because four of the six up
     * vectors legitimately point DOWN and every one of them looks like a typo.
     * See kFaces in the .cpp. `face` is 0..5 in GL's order (+X, -X, +Y, -Y, +Z,
     * -Z); out of range returns identity. */
    /* `nearPlane` defaults to what a probe wants and is a PARAMETER because the
     * decal visibility capture wants something much smaller. A capture's near
     * plane deletes everything closer to the eye than itself, which for a probe
     * in the middle of a room is nothing and for a decal on a wall is the
     * adjoining wall of every corner it is placed in — the capture then says
     * "nothing in the way" exactly where the wall is, and the mark bleeds
     * through. Passing it in keeps the ONE face table this comment is about
     * while letting the two callers disagree about the only value that differs. */
    static Mat4 faceViewProjection(int face, Vec3 eye, float farPlane,
                                   float nearPlane = 0.05f);

    explicit DeviceProbeSet(rhi::IRenderDevice& device);
    ~DeviceProbeSet();

    DeviceProbeSet(const DeviceProbeSet&) = delete;
    DeviceProbeSet& operator=(const DeviceProbeSet&) = delete;

    /* Allocates the array at kMaxProbes layers, its capture depth buffer, its
     * sampler and its uniform block. False means the caller should carry on
     * with the analytic sky — no probes is a flatter frame, not a broken one,
     * and that is exactly what environmentSpecular falls back to. */
    bool create();

    bool valid() const { return array_.valid(); }

    /* ---- placement ----------------------------------------------------
     * WHERE the probes go is not the engine's business. A probe belongs to a
     * "room", and what counts as a room is a question about the game's world —
     * a flooded cell partition here, a portal graph or a hand-placed volume
     * elsewhere. So this owns the array, the schedule and the block, and takes
     * the volumes as data. See game/render/scene/ProbePlacement.hpp.
     *
     * Placement captures nothing. Every layer is stale afterwards. */
    void clear();

    /* Ignored past kMaxProbes — the shader's volume arrays are that long, so a
     * seventeenth probe has nowhere to be. Returns false when it was dropped,
     * so a caller that cares can say which rooms lost out. */
    bool addProbe(const Volume& volume);

    /* The far plane every face is captured with. The scene has to fit inside it
     * or reflections clip; the caller knows the world's extent and this does
     * not. Chains, per the project's API style. */
    DeviceProbeSet& withCaptureFar(float distance);

    int probeCount() const { return static_cast<int>(volumes_.size()); }
    const std::vector<Volume>& probes() const { return volumes_; }

    /* ---- what a pass binds --------------------------------------------
     *
     * THE EMPTY ARRAY IS NOT AN OVERSIGHT. `texture()` is what the lit pass
     * samples and `emptyTexture()` is what the CAPTURE pass binds in its place,
     * because a texture that is simultaneously a colour attachment and bound to
     * a live sampler is undefined on every backend — even when the shader's
     * probe count is zero and nothing reads it. A 1x1 cube array costs nothing
     * and removes the hazard rather than arguing about it. */
    rhi::TextureHandle texture() const { return array_; }
    rhi::TextureHandle emptyTexture() const { return empty_; }
    rhi::SamplerHandle sampler() const { return sampler_; }

    /* The capture pass's depth attachment — one face's worth, reused by every
     * face. Handed out rather than kept private because the pass that needs it
     * is ScenePipeline's, and this object deliberately opens none. */
    rhi::TextureHandle captureDepth() const { return depth_; }

    /* ---- prefiltering -------------------------------------------------
     *
     * THE SAMPLER THE PREFILTER READS ITS SOURCE THROUGH, clamped to exactly one
     * level. That clamp is not tidiness: the prefilter renders INTO level N of
     * the array while sampling level N-1 of the same array, and a texture that is
     * both an attachment and a reachable sampler read is undefined. Clamping the
     * sampler to N-1 makes the level being written unreachable, which is what
     * makes the pass legal rather than merely lucky.
     *
     * One per source level, built once — a sampler object is a handful of bytes
     * and rebuilding one per dispatch would be a state change per level to
     * express a constant. */
    rhi::SamplerHandle levelSampler(int sourceLevel) const;

    /* WHICH PROBE HAS A COMPLETE, CURRENT SET OF SIX FACES and has not been
     * prefiltered since. -1 when there is nothing to do.
     *
     * PER PROBE, NEVER PER FACE, and this is the constraint the whole schedule
     * bends around. A GGX lobe at high roughness reaches across face boundaries,
     * so prefiltering +X while -Z still holds pre-rebuild content bakes stale
     * data into the chain permanently — and it is baked, so the next sweep does
     * not repair it. The capture walks one face at a time; the prefilter waits
     * for all six. */
    int probeReadyToPrefilter() const;

    /* Says that probe's chain is now current. Separate from the query so the
     * caller cannot mark work done that it did not do. */
    void markPrefiltered(int probe);

    /* The volume block, at binding 0 — see rhi/probes.glsl. Re-uploaded only
     * when the volumes change, which is when the world does. */
    rhi::BufferHandle block() const { return block_; }

    /* ---- the schedule --------------------------------------------------
     *
     * ONE PAIR PER FRAME IS THE STEADY STATE, which is the whole reason the
     * cursor walks (probe, face) pairs rather than faces within a probe: the
     * cost is one extra scene render per frame no matter how many rooms the map
     * has. The price is staleness — 6 x probeCount frames for a full sweep, so
     * a 12-room board takes about 1.2s at 60fps to fully settle. On a tactical
     * camera watching a world that changes when something is destroyed, nobody
     * sees that.
     *
     * Returns false when there is nothing to capture. Advancing the cursor is
     * this call's job, so a caller that takes a Face is committed to drawing
     * it — which is right, because the alternative is a scheduler that can
     * silently never finish. */
    bool nextFace(Face& out);

    /* Marks every layer stale, so the cursor has a full sweep of work. Costs
     * nothing itself — it does not render. */
    void markAllStale()
    {
        staleFaces_ = probeCount() * 6;

        /* AND EVERY FACE IS NOW UNCAPTURED, not merely out of date. Leaving the
         * masks set would let probeReadyToPrefilter see six "current" faces that
         * describe a world which no longer exists, and prefilter them — baking
         * the old building into a chain the next sweep cannot repair, because
         * the sweep rewrites level 0 and the prefilter only reruns when all six
         * faces come round again. */
        std::fill(faceMask_.begin(), faceMask_.end(), uint8_t{ 0 });
    }
    bool stale() const { return staleFaces_ > 0; }

    /* How many (probe, face) pairs still hold pre-rebuild content. Worth
     * showing in the dev overlay: a probe set that never reaches zero is a
     * scheduler bug, and it is otherwise completely invisible. */
    int staleFaceCount() const { return staleFaces_; }

private:
    void release();
    void uploadBlock();

    rhi::IRenderDevice& device_;

    rhi::TextureHandle array_;    /* cube array, kMaxProbes * 6 layers      */
    rhi::TextureHandle empty_;    /* 1x1 cube array, bound during capture   */

    /* THE CAPTURE'S DEPTH, one face's worth, reused by every face. It is
     * cleared at the top of each pass and never sampled, so one buffer serves
     * all ninety-six slices — a per-slice depth array would be 6 MB describing
     * nothing that outlives its own pass. */
    rhi::TextureHandle depth_;

    rhi::SamplerHandle sampler_;
    rhi::BufferHandle  block_;

    std::vector<Volume> volumes_;

    /* Set by addProbe and clear(), consumed by the next capture. The block is
     * uploaded lazily rather than per frame because it changes when the world
     * does — a wall coming down — and not otherwise. */
    bool blockDirty_ = false;

    /* The round-robin cursor, over (probe, face) pairs flattened as
     * probe * 6 + face. */
    int cursor_ = 0;
    int staleFaces_ = 0;

    /* PER PROBE, A BIT PER FACE: set as the round robin recaptures that face,
     * cleared wholesale when the chain is rebuilt from them. All six set
     * therefore means "every face is newer than the chain", which fires once per
     * sweep — where a separate already-prefiltered flag fired every frame.
     * The global stale count above cannot answer the prefilter's question — it
     * says how much work is outstanding across the set, not whether any single
     * probe is currently whole. A prefilter driven off the global count would
     * run on a probe with five fresh faces and one stale one, which is exactly
     * the failure the per-probe rule exists to prevent. */
    std::vector<uint8_t> faceMask_;

    /* One per SOURCE level, so kMipLevels - 1 of them are ever used. */
    std::vector<rhi::SamplerHandle> levelSamplers_;

    float captureFar_ = 100.0f;
};

}  // namespace cromwell
