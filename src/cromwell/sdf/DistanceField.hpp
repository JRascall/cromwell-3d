/* DistanceField.hpp — what a distance field texel means, stated once.
 *
 * SINGLE RESPONSIBILITY: the encoding contract shared by everything that
 * produces a distance field and everything that samples one. No generation, no
 * rendering, no ownership of any texture — just the numbers that both halves
 * have to agree on.
 *
 * WHY A HEADER FOR FOUR CONSTANTS. Because the failure mode when they drift is
 * invisible. A producer that bakes with a range of 4 and a shader that decodes
 * with 2 does not crash, does not warn, and does not look obviously wrong — it
 * looks like slightly crunchy edges, which reads as "the antialiasing needs
 * tuning" and sends somebody to the wrong file for an afternoon. The contract
 * is not complicated; it is just unenforceable at compile time across a C++/
 * GLSL boundary, so the mitigation is that there is exactly one place to read
 * it and the GLSL side (assets/shaders/common/sdf.glsl) quotes the same words.
 *
 * WHY DISTANCE FIELDS ARE AN ENGINE CONCERN AND NOT A TEXT ONE. Storing a
 * boundary as "distance to the nearest edge" rather than as coverage is a
 * general technique, and the reason to put it here rather than inside a font
 * renderer is that the consumers have nothing else in common:
 *
 *   - TEXT, from vector outlines. Multi-channel, because glyph corners must
 *     survive. One atlas serves every size, which is the entire reason to use
 *     it in world space where the on-screen size changes per frame.
 *   - COASTLINES AND TERRAIN BOUNDARIES, from a raster mask. Single channel.
 *     The field is sampled far from the edge as well as at it — shallows
 *     ramping out from a shore, a material blending over a metre or two — so
 *     it wants a much wider range than text does.
 *   - DECAL AND EFFECT MASKS, from either. Soft edges that stay soft under
 *     magnification, and cheap outlines and glows because the distance is
 *     right there.
 *
 * Two of those are baked from vector input and one from a raster, which is why
 * there is no single generator here and no attempt to invent one: the inputs
 * share nothing. The DECODE is what is genuinely common, and that lives in the
 * GLSL. This header is the contract between them.
 *
 * WHAT THIS IS NOT FOR. The UI widget kit draws its rounded boxes, rings and
 * capsules as exact geometry with a one-pixel feather, and that decision is
 * documented at length in ui/shape/Shapes.hpp with the two failure modes that
 * produced it. Nothing here supersedes it. The relevant difference is that a
 * shape shader evaluates an analytic distance in the shape's LOCAL space,
 * where the antialiasing band cannot be a screen pixel; a sampled field
 * decoded through sdf.glsl derives its band from the screen-space derivative,
 * which is a different technique that happens to share a name.
 */
#pragma once

namespace cromwell::sdf {

/* The value a texel holds exactly on the boundary. Inside is above, outside
 * below.
 *
 * Half rather than zero because these fields live in unsigned 8-bit textures,
 * so the representable range is 0..1 and both signs need room. It also means a
 * field can be authored or debugged as an ordinary greyscale image where mid
 * grey is the outline, which is worth more than it sounds the first time one
 * of these bakes wrong. */
inline constexpr float kEdgeValue = 0.5f;

/* How many texels of the field the full 0..1 range spans — the "spread".
 *
 * This is the one number that MUST travel with the atlas rather than being
 * agreed by convention, because the right value is a property of what the
 * field is for:
 *
 *   - Past this distance from the boundary the field clamps and has no
 *     gradient, so any effect reaching further than the range simply stops.
 *     A glow that wants to reach eight pixels needs a range that reaches
 *     eight pixels.
 *   - Every texel of range is texel of the atlas NOT spent on the shape, so a
 *     generous range costs resolution where it matters.
 *
 * The defaults below are starting points for the two known consumers, not
 * limits. A bake writes the value it actually used into its metrics, and the
 * shader is handed that; these constants exist so a caller with no reason to
 * think about it does not have to. */
inline constexpr float kTextPxRange = 4.0f;

/* Wider, deliberately. A coastline is sampled well inland and well out to sea
 * — shallows, foam, a material blend — where text is only ever sampled within
 * a pixel or two of its outline. */
inline constexpr float kMaskPxRange = 16.0f;

/* How many channels a field carries, which is decided by what generated it
 * rather than by preference. */
enum class Channels {
    /* One. From a raster mask, where the input has no corners to preserve
     * because it never had exact ones to begin with. A third of the memory. */
    Single,

    /* Three, each carrying a different subset of the shape's edges so that the
     * median of them reconstructs a corner exactly. From vector outlines,
     * where corners are real and rounding them off is visible — see the note
     * on sdfMedian in sdf.glsl. */
    Multi,
};

/* The default range for a field of this kind. Convenience, so a caller that
 * has not thought about spread gets a sane one rather than a zero. */
constexpr float defaultPxRange(Channels channels)
{
    return channels == Channels::Multi ? kTextPxRange : kMaskPxRange;
}

}  // namespace cromwell::sdf
