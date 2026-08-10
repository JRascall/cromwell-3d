/* ProbeSpheres.hpp — the reflection probes, drawn as chrome balls.
 *
 * SINGLE RESPONSIBILITY: own a sphere mesh and a mirror shader, and put one
 * ball at each probe's capture point. It reads ReflectionProbeSet and changes
 * nothing in it.
 *
 * WHY IT IS ITS OWN CLASS. ReflectionProbeSet owns the array, the volumes and
 * the capture schedule; how a probe is DRAWN for a human is a different job
 * with a different lifetime — it loads a shader and a mesh that the probe
 * system itself has no use for, and it is the first thing that would be cut
 * from a shipping build.
 *
 * WHAT IT IS FOR. A single probe raised one question — is the cubemap aimed
 * correctly — and the old all-surfaces-mirror view answered it. A probe SET
 * raises three more that the mirror view cannot touch: where are they, how
 * many are there, and did this room get its own? A ball at each capture point
 * answers all three at once, which is why Unreal and Source both draw
 * reflection captures exactly this way.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class ReflectionProbeSet;
class SunLight;

class ProbeSpheres {
public:
    ProbeSpheres() = default;
    ~ProbeSpheres();

    ProbeSpheres(const ProbeSpheres&) = delete;
    ProbeSpheres& operator=(const ProbeSpheres&) = delete;

    /* False means the balls cannot be drawn — a debug view that fails to load
     * costs a debug view, not a frame. */
    bool load();
    bool valid() const { return shader_.id != 0 && mesh_.vertexCount > 0; }

    /* Draws one ball per probe. Call INSIDE the scene's BeginMode3D and inside
     * the HDR target: the shader writes linear radiance like every other lit
     * pass here, so the balls tonemap with the rest of the frame rather than
     * arriving pre-brightened.
     *
     * The probe set's cubemap array has to be bound to its texture unit
     * already — PbrShader::setEnvironmentProbes does that for the frame, and
     * this reads the same unit rather than binding a second time. */
    void draw(const ReflectionProbeSet& probes, const SunLight& sun,
              Vector3 cameraPosition, float ambientIntensity) const;

private:
    Shader   shader_{};
    Mesh     mesh_{};
    Material material_{};

    int locCameraPosition_ = -1;
    int locZenithColour_ = -1;
    int locHorizonColour_ = -1;
    int locGroundColour_ = -1;
    int locAmbientIntensity_ = -1;
    int locProbeLayer_ = -1;
    int locProbeTint_ = -1;
};

}  // namespace cromwell
