#include "cromwell/camera/CameraSet.hpp"

#include "cromwell/diag/Logger.hpp"

#include <cmath>

namespace cromwell {

namespace {

/* The golden ratio's fractional part. Stepping a phase by this each time lands
 * successive cameras about as far from each other as they can be, for ANY
 * number of them, without any of them needing to know how many there are — the
 * same property that makes it the standard choice for spreading sample points
 * and hues. See the note on nextPhaseFraction. */
constexpr float kGoldenStep = 0.61803399f;

}  // namespace

float CameraSet::nextPhaseFraction() const
{
    const float raw = static_cast<float>(added_) * kGoldenStep;
    return raw - std::floor(raw);
}

CameraId CameraSet::add(CameraDesc&& desc)
{
    auto camera = std::make_unique<Camera>(std::move(desc.camera));

    /* THE ORDER, DONE ONCE AND CORRECTLY. The target has to exist before the
     * screen-space buffers can be sized against it — that is the step a caller
     * assembling this by hand gets wrong, and it fails silently rather than
     * loudly. */
    camera->renderingToTexture(desc.width, desc.height);
    if (!camera->rendersToTexture()) {
        LOGGER->warn("camera set: '{}' could not be given a {}x{} target", desc.name,
                     desc.width, desc.height);
        return 0;
    }

    /* NOTHING TO ASK FOR HERE. Whether this camera gets a depth prepass follows
     * from its own layers — see ViewLayers::needsDepthPrepass — so giving it a
     * target is all it takes. There used to be a `screenSpaceEffects` field on
     * the descriptor beside the feature flags, which meant a camera could ask
     * for occlusion and silently not get it. */
    if (camera->layers().needsDepthPrepass() && !camera->hasScreenSpaceEffects()) {
        /* NOT FATAL. A camera without its own prepass is still a picture — it
         * just has no occlusion and no decals, which the renderer already
         * handles by switching those features off. Better a feed that works and
         * looks flatter than no feed at all. */
        LOGGER->warn("camera set: '{}' asked for screen-space features but has no depth "
                     "prepass; its occlusion and decals will be off",
                     desc.name);
    }

    CaptureSchedule schedule = desc.schedule;
    if (desc.autoStagger) schedule.setPhase(schedule.intervalSeconds() * nextPhaseFraction());
    camera->schedule() = schedule;

    const CameraId id = nextId_++;
    entries_.push_back(Entry{ id, std::move(desc.name), std::move(camera) });
    ++added_;
    return id;
}

void CameraSet::remove(CameraId id)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->id != id) continue;
        entries_.erase(it);
        return;
    }
    /* A stale handle is not an error — removing twice, or removing something a
     * failed add() never created, are both things callers do. */
}

void CameraSet::clear()
{
    entries_.clear();
    /* nextId_ and added_ are NOT reset: a handle from before a clear must not
     * come back to life pointing at a different camera, and the phase sequence
     * should keep advancing so a rebuilt set does not land every camera back on
     * the same frame. */
}

Camera* CameraSet::find(CameraId id)
{
    for (Entry& entry : entries_) {
        if (entry.id == id) return entry.camera.get();
    }
    return nullptr;
}

const Camera* CameraSet::find(CameraId id) const
{
    for (const Entry& entry : entries_) {
        if (entry.id == id) return entry.camera.get();
    }
    return nullptr;
}

int CameraSet::renderAll(float deltaSeconds, const DrawScene& draw)
{
    if (!draw) return 0;

    int rendered = 0;
    for (Entry& entry : entries_) {
        /* NO FORWARDING WRAPPER — Camera::capture hands the callback the camera
         * itself, which is where the caller reads that camera's own layers and
         * buffers. That used to be a per-camera lambda wiring an object back to
         * itself, which was the step that got copied wrong. */
        if (entry.camera->capture(deltaSeconds, draw, entry.name.c_str())) ++rendered;
    }
    return rendered;
}

}  // namespace cromwell
