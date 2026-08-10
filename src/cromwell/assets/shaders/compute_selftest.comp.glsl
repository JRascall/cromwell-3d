#version 430

/* compute_selftest.comp.glsl — proves the compute path end to end.
 *
 * Deliberately trivial arithmetic. What is under test is not the maths, it is
 * every link in the chain around it: ShaderLibrary's splice reaching a .comp
 * file, rlCompileShader accepting it, rlLoadComputeShaderProgram linking it,
 * the storage buffer binding at the index the C++ side thinks it used, the
 * dispatch covering every item, and the barrier making the writes visible to
 * the readback that follows.
 *
 * If this reports a mismatch, the compute infrastructure is broken and no pass
 * built on it will work. If it reports OK, the next pass only has to debug
 * itself. */

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer InputBuffer {
    uint uInput[];
};

layout(std430, binding = 1) writeonly buffer OutputBuffer {
    uint uOutput[];
};

uniform uint uCount;

void main()
{
    uint index = gl_GlobalInvocationID.x;

    /* THE PARTIAL LAST GROUP. dispatchItems rounds the item count up to whole
     * groups, so the final group runs invocations past the end of the data.
     * Without this test they would write outside the buffer. Every compute
     * shader here needs the equivalent line. */
    if (index >= uCount) return;

    /* Not a pure copy: a pure copy would still pass if the dispatch never ran
     * and the output buffer happened to be zero-initialised alongside a
     * zero-filled input. The transform makes silence distinguishable from
     * success. */
    uOutput[index] = uInput[index] * 2u + 1u;
}
