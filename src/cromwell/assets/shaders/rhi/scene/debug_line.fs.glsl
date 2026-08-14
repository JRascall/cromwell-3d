#version 450 core
/* rhi/scene/debug_line.fs.glsl — the segment's colour, divided out of exposure.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ============ WHY A DEBUG COLOUR NEEDS ARITHMETIC AT ALL ==================
 *
 * These lines are drawn into the SCENE target, which is linear radiance, and
 * the resolve then puts every pixel of it through `filmicDisplay(radiance,
 * exposure)`. A debug red written as 1.0 would therefore arrive on screen as
 * whatever the filmic curve makes of one unit of radiance at the current
 * exposure — which on a bright day is a dim brown. The colour a caller asked
 * for would depend on the time of day, which is absurd for a diagnostic.
 *
 * So the radiance written here is the wanted DISPLAY value divided by exposure,
 * which is what the resolve's multiply then undoes.
 *
 * IT IS AN APPROXIMATION AND THE LIMIT IS MEASURED. The filmic curve still runs
 * afterwards and compresses the top end, so a saturated debug colour arrives
 * darker than the byte value asked for. Sampled off the same frame drawn both
 * ways, taking the most saturated pixel of each line:
 *
 *     cyan    raylib (62, 224, 224)    rhi (43, 141, 148)
 *     red     raylib (178, 97, 99)     rhi (148, 43, 43)
 *
 * The HUE survives, which is what a diagnostic needs — the colours exist to be
 * told apart, and DebugDraw picks saturated primaries for exactly that reason.
 * The brightness lands roughly a third low.
 *
 * NO CORRECTION FACTOR IS APPLIED, deliberately. Dividing by exposure is a
 * statement about what the resolve does; a second constant chosen to make these
 * two numbers match would be tuning invented rather than derived, which is the
 * trap written up in cromwell/rhi/MIGRATION.md. If debug lines ever need to be
 * brighter the honest fix is an approximate INVERSE of filmicDisplay, and the
 * numbers above are what it would have to beat.
 *
 * WHY NOT DRAW AFTER THE RESOLVE, where display colour would be exact: the
 * depth-tested pass needs the scene's depth buffer, and that buffer belongs to
 * the supersampled scene target rather than to the backbuffer. Drawing after
 * the resolve would mean x-ray lines only, and "did this ray clear the parapet"
 * is answered by occlusion — see DebugRenderer.hpp on why both passes exist.
 */
#include "rhi/include/scene_block.glsl"

layout(location = 0) in vec4 vColour;

layout(location = 0) out vec4 outColour;

void main()
{
    float exposure = max(uExposureAndAmbient.x, 1.0e-4);
    outColour = vec4(vColour.rgb / exposure, vColour.a);
}
