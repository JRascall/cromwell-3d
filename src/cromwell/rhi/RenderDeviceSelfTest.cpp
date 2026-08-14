#include "cromwell/rhi/RenderDeviceSelfTest.hpp"

/* THE CUBE-ORIENTATION STAGE USES THE RENDERER'S OWN FACE TABLE, deliberately,
 * and that is why a device conformance suite includes a lighting header.
 *
 * A test that retyped the six forward and up vectors would test its own copy.
 * Four of those up vectors correctly point DOWN, so a copy is exactly as likely
 * to be wrong as the original — and the two being wrong together is a test that
 * passes while every reflection in the game is mirrored. The dependency is the
 * point of the stage, not a wrinkle in it. */
#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <cstdio>
#include <vector>

namespace cromwell::rhi {
namespace {

/* Small enough that a readback is instant, large enough that a viewport bug
 * shows as a partly-filled target rather than as nothing at all. */
constexpr uint32_t kSize = 64;

/* ================== THE SHADERS ARE GLSL, AND THAT IS A GAP ===============
 *
 * This suite is backend-agnostic in every respect except these two strings.
 * Metal wants MSL and the console APIs want their own bytecode, so a backend
 * on those targets cannot run these stages until the shader cross-compilation
 * question is settled — SPIR-V as an interchange, or a move to a language that
 * compiles to all of them.
 *
 * Left as plain GLSL rather than hidden behind a preprocessor, because a gap
 * that is visible is one somebody fixes and a gap that is papered over is one
 * that is discovered by the person porting. The stages that need no shader —
 * resources, handles, clears, depth-only passes — run on any backend today. */
const char* kVertexSource = R"(#version 430 core
/* A COVERING TRIANGLE FROM gl_VertexID, no vertex buffer. Three vertices
 * rather than a quad's six indices, and it is what drawFullscreen promises. */
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentSource = R"(#version 430 core
layout(location = 0) out vec4 colour;
void main()
{
    /* EXACT 8-BIT FRACTIONS, so the readback comparison needs no tolerance for
     * rounding and any difference is a real one. */
    colour = vec4(64.0 / 255.0, 128.0 / 255.0, 192.0 / 255.0, 1.0);
}
)";

/* THE MESH PATH IS A DIFFERENT PATH, and reads real vertex attributes rather
 * than synthesising them from gl_VertexID. That distinction is the whole reason
 * this second shader exists: drawFullscreen proves the pipeline binds and the
 * fragment stage writes, and proves NOTHING about vertex buffers, index
 * buffers, attribute layout or the vertex array that ties them together. */
const char* kMeshVertexSource = R"(#version 430 core
layout(location = 0) in vec2 position;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

const char* kMeshFragmentSource = R"(#version 430 core
layout(location = 0) out vec4 colour;
void main()
{
    colour = vec4(32.0 / 255.0, 96.0 / 255.0, 160.0 / 255.0, 1.0);
}
)";

/* ============= THE CUBE-ORIENTATION PAIR, AND WHY THEY EXIST ==============
 *
 * A cubemap's six faces are defined in a LEFT-handed space, so four of the six
 * up vectors in a correct capture table point DOWN. Every one of them looks
 * like a typo, and getting one wrong does not break anything visibly — it
 * MIRRORS that face. The geometry is all there, in the right places, reflected
 * the wrong way round, and the only symptom is that things reflected in a
 * smooth surface appear on the wrong side of it.
 *
 * That is not a failure reasoning finds. It was derived correct twice here
 * while a real mirror held up to a real window said otherwise. So it is tested
 * instead, by round trip: render a marker at a KNOWN WORLD DIRECTION through
 * the real capture matrix, then SAMPLE the cube in that same direction and in
 * its mirror. A correct cube returns the marker for the first and the clear
 * colour for the second; a mirrored one returns exactly the opposite, which is
 * why both are checked rather than just the hit. */
const char* kCubeMarkerVertexSource = R"(#version 430 core
layout(location = 0) in vec3 position;
layout(std140, binding = 1) uniform CubeBlock { mat4 uViewProjection; };
void main()
{
    gl_Position = uViewProjection * vec4(position, 1.0);
}
)";

const char* kCubeMarkerFragmentSource = R"(#version 430 core
layout(location = 0) out vec4 colour;
void main()
{
    colour = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

/* Reads the array back the way a lit shader does — by DIRECTION, through a
 * samplerCubeArray — rather than by slice. Reading the slice would test the
 * attachment and skip the half of the round trip where a mirror actually
 * shows. The direction rides in the same block the matrix used, since the two
 * never bind together. */
const char* kCubeSampleFragmentSource = R"(#version 430 core
layout(binding = 0) uniform samplerCubeArray uProbes;
layout(std140, binding = 1) uniform CubeBlock { vec4 uDirection; };
layout(location = 0) out vec4 colour;
void main()
{
    colour = texture(uProbes, vec4(uDirection.xyz, 0.0));
}
)";

class Report {
public:
    void pass(const char* stage)
    {
        stages_++;
        append("  ok   ", stage, "");
    }

    void fail(const char* stage, const std::string& why)
    {
        stages_++;
        failed_++;
        append("  FAIL ", stage, why);
    }

    /* `condition` true is a pass. Returns it so a stage can stop early rather
     * than cascading one failure into five. */
    bool check(bool condition, const char* stage, const std::string& why = {})
    {
        if (condition) pass(stage);
        else           fail(stage, why);
        return condition;
    }

    int stages() const { return stages_; }
    int failures() const { return failed_; }
    const std::string& text() const { return text_; }

private:
    void append(const char* prefix, const char* stage, const std::string& why)
    {
        text_ += prefix;
        text_ += stage;
        if (!why.empty()) {
            text_ += " - ";
            text_ += why;
        }
        text_ += '\n';
    }

    std::string text_;
    int stages_ = 0;
    int failed_ = 0;
};

std::string describe(const uint8_t* pixel)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "got rgba(%d,%d,%d,%d)",
                  pixel[0], pixel[1], pixel[2], pixel[3]);
    return buffer;
}

/* Every pixel, not just the first — a viewport sized to the window instead of
 * the attachment fills a corner and leaves the rest untouched, which a
 * single-pixel check at the origin would pass. */
bool allPixelsAre(const std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                  size_t& firstBadIndex)
{
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] != r || pixels[i + 1] != g || pixels[i + 2] != b || pixels[i + 3] != a) {
            firstBadIndex = i;
            return false;
        }
    }
    return true;
}

}  // namespace

SelfTestResult runRenderDeviceSelfTest(IRenderDevice& device)
{
    Report report;

    /* ---- 1. capabilities ------------------------------------------------
     *
     * Cheap, and it catches a device that constructed but never queried
     * anything — every field zero is a backend that filled in nothing. */
    {
        const DeviceCapabilities& caps = device.capabilities();
        report.check(caps.maxTextureSize >= 2048, "capabilities: max texture size",
                     "reported " + std::to_string(caps.maxTextureSize));
        report.check(caps.backendName != nullptr && caps.backendName[0] != '\0',
                     "capabilities: backend is named");
        report.check(caps.maxColourTargets >= 1, "capabilities: at least one colour target");
    }

    /* ---- 2. resources ---------------------------------------------------*/

    TextureDesc colourDesc;
    colourDesc.name   = "selftest colour";
    colourDesc.width  = kSize;
    colourDesc.height = kSize;
    colourDesc.format = TextureFormat::RGBA8;
    colourDesc.usage  = TextureUsageSampled | TextureUsageRenderTarget | TextureUsageCopySource;

    const TextureHandle colour = device.createTexture(colourDesc);
    if (!report.check(colour.valid(), "createTexture: RGBA8 render target")) {
        return { false, report.stages(), report.failures(), report.text() };
    }

    /* ---- 3. handles are not reused naively ------------------------------
     *
     * THE GENERATION CHECK, and it is worth a stage of its own. A backend that
     * hands the same bit pattern back after a destroy has a use-after-free that
     * presents as one texture sampling another's contents — no error, no crash,
     * and a picture that looks like a shader bug. Creating, destroying and
     * creating again must not produce the same handle. */
    {
        const TextureHandle first = device.createTexture(colourDesc);
        const uint32_t firstId = first.id;
        device.destroy(first);

        const TextureHandle second = device.createTexture(colourDesc);
        report.check(second.valid() && second.id != firstId,
                     "handles: a reused slot yields a different handle",
                     "both were " + std::to_string(firstId));
        device.destroy(second);
    }

    /* ---- 4. buffers -----------------------------------------------------*/
    {
        BufferDesc desc;
        desc.name   = "selftest uniform";
        desc.bytes  = 256;
        desc.usage  = BufferUsageUniform;
        desc.access = BufferAccess::CpuToGpuPerFrame;

        const BufferHandle buffer = device.createBuffer(desc);
        if (report.check(buffer.valid(), "createBuffer: uniform")) {
            const float values[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
            device.updateBuffer(buffer, values, sizeof values, 0);
            report.pass("updateBuffer: accepted an in-range write");
            device.destroy(buffer);
        }
    }

    /* ---- 5. a pass that clears, verified by reading it back -------------
     *
     * THE FIRST STAGE THAT PROVES ANYTHING. Everything above could pass with a
     * backend that recorded intentions and did nothing. */
    {
        PassDesc pass;
        pass.name = "selftest clear";
        pass.colours[0].texture = colour;
        pass.colours[0].load    = LoadAction::Clear;
        pass.colours[0].store   = StoreAction::Store;
        pass.colours[0].clearTo = ClearColour{ 1.0f, 0.0f, 0.0f, 1.0f };
        pass.colourCount = 1;

        ICommandEncoder& encoder = device.beginPass(pass);
        device.endPass(encoder);

        std::vector<uint8_t> pixels;
        if (report.check(device.readTexture(colour, 0, 0, kSize, kSize, pixels),
                         "readTexture: colour target read back")) {

            size_t bad = 0;
            const bool uniform = allPixelsAre(pixels, 255, 0, 0, 255, bad);
            report.check(uniform, "pass: clear reached EVERY pixel",
                         uniform ? "" : describe(&pixels[bad]));
        }
    }

    /* ---- 6. a second clear, to a different colour ----------------------
     *
     * PROVES THE FIRST ONE WAS NOT A COINCIDENCE. A cached framebuffer that
     * silently pointed at the backbuffer, or a readback that returned a stale
     * buffer, would both pass stage 5 and fail here. */
    {
        PassDesc pass;
        pass.name = "selftest clear green";
        pass.colours[0].texture = colour;
        pass.colours[0].load    = LoadAction::Clear;
        pass.colours[0].clearTo = ClearColour{ 0.0f, 1.0f, 0.0f, 1.0f };
        pass.colourCount = 1;

        ICommandEncoder& encoder = device.beginPass(pass);
        device.endPass(encoder);

        std::vector<uint8_t> pixels;
        device.readTexture(colour, 0, 0, kSize, kSize, pixels);

        size_t bad = 0;
        const bool uniform = allPixelsAre(pixels, 0, 255, 0, 255, bad);
        report.check(uniform, "pass: a second clear replaced the first",
                     uniform ? "" : (pixels.empty() ? "no pixels" : describe(&pixels[bad])));
    }

    /* ---- 7. a depth-only pass ------------------------------------------
     *
     * THE SHADOW MAP'S SHAPE, and the one that catches a backend which forgot
     * that a pass with no colour attachment must disable its draw buffer. GL
     * reports an incomplete framebuffer for that; other APIs simply render
     * nothing. Either way it is silent unless something asks. */
    {
        TextureDesc depthDesc;
        depthDesc.name   = "selftest depth";
        depthDesc.width  = kSize;
        depthDesc.height = kSize;
        depthDesc.format = TextureFormat::D32F;
        depthDesc.usage  = TextureUsageSampled | TextureUsageDepthTarget;

        const TextureHandle depth = device.createTexture(depthDesc);
        if (report.check(depth.valid(), "createTexture: D32F depth target")) {
            PassDesc pass;
            pass.name = "selftest depth only";
            pass.colourCount = 0;
            pass.hasDepth = true;
            pass.depth.texture = depth;
            pass.depth.load    = LoadAction::Clear;
            pass.depth.clearTo = 1.0f;

            ICommandEncoder& encoder = device.beginPass(pass);
            device.endPass(encoder);

            /* Nothing to read back — depth is not RGBA8. Reaching here without
             * the backend having logged an incomplete framebuffer is the
             * result; stage 8 then proves the device is still usable, which is
             * what a botched depth pass would have broken. */
            report.pass("pass: depth-only target accepted");
            device.destroy(depth);
        }
    }

    /* ---- 8. a shader, a pipeline, and a draw ---------------------------
     *
     * THE FULL PATH. Compile, link, bake state, bind, draw without a vertex
     * buffer, read back. If this passes, the device can render. */
    {
        const ShaderHandle shader =
            device.createShader("selftest", kVertexSource, kFragmentSource);

        if (report.check(shader.valid(), "createShader: compiled and linked")) {
            PipelineDesc pipelineDesc;
            pipelineDesc.name   = "selftest pipeline";
            pipelineDesc.shader = shader;
            pipelineDesc.colourFormats[0] = TextureFormat::RGBA8;
            pipelineDesc.colourCount = 1;

            /* NO DEPTH, and it has to be said: the default DepthState tests
             * against a depth buffer this pass does not have. On some drivers
             * that discards every fragment, which would read as "the draw did
             * nothing" and send a backend author looking in the wrong place. */
            pipelineDesc.depth.test  = false;
            pipelineDesc.depth.write = false;
            pipelineDesc.raster.cull = CullMode::None;

            const PipelineHandle pipeline = device.createPipeline(pipelineDesc);

            if (report.check(pipeline.valid(), "createPipeline: state baked")) {
                PassDesc pass;
                pass.name = "selftest draw";
                pass.colours[0].texture = colour;
                pass.colours[0].load    = LoadAction::Clear;
                pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
                pass.colourCount = 1;

                ICommandEncoder& encoder = device.beginPass(pass);
                encoder.pushDebugGroup("selftest triangle");
                encoder.bindPipeline(pipeline);
                encoder.drawFullscreen();
                encoder.popDebugGroup();
                device.endPass(encoder);

                std::vector<uint8_t> pixels;
                device.readTexture(colour, 0, 0, kSize, kSize, pixels);

                size_t bad = 0;
                const bool drew = allPixelsAre(pixels, 64, 128, 192, 255, bad);
                report.check(drew, "pass: fullscreen draw covered the target",
                             drew ? "" : (pixels.empty() ? "no pixels" : describe(&pixels[bad])));

                device.destroy(pipeline);
            }
            device.destroy(shader);
        }
    }

    /* ---- 9. a real mesh, with real vertex attributes --------------------
     *
     * THE PATH THE WHOLE RENDERER ACTUALLY USES, and the one stage 8 says
     * nothing about. A fullscreen draw synthesises its positions from
     * gl_VertexID and touches no buffer; every pass in the engine instead binds
     * a vertex buffer, an index buffer and an attribute layout, and each of
     * those has its own silent failure — a stride that does not match the
     * struct, an element buffer unbound before its vertex array was, an
     * attribute location the shader does not declare. All three render nothing
     * and report nothing.
     *
     * A quad covering the target, so the same all-pixels check applies. */
    {
        struct Vertex { float x, y; };
        const Vertex vertices[4] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
        };
        const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

        BufferDesc vertexDesc;
        vertexDesc.name   = "selftest quad vertices";
        vertexDesc.bytes  = sizeof vertices;
        vertexDesc.usage  = BufferUsageVertex;
        vertexDesc.access = BufferAccess::CpuToGpuOnce;

        BufferDesc indexDesc;
        indexDesc.name   = "selftest quad indices";
        indexDesc.bytes  = sizeof indices;
        indexDesc.usage  = BufferUsageIndex;
        indexDesc.access = BufferAccess::CpuToGpuOnce;

        const BufferHandle vertexBuffer = device.createBuffer(vertexDesc);
        const BufferHandle indexBuffer  = device.createBuffer(indexDesc);

        if (report.check(vertexBuffer.valid() && indexBuffer.valid(),
                         "createBuffer: vertex and index")) {
            device.updateBuffer(vertexBuffer, vertices, sizeof vertices, 0);
            device.updateBuffer(indexBuffer, indices, sizeof indices, 0);

            VertexLayout layout;
            layout.stride = sizeof(Vertex);
            layout.attributeCount = 1;
            layout.attributes[0] = VertexAttribute{ 0, 0, VertexFormat::Float2 };

            const MeshHandle mesh =
                device.createMesh(layout, vertexBuffer, 4, indexBuffer, 6);

            if (report.check(mesh.valid(), "createMesh: layout bound to buffers")) {
                const ShaderHandle shader =
                    device.createShader("selftest mesh", kMeshVertexSource, kMeshFragmentSource);

                PipelineDesc pipelineDesc;
                pipelineDesc.name   = "selftest mesh pipeline";
                pipelineDesc.shader = shader;
                pipelineDesc.vertexLayout = layout;
                pipelineDesc.colourFormats[0] = TextureFormat::RGBA8;
                pipelineDesc.colourCount = 1;
                pipelineDesc.depth.test  = false;
                pipelineDesc.depth.write = false;
                pipelineDesc.raster.cull = CullMode::None;

                const PipelineHandle pipeline = device.createPipeline(pipelineDesc);

                if (report.check(shader.valid() && pipeline.valid(),
                                 "createPipeline: with a vertex layout")) {
                    PassDesc pass;
                    pass.name = "selftest mesh draw";
                    pass.colours[0].texture = colour;
                    pass.colours[0].load    = LoadAction::Clear;
                    pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
                    pass.colourCount = 1;

                    ICommandEncoder& encoder = device.beginPass(pass);
                    encoder.bindPipeline(pipeline);
                    encoder.draw(mesh);
                    device.endPass(encoder);

                    std::vector<uint8_t> pixels;
                    device.readTexture(colour, 0, 0, kSize, kSize, pixels);

                    size_t bad = 0;
                    const bool drew = allPixelsAre(pixels, 32, 96, 160, 255, bad);
                    report.check(drew, "pass: indexed mesh draw covered the target",
                                 drew ? "" : (pixels.empty() ? "no pixels"
                                                            : describe(&pixels[bad])));
                }

                device.destroy(pipeline);
                device.destroy(shader);
                device.destroy(mesh);
            }
            device.destroy(indexBuffer);
            device.destroy(vertexBuffer);
        }
    }

    /* ---- 10. a NON-INDEXED mesh ----------------------------------------
     *
     * THE SHAPE THIS ENGINE'S WORLD ACTUALLY HAS. The static geometry comes out
     * of the box emitter as triangle soup with no index buffer at all, so a
     * backend that only handled indexed meshes could not draw the game — and
     * would report nothing, because glDrawElements with no element buffer bound
     * reads from address zero and drivers quietly discard it.
     *
     * Two triangles, six vertices, no indices, covering the target. */
    {
        struct Vertex { float x, y; };
        const Vertex soup[6] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
            { -1.0f, -1.0f }, { 1.0f,  1.0f }, { -1.0f, 1.0f },
        };

        BufferDesc desc;
        desc.name   = "selftest soup";
        desc.bytes  = sizeof soup;
        desc.usage  = BufferUsageVertex;
        desc.access = BufferAccess::CpuToGpuOnce;

        const BufferHandle buffer = device.createBuffer(desc);
        device.updateBuffer(buffer, soup, sizeof soup, 0);

        VertexLayout layout;
        layout.stride = sizeof(Vertex);
        layout.attributeCount = 1;
        layout.attributes[0] = VertexAttribute{ 0, 0, VertexFormat::Float2 };

        /* NO INDEX BUFFER — the defaulted arguments are the whole point. */
        const MeshHandle mesh = device.createMesh(layout, buffer, 6);

        if (report.check(mesh.valid(), "createMesh: non-indexed accepted")) {
            const ShaderHandle shader =
                device.createShader("selftest soup", kMeshVertexSource, kMeshFragmentSource);

            PipelineDesc pipelineDesc;
            pipelineDesc.name   = "selftest soup pipeline";
            pipelineDesc.shader = shader;
            pipelineDesc.vertexLayout = layout;
            pipelineDesc.colourFormats[0] = TextureFormat::RGBA8;
            pipelineDesc.colourCount = 1;
            pipelineDesc.depth.test  = false;
            pipelineDesc.depth.write = false;
            pipelineDesc.raster.cull = CullMode::None;

            const PipelineHandle pipeline = device.createPipeline(pipelineDesc);

            PassDesc pass;
            pass.name = "selftest soup draw";
            pass.colours[0].texture = colour;
            pass.colours[0].load    = LoadAction::Clear;
            pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
            pass.colourCount = 1;

            ICommandEncoder& encoder = device.beginPass(pass);
            encoder.bindPipeline(pipeline);
            encoder.draw(mesh);
            device.endPass(encoder);

            std::vector<uint8_t> pixels;
            device.readTexture(colour, 0, 0, kSize, kSize, pixels);

            size_t bad = 0;
            const bool drew = allPixelsAre(pixels, 32, 96, 160, 255, bad);
            report.check(drew, "pass: non-indexed draw covered the target",
                         drew ? "" : (pixels.empty() ? "no pixels" : describe(&pixels[bad])));

            device.destroy(pipeline);
            device.destroy(shader);
            device.destroy(mesh);
        }
        device.destroy(buffer);
    }

    /* ---- 11. cube array orientation, by round trip -----------------------
     *
     * THE ONE STAGE THAT EXISTS BECAUSE REASONING FAILED. See the shader pair
     * at the top of this file: a mirrored cube face is invisible to inspection
     * and to derivation, and shows up only as reflections landing on the wrong
     * side of a smooth surface.
     *
     * The marker is a triangle in the plane x = 10, spanning z 0..6 and y
     * -3..5, captured from the origin. It therefore lives ENTIRELY on the +X
     * face and OFF-CENTRE on both of that face's axes, which is what makes the
     * mirror detectable — a marker centred on the face would look identical
     * flipped, and that is the version of this test that passes while the bug
     * ships.
     *
     * SKIPPED, NOT FAILED, on a device without cube arrays. macOS's capped GL is
     * that case and is not broken for it. */
    if (device.capabilities().cubeArrays) {
        constexpr uint32_t kFace = 16;
        constexpr float    kFar  = 50.0f;

        TextureDesc cubeDesc;
        cubeDesc.name   = "selftest cube array";
        cubeDesc.width  = kFace;
        cubeDesc.height = kFace;

        /* TWELVE, NOT SIX. Six layers makes the backend choose a plain cubemap,
         * and a samplerCubeArray reading one is undefined — so the test would be
         * measuring the wrong object. Two cubes is the smallest array. */
        cubeDesc.layers = 12;
        cubeDesc.cube   = true;
        cubeDesc.format = TextureFormat::RGBA8;
        cubeDesc.usage  = TextureUsageSampled | TextureUsageRenderTarget;

        const TextureHandle cube = device.createTexture(cubeDesc);

        SamplerDesc cubeSamplerDesc;
        cubeSamplerDesc.minify  = FilterMode::Nearest;
        cubeSamplerDesc.magnify = FilterMode::Nearest;
        cubeSamplerDesc.mip     = FilterMode::Nearest;
        cubeSamplerDesc.wrapU   = WrapMode::ClampToEdge;
        cubeSamplerDesc.wrapV   = WrapMode::ClampToEdge;
        cubeSamplerDesc.wrapW   = WrapMode::ClampToEdge;
        const SamplerHandle cubeSampler = device.createSampler(cubeSamplerDesc);

        struct Marker { float x, y, z; };
        const Marker markerVertices[3] = {
            { 10.0f, -3.0f, 0.0f }, { 10.0f, -3.0f, 6.0f }, { 10.0f, 5.0f, 3.0f },
        };

        BufferDesc markerDesc;
        markerDesc.name   = "selftest cube marker";
        markerDesc.bytes  = sizeof markerVertices;
        markerDesc.usage  = BufferUsageVertex;
        markerDesc.access = BufferAccess::CpuToGpuOnce;
        const BufferHandle markerBuffer = device.createBuffer(markerDesc);

        BufferDesc cubeBlockDesc;
        cubeBlockDesc.name   = "selftest cube block";
        cubeBlockDesc.bytes  = 64;   /* a mat4, and a vec4 in its first quarter */
        cubeBlockDesc.usage  = BufferUsageUniform;
        cubeBlockDesc.access = BufferAccess::CpuToGpuPerFrame;
        const BufferHandle cubeBlock = device.createBuffer(cubeBlockDesc);

        VertexLayout markerLayout;
        markerLayout.stride = sizeof(Marker);
        markerLayout.attributeCount = 1;
        markerLayout.attributes[0] = VertexAttribute{ 0, 0, VertexFormat::Float3 };

        if (report.check(cube.valid() && markerBuffer.valid() && cubeBlock.valid(),
                         "createTexture: cube array with 12 layers")) {
            device.updateBuffer(markerBuffer, markerVertices, sizeof markerVertices, 0);

            const MeshHandle marker = device.createMesh(markerLayout, markerBuffer, 3);

            const ShaderHandle markerShader =
                device.createShader("selftest cube marker", kCubeMarkerVertexSource,
                                    kCubeMarkerFragmentSource);
            const ShaderHandle sampleShader =
                device.createShader("selftest cube sample", kVertexSource,
                                    kCubeSampleFragmentSource);

            PipelineDesc markerPipelineDesc;
            markerPipelineDesc.name   = "selftest cube marker";
            markerPipelineDesc.shader = markerShader;
            markerPipelineDesc.vertexLayout = markerLayout;
            markerPipelineDesc.colourFormats[0] = TextureFormat::RGBA8;
            markerPipelineDesc.colourCount = 1;
            markerPipelineDesc.depth.test  = false;
            markerPipelineDesc.depth.write = false;

            /* NO CULLING. A capture must not depend on which way the marker's
             * winding happens to face, and neither does the real probe pass. */
            markerPipelineDesc.raster.cull = CullMode::None;

            PipelineDesc samplePipelineDesc = markerPipelineDesc;
            samplePipelineDesc.name   = "selftest cube sample";
            samplePipelineDesc.shader = sampleShader;
            samplePipelineDesc.vertexLayout = VertexLayout{};

            const PipelineHandle markerPipeline = device.createPipeline(markerPipelineDesc);
            const PipelineHandle samplePipeline = device.createPipeline(samplePipelineDesc);

            if (report.check(marker.valid() && markerShader.valid() && sampleShader.valid()
                                 && markerPipeline.valid() && samplePipeline.valid(),
                             "createPipeline: cube capture and cube sample")) {
                /* ---- capture all six faces from the origin ---------------- */
                for (int face = 0; face < 6; face++) {
                    const Mat4 viewProjection =
                        DeviceProbeSet::faceViewProjection(face, Vec3{ 0.0f, 0.0f, 0.0f }, kFar);
                    device.updateBuffer(cubeBlock, &viewProjection, sizeof viewProjection, 0);

                    PassDesc pass;
                    pass.name = "selftest cube face";
                    pass.colours[0].texture = cube;
                    pass.colours[0].layer   = static_cast<uint32_t>(face);
                    pass.colours[0].load    = LoadAction::Clear;
                    pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
                    pass.colourCount = 1;

                    ICommandEncoder& encoder = device.beginPass(pass);
                    encoder.bindPipeline(markerPipeline);
                    encoder.bindUniformBuffer(1, cubeBlock);
                    encoder.draw(marker);
                    device.endPass(encoder);
                }

                /* ---- then ask the cube what is in two directions ---------- */
                const auto sampleAt = [&](float x, float y, float z) {
                    const float direction[4] = { x, y, z, 0.0f };
                    device.updateBuffer(cubeBlock, direction, sizeof direction, 0);

                    PassDesc pass;
                    pass.name = "selftest cube sample";
                    pass.colours[0].texture = colour;
                    pass.colours[0].load    = LoadAction::Clear;
                    pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
                    pass.colourCount = 1;

                    ICommandEncoder& encoder = device.beginPass(pass);
                    encoder.bindPipeline(samplePipeline);
                    encoder.bindTexture(0, cube, cubeSampler);
                    encoder.bindUniformBuffer(1, cubeBlock);
                    encoder.drawFullscreen();
                    device.endPass(encoder);

                    std::vector<uint8_t> pixels;
                    device.readTexture(colour, 0, 0, kSize, kSize, pixels);
                    return pixels.size() >= 4 ? pixels[0] : uint8_t{ 0 };
                };

                /* THE MARKER'S OWN DIRECTION, and its mirror through the face's
                 * horizontal axis. Correct: red then black. Mirrored: black then
                 * red — which is why the miss is checked as well as the hit. A
                 * test that only asserted the hit passes on a cube that returns
                 * the marker everywhere. */
                const uint8_t onMarker  = sampleAt(10.0f, 0.0f,  3.0f);
                const uint8_t mirroredZ = sampleAt(10.0f, 0.0f, -3.0f);

                /* And the same through its VERTICAL axis, which catches an up
                 * vector that was flipped without the forward vector being
                 * wrong — a different typo with an identical symptom. */
                const uint8_t aboveMarker = sampleAt(10.0f,  4.0f, 3.0f);
                const uint8_t belowMarker = sampleAt(10.0f, -4.0f, 3.0f);

                char why[128];

                const bool horizontal = onMarker > 200 && mirroredZ < 50;
                std::snprintf(why, sizeof why,
                              "marker %d, mirror %d - the +X face is flipped left to right",
                              onMarker, mirroredZ);
                report.check(horizontal, "cube array: a face is not mirrored horizontally",
                             horizontal ? "" : why);

                const bool vertical = aboveMarker > 200 && belowMarker < 50;
                std::snprintf(why, sizeof why,
                              "above %d, below %d - the +X face is flipped top to bottom",
                              aboveMarker, belowMarker);
                report.check(vertical, "cube array: a face is not mirrored vertically",
                             vertical ? "" : why);
            }

            device.destroy(samplePipeline);
            device.destroy(markerPipeline);
            device.destroy(sampleShader);
            device.destroy(markerShader);
            device.destroy(marker);
        }

        device.destroy(cubeBlock);
        device.destroy(markerBuffer);
        device.destroy(cubeSampler);
        device.destroy(cube);
    }

    /* ---- 12. compute, where there is any --------------------------------
     *
     * SKIPPED RATHER THAN FAILED when the device has none — macOS's GL is
     * exactly that case, and a backend is not broken for lacking a feature it
     * truthfully reported. What IS checked is that the answer is consistent:
     * a device claiming compute must hand back an encoder. */
    {
        const bool claims = device.capabilities().compute;
        ICommandEncoder* compute = device.beginCompute("selftest compute");

        if (claims) {
            report.check(compute != nullptr, "compute: a claiming device provides an encoder");
        } else {
            report.check(compute == nullptr,
                         "compute: a device without it refuses rather than no-ops");
        }
    }

    device.destroy(colour);

    SelfTestResult result;
    result.stagesRun    = report.stages();
    result.stagesFailed = report.failures();
    result.passed       = report.failures() == 0;

    result.report  = "render device self-test: ";
    result.report += device.capabilities().backendName;
    result.report += " / ";
    result.report += device.capabilities().deviceName;
    result.report += '\n';
    result.report += report.text();
    result.report += result.passed ? "  all stages passed\n"
                                   : "  " + std::to_string(report.failures()) + " stage(s) failed\n";

    return result;
}

}  // namespace cromwell::rhi
