#include "cromwell/gpu/compute/ComputeShader.hpp"

/* GL.hpp first — it brings glad in ahead of anything with a system GL header,
 * and it is where the calls rlgl does not wrap live. */
#include "cromwell/gpu/GL.hpp"

#include "raylib.h"
#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"

#include <cstring>
#include <utility>

namespace cromwell {

/* ---- ShaderStorageBuffer ------------------------------------------------ */

ShaderStorageBuffer::ShaderStorageBuffer(ShaderStorageBuffer&& other) noexcept
    : id_(other.id_), size_(other.size_)
{
    other.id_   = 0;
    other.size_ = 0;
}

ShaderStorageBuffer& ShaderStorageBuffer::operator=(ShaderStorageBuffer&& other) noexcept
{
    if (this != &other) {
        destroy();
        id_         = other.id_;
        size_       = other.size_;
        other.id_   = 0;
        other.size_ = 0;
    }
    return *this;
}

bool ShaderStorageBuffer::create(std::size_t sizeBytes, const void* data, bool dynamic)
{
    destroy();

    if (sizeBytes == 0) {
        TraceLog(LOG_WARNING, "COMPUTE: refusing to create a zero-byte storage buffer");
        return false;
    }

    if (!gl::computeAvailable()) return false;

    /* COPY, not DRAW: these buffers are written by the GPU and read by the
     * GPU. DRAW would tell the driver the CPU is the producer, which is the
     * opposite of the access pattern here. */
    id_ = rlLoadShaderBuffer(static_cast<unsigned int>(sizeBytes), data,
                             dynamic ? RL_DYNAMIC_COPY : RL_STATIC_COPY);

    if (id_ == 0) {
        TraceLog(LOG_WARNING, "COMPUTE: storage buffer of %u bytes could not be created",
                 static_cast<unsigned>(sizeBytes));
        return false;
    }

    size_ = sizeBytes;
    return true;
}

void ShaderStorageBuffer::destroy()
{
    if (id_ == 0) return;
    rlUnloadShaderBuffer(id_);
    id_   = 0;
    size_ = 0;
}

void ShaderStorageBuffer::update(const void* data, std::size_t sizeBytes,
                                 std::size_t offsetBytes)
{
    if (id_ == 0 || data == nullptr || sizeBytes == 0) return;

    /* Checked here rather than trusted, because the failure mode of an
     * overrunning SSBO write is silent corruption of whatever the driver
     * placed next, which surfaces as a bug somewhere else entirely. */
    if (offsetBytes + sizeBytes > size_) {
        TraceLog(LOG_ERROR,
                 "COMPUTE: storage buffer write of %u bytes at %u overruns its %u-byte "
                 "allocation - ignored",
                 static_cast<unsigned>(sizeBytes), static_cast<unsigned>(offsetBytes),
                 static_cast<unsigned>(size_));
        return;
    }

    rlUpdateShaderBuffer(id_, data, static_cast<unsigned int>(sizeBytes),
                         static_cast<unsigned int>(offsetBytes));
}

void ShaderStorageBuffer::bind(unsigned int bindingIndex) const
{
    if (id_ == 0) return;
    rlBindShaderBuffer(id_, bindingIndex);
}

void ShaderStorageBuffer::bindRange(unsigned int bindingIndex, std::size_t offsetBytes,
                                    std::size_t sizeBytes) const
{
    if (id_ == 0) return;

    if (offsetBytes + sizeBytes > size_) {
        TraceLog(LOG_ERROR,
                 "COMPUTE: storage buffer range %u+%u overruns its %u-byte allocation "
                 "- ignored",
                 static_cast<unsigned>(offsetBytes), static_cast<unsigned>(sizeBytes),
                 static_cast<unsigned>(size_));
        return;
    }

    gl::bindBufferRange(GL_SHADER_STORAGE_BUFFER, bindingIndex, id_,
                        static_cast<ptrdiff_t>(offsetBytes),
                        static_cast<ptrdiff_t>(sizeBytes));
}

void ShaderStorageBuffer::read(void* dest, std::size_t sizeBytes,
                               std::size_t offsetBytes) const
{
    if (id_ == 0 || dest == nullptr || sizeBytes == 0) return;

    if (offsetBytes + sizeBytes > size_) {
        TraceLog(LOG_ERROR,
                 "COMPUTE: storage buffer read of %u bytes at %u overruns its %u-byte "
                 "allocation - ignored",
                 static_cast<unsigned>(sizeBytes), static_cast<unsigned>(offsetBytes),
                 static_cast<unsigned>(size_));
        return;
    }

    /* The caller is about to look at GPU-written memory from the CPU, so the
     * writes have to have landed. Without this the read races whatever pass
     * produced the data. */
    gl::memoryBarrier(gl::BarrierBufferUpdate);

    rlReadShaderBuffer(id_, dest, static_cast<unsigned int>(sizeBytes),
                       static_cast<unsigned int>(offsetBytes));
}

/* ---- ComputeShader ------------------------------------------------------ */

bool ComputeShader::load(const char* computeName)
{
    destroy();

    if (computeName == nullptr) return false;

    /* Reported once here rather than once per dispatch. */
    if (!gl::computeAvailable()) {
        TraceLog(LOG_WARNING, "COMPUTE: %s not loaded - no compute support", computeName);
        return false;
    }

    const std::string source = ShaderLibrary::preprocess(computeName);

    if (source.empty()) {
        TraceLog(LOG_WARNING, "COMPUTE: %s is missing or empty", computeName);
        return false;
    }

    const unsigned int shaderId = rlCompileShader(source.c_str(), RL_COMPUTE_SHADER);

    if (shaderId == 0) {
        /* rlCompileShader has already logged the driver's message. What it
         * cannot say is that the line numbers refer to the SPLICED source,
         * which exists in no file — same problem, same answer as
         * ShaderLibrary::load. */
        TraceLog(LOG_WARNING,
                 "COMPUTE: %s failed to compile - the line numbers above are against "
                 "the spliced source, not the file on disk",
                 computeName);
        return false;
    }

    program_ = rlLoadComputeShaderProgram(shaderId);

    /* rlLoadComputeShaderProgram deletes the shader object itself once linked,
     * so there is nothing to release here on either path. */
    if (program_ == 0) {
        TraceLog(LOG_WARNING, "COMPUTE: %s compiled but failed to link", computeName);
        return false;
    }

    TraceLog(LOG_INFO, "COMPUTE: %s loaded (program %u)", computeName, program_);
    return true;
}

void ComputeShader::destroy()
{
    if (program_ != 0) {
        rlUnloadShaderProgram(program_);
        program_ = 0;
    }
    uniforms_.clear();
}

int ComputeShader::uniformLocation(const char* name)
{
    if (program_ == 0 || name == nullptr) return -1;

    for (const Uniform& uniform : uniforms_) {
        if (uniform.name == name) return uniform.location;
    }

    /* Cached even when absent (-1), so a uniform the compiler removed costs
     * one lookup rather than one per frame. */
    const int location = rlGetLocationUniform(program_, name);
    uniforms_.push_back(Uniform{ name, location });
    return location;
}

void ComputeShader::setInt(const char* name, int value)
{
    const int location = uniformLocation(name);
    if (location < 0) return;

    rlEnableShader(program_);
    rlSetUniform(location, &value, RL_SHADER_UNIFORM_INT, 1);
}

void ComputeShader::setUInt(const char* name, unsigned int value)
{
    const int location = uniformLocation(name);
    if (location < 0) return;

    rlEnableShader(program_);
    rlSetUniform(location, &value, RL_SHADER_UNIFORM_UINT, 1);
}

void ComputeShader::setFloat(const char* name, float value)
{
    const int location = uniformLocation(name);
    if (location < 0) return;

    rlEnableShader(program_);
    rlSetUniform(location, &value, RL_SHADER_UNIFORM_FLOAT, 1);
}

void ComputeShader::setVec3(const char* name, const float* xyz)
{
    const int location = uniformLocation(name);
    if (location < 0 || xyz == nullptr) return;

    rlEnableShader(program_);
    rlSetUniform(location, xyz, RL_SHADER_UNIFORM_VEC3, 1);
}

void ComputeShader::setVec4(const char* name, const float* xyzw)
{
    const int location = uniformLocation(name);
    if (location < 0 || xyzw == nullptr) return;

    rlEnableShader(program_);
    rlSetUniform(location, xyzw, RL_SHADER_UNIFORM_VEC4, 1);
}

void ComputeShader::setMatrix(const char* name, const float* sixteenFloats)
{
    const int location = uniformLocation(name);
    if (location < 0 || sixteenFloats == nullptr) return;

    Matrix matrix{};
    std::memcpy(&matrix, sixteenFloats, sizeof(Matrix));

    rlEnableShader(program_);
    rlSetUniformMatrix(location, matrix);
}

/* ---- ComputePass -------------------------------------------------------- */

ComputePass::ComputePass(ComputeShader& shader, unsigned int barrierBits)
    : barrierBits_(barrierBits)
{
    if (!shader.valid()) return;

    rlEnableShader(shader.program());
    active_ = true;
}

ComputePass::~ComputePass()
{
    if (!active_) return;

    /* THE REASON THIS CLASS EXISTS. Issued whatever the pass dispatched, and
     * before the shader is unbound, so the ordering is a property of the scope
     * rather than of the caller remembering. */
    if (barrierBits_ != 0) gl::memoryBarrier(barrierBits_);

    rlDisableShader();
}

void ComputePass::dispatchItems(unsigned int itemCount, unsigned int localSize)
{
    if (!active_ || itemCount == 0) return;

    if (localSize == 0) {
        TraceLog(LOG_ERROR, "COMPUTE: dispatch with a local size of zero - ignored");
        return;
    }

    /* Ceiling division. The last group is normally partial, which is why the
     * shader must test its invocation id against the real count — see the
     * header. */
    const unsigned int groups = (itemCount + localSize - 1) / localSize;
    rlComputeShaderDispatch(groups, 1, 1);
}

void ComputePass::dispatchGroups(unsigned int groupsX, unsigned int groupsY,
                                 unsigned int groupsZ)
{
    if (!active_) return;
    if (groupsX == 0 || groupsY == 0 || groupsZ == 0) return;

    rlComputeShaderDispatch(groupsX, groupsY, groupsZ);
}

}  // namespace cromwell
