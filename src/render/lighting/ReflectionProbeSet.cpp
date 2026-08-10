#include "render/lighting/ReflectionProbeSet.hpp"

/* GL DIRECTLY, THROUGH THE ONE DOOR. rlgl has no API for cubemap arrays — no
 * create, no per-layer framebuffer attach — so this file calls glTexImage3D
 * and glFramebufferTextureLayer itself.
 *
 * It used to include glad.h. It now goes through render/gpu/GL.hpp, which owns
 * that include and the ordering rule behind it, so there is one boundary
 * rather than a growing list of files that each reached past rlgl for their
 * own good reason. See the note in CMakeLists.txt. */
#include "render/gpu/GL.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "core/lattice/Constants.hpp"
#include "core/lattice/Lattice.hpp"
#include "core/light/RoomPartition.hpp"
#include "render/gpu/ShaderLibrary.hpp"

#include <algorithm>
#include <vector>

namespace xcom {
namespace {

/* The six faces, in GL's order and GL's orientations — identical to
 * EnvironmentProbe's table and for the identical reason: a cubemap's faces are
 * defined in a LEFT-handed space, so four of the six up vectors point down. A
 * face rendered with the intuitive up vector comes out mirrored, which is far
 * harder to spot than one that is obviously broken. */
struct CubeFace {
    Vector3 forward;
    Vector3 up;
};

constexpr CubeFace kFaces[6] = {
    { {  1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* +X */
    { { -1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* -X */
    { {  0.0f,  1.0f,  0.0f }, { 0.0f,  0.0f,  1.0f } },   /* +Y */
    { {  0.0f, -1.0f,  0.0f }, { 0.0f,  0.0f, -1.0f } },   /* -Y */
    { {  0.0f,  0.0f,  1.0f }, { 0.0f, -1.0f,  0.0f } },   /* +Z */
    { {  0.0f,  0.0f, -1.0f }, { 0.0f, -1.0f,  0.0f } },   /* -Z */
};

/* A cell's world-space box. THE AXIS SWAP LIVES HERE and nowhere else: the
 * lattice's y is the world's z, because the world is y-up and the lattice is a
 * floor plan. RoomPartition deliberately reports cells rather than world
 * boxes so that this conversion has exactly one home. */
void cellBox(const Cell& minimum, const Cell& maximum, Vector3& outMin, Vector3& outMax)
{
    outMin = Vector3{ static_cast<float>(minimum.x),
                      Lattice::cellBaseHeight(minimum.z),
                      static_cast<float>(minimum.y) };
    outMax = Vector3{ static_cast<float>(maximum.x) + 1.0f,
                      Lattice::cellBaseHeight(maximum.z + 1),
                      static_cast<float>(maximum.y) + 1.0f };
}

float boxVolume(Vector3 minimum, Vector3 maximum)
{
    const Vector3 size = Vector3Subtract(maximum, minimum);
    return std::max(size.x, 0.0f) * std::max(size.y, 0.0f) * std::max(size.z, 0.0f);
}

}  // namespace

ReflectionProbeSet::~ReflectionProbeSet() { releaseResources(); }

void ReflectionProbeSet::releaseResources()
{
    if (preview_.id)       { UnloadRenderTexture(preview_); preview_ = RenderTexture2D{}; }
    if (previewShader_.id) { UnloadShader(previewShader_);  previewShader_ = Shader{}; }
    if (framebufferId_)    { rlUnloadFramebuffer(framebufferId_); framebufferId_ = 0; }
    if (arrayId_) {
        glDeleteTextures(1, &arrayId_);
        arrayId_ = 0;
    }
    /* The depth renderbuffer was an attachment of the framebuffer just
     * deleted, which frees it. */
    depthBufferId_ = 0;
}

bool ReflectionProbeSet::create()
{
    releaseResources();

    /* GL 4.0 for GL_TEXTURE_CUBE_MAP_ARRAY. The context is asked for 4.3 in
     * CMakeLists; checking rather than assuming, because the failure mode of
     * assuming is a GL_INVALID_ENUM per call and six blank faces. */
    if (!GLAD_GL_VERSION_4_0) {
        TraceLog(LOG_WARNING,
                 "PROBES: no GL 4.0 - no cubemap arrays, surfaces keep the analytic sky");
        return false;
    }

    glGenTextures(1, &arrayId_);
    if (arrayId_ == 0) {
        TraceLog(LOG_WARNING, "PROBES: no array texture - surfaces keep the analytic sky");
        return false;
    }

    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);

    /* RGBA16F, and the depth of a cube array is layers x 6 — every face of
     * every probe is one slice. Null data is legal here in a way it was not
     * for rlLoadTextureCubemap, which is the one nicety of dropping to raw GL.
     *
     * Alpha carries COVERAGE exactly as the single probe's did: faces clear to
     * transparent black and lit geometry writes 1, so the shader can tell
     * "world in this direction" from "open sky" and fall back to the analytic
     * sky gradient for the latter. */
    glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA16F,
                 kFaceSize, kFaceSize, kMaxProbes * 6, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);

    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);

    /* Filters ACROSS face boundaries rather than clamping at them. Without it
     * every probe shows three seams meeting at each corner, which on a smooth
     * surface reads as a crack in the reflected world. It is context state,
     * not texture state, so it is set once and applies to every cubemap the
     * frame samples — including the single-probe path, which wanted it too. */
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    framebufferId_ = rlLoadFramebuffer();
    if (framebufferId_ == 0) {
        TraceLog(LOG_WARNING, "PROBES: no framebuffer - surfaces keep the analytic sky");
        releaseResources();
        return false;
    }

    /* One shared depth renderbuffer for every face of every probe: it is
     * needed WHILE a face renders and never read afterwards, and the faces are
     * drawn one at a time. */
    rlEnableFramebuffer(framebufferId_);
    depthBufferId_ = rlLoadTextureDepth(kFaceSize, kFaceSize, true);
    rlFramebufferAttach(framebufferId_, depthBufferId_, RL_ATTACHMENT_DEPTH,
                        RL_ATTACHMENT_RENDERBUFFER, 0);

    /* Layer 0 attached purely so completeness can be asked once here rather
     * than discovered on the first capture. */
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arrayId_, 0, 0);
    const bool complete = rlFramebufferComplete(framebufferId_);
    rlDisableFramebuffer();

    if (!complete) {
        TraceLog(LOG_WARNING, "PROBES: incomplete framebuffer - keeping the analytic sky");
        releaseResources();
        return false;
    }

    constexpr int kPreviewFace = 128;
    preview_ = LoadRenderTexture(kPreviewFace * 6, kPreviewFace);
    previewShader_ = ShaderLibrary::load(nullptr, "cubemap_preview.fs.glsl");
    if (previewShader_.id != 0) {
        const int unit = 1;
        const int location = GetShaderLocation(previewShader_, "uCubemap");
        if (location >= 0) SetShaderValue(previewShader_, location, &unit, SHADER_UNIFORM_INT);
        locPreviewLayer_ = GetShaderLocation(previewShader_, "uLayer");
    } else {
        TraceLog(LOG_WARNING, "PROBES: no preview shader - the probes cannot be inspected");
    }

    TraceLog(LOG_INFO, "PROBES: %dx%d HDR cubemap array, %d layers",
             kFaceSize, kFaceSize, kMaxProbes);
    return true;
}

void ReflectionProbeSet::bindTo(int textureUnit) const
{
    rlActiveTextureSlot(textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);
    rlActiveTextureSlot(0);
}

void ReflectionProbeSet::unbindFrom(int textureUnit)
{
    rlActiveTextureSlot(textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    rlActiveTextureSlot(0);
}

void ReflectionProbeSet::build(const RoomPartition& rooms, const Lattice& lattice,
                               Vector3 worldMinimum, Vector3 worldMaximum)
{
    probes_.clear();
    if (!valid()) return;

    captureFar_ = Vector3Length(Vector3Subtract(worldMaximum, worldMinimum)) + 1.0f;

    /* Rooms are placed largest-first so that if a map has more than kMaxProbes
     * of them, the ones dropped are the smallest — a broom cupboard falling
     * back to its parent volume's reflection is a far better failure than the
     * main hall doing so. */
    std::vector<int> order;
    for (int i = 0; i < rooms.roomCount(); i++) {
        /* A room of one or two cells is a doorway recess or the inside of a
         * stack of cover, not a space with its own environment. Placing a
         * probe there spends a layer to describe six walls. */
        if (rooms.rooms()[static_cast<std::size_t>(i)].cellCount < 4) continue;
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return rooms.rooms()[static_cast<std::size_t>(a)].cellCount >
               rooms.rooms()[static_cast<std::size_t>(b)].cellCount;
    });

    if (static_cast<int>(order.size()) > kMaxProbes) {
        TraceLog(LOG_WARNING, "PROBES: %d rooms, %d layers - dropping the %d smallest",
                 static_cast<int>(order.size()), kMaxProbes,
                 static_cast<int>(order.size()) - kMaxProbes);
        order.resize(kMaxProbes);
    }

    for (int index : order) {
        const RoomVolume& room = rooms.rooms()[static_cast<std::size_t>(index)];

        ProbeVolume probe;
        probe.interior = !room.outdoor;

        cellBox(room.minimum, room.maximum, probe.parallaxMin, probe.parallaxMax);

        /* THE CAPTURE POINT IS NOT THE BOX CENTRE. It is the middle of an OPEN
         * cell near the middle of the room — RoomPartition picked it, because
         * the centroid of an L-shaped room is inside the wall between its
         * arms, and a capture from inside solid geometry is six black faces
         * that fail silently. */
        probe.capture = Vector3{ static_cast<float>(room.capture.x) + 0.5f,
                                 Lattice::cellBaseHeight(room.capture.z) + kCellHeight * 0.5f,
                                 static_cast<float>(room.capture.y) + 0.5f };

        if (room.outdoor) {
            /* THE OUTDOOR PROBE IS THE FALLBACK, so its influence is the whole
             * board rather than the flooded cells. The flood stops at every
             * building's outer wall, but a fragment ON that wall's exterior
             * face sits fractionally OUTSIDE the open cells — and if no
             * influence volume contained it, the facade would drop to the
             * analytic sky and read as unlit next to its own street.
             *
             * Its parallax box stays the world, which is what the single probe
             * always did and is correct for the one volume that really is
             * board-sized. */
            probe.parallaxMin  = worldMinimum;
            probe.parallaxMax  = worldMaximum;
            probe.influenceMin = worldMinimum;
            probe.influenceMax = worldMaximum;

            /* Lowest priority by construction: it is the biggest box, so any
             * interior containing a fragment wins over it. */
            probe.priority   = boxVolume(worldMinimum, worldMaximum);
            probe.transition = 0.0f;   /* nothing to fade INTO; it is the floor */
        } else {
            /* INFLUENCE IS EXACTLY THE ROOM'S CELLS — no margin, deliberately.
             *
             * Growing the box was tried, to stop an interior wall face landing
             * ambiguously on the boundary it sits astride. It fixes that and
             * causes something worse: a wall is 0.09 thick and centred on the
             * cell boundary, so a margin big enough to capture the interior
             * face also captures the EXTERIOR one, and the room starts
             * claiming fragments on the street. That is the leak this whole
             * change exists to close, arriving from the other direction.
             *
             * The shader disambiguates by stepping along the surface normal
             * before it tests — a surface belongs to the volume it faces. See
             * probeSamplePoint in environment.glsl. With that, exact bounds are
             * both correct and the only thing that stays correct when somebody
             * changes how thick a wall is. */
            probe.influenceMin = probe.parallaxMin;
            probe.influenceMax = probe.parallaxMax;

            probe.priority = boxVolume(probe.influenceMin, probe.influenceMax);

            /* Shorter than the shader's normal step, so a wall face lands at
             * full weight rather than part-blended with the street: the step
             * is 0.25 and the thickest surface puts a face 0.08 off the
             * boundary, leaving 0.17 of clearance for a 0.15 band. What the
             * band is really for is a soldier walking through a doorway, and
             * 0.15 of a tile is enough to make that a crossfade. */
            probe.transition = 0.15f;
        }

        /* PER ROOM, not just a total. "4 rooms" is true of a correct partition
         * and of one that split the street into quarters, and the difference
         * between those is the whole feature. The bounds and the capture point
         * are what make it checkable against the map. */
        TraceLog(LOG_INFO,
                 "PROBES:   [%d] %s %d cells, %.0f..%.0f x %.1f..%.1f x %.0f..%.0f, from (%.1f %.1f %.1f)",
                 static_cast<int>(probes_.size()), room.outdoor ? "outdoor " : "interior",
                 room.cellCount,
                 probe.parallaxMin.x, probe.parallaxMax.x,
                 probe.parallaxMin.y, probe.parallaxMax.y,
                 probe.parallaxMin.z, probe.parallaxMax.z,
                 probe.capture.x, probe.capture.y, probe.capture.z);

        probes_.push_back(probe);
    }

    (void)lattice;   /* placement is entirely in world space; the lattice is
                      * only here so callers cannot forget which one the
                      * partition was flooded from */

    if (previewProbe_ >= probeCount()) previewProbe_ = 0;
    cursor_ = 0;
    markAllStale();

    TraceLog(LOG_INFO, "PROBES: %d rooms placed (%d interior)", probeCount(),
             static_cast<int>(std::count_if(probes_.begin(), probes_.end(),
                                            [](const ProbeVolume& p) { return p.interior; })));
}

void ReflectionProbeSet::capture(const std::function<void(Vector3 eye)>& drawScene,
                                 int faceCount)
{
    if (!valid() || !drawScene) return;
    if (probes_.empty()) return;
    if (faceCount <= 0) return;

    const int pairCount = probeCount() * 6;
    faceCount = std::min(faceCount, pairCount);

    const Matrix projection = MatrixPerspective(90.0 * DEG2RAD, 1.0, 0.05,
                                                static_cast<double>(captureFar_));

    rlEnableFramebuffer(framebufferId_);
    rlViewport(0, 0, kFaceSize, kFaceSize);
    rlEnableDepthTest();
    rlEnableDepthMask();

    /* CULLING OFF. A cubemap's faces are defined in a LEFT-handed space, so a
     * view matrix built for one flips handedness — and flipping handedness
     * reverses triangle winding, turning every front face into a back face the
     * driver then discards. The framebuffer is complete, the draws are
     * submitted, the log is clean, and nothing lands. Not culling is correct
     * whether the handedness argument holds on every face or not. */
    rlDisableBackfaceCulling();

    for (int step = 0; step < faceCount; step++) {
        const int pair  = cursor_ % pairCount;
        const int probe = pair / 6;
        const int face  = pair % 6;
        cursor_ = (cursor_ + 1) % pairCount;

        /* DRAINED FIRST, or the check below is not a check. glGetError reports
         * the OLDEST error still queued and clears one entry — so an error
         * left behind by any earlier GL call in the frame would be read here
         * and blamed on the attach. Emptying the queue immediately before the
         * call is what makes the next glGetError describe THIS call. */
        if (!attachChecked_)
            while (glGetError() != GL_NO_ERROR) { }

        /* THE LAYER-FACE INDEX, which is what a cube array attachment wants:
         * one flat slice number, not a (layer, face) pair. */
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  arrayId_, 0, probe * 6 + face);

        /* CHECKED, ONCE, AND LOUDLY. A rejected attach does not clear the
         * colour attachment — it leaves whatever was there, which is layer 0
         * from create(). Every probe would then capture onto layer 0, every
         * interior would sample an empty layer and fall back to the sky, and
         * the one populated layer would hold whichever room was captured last.
         * That failure looks exactly like "the reflections are wrong" and
         * gives no hint that the write, not the read, is at fault. */
        if (!attachChecked_) {
            attachChecked_ = true;
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR)
                TraceLog(LOG_WARNING, "PROBES: layer attach rejected (GL 0x%04x) - "
                                      "every probe is writing to layer 0", error);
            else if (!rlFramebufferComplete(framebufferId_))
                TraceLog(LOG_WARNING, "PROBES: layered framebuffer incomplete");
            /* rlFramebufferComplete binds and unbinds, so put the target back
             * before anything is drawn into it. */
            rlEnableFramebuffer(framebufferId_);
        }

        rlViewport(0, 0, kFaceSize, kFaceSize);

        /* PER FACE, not once around the loop. The OPAQUE pass replaces and
         * must not composite — but drawScene draws glass afterwards, and that
         * pass turns blending back on because a window has to composite over
         * what is behind it or its coverage alpha lands in the cubemap as a
         * sky-bright hole. See the glass block in Application::drawGeometryLit.
         * Resetting here is what stops the previous face leaving it on. */
        rlDisableColorBlend();

        /* TRANSPARENT black, not a sky colour — alpha is how the shader tells
         * "world in this direction" from "open sky". */
        rlClearColor(0, 0, 0, 0);
        rlClearScreenBuffers();

        const CubeFace& current = kFaces[face];
        const Vector3 eye = probes_[static_cast<std::size_t>(probe)].capture;

        rlSetMatrixProjection(projection);
        rlSetMatrixModelview(MatrixLookAt(eye, Vector3Add(eye, current.forward), current.up));

        drawScene(eye);

        /* Anything the caller left batched belongs to THIS face. */
        rlDrawRenderBatchActive();

        if (staleFaces_ > 0) staleFaces_--;
    }

    rlEnableBackfaceCulling();
    rlEnableColorBlend();
    rlDisableDepthTest();
    rlDisableFramebuffer();

    rlSetMatrixProjection(MatrixIdentity());
    rlSetMatrixModelview(MatrixIdentity());

    updatePreview();

    /* The caller's own pass owns the viewport from here: restoring it to the
     * window is wrong when the scene target is supersampled, so whoever calls
     * capture() re-establishes it by entering its own target. */
}

void ReflectionProbeSet::setPreviewProbe(int index)
{
    if (probes_.empty()) { previewProbe_ = 0; return; }
    previewProbe_ = std::clamp(index, 0, probeCount() - 1);
}

void ReflectionProbeSet::updatePreview()
{
    if (preview_.id == 0 || previewShader_.id == 0) return;

    BeginTextureMode(preview_);
    BeginShaderMode(previewShader_);

    if (locPreviewLayer_ >= 0) {
        const float layer = static_cast<float>(previewProbe_);
        SetShaderValue(previewShader_, locPreviewLayer_, &layer, SHADER_UNIFORM_FLOAT);
    }

    /* Bound by hand, and NOT with SetShaderValueTexture: that registers a 2D
     * texture with rlgl's batch, which would rebind unit 1 as GL_TEXTURE_2D
     * and leave the sampler reading nothing. */
    rlActiveTextureSlot(1);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);

    DrawRectangle(0, 0, preview_.texture.width, preview_.texture.height, WHITE);
    rlDrawRenderBatchActive();          /* flush while the array is still bound */

    rlActiveTextureSlot(1);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    rlActiveTextureSlot(0);

    EndShaderMode();
    EndTextureMode();
}

}  // namespace xcom
