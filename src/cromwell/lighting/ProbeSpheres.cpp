#include "cromwell/lighting/ProbeSpheres.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/lighting/ReflectionProbeSet.hpp"
#include "cromwell/lighting/SunLight.hpp"
#include "cromwell/material/PbrMaterial.hpp"

namespace cromwell {
namespace {

/* Big enough to read the reflection in from a tactical camera, small enough
 * that a ball in a one-cell room does not fill the room. A cell is 1.0 across
 * and 0.667 tall, so a 0.28 radius clears the floor and ceiling of the
 * tightest volume that gets a probe at all. */
constexpr float kRadius = 0.28f;

void setVector3(Shader shader, int location, Vector3 value)
{
    if (location < 0) return;
    const float packed[3] = { value.x, value.y, value.z };
    SetShaderValue(shader, location, packed, SHADER_UNIFORM_VEC3);
}

}  // namespace

ProbeSpheres::~ProbeSpheres()
{
    if (mesh_.vertexCount > 0) UnloadMesh(mesh_);

    /* NO UnloadMaterial, AND THAT IS NOT AN OVERSIGHT. raylib's UnloadMaterial
     * unloads material.shader as well as the map array, and this material
     * borrows the shader below rather than owning it — calling both is a
     * double free of the same program, which shows up as heap corruption at
     * exit rather than anywhere near this line. Every sibling here that pairs
     * LoadMaterialDefault with a custom shader does the same thing:
     * PrepassShader, ShadowMap, RibbonShader, CustomDepthStencil. The map
     * array is a few hundred bytes leaked once for the process lifetime. */
    if (shader_.id != 0) UnloadShader(shader_);
}

bool ProbeSpheres::load()
{
    shader_ = ShaderLibrary::load("probe_sphere.vs.glsl", "probe_sphere.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "PROBES: no sphere shader - the probe view has no balls");
        return false;
    }

    locCameraPosition_   = GetShaderLocation(shader_, "uCameraPosition");
    locZenithColour_     = GetShaderLocation(shader_, "uZenithColour");
    locHorizonColour_    = GetShaderLocation(shader_, "uHorizonColour");
    locGroundColour_     = GetShaderLocation(shader_, "uGroundColour");
    locAmbientIntensity_ = GetShaderLocation(shader_, "uAmbientIntensity");
    locProbeLayer_       = GetShaderLocation(shader_, "uProbeLayer");
    locProbeTint_        = GetShaderLocation(shader_, "uProbeTint");

    /* The cubemap array lives on a fixed frame-global unit and the sampler
     * uniform is just an int naming it. Pointed here once, exactly as
     * PbrShader does for its own program — a sampler left at 0 reads whatever
     * happens to be bound there and the effect quietly does nothing. */
    const int unit = kUnitEnvironment;
    const int location = GetShaderLocation(shader_, "uEnvironmentMap");
    if (location >= 0) SetShaderValue(shader_, location, &unit, SHADER_UNIFORM_INT);

    /* 16 rings and slices: this is a debug ball, and the reflection is what is
     * being looked at rather than the silhouette.
     *
     * No UploadMesh after this. GenMesh* uploads what it generates, and
     * calling it again re-registers the same VAO — raylib logs "trying to
     * re-load an already loaded mesh" and leaks the first one. */
    mesh_ = GenMeshSphere(kRadius, 16, 16);

    material_ = LoadMaterialDefault();
    material_.shader = shader_;

    return valid();
}

void ProbeSpheres::draw(const ReflectionProbeSet& probes, const SunLight& sun,
                        Vector3 cameraPosition, float ambientIntensity) const
{
    if (!valid() || probes.probeCount() == 0) return;

    /* The sky half of the fallback, so an uncovered direction on the ball
     * matches what a real surface would reflect there. Pushed per draw call
     * rather than per frame because this pass runs only in a debug view and
     * four uniforms is not worth a dirty flag. */
    setVector3(shader_, locCameraPosition_, cameraPosition);
    setVector3(shader_, locZenithColour_,   sun.zenithColour());
    setVector3(shader_, locHorizonColour_,  sun.horizonColour());
    setVector3(shader_, locGroundColour_,   sun.groundColour());
    if (locAmbientIntensity_ >= 0)
        SetShaderValue(shader_, locAmbientIntensity_, &ambientIntensity,
                       SHADER_UNIFORM_FLOAT);

    for (int i = 0; i < probes.probeCount(); i++) {
        const ProbeVolume& probe = probes.probes()[static_cast<std::size_t>(i)];

        const float layer = static_cast<float>(i);
        const float tint  = probe.interior ? 0.0f : 1.0f;
        if (locProbeLayer_ >= 0)
            SetShaderValue(shader_, locProbeLayer_, &layer, SHADER_UNIFORM_FLOAT);
        if (locProbeTint_ >= 0)
            SetShaderValue(shader_, locProbeTint_, &tint, SHADER_UNIFORM_FLOAT);

        /* THE OUTDOOR PROBE'S BALL IS DRAWN TOO, and wants to be: its capture
         * point is the middle of the board, which is often inside a building,
         * and seeing that is how you find out the fallback probe is capturing
         * from somewhere daft. */
        DrawMesh(mesh_, material_, MatrixTranslate(probe.capture.x,
                                                   probe.capture.y,
                                                   probe.capture.z));
    }
}

}  // namespace cromwell
