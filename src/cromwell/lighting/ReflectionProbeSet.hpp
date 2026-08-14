/* ReflectionProbeSet.hpp — one reflection probe per room, in a cubemap array.
 *
 * SINGLE RESPONSIBILITY: own the array texture, the per-probe volumes, and the
 * schedule that keeps its faces current. Which rooms exist is RoomPartition's
 * business; what a probe means to a surface is the shader's.
 *
 * WHY THIS REPLACES THE SINGLE PROBE. EnvironmentProbe captured the world from
 * one point in the middle of it, and parallax-corrected against the whole
 * board. That works exactly as long as the board is one room. It is not: the
 * correction re-aims the reflection ray FROM the probe's centre, so a wall's
 * interior face fetches the cubemap along a ray that starts on the far side of
 * that wall, and returns geometry the wall blocks. The wall reads as
 * transparent, and worse, the parallax correction is what makes it read that
 * way — uncorrected, the same leak is a distant blur nobody would question.
 *
 * AND THE OUTDOOR VOLUME KEPT THE OLD ARRANGEMENT, WHICH KEPT THE OLD BUG.
 * Every interior got a room-sized box; the outdoors was left correcting against
 * the whole board from one capture point in the middle of it, on the grounds
 * that it is the one volume genuinely that big. That reasoning confuses the box
 * being the right SIZE with the box being a usable PROXY. It is not one: a
 * window on a building's east wall is ten tiles from the capture point, its
 * reflection ray runs twenty tiles to the far edge before being re-aimed, and
 * re-aiming that from the map's centre points it at the opposite half of the
 * world. Geometry behind the camera then appears in the pane on the WRONG SIDE.
 *
 * That looks exactly like a mirrored cubemap and is not one — all six faces are
 * correct, and it is the lookup INTO them that is aimed wrong. It survived
 * review because the paragraph above had already been written and read as
 * though it covered the outdoor case; it does cover it, and the outdoor case
 * was the one exception nobody re-read it against.
 *
 * So the outdoor volume now sets `parallax = false` and is sampled as an
 * environment at infinity, which is what a board-sized capture from a single
 * point actually is. This is the same sentence as above, applied to the one
 * volume that was exempted from it.
 *
 * TWO VOLUMES PER PROBE, AND THEY ARE NOT THE SAME BOX. This is the part both
 * Source 2 (env_cubemap_box) and Unreal (reflection captures) get right and a
 * naive port misses:
 *
 *   influence — where this probe APPLIES. A fragment picks its probe by
 *               asking which influence volumes contain it.
 *   parallax  — what its reflection ray is CORRECTED against. The room's own
 *               walls, so a reflection lands on the surface that produced it.
 *
 * They are usually similar and must not be conflated. Influence needs a
 * transition band so walking through a doorway crossfades rather than pops;
 * parallax needs hard bounds, because a soft box is a reflection that slides.
 *
 * SELECTION IS PER-PIXEL, not per-object, and that is forced rather than
 * chosen. Source 2 assigns a cubemap per mesh entity, which it can afford
 * because its world is chunked. Here the static world is batched per storey
 * and per material — one draw call is every wall on a floor — and a wall is a
 * single box carrying BOTH its faces. The interior face and the exterior face
 * need different probes and are in the same draw call, so no per-draw uniform
 * can separate them. A per-pixel test against vWorldPosition can, and gets the
 * two faces right for free.
 *
 * THE ARRAY IS RAW GL. rlgl has no cubemap-array anything: no creation, no
 * per-layer framebuffer attach. This is the one file that calls GL directly —
 * see the note in CMakeLists.txt.
 */
#pragma once

#include "raylib.h"

#include <functional>
#include <vector>

namespace cromwell {

/* One probe's placement, in WORLD space. Everything here is what the shader
 * needs; nothing here knows about cells. */
struct ProbeVolume {
    Vector3 capture{};        /* where the cubemap is rendered from        */

    Vector3 parallaxMin{};    /* hard bounds for the reflection ray        */
    Vector3 parallaxMax{};

    Vector3 influenceMin{};   /* where this probe applies, plus a fade band */
    Vector3 influenceMax{};

    /* How far inside the influence box the probe reaches full strength.
     * Source 2 calls this the edge fade distance. Zero would make a doorway a
     * hard switch between two completely different reflections, which reads as
     * a flicker rather than as a room change. */
    float transition = 0.5f;

    /* WHICH VOLUME WINS WHERE TWO CONTAIN THE SAME POINT. HIGH WINS, ties
     * broken by depth inside the box, so volumes may OVERLAP and crossfade.
     * Designer-set rather than derived from box size — see the long note on
     * DeviceProbeSet::Volume::priority. */
    float priority = 0.0f;

    /* False for the room that IS the outdoors, which is the fallback every
     * fragment outside every interior lands on. */
    bool interior = true;

    /* WHETHER THE BOX IS A GOOD ENOUGH PROXY TO AIM AT. Off for the outdoor
     * volume, which is sampled as an environment at infinity instead — see the
     * long note on DeviceProbeSet::Volume::parallax, and the paragraph added
     * to this file's header. */
    bool parallax = true;
};

class ReflectionProbeSet {
public:
    /* 128 per face rather than the single probe's 256. There are now many of
     * these and they are cubemap-array LAYERS, so the memory is multiplied:
     * 128^2 RGBA16F x 6 faces is 786 KB per probe, and a 16-probe board is
     * 12 MB. At 256 the same board would be 50 MB to describe reflections
     * that blend to the analytic sky by roughness 0.55 anyway. */
    static constexpr int kFaceSize = 128;

    /* The hard ceiling on probes, and therefore on array layers. The shader
     * carries volume arrays of this length, so raising it costs uniform space
     * in every fragment — 16 rooms is a generous tactical board. Rooms beyond
     * this are dropped smallest-first, with a warning. */
    static constexpr int kMaxProbes = 16;

    ReflectionProbeSet() = default;
    ~ReflectionProbeSet();

    ReflectionProbeSet(const ReflectionProbeSet&) = delete;
    ReflectionProbeSet& operator=(const ReflectionProbeSet&) = delete;

    /* Allocates the array at kMaxProbes layers and its framebuffer. False
     * means the caller should carry on with the analytic sky — no probes is a
     * flatter frame, not a broken one. */
    bool create();

    bool valid() const { return arrayId_ != 0; }

    /* ---- placement ----------------------------------------------------
     * WHERE the probes go is not the engine's business. A probe belongs to a
     * "room", and what counts as a room is a question about the game's world
     * — a flooded cell partition here, a portal graph or a hand-placed volume
     * elsewhere. So the set owns the array, the schedule and the capture, and
     * takes the volumes as data.
     *
     * The game's placement pass lives in game/render/ProbePlacement.hpp; it
     * calls clear(), then addProbe() per room, then markAllStale().
     *
     * Placement captures nothing. Every layer is stale afterwards, which is
     * what markAllStale is for. */
    void clear()
    {
        probes_.clear();
        cursor_ = 0;
        staleFaces_ = 0;
        previewProbe_ = 0;
    }

    /* Ignored past kMaxProbes — the shader's volume arrays are that long, so
     * a 17th probe has nowhere to be. Returns false when it was dropped, so a
     * caller that cares can say which rooms lost out. */
    bool addProbe(const ProbeVolume& probe);

    /* The far plane every face is captured with. The scene has to fit inside
     * it or reflections clip; the caller knows the world's extent and the set
     * does not. */
    void setCaptureFar(float distance) { captureFar_ = distance; }

    const std::vector<ProbeVolume>& probes() const { return probes_; }
    int probeCount() const { return static_cast<int>(probes_.size()); }
    unsigned int textureId() const { return arrayId_; }

    /* Binds the array to a texture unit for sampling, and unbinds it again.
     *
     * These exist so PbrShader does not have to: rlgl's rlEnableTextureCubemap
     * binds GL_TEXTURE_CUBE_MAP, and an array is a different target that would
     * leave the sampler reading nothing. Keeping the raw call here is what
     * keeps GL out of the rest of the renderer. */
    void bindTo(int textureUnit) const;
    static void unbindFrom(int textureUnit);

    /* Renders `faceCount` (probe, face) pairs from the round-robin cursor,
     * invoking `drawScene` once per face with that face's matrices installed —
     * the same shape as ShadowMap::Scope, and for the same reason: the caller
     * owns what geometry means.
     *
     * THE CALLBACK IS HANDED THE EYE POSITION, which the single-probe version
     * did not need because there was only ever one. Every view-dependent term
     * — specular, Fresnel, the glass ramp — has to be evaluated where the
     * reflection is being GATHERED, so the caller has to re-push the camera
     * uniform per probe. Passing it in is what stops that being a thing the
     * caller can silently forget: with one probe a stale eye position was a
     * subtly wrong highlight, with twelve it is eleven wrong ones.
     *
     * ONE PAIR PER FRAME IS THE STEADY STATE, which is the whole reason the
     * cursor walks (probe, face) pairs rather than faces within a probe: the
     * cost is one extra scene render per frame no matter how many rooms the
     * map has. The price is staleness — 6 x probeCount frames for a full
     * sweep, so a 12-room board takes about 1.2s at 60fps to fully settle.
     * On a tactical camera watching a world that changes when something is
     * destroyed, nobody sees that. */
    void capture(const std::function<void(Vector3 eye)>& drawScene, int faceCount = 1);

    /* Marks every layer stale, so the cursor has a full sweep of work. Costs
     * nothing itself — it does not render. */
    void markAllStale() { staleFaces_ = probeCount() * 6; }
    bool stale() const { return staleFaces_ > 0; }

    /* How many (probe, face) pairs still hold pre-rebuild content. Drawn in
     * the dev overlay: a probe set that never reaches zero is a scheduler
     * bug, and it is otherwise completely invisible. */
    int staleFaceCount() const { return staleFaces_; }

    /* The six faces of ONE probe unrolled into a 2D strip, for the inspector.
     * A cubemap array you cannot look at is worse to debug than the single
     * cubemap was, because now there is also the question of WHICH layer is
     * wrong. Zero id if the preview shader is missing. */
    Texture2D previewTexture() const { return preview_.texture; }
    void      setPreviewProbe(int index);
    int       previewProbe() const { return previewProbe_; }

private:
    void releaseResources();
    void updatePreview();

    unsigned int arrayId_ = 0;          /* GL_TEXTURE_CUBE_MAP_ARRAY */
    unsigned int framebufferId_ = 0;
    unsigned int depthBufferId_ = 0;

    RenderTexture2D preview_{};
    Shader          previewShader_{};
    int             previewProbe_ = 0;
    int             locPreviewLayer_ = -1;

    std::vector<ProbeVolume> probes_;

    /* The round-robin cursor, over (probe, face) pairs flattened as
     * probe * 6 + face. */
    int cursor_ = 0;
    int staleFaces_ = 0;

    /* The per-layer framebuffer attach is verified on the first slice only.
     * It either works for every slice or for none, and glGetError in a
     * per-frame loop is a pipeline stall for a question already answered. */
    bool attachChecked_ = false;

    float captureFar_ = 100.0f;
};

}  // namespace cromwell
