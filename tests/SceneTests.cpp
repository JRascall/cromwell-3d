/* SceneTests.cpp — the render scene, minus the rendering.
 *
 * WHY THIS CAN BE A TEST AT ALL is the reason RenderScene.cpp lives in
 * cromwell_base: storage, frustum culling, the per-view filters and the two
 * sorts are arithmetic over boxes and integers. None of it touches a graphics
 * API, so all of it can be asserted about without opening a window — and it is
 * the code most worth asserting about, because every one of its failures is an
 * ABSENCE. A renderable that is wrongly culled produces no error, no warning
 * and no wrong pixel: it produces a hole in the world at one camera angle, and
 * whoever finds it starts by suspecting the geometry.
 *
 * WHAT IS DELIBERATELY NOT HERE: whether the picture is right. That is a
 * screenshot in front of a human, and `--shot` is how it is taken.
 *
 * The four things below each guard a specific, already-identified mistake:
 *
 *   THE FILTER'S SENSE. The cutaway has two independent axes and a surface must
 *   pass BOTH. Under the obvious show-if-any-match design a wall on a hidden
 *   storey whose facing is shown still draws, which is a storey cut that leaks
 *   the moment the facing cut is used. See Renderable.hpp.
 *
 *   THE DERIVED VIEW. The sun's view and a probe face's must carry the viewer
 *   and DROP the cutaway. Getting it the other way round made the lighting
 *   change when the player changed floor — the episode CutawayView.hpp records.
 *
 *   THE GENERATION. A stale id must address nothing rather than whatever took
 *   the slot, or a removed overlay's id silently moves a live soldier.
 *
 *   THE TRANSLUCENT ORDER. Back to front, per view. This is the bug §4.12 calls
 *   "the limitation most likely to force the issue", and it is the one thing
 *   the old architecture could not have fixed at all.
 */
#include "cromwell/material/IMaterialQuery.hpp"
#include "cromwell/render/RenderScene.hpp"
#include "cromwell/render/View.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <cstdio>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* A DEVICE THAT MAKES NOTHING. The scene only needs one for the reflection
 * probe set it owns, and nothing here calls initialise(), so every method is
 * unreachable — it exists to satisfy a reference. */
class NullDevice final : public rhi::IRenderDevice {
public:
    const rhi::DeviceCapabilities& capabilities() const override { return caps_; }

    rhi::TextureHandle  createTexture(const rhi::TextureDesc&) override { return {}; }
    rhi::SamplerHandle  createSampler(const rhi::SamplerDesc&) override { return {}; }
    rhi::BufferHandle   createBuffer(const rhi::BufferDesc&) override { return {}; }
    rhi::PipelineHandle createPipeline(const rhi::PipelineDesc&) override { return {}; }
    rhi::ShaderHandle   createShader(const char*, const char*, const char*) override { return {}; }
    rhi::ShaderHandle   createComputeShader(const char*, const char*) override { return {}; }
    rhi::MeshHandle     createMesh(const rhi::VertexLayout&, rhi::BufferHandle, uint32_t,
                                   rhi::BufferHandle, uint32_t) override { return {}; }

    void destroy(rhi::TextureHandle) override {}
    void destroy(rhi::SamplerHandle) override {}
    void destroy(rhi::BufferHandle) override {}
    void destroy(rhi::PipelineHandle) override {}
    void destroy(rhi::ShaderHandle) override {}
    void destroy(rhi::MeshHandle) override {}

    void updateBuffer(rhi::BufferHandle, const void*, uint64_t, uint64_t) override {}
    void updateTexture(rhi::TextureHandle, const void*, uint32_t, uint32_t) override {}
    void generateMips(rhi::TextureHandle) override {}

    rhi::ICommandEncoder& beginPass(const rhi::PassDesc&) override { return *encoder_; }
    void endPass(rhi::ICommandEncoder&) override {}
    rhi::ICommandEncoder* beginCompute(const char*) override { return nullptr; }
    void present() override {}

    bool readTexture(rhi::TextureHandle, uint32_t, uint32_t, uint32_t, uint32_t,
                     std::vector<uint8_t>&, uint32_t) override { return false; }
    bool copyBackbufferToTexture(rhi::TextureHandle, uint32_t, uint32_t,
                                 uint32_t, uint32_t) override { return false; }
    void backbufferSize(uint32_t& width, uint32_t& height) const override
    {
        width = 0;
        height = 0;
    }
    void setBackbufferSize(uint32_t, uint32_t) override {}

private:
    rhi::DeviceCapabilities caps_;
    rhi::ICommandEncoder*   encoder_ = nullptr;
};

/* EVERY MATERIAL IS OPAQUE EXCEPT ONE, so the two buckets can be told apart
 * without dragging in the real table — which cannot be linked here, and whose
 * absence is the whole reason IMaterialQuery is an interface. */
class TestMaterials final : public IMaterialQuery {
public:
    static constexpr std::uint32_t kGlass = 7;

    bool isTranslucent(MaterialId material) const override
    {
        return material.value == kGlass;
    }
};

Aabb unitBoxAt(float x, float y, float z)
{
    return Aabb{ Vec3{ x - 0.5f, y - 0.5f, z - 0.5f }, Vec3{ x + 0.5f, y + 0.5f, z + 0.5f } };
}

/* A CAMERA LOOKING DOWN -Z FROM THE ORIGIN. Everything below is placed in front
 * of it or behind it, so "is it culled" has an obvious right answer. */
View cameraLookingDownMinusZ(RenderScene& scene)
{
    View view;
    view.withScene(scene)
        .withKind(ViewKind::Camera)
        .withEye(Mat4::lookAt(Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, -1.0f },
                              Vec3{ 0.0f, 1.0f, 0.0f }),
                 Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f),
                 Vec3{ 0.0f, 0.0f, 0.0f });
    return view;
}

RenderableDesc boxAt(float x, float y, float z, rhi::MeshHandle mesh)
{
    return RenderableDesc()
        .withMesh(mesh, unitBoxAt(0.0f, 0.0f, 0.0f))
        .withTransform(Mat4::translation(Vec3{ x, y, z }));
}

/* ---- 1. the frustum actually rejects something --------------------------
 *
 * AND THE FAILURE THIS GUARDS IS "IT REJECTS NOTHING". A culler that always
 * answers yes is invisible: the frame is correct and merely slower, so it
 * survives every visual check ever made of it. The count is what catches it. */
void testFrustumCullsWhatIsBehindTheEye()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle mesh{ 1 };

    scene.add(boxAt(0.0f, 0.0f, -5.0f, mesh));    /* in front  */
    scene.add(boxAt(0.0f, 0.0f, 20.0f, mesh));    /* behind    */
    scene.add(boxAt(0.0f, 0.0f, 40.0f, mesh));    /* well behind */

    SceneDrawList list;
    scene.collect(cameraLookingDownMinusZ(scene), list);

    CHECK(list.opaque().size() == 1, "one box is in front of the eye, %zu collected",
          list.opaque().size());
    CHECK(list.culled == 2, "two boxes are behind the eye, %d culled", list.culled);
}

/* ---- 2. the two filter axes compose ------------------------------------
 *
 * THE ONE THAT WOULD HAVE SHIPPED WRONG. §4.12's first draft proposed a filter
 * KEY on the renderable ANDed against a MASK on the view, drawn where non-zero.
 * That is show-if-any-match, and it cannot express "hidden storey AND shown
 * facing" — the wall draws, and the storey cut leaks exactly when the facing
 * cut is in use, which is every frame of an ordinary camera angle. */
void testHiddenFlagsAreConjunctiveAcrossAxes()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle mesh{ 1 };

    constexpr FilterFlags kStorey1 = 1u << 1;
    constexpr FilterFlags kStorey2 = 1u << 2;
    constexpr FilterFlags kFacingNorth = 1u << 10;
    constexpr FilterFlags kFacingSouth = 1u << 11;

    /* On the hidden storey, facing the shown way. Under the rejected design
     * this is the one that wrongly draws. */
    scene.add(boxAt(0.0f, 0.0f, -5.0f, mesh).withFilterFlags(kStorey2 | kFacingSouth));

    /* On a shown storey, facing the shown way: the only one that should draw. */
    scene.add(boxAt(1.0f, 0.0f, -5.0f, mesh).withFilterFlags(kStorey1 | kFacingSouth));

    /* On a shown storey, facing the hidden way. */
    scene.add(boxAt(-1.0f, 0.0f, -5.0f, mesh).withFilterFlags(kStorey1 | kFacingNorth));

    View view = cameraLookingDownMinusZ(scene);
    view.withHiddenFlags(kStorey2 | kFacingNorth);

    SceneDrawList list;
    scene.collect(view, list);

    CHECK(list.opaque().size() == 1,
          "only the shown storey with the shown facing may draw, %zu drew",
          list.opaque().size());
}

/* ---- 3. a derived view drops the cutaway and keeps the viewer -----------
 *
 * BOTH HALVES MATTER AND THEY FAIL DIFFERENTLY. A derived view that copied the
 * hidden flags would put the camera's storey cut into the sun's depth pass, and
 * the room below the cut would jump to full sunlight — the lighting becoming a
 * function of where the camera was cut. One that dropped the viewer would put
 * one player's overlays into another player's shadow map. */
void testDerivedViewDropsTheCutawayAndKeepsTheViewer()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle mesh{ 1 };
    constexpr FilterFlags kHiddenStorey = 1u << 3;

    /* A caster on the storey the CAMERA has cut away. The sun must still see
     * it: a wall on the floor above still shadows the street. */
    scene.add(boxAt(0.0f, 0.0f, -5.0f, mesh)
                  .withFilterFlags(kHiddenStorey)
                  .withViewers(viewerBit(1)));

    /* And something that does not cast at all — a marker, an overlay. */
    scene.add(boxAt(1.0f, 0.0f, -5.0f, mesh)
                  .withViewers(viewerBit(1))
                  .withCastsShadow(false));

    View camera = cameraLookingDownMinusZ(scene);
    camera.withHiddenFlags(kHiddenStorey).withViewer(1);

    SceneDrawList cameraList;
    scene.collect(camera, cameraList);
    CHECK(cameraList.opaque().size() == 1,
          "the camera hides the cut storey, %zu drew", cameraList.opaque().size());

    const View sun = camera.derived(ViewKind::Sun, camera.viewMatrix(),
                                    camera.projectionMatrix(), camera.position());

    SceneDrawList sunList;
    scene.collect(sun, sunList);

    /* The cut caster is back; the non-caster is not. */
    CHECK(sunList.opaque().size() == 1,
          "the sun ignores the cutaway and takes only casters, %zu drew",
          sunList.opaque().size());
    CHECK(sun.hiddenFlags() == kNoFilterFlags, "a derived view must hide nothing");
    CHECK(sun.viewerMask() == viewerBit(1), "a derived view must keep its viewer");

    /* AND THE VIEWER IS LOAD-BEARING: another player's view sees neither. */
    View other = cameraLookingDownMinusZ(scene);
    other.withViewer(2);

    SceneDrawList otherList;
    scene.collect(other, otherList);
    CHECK(otherList.opaque().empty(),
          "player two owns none of these, %zu drew", otherList.opaque().size());
}

/* ---- 4. a stale id addresses nothing ------------------------------------
 *
 * The slot is recycled deliberately — indices stay stable and ids stay
 * meaningful — so the generation is the only thing standing between a stale id
 * and a live renderable. Without it, a removed overlay's id moves a soldier. */
void testStaleIdsAreIgnoredAfterTheSlotIsReused()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle mesh{ 1 };

    const RenderableId first = scene.add(boxAt(0.0f, 0.0f, -5.0f, mesh));
    CHECK(first.valid(), "a well-formed renderable must register");

    scene.remove(first);

    const RenderableId second = scene.add(boxAt(0.0f, 0.0f, -5.0f, mesh));
    CHECK(second.index == first.index, "the freed slot should be reused");
    CHECK(second.generation != first.generation, "and must carry a new generation");

    /* The stale id must do NOTHING rather than move the new renderable. */
    scene.setVisible(first, false);

    SceneDrawList list;
    scene.collect(cameraLookingDownMinusZ(scene), list);
    CHECK(list.opaque().size() == 1,
          "a stale id must not hide the renderable that took its slot, %zu drew",
          list.opaque().size());

    /* And a renderable that could never draw is refused rather than registered
     * and silently skipped forever — the opposite of Source's
     * RENDER_GROUP_OTHER, which is "unclassified, won't get drawn". */
    CHECK(!scene.add(RenderableDesc()).valid(), "a renderable with no mesh must be refused");
}

/* ---- 5. translucent draws back to front, per view -----------------------
 *
 * THE BUG THE WHOLE DESIGN WAS MOST LIKELY TO BE FORCED BY. The old path drew
 * the transparent bucket in BUCKET order, so two overlapping panes blended in
 * whatever order the geometry happened to be built in — and nothing in that
 * architecture could fix it, because the engine did not own the draws.
 *
 * PER VIEW is the other half, and it is what Source carries
 * `m_TranslucencyCalculatedView` for: two split-screen panes see the same pane
 * of glass at two different depths. Here the distance is computed during
 * collection and lives in the per-view list, so the second view below must come
 * back with the opposite order rather than with a cached answer. */
void testTranslucentSortsBackToFrontForEachView()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle near { 1 };
    const rhi::MeshHandle far  { 2 };
    const MaterialId glass{ TestMaterials::kGlass };

    scene.add(boxAt(0.0f, 0.0f, -4.0f, near).withMaterial(glass));
    scene.add(boxAt(0.0f, 0.0f, -9.0f, far).withMaterial(glass));

    SceneDrawList list;
    scene.collect(cameraLookingDownMinusZ(scene), list);

    CHECK(list.opaque().empty(), "glass belongs in the other bucket");
    CHECK(list.translucent().size() == 2, "both panes are in front of the eye");

    if (list.translucent().size() == 2) {
        CHECK(list.translucent()[0].mesh == far,
              "the furthest pane must be drawn first");
        CHECK(list.translucent()[1].mesh == near,
              "and the nearest last");
    }

    /* THE SAME SCENE FROM THE OTHER SIDE. A cached distance on the renderable
     * would hand back the same order and be wrong for this eye. */
    View behind;
    behind.withScene(scene)
          .withKind(ViewKind::Camera)
          .withEye(Mat4::lookAt(Vec3{ 0.0f, 0.0f, -20.0f }, Vec3{ 0.0f, 0.0f, 0.0f },
                                Vec3{ 0.0f, 1.0f, 0.0f }),
                   Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f),
                   Vec3{ 0.0f, 0.0f, -20.0f });

    SceneDrawList reversed;
    scene.collect(behind, reversed);

    CHECK(reversed.translucent().size() == 2, "both panes are visible from here too");
    if (reversed.translucent().size() == 2) {
        CHECK(reversed.translucent()[0].mesh == near,
              "from the far side the order must invert");
        CHECK(reversed.translucent()[1].mesh == far,
              "or the sort was cached on the renderable rather than per view");
    }
}

/* ---- 6. opaque draws biggest first --------------------------------------
 *
 * Source's render groups lead with OPAQUE_STATIC_HUGE so the largest things
 * occlude and the depth test rejects more of what follows. It is a free early-z
 * win from ordering alone, it costs one comparison in a sort that was happening
 * anyway, and it is not something this design would have thought of. */
void testOpaqueDrawsTheBiggestFirst()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    const rhi::MeshHandle small{ 1 };
    const rhi::MeshHandle huge { 2 };

    scene.add(RenderableDesc()
                  .withMesh(small, unitBoxAt(0.0f, 0.0f, 0.0f))
                  .withTransform(Mat4::translation(Vec3{ 0.0f, 0.0f, -8.0f })));

    scene.add(RenderableDesc()
                  .withMesh(huge, Aabb{ Vec3{ -8.0f, -8.0f, -8.0f }, Vec3{ 8.0f, 8.0f, 8.0f } })
                  .withTransform(Mat4::translation(Vec3{ 0.0f, 0.0f, -20.0f })));

    SceneDrawList list;
    scene.collect(cameraLookingDownMinusZ(scene), list);

    CHECK(list.opaque().size() == 2, "both are in front of the eye, %zu collected",
          list.opaque().size());
    if (list.opaque().size() == 2)
        CHECK(list.opaque()[0].mesh == huge, "the huge one must be drawn first");
}

/* ---- 7. the world's extent is the union of what is registered ----------- */
void testWorldBoundsAreTheUnionAndEmptyWhenNothingIsRegistered()
{
    NullDevice device;
    TestMaterials materials;
    RenderScene scene(device, materials);

    /* AN EMPTY SCENE REPORTS AN EMPTY BOX rather than a point at the origin.
     * The shadow pass checks for exactly this and skips, where a point would
     * frame the sun's projection around nothing and produce a NaN matrix. */
    CHECK(scene.worldBounds().empty(), "an empty scene must report an empty box");

    const rhi::MeshHandle mesh{ 1 };
    scene.add(boxAt(0.0f, 0.0f, 0.0f, mesh));
    scene.add(boxAt(10.0f, 4.0f, -6.0f, mesh));

    const Aabb bounds = scene.worldBounds();
    CHECK(!bounds.empty(), "two renderables must produce a box");
    CHECK(bounds.min.x <= -0.5f && bounds.max.x >= 10.5f, "the union must span both in x");
    CHECK(bounds.min.z <= -6.5f && bounds.max.z >= 0.5f, "and in z");

    /* AND IT MUST SHRINK AGAIN. A union has no cheap incremental update
     * precisely because removal is the hard direction, so this is the case a
     * grow-only implementation passes every other test while failing. */
    scene.clear();
    CHECK(scene.worldBounds().empty(), "a cleared scene must report an empty box again");
}

}  // namespace

int main()
{
    testFrustumCullsWhatIsBehindTheEye();
    testHiddenFlagsAreConjunctiveAcrossAxes();
    testDerivedViewDropsTheCutawayAndKeepsTheViewer();
    testStaleIdsAreIgnoredAfterTheSlotIsReused();
    testTranslucentSortsBackToFrontForEachView();
    testOpaqueDrawsTheBiggestFirst();
    testWorldBoundsAreTheUnionAndEmptyWhenNothingIsRegistered();

    if (g_failures == 0) std::printf("SceneTests: all checks passed\n");
    else                 std::printf("SceneTests: %d check(s) failed\n", g_failures);

    return g_failures == 0 ? 0 : 1;
}
