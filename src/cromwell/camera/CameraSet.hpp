/* CameraSet.hpp — several cameras, made and driven as one thing.
 *
 * SINGLE RESPONSIBILITY: own a collection of Cameras, build them from a
 * description, keep them from all redrawing on the same frame, and render them
 * in one call.
 *
 * ===================== WHY THIS EXISTS ABOVE Camera ==================
 *
 * A SINGLE Camera IS ASSEMBLED IN FOUR ORDERED STEPS — create the target,
 * enable its buffers, set its camera, set its layers — and three of those are
 * only correct after the first. That is a fine shape for the class that owns
 * the resources and a poor one for the code that just wants a camera in a
 * corridor. Get the order wrong and the failure is silent: buffers sized
 * against a target that did not exist yet.
 *
 * And the moment there are TWO, the caller is also on the hook for three things
 * that have nothing to do with what it is trying to say:
 *
 *   FORWARDING. Each capture needs a callback that reaches back into that same
 *   capture for its layers and its buffers and hands them to the renderer. That
 *   is the caller wiring an object to itself, once per camera, and it is the
 *   part most likely to be copied wrong — pass the previous capture's layers
 *   and the picture is subtly, unreportably incorrect.
 *
 *   STAGGERING. N captures on one interval, all created together, redraw on the
 *   same frame forever. See CaptureSchedule; the fix is a phase per capture, and
 *   nobody hand-assigns those correctly for a collection whose size changes.
 *
 *   NAMING. Every capture needs a distinct profiler zone or a capture cannot
 *   say which camera cost the frame.
 *
 * So: one descriptor in, one handle back, one renderAll. What varies between
 * cameras — where it is, what projection, which layers, how often, whether it
 * gets screen-space effects — is DATA in the descriptor, and adding the tenth
 * camera is the same amount of code as the first.
 *
 * ========================= WHAT IT DELIBERATELY IS NOT =====================
 *
 * NOT A RENDERER. It calls back once per capture with everything a scene pass
 * needs and has no idea what happens inside. Same division as Camera
 * itself, and for the same reason: what a scene contains is the game's.
 *
 * NOT A SCHEDULER ACROSS CAPTURES. It spreads their phases so they do not
 * collide, but it will not skip one because the frame is already expensive —
 * that is a frame-pacing policy, it needs a budget this class has no way to
 * know, and a capture that silently did not run is worse than one that costs
 * what it said it would.
 *
 * HANDLES, NOT POINTERS OR INDICES. A capture can be removed, so an index goes
 * stale silently and a pointer goes dangling. An id that no longer resolves
 * returns null, which a caller can check.
 */
#pragma once

#include "cromwell/camera/Camera.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cromwell {

/* Everything about one camera, stated at once.
 *
 * ONE-SHOT DATA CARRIER (see ui/core/UiColor.hpp): filled at the call site,
 * read by add(), dead after. The capture it produces owns its own copies. */
struct CameraDesc {
    /* Names the profiler zone and any diagnostic. Distinct per capture, or a
     * profile cannot tell two cameras apart — which is the only question worth
     * asking of a second one. */
    std::string name = "capture";

    /* Size of the TEXTURE that comes out. The HDR buffer behind it is
     * supersampled and resolved down; see Camera. */
    int width = 512;
    int height = 512;

    /* THE CAMERA ITSELF, already placed and configured. Built with Camera's
     * own fluent methods, so everything about a viewpoint is stated in one
     * vocabulary:
     *
     *     desc.camera = Camera::perspective(70.0f);
     *     desc.camera.at({ 0, 10, 0 }).lookingAt({ 0, 0, 12 })
     *                .withLayers(ViewLayers::worldOnly());
     *
     * Moved in — a camera owns GPU resources and is never copied silently. The
     * output and the schedule are applied by add(), so a descriptor's camera
     * does not need renderingToTexture called on it first. */
    Camera camera = Camera::perspective();

    /* How often it redraws. A capture is a whole extra scene pass; this is the
     * knob that decides whether that matters. See CaptureSchedule, which is
     * where the argument for stating it explicitly lives. */
    CaptureSchedule schedule = CaptureSchedule::everyFrame();

    /* Spread this one's redraws away from its neighbours' automatically. On by
     * default because the failure it prevents is invisible in an average and
     * obvious in a frame graph. Turn it off only to phase a capture by hand —
     * two cameras that genuinely must redraw on the same frame. */
    bool autoStagger = true;
};

/* Opaque, and stable across additions and removals — unlike an index. Zero is
 * "no capture", so a default-constructed handle resolves to null rather than to
 * whatever happens to be first. */
using CameraId = int;

class CameraSet {
public:
    CameraSet() = default;

    CameraSet(const CameraSet&) = delete;
    CameraSet& operator=(const CameraSet&) = delete;

    /* Builds a capture and takes ownership. Does the four ordered steps in the
     * order that works, so a caller cannot get it wrong.
     *
     * Returns 0 if the target could not be made — the same answer as "no
     * capture", so a caller that ignores it gets nothing drawn rather than a
     * crash. Anything it did allocate is released. */
    CameraId add(CameraDesc&& desc);

    /* Frees the capture and its buffers. Existing handles to OTHER captures
     * stay valid; this one's resolves to null from here on. */
    void remove(CameraId id);
    void clear();

    /* Null when the handle is stale or was never valid. Use it to move a
     * camera, change its layers or force a redraw:
     *
     *     if (Camera* feed = captures.find(cctv)) {
     *         feed->at(post).lookingAt(target);
     *         feed->schedule().request();
     *     }
     */
    Camera*       find(CameraId id);
    const Camera* find(CameraId id) const;

    int size() const { return static_cast<int>(entries_.size()); }
    bool empty() const { return entries_.empty(); }

    /* Renders every capture whose schedule says so, each under its own profiler
     * zone.
     *
     * THE CALLBACK GETS THE CAPTURE ITSELF, which is what removes the
     * per-camera wiring: read `capture.layers()` and `capture.buffers()` off it
     * rather than having them threaded in from outside. One function serves
     * every camera in the set, however many there are and however differently
     * they are configured.
     *
     *     cameras.renderAll(dt, [&](Camera& camera, Camera::ScenePhase phase,
     *                                float width, float height) {
     *         drawScene(camera, phase, width, height);
     *     });
     *
     * The phase is forwarded rather than hidden because a caller has to honour
     * it — see Camera::ScenePhase, and the raylib constraint it exists for.
     *
     * Returns how many actually drew, for a caller that wants to know what this
     * frame cost.
     *
     * THE SAME CALLBACK TYPE Camera::capture TAKES, on purpose: a draw function
     * written for one camera serves a set of them, and the other way round. */
    using DrawScene = Camera::DrawScene;
    int renderAll(float deltaSeconds, const DrawScene& draw);

    /* Iteration, for a caller that wants to show every capture's texture or
     * list them in a panel. */
    template <typename Visit>
    void forEach(Visit&& visit) const
    {
        for (const Entry& entry : entries_) visit(entry.id, entry.name, *entry.camera);
    }

private:
    struct Entry {
        CameraId id = 0;
        std::string name;
        std::unique_ptr<Camera> camera;
    };

    /* The phase for the next capture added, as a fraction of its own interval.
     *
     * GOLDEN-RATIO SPACING rather than "index over count", because the count is
     * not known when a capture is added and changes afterwards. Stepping by
     * 0.618 of a turn spreads any number of entries about as evenly as they can
     * be spread, and — the part that matters — adding the fifth does not move
     * the other four. Dividing by the count would re-phase every existing
     * capture on every add, which is a visible stutter for a bookkeeping
     * change. */
    float nextPhaseFraction() const;

    std::vector<Entry> entries_;
    CameraId nextId_ = 1;   /* 0 is the null handle */
    int added_ = 0;               /* only ever grows; drives the phase sequence */
};

}  // namespace cromwell
