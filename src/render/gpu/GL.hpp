/* GL.hpp — the one door to raw OpenGL, and the reason each thing is behind it.
 *
 * SINGLE RESPONSIBILITY: declare the GL entry points that rlgl does not wrap,
 * so that every translation unit needing one includes this header rather than
 * reaching for glad.h on its own.
 *
 * WHY THIS EXISTS AT ALL. raylib is worth keeping — it owns the window, input,
 * model loading, and (the day soldiers animate) skeletal animation with GPU
 * skinning, none of which we want to rebuild. But rlgl wraps only the subset of
 * GL raylib itself needs, and that subset stops short of compute. Everything
 * below is a function that exists in our 4.3 core context, that rlgl has no
 * wrapper for, and that some pass here genuinely needs.
 *
 * The alternative was to drop raylib for raw GL. Measured at ~17k lines across
 * 77 files, to gain nothing we are asking for: compute programs, dispatch,
 * SSBOs and image load/store are ALREADY in rlgl (rlLoadComputeShaderProgram,
 * rlComputeShaderDispatch, rlLoadShaderBuffer, rlBindImageTexture). What is
 * missing is this header's worth of entry points, so this header is the port.
 *
 * THIS REPLACES AN EARLIER RULE. The renderer used to allow exactly one file —
 * ReflectionProbeSet.cpp — to call GL directly, on the grounds that one
 * exception is easier to police than a boundary. That held while the exception
 * was one file wanting cubemap arrays. It stops holding once compute arrives,
 * because the second and third exception would be argued the same way and
 * nobody would be counting. One owned door beats two unowned ones.
 *
 * THE RULE NOW: if rlgl wraps it, call rlgl. If it does not, declare it here
 * with a comment saying which pass needs it. Do not include glad.h elsewhere.
 *
 * ON glad.h. raylib bundles glad and declares its loaders `extern`, so
 * including raylib's own copy links against the pointers raylib already
 * resolved at InitWindow. GLAD_GL_IMPLEMENTATION must NOT be defined — that
 * macro would define a second, unloaded copy of every function pointer and
 * every call would go through a null.
 *
 * ORDERING. glad.h must precede anything that might pull in a system GL header.
 * Include this header before raylib.h/rlgl.h in any .cpp that needs both.
 */
#pragma once

/* Declares, does not implement — see the note above. */
#include "glad.h"

#include <cstddef>

namespace xcom::gl {

/* ---- what compute needs, and what breaks without it ---------------------
 *
 * rlgl gives us the compute program and the dispatch. It does not give us the
 * means to make the results VISIBLE, which is the part that fails silently.
 *
 * A dispatch that writes an SSBO and a draw that reads it are not ordered by
 * the API. Without a barrier between them the read may see the previous
 * frame's contents, or a torn mix. It will usually look correct on the machine
 * it was written on and race somewhere else, which is the worst failure shape
 * available. computeBarrier() below exists so the barrier is not a thing you
 * remember to do.
 *
 * glMemoryBarrier is core 4.2, glBindBufferRange core 3.1, the indirect draws
 * core 4.0/4.3, the query objects core 3.3. All of them are inside our 4.3
 * context; none of them are inside rlgl.
 */

/* Bind a SUB-RANGE of a buffer, where rlBindShaderBuffer binds the whole
 * thing. Wanted so that one large instance buffer can serve many batches at
 * different offsets — RE ENGINE's "mesh-specific offset + instance id" layout,
 * see study/re_engine_rendering.md §9.1 — instead of one buffer per batch. */
void bindBufferRange(unsigned int target, unsigned int index, unsigned int buffer,
                     std::ptrdiff_t offset, std::ptrdiff_t size);

/* ---- ordering ----------------------------------------------------------- */

/* Barrier bits, named so call sites read as intent rather than as GL. Combine
 * with |. These are the ones a compute pass here would plausibly want; add
 * more from the GL_*_BARRIER_BIT set as passes need them. */
enum BarrierBits : unsigned int {
    BarrierShaderStorage = GL_SHADER_STORAGE_BARRIER_BIT,  /* SSBO writes -> any read */
    BarrierImageAccess   = GL_SHADER_IMAGE_ACCESS_BARRIER_BIT, /* imageStore -> texture read */
    BarrierTextureFetch  = GL_TEXTURE_FETCH_BARRIER_BIT,   /* -> texture()/texelFetch */
    BarrierVertexAttrib  = GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT, /* -> vertex pulling */
    BarrierCommand       = GL_COMMAND_BARRIER_BIT,         /* -> indirect draw args */
    BarrierBufferUpdate  = GL_BUFFER_UPDATE_BARRIER_BIT,   /* -> glBufferSubData/readback */
};

/* The general form. Prefer ComputePass (ComputeShader.hpp), which pairs the
 * dispatch with its barrier so the two cannot drift apart. */
void memoryBarrier(unsigned int bits);

/* ---- GPU-driven drawing -------------------------------------------------
 *
 * Not used yet, and deliberately declared anyway: these are the calls the
 * instancing work in study/re_engine_rendering.md §9.1 tier 3 would need, and
 * having them named here is what makes that a day of work rather than a
 * re-litigation of this header. A compute pass writes the draw arguments and
 * decrements InstanceCount for culled batches; a culled batch draws zero
 * instances rather than being removed, so no readback and no frame of latency.
 *
 * NOTE glMultiDrawElementsIndirectCount is core 4.6 and therefore NOT here.
 * Without it every command in the buffer is dispatched, some with an instance
 * count of zero. That is exactly what Capcom describe doing, so it costs
 * nothing, but it does mean the command count is a CPU-side constant. */
void drawElementsIndirect(unsigned int mode, unsigned int type, const void* indirect);
void multiDrawElementsIndirect(unsigned int mode, unsigned int type,
                               const void* indirect, int drawCount, int stride);

/* Dispatch sized by a buffer a previous pass wrote, rather than by the CPU. */
void dispatchComputeIndirect(std::ptrdiff_t indirectOffset);

/* ---- introspection ------------------------------------------------------ */

/* Look up an SSBO block's index by name, so shader storage bindings can be
 * resolved the way uniforms already are instead of being hardcoded on both
 * sides and silently disagreeing. Returns GL_INVALID_INDEX if absent. */
unsigned int shaderStorageBlockIndex(unsigned int program, const char* name);

/* ---- timing -------------------------------------------------------------
 *
 * GPU timer queries. The whole argument for moving work to compute is that it
 * is faster, and that claim is not checkable from the CPU: the dispatch
 * returns immediately and the cost lands later. Without these, "is the GPU
 * cull paying for itself" is unanswerable and the honest answer is no. */
class TimerQuery {
public:
    TimerQuery() = default;
    ~TimerQuery() { destroy(); }

    TimerQuery(const TimerQuery&) = delete;
    TimerQuery& operator=(const TimerQuery&) = delete;

    void create();
    void destroy();

    bool valid() const { return id_ != 0; }

    void begin();
    void end();

    /* Non-blocking. Returns false while the GPU has not finished, which for a
     * query issued this frame is the normal case — poll it next frame rather
     * than stalling on it. */
    bool resultReady() const;
    double milliseconds() const;

private:
    unsigned int id_ = 0;
};

/* ---- diagnostics -------------------------------------------------------- */

/* Drains the error queue, then reports whether anything was pending, logging
 * each code against `what`. Call AFTER an operation to check it; the drain is
 * what makes the check describe that operation and not an older one.
 *
 * Returns true when clean. */
bool checkErrors(const char* what);

/* True once a 4.3 context with compute support is confirmed present. Checked
 * once, cheap thereafter. A build configured for GL 3.3, or a driver that
 * refused the 4.3 context, lands here rather than in a null function pointer
 * at the first dispatch. */
bool computeAvailable();

}  // namespace xcom::gl
