/* RibbonConstants.hpp — the recovered MovementBorder parameters.
 *
 * SINGLE RESPONSIBILITY: state the numbers. The shader is a port of
 * UI_3D.Tile.MovementBorder, recovered node-for-node from the WotC SDK package
 * and written out as readable HLSL in study/xcom2_movement_border.hlsl. The
 * GLSL that actually runs lives in assets/shaders/ribbon.{vs,fs}.glsl.
 *
 * Three things carry the look:
 *
 *   - It is MLM_Unlit. The emissive IS the output, and in-game the glow around
 *     the ribbon is the scene's bloom picking that emissive up. We have no
 *     bloom chain, so GlowPass re-draws the ribbon overbright into a half-res
 *     target and blurs it back over the frame.
 *
 *   - It is bDisableDepthTest. Occlusion is done in the pixel shader by
 *     comparing PixelDepth against DestDepth, which is what makes it SOFT: the
 *     ribbon has ~10uu of grace behind the world, then ~10uu of falloff. That
 *     is what lets a stepped staircase rise through a ribbon riding the smooth
 *     plane underneath and dissolve it rather than slice it.
 *
 *   - The height fade is XCOM's own Custom node, verbatim:
 *       1 - saturate((WorldPos.z - (FadeHeight - FadeDistance)) / FadeDistance)
 *     which hides border ABOVE the viewing level. One-sided, not a window:
 *     floors below you stay fully visible, which is why walking upstairs
 *     reveals rather than swaps.
 */
#pragma once

namespace xcom {

/* One unreal unit. XCOM works in uu with z up; we work in tiles with y up, and
 * a tile is WORLD_StepSize = 96uu. Every constant below is a recovered uu value
 * carried across by this one factor, so they can be checked against the study
 * file by eye. */
inline constexpr float kUnrealUnit = 1.0f / 96.0f;

/* [XComGame.XComMovementGridComponent], XComGame.ini */
inline constexpr float kRibbonLift   = 4.0f * kUnrealUnit;  /* MovementBorderHeightOffset */
inline constexpr float kRibbonWidth  = 5.0f * kUnrealUnit;  /* MovementBorderWidth        */
inline constexpr float kRibbonUvTile = 1.0f;                /* UVTilingDistance 96uu      */

/* UI_3D.Tile.MovementBorder's own numbers. kRibbonDepthRate is per WORLD unit
 * rather than per uu, which is the same 0.05 seen through kUnrealUnit. */
inline constexpr float kRibbonWpoPush    = 8.0f  * kUnrealUnit;   /* Constant_5           */
inline constexpr float kRibbonHideFade   = 48.0f * kUnrealUnit;   /* Constant_39          */
inline constexpr float kRibbonDepthRate  = 0.05f / kUnrealUnit;   /* Constant_41          */
inline constexpr float kRibbonDepthFloor = 0.5f;                  /* ConstantClamp_11.Min */
inline constexpr float kRibbonPanSpeed   = 0.5f;                  /* Panner_0.SpeedY      */

/* Ours, not XCOM's. An MLM_Unlit emissive only looks emissive because a bloom
 * chain is downstream of it; these drive the stand-in for that chain.
 *   Emissive  how far overbright the glow pass re-draws the ribbon. Needs a
 *             float target to survive, which is why the glow buffers are
 *             RGBA16F — at 8 bits per channel this clamps to 1 and does
 *             nothing at all.
 *   Steps     how many widening blur iterations. Each doubles its tap spacing,
 *             so the halo reaches 2^n texels and the sum across iterations
 *             gives the tight core AND the wide skirt.
 *   Gain      overall strength of the halo added back over the frame. */
inline constexpr float kRibbonEmissive  = 4.5f;
inline constexpr int   kRibbonGlowSteps = 3;
inline constexpr float kRibbonGlowGain  = 0.85f;

/* How fast each widening octave is weighted down. Lower keeps the halo tight
 * around the line; higher spreads it into a haze that reads as a FATTER ribbon
 * rather than a brighter one, which is the trap here — the ribbon's width is
 * fixed by MovementBorderWidth and should not be argued with from the glow. */
inline constexpr float kRibbonGlowFalloff = 0.45f;

/* MovementBorderLengthFactor 0.8 => the edge stops a fifth short of each
 * corner, and the gap becomes the 45-degree connector.
 *
 * INSET IS ZERO ON PURPOSE. XCOM's border config has Width, HeightOffset,
 * LengthFactor, CurveSmoothing, CurveResolution and UVTilingDistance — and no
 * inset at all. The ribbon straddles the tile boundary rather than sitting
 * back from it, which is what makes it cap a kerb instead of floating behind
 * the lip.
 *
 * kRibbonWallClearance is used ONLY where the band boundary is a wall — our
 * wall art is 0.09 thick and straddles the tile edge (+-0.045), so a line on
 * the edge would sit entirely inside it. 0.045 of wall + ~0.028 of ribbon
 * half-width, rounded up. Everywhere else the line stays on the grid line. */
inline constexpr float kRibbonWallClearance = 0.08f;
inline constexpr float kRibbonChamfer       = 0.11f;

/* XCOM parks HideHeight at 1000 to disable the height fade entirely. */
inline constexpr float kHideHeightOff = 1.0e6f;

}  // namespace xcom
