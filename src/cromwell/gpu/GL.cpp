#include "cromwell/gpu/GL.hpp"

/* After GL.hpp, which brings glad in first — see the ordering note there. The
 * render layer reports through TraceLog with a subsystem prefix, and raylib's
 * log bridge forwards that into the process logger. */
#include "raylib.h"

namespace cromwell::gl {

void bindBufferRange(unsigned int target, unsigned int index, unsigned int buffer,
                     std::ptrdiff_t offset, std::ptrdiff_t size)
{
    glBindBufferRange(target, index, buffer,
                      static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
}

void memoryBarrier(unsigned int bits)
{
    glMemoryBarrier(bits);
}

void drawElementsIndirect(unsigned int mode, unsigned int type, const void* indirect)
{
    glDrawElementsIndirect(mode, type, indirect);
}

void multiDrawElementsIndirect(unsigned int mode, unsigned int type,
                               const void* indirect, int drawCount, int stride)
{
    glMultiDrawElementsIndirect(mode, type, indirect, drawCount, stride);
}

void dispatchComputeIndirect(std::ptrdiff_t indirectOffset)
{
    glDispatchComputeIndirect(static_cast<GLintptr>(indirectOffset));
}

unsigned int shaderStorageBlockIndex(unsigned int program, const char* name)
{
    return glGetProgramResourceIndex(program, GL_SHADER_STORAGE_BLOCK, name);
}

/* ---- TimerQuery --------------------------------------------------------- */

void TimerQuery::create()
{
    if (id_ != 0) return;
    glGenQueries(1, &id_);
}

void TimerQuery::destroy()
{
    if (id_ == 0) return;
    glDeleteQueries(1, &id_);
    id_ = 0;
}

void TimerQuery::begin()
{
    if (id_ != 0) glBeginQuery(GL_TIME_ELAPSED, id_);
}

void TimerQuery::end()
{
    if (id_ != 0) glEndQuery(GL_TIME_ELAPSED);
}

bool TimerQuery::resultReady() const
{
    if (id_ == 0) return false;

    GLuint available = GL_FALSE;
    glGetQueryObjectuiv(id_, GL_QUERY_RESULT_AVAILABLE, &available);
    return available == GL_TRUE;
}

double TimerQuery::milliseconds() const
{
    if (id_ == 0) return 0.0;

    /* GL_TIME_ELAPSED is in nanoseconds, and 64-bit because a long frame
     * overflows 32. Blocks if the result is not ready — resultReady() is the
     * guard, and calling this without it is the stall this class exists to
     * avoid. */
    GLuint64 elapsedNs = 0;
    glGetQueryObjectui64v(id_, GL_QUERY_RESULT, &elapsedNs);
    return static_cast<double>(elapsedNs) / 1.0e6;
}

/* ---- diagnostics -------------------------------------------------------- */

namespace {

const char* errorName(GLenum code)
{
    switch (code) {
        case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
        case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
        case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
        default:                               return "GL_<unknown>";
    }
}

}  // namespace

bool checkErrors(const char* what)
{
    bool clean = true;

    /* The queue holds several codes, and reporting only the first hides the
     * rest — so drain rather than test once. */
    for (GLenum code = glGetError(); code != GL_NO_ERROR; code = glGetError()) {
        clean = false;
        TraceLog(LOG_ERROR, "GL: %s failed with %s (0x%04X)", what, errorName(code),
                 static_cast<unsigned>(code));
    }

    return clean;
}

bool computeAvailable()
{
    /* Resolved once. A null function pointer here means either a context below
     * 4.3 or a driver that refused one — both of which we want to hear about
     * as a message rather than as a crash at the first dispatch. */
    static const bool available = [] {
        const bool ok = (glDispatchCompute != nullptr) &&
                        (glMemoryBarrier != nullptr) &&
                        (glBindBufferBase != nullptr);

        if (!ok) {
            TraceLog(LOG_WARNING,
                     "GL: no compute support in this context - expected a 4.3 core "
                     "context (see OPENGL_VERSION in CMakeLists.txt). Compute passes "
                     "will be skipped.");
        }

        return ok;
    }();

    return available;
}

}  // namespace cromwell::gl
