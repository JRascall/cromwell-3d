/* ComputeShader.hpp — a compute program, and the buffers it reads and writes.
 *
 * SINGLE RESPONSIBILITY: own a compute program and the shader-storage buffers
 * bound to it, and make a dispatch impossible to issue without the barrier
 * that makes its results visible.
 *
 * WHY A WRAPPER AT ALL, when rlgl already has rlLoadComputeShaderProgram and
 * rlComputeShaderDispatch. Three reasons, in order of how much they cost when
 * absent:
 *
 *   1. THE BARRIER. rlgl dispatches and returns. Nothing in the API orders a
 *      compute write against the draw that reads it, so a missing
 *      glMemoryBarrier produces a frame that is correct on the machine it was
 *      written on and races elsewhere. ComputePass pairs the two in a scope so
 *      the barrier is structural rather than remembered.
 *   2. #include. Compute shaders want the same shared GLSL as everything else
 *      — the lattice constants, the packing helpers — and GLSL has no
 *      #include. ShaderLibrary::preprocess already solves that for the raster
 *      shaders; this routes compute through the same splice rather than
 *      growing a second, subtly different loader.
 *   3. rlgl's loader reports failure by returning 0 and logging into raylib's
 *      own channel, which loses the spliced line numbers that make a compile
 *      error readable. Same problem ShaderLibrary::load already solved.
 *
 * DISPATCH SIZE IS IN ITEMS, NOT GROUPS. Every caller that has taken a group
 * count has eventually got the ceiling division wrong and quietly skipped the
 * last partial group. dispatchItems() takes the item count and the local size
 * and does the division, so the shader's own bounds check is the only place
 * the tail is handled.
 *
 * NOT A RENDER PASS. This owns no scheduling opinion: what runs, in what
 * order, and whether it runs at all belongs to the caller.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace xcom {

/* ---- shader storage buffer ---------------------------------------------
 *
 * Thin RAII over rlgl's SSBO calls, plus the sub-range binding rlgl lacks.
 * Held separately from the program because the interesting buffers outlive any
 * one pass — an instance buffer is written by a cull pass and read by a draw.
 */
class ShaderStorageBuffer {
public:
    ShaderStorageBuffer() = default;
    ~ShaderStorageBuffer() { destroy(); }

    ShaderStorageBuffer(const ShaderStorageBuffer&) = delete;
    ShaderStorageBuffer& operator=(const ShaderStorageBuffer&) = delete;

    ShaderStorageBuffer(ShaderStorageBuffer&& other) noexcept;
    ShaderStorageBuffer& operator=(ShaderStorageBuffer&& other) noexcept;

    /* `data` may be null to allocate without initialising. Dynamic means the
     * CPU expects to rewrite it; buffers only ever written by the GPU should
     * be static, which is a hint the driver takes seriously for placement. */
    bool create(std::size_t sizeBytes, const void* data = nullptr, bool dynamic = true);
    void destroy();

    bool        valid() const { return id_ != 0; }
    std::size_t size()  const { return size_; }
    unsigned    id()    const { return id_; }

    /* Partial update. Out-of-range writes are rejected with a complaint rather
     * than corrupting whatever follows. */
    void update(const void* data, std::size_t sizeBytes, std::size_t offsetBytes = 0);

    /* Binds the whole buffer to a binding point. */
    void bind(unsigned int bindingIndex) const;

    /* Binds a sub-range, so one buffer can serve many batches at different
     * offsets — see GL.hpp's note on bindBufferRange. */
    void bindRange(unsigned int bindingIndex, std::size_t offsetBytes,
                   std::size_t sizeBytes) const;

    /* GPU -> CPU. STALLS THE PIPELINE: the driver must wait for every pass
     * that could have written this buffer. Fine for a debug inspection or a
     * one-off bake, wrong in a frame loop — the whole point of writing draw
     * arguments on the GPU is that nobody reads them back. */
    void read(void* dest, std::size_t sizeBytes, std::size_t offsetBytes = 0) const;

private:
    unsigned    id_   = 0;
    std::size_t size_ = 0;
};

/* ---- compute program ---------------------------------------------------- */

class ComputeShader {
public:
    ComputeShader() = default;
    ~ComputeShader() { destroy(); }

    ComputeShader(const ComputeShader&) = delete;
    ComputeShader& operator=(const ComputeShader&) = delete;

    /* Loads assets/shaders/<name>, splicing #includes through
     * ShaderLibrary::preprocess. Returns false and names the file on failure;
     * the caller should disable whatever needed it rather than dispatching a
     * program that is not there.
     *
     * Fails early and quietly when the context has no compute support, so a
     * 3.3 build reports one warning at load instead of one per frame. */
    bool load(const char* computeName);
    void destroy();

    bool     valid() const { return program_ != 0; }
    unsigned program() const { return program_; }

    /* Uniform plumbing. Locations are resolved and cached on first use; an
     * absent name is a no-op, because a uniform optimised out by the compiler
     * is not an error and should not need a call-site guard. */
    void setInt(const char* name, int value);
    void setUInt(const char* name, unsigned int value);
    void setFloat(const char* name, float value);
    void setVec3(const char* name, const float* xyz);
    void setVec4(const char* name, const float* xyzw);
    void setMatrix(const char* name, const float* sixteenFloats);

private:
    friend class ComputePass;

    int uniformLocation(const char* name);

    unsigned program_ = 0;

    /* Small and linear-scanned on purpose: a compute program has a handful of
     * uniforms, and a hash map would cost more than the scan it replaces. */
    struct Uniform {
        std::string name;
        int         location = -1;
    };
    std::vector<Uniform> uniforms_;
};

/* ---- a dispatch, with its barrier ---------------------------------------
 *
 * Scoped so the barrier cannot be forgotten:
 *
 *     {
 *         ComputePass pass(cullShader, gl::BarrierShaderStorage | gl::BarrierCommand);
 *         instances.bind(0);
 *         drawArgs.bind(1);
 *         pass.dispatchItems(instanceCount, 64);
 *     }   // barrier here, before anything reads drawArgs
 *
 * The barrier fires on destruction, once, whatever the pass dispatched. Choose
 * the bits by what READS the results, not by what wrote them: a compute pass
 * writing indirect draw arguments needs BarrierCommand, one writing an SSBO a
 * later shader reads needs BarrierShaderStorage, and one doing both needs both.
 */
class ComputePass {
public:
    /* `barrierBits` are xcom::gl::BarrierBits, combined with |. Passing 0 is
     * legal and means "this pass's results are consumed by another compute
     * pass that will issue its own barrier" — rare, and worth a comment where
     * it happens. */
    ComputePass(ComputeShader& shader, unsigned int barrierBits);
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    bool valid() const { return active_; }

    /* Dispatch by ITEM count. Rounds up to whole groups, so the shader must
     * bounds-check its own invocation id against the real count — the last
     * group is normally partial. `localSize` must match the shader's
     * layout(local_size_x = ...) or the arithmetic is wrong in silence. */
    void dispatchItems(unsigned int itemCount, unsigned int localSize);

    /* Dispatch by group count, for 2D/3D domains where the caller has already
     * done the division. */
    void dispatchGroups(unsigned int groupsX, unsigned int groupsY = 1,
                        unsigned int groupsZ = 1);

private:
    unsigned int barrierBits_ = 0;
    bool         active_      = false;
};

}  // namespace xcom
