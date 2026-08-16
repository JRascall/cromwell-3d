//=============================================================================
// XCOM 2 - UI_3D.Tile.MovementBorder
//
// The material the tactical movement-coverage ribbon is drawn with, recovered
// node-for-node from the WotC SDK and written out as readable HLSL.
//
//   package  XComGame/Content/XCOM_2/Packages/UI/UI_3D.upk
//   object   Tile.MovementBorder            (UE3 package ver 845, licensee 105)
//   bound by XComGame/Config/XComEngine.ini:
//              MovementGridMaterialName=UI_3D.Tile.MovementBorder
//              MovementGridDotsMaterialName=UI_3D.Tile.MovementTile
//
// UE3 materials are node graphs, not source. Everything below - every constant,
// every connection, the two Custom-node bodies verbatim - is transcribed from
// the serialised MaterialExpression objects in that package. MovementBorder_Line,
// the 64x64 BC5 profile texture, was originally the one exception; it has since
// been read out exactly, and LINE PROFILE at the bottom carries the numbers plus
// the three places the shipped port's analytic version differs from them.
//
// The sibling material is ./xcom2_path_ribbon.hlsl - CursorRibbon, the path line
// from the unit to the puck. Same package, same week, same house profile curve,
// and a deliberately different answer to occlusion; read them together.
//
// See ../src/render/ribbon.c + ../assets/shaders/ribbon.*.glsl for the port
// this project actually runs, and study/README.md for provenance.
//=============================================================================

//-----------------------------------------------------------------------------
// Material settings (UMaterial properties, as serialised)
//-----------------------------------------------------------------------------
//   BlendMode                    BLEND_Translucent
//   LightingModel                MLM_Unlit          <- EmissiveColor IS the output
//   TransLightingType            TLT_NonDirectional
//   TwoSided                     true
//   bDisableDepthTest            true               <- draws over everything;
//                                                      occlusion is done in the
//                                                      shader, see OPACITY
//   bAllowFog                    false
//   bForceRenderBeforeFOW        true
//   bIs3DUI                      true
//   bUsedWithMovementGrid        true
//   ForceNoHaveSeenFOW           EHVF_FullVisible
//
// Being MLM_Unlit means no lighting term is ever evaluated: the pixel shader
// returns EmissiveColor and the translucent blend does the rest.
//
// THE GLOW IS NOT A GLOW. This file used to say the ribbon's halo was its
// emissive carried into the scene's bloom. That is wrong three times over, and
// the correction matters because the port was built on it - see WHY IT READS AS
// FLUORESCENT at the bottom.
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Parameters (MaterialExpressionScalar/VectorParameter defaults)
//-----------------------------------------------------------------------------
float4 Color            = float4(0.17677, 0.66612, 0.66612, 1.0);  // teal
float4 Color2           = float4(0.17677, 0.66612, 0.66612, 1.0);  // see DEAD NODES
float4 Color3           = float4(1.0,     0.69408, 0.19752, 0.0);  // amber
float  HideHeight       = 1000.0;   // uu; parked high == height fade disabled
float  BorderRelevance  = 1.0;      // 0 = scrolling line, 1 = static line

Texture2D    MovementBorder_Line;   // 64x64, PF_BC5, TC_TwoChannels, TA_Clamp,
SamplerState LineSampler;           // TMGS_NoMipmaps. Two 1-D cross-section
                                    // profiles packed as R and G.

//-----------------------------------------------------------------------------
// Baked constants, with the expression each one came from
//-----------------------------------------------------------------------------
static const float WPO_PUSH      = 8.0;    // Constant_5   uu toward the camera
static const float HEIGHT_FADE   = 48.0;   // Constant_39  uu, half a tile
static const float DEPTH_RATE    = 0.05;   // Constant_41  1/uu -> 20uu of fade
static const float DEPTH_FLOOR   = 0.5;    // ConstantClamp_11.Min
static const float OPACITY_SCALE = 2.0;    // ConstantScale_9.Scale
static const float PAN_SPEED_V   = 0.5;    // Panner_0.SpeedY, UV/second

// Related gameplay-side constants, from XComGame/Config/XComGame.ini
// [XComGame.XComMovementGridComponent] - these shape the ribbon MESH, not the
// material, but the material's UVs only make sense alongside them:
//   MovementBorderWidth         5.0    uu across
//   MovementBorderHeightOffset  4.0    uu off the floor
//   MovementBorderLengthFactor  0.8    edge stops a fifth short of each corner
//   CurveSmoothing             16.0
//   CurveResolution             0.5
//   UVTilingDistance           96.0    uu -> V repeats once per tile

//-----------------------------------------------------------------------------
// Custom nodes - these two are verbatim, they are the only literal HLSL that
// ships inside the material.
//-----------------------------------------------------------------------------

// MaterialExpressionCustom_15, Description = "Height Fading", CMOT_Float1.
// Inputs: FadeHeight <- HideHeight, WorldPos <- WorldPosition, FadeDistance <- 48.
//
// One-sided: 1 below the cut, ramping to 0 across FadeDistance above it. Floors
// BELOW the storey you are looking at stay fully lit, which is why stepping up
// a level reveals the next ribbon rather than swapping to it.
float HeightFading(float3 WorldPos, float FadeHeight, float FadeDistance)
{
    return 1.0-saturate((WorldPos.z - (FadeHeight-FadeDistance))/ FadeDistance);
}

// The sibling engine material XComEngineMaterials.MovementGrid_Material carries
// a second Custom node that this one does not - a cursor-proximity brighten that
// feeds ITS emissive. Kept here because it is the only place XCOM's movement
// visuals read the cursor, and because it explains the XComCursorPosition node:
//   return 1.0-saturate(length(CursorPos - WorldPos )/ FadeDistance);   // 400uu

//-----------------------------------------------------------------------------
// VERTEX: WorldPositionOffset
//-----------------------------------------------------------------------------
// Multiply_8 = Normalize_0(Subtract_0(CameraWorldPosition, WorldPosition)) * 8
//
// The whole ribbon is shoved 8uu along the eye ray before projection. With
// bDisableDepthTest the ribbon cannot z-fight anyway; the push is what keeps
// the 4uu-lifted strip from being swallowed by the floor it lies on at grazing
// camera angles, where 4uu of vertical lift projects to nothing.
float3 GetWorldPositionOffset(float3 WorldPos, float3 CameraWorldPos)
{
    return normalize(CameraWorldPos - WorldPos) * WPO_PUSH;
}

//-----------------------------------------------------------------------------
// PIXEL: EmissiveColor
//-----------------------------------------------------------------------------
// EmissiveColor <- VectorParameter_4 "Color", masked to RGB. That is the entire
// chain: one parameter, straight to the output. Everything expressive about the
// ribbon lives in Opacity; the colour is flat and is driven per-band from code
// (teal for the move ring, amber for the dash ring).
float3 GetMaterialEmissive()
{
    return Color.rgb;
}

//-----------------------------------------------------------------------------
// PIXEL: Opacity
//-----------------------------------------------------------------------------
// ConstantClamp_0( Multiply_41( ConstantScale_9(lerp), Min_1(height, depth) ) )
//
// Three factors: which line profile to draw, how far above the viewing storey
// this fragment is, and how far behind the world it has sunk.
float GetMaterialOpacity(float2 UV, float3 WorldPos, float PixelDepth, float DestDepth, float Time)
{
    // -- line profile ---------------------------------------------------------
    // LinearInterpolate_15: A = Line(Panner(UV)).r, B = Line(UV).g, Alpha =
    // BorderRelevance. Same texture sampled twice; the R channel is the profile
    // that scrolls, the G channel the one that sits still. Default 1.0 means the
    // static line - the scroll is opted into per band, not always on.
    float2 pannedUV  = UV + float2(0.0, PAN_SPEED_V * Time);  // Panner_0, SpeedY 0.5
    float  scrolling = MovementBorder_Line.Sample(LineSampler, pannedUV).r;
    float  standing  = MovementBorder_Line.Sample(LineSampler, UV).g;
    float  lineMask  = lerp(scrolling, standing, BorderRelevance);

    // -- height fade ----------------------------------------------------------
    float height = clamp(HeightFading(WorldPos, HideHeight, HEIGHT_FADE), 0.0, 1.0);

    // -- soft depth occlusion -------------------------------------------------
    // Subtract_14 is PixelDepth - DestDepth: positive means this fragment sits
    // BEHIND what is already in the depth buffer. DestDepth is un-normalised, so
    // the difference is in unreal units.
    //
    // The clamp floor of 0.5 is the trick. It gives a 10uu dead zone in which
    // the ribbon is at full strength even though it is technically buried, then
    // 10uu of falloff, then nothing:
    //
    //     0uu behind   ->  clamp(0.00, .5, 1) = 0.5  ->  0.5   full
    //    10uu behind   ->  clamp(0.50, .5, 1) = 0.5  ->  0.5   full
    //    15uu behind   ->  clamp(0.75, .5, 1) = 0.75 ->  0.25  fading
    //    20uu behind   ->  clamp(1.00, .5, 1) = 1.0  ->  0.0   gone
    //
    // Because the material never depth-tests, this IS the depth test - a soft
    // one. It is what lets a decorative stepped staircase rise through a ribbon
    // riding the smooth plane underneath and dissolve it instead of clipping it,
    // and what stops the ribbon painting itself across the wall in front of it.
    float behind = PixelDepth - DestDepth;
    float depth  = 1.0 - clamp(behind * DEPTH_RATE, DEPTH_FLOOR, 1.0);   // [0, 0.5]

    // Min_1, not a multiply: whichever reason to disappear is stronger wins.
    // The depth term maxes out at 0.5, which is exactly what ConstantScale_9's
    // 2.0 undoes - an unoccluded fragment lands back at full lineMask strength.
    return clamp(OPACITY_SCALE * lineMask * min(height, depth), 0.0, 1.0);
}

//-----------------------------------------------------------------------------
// Assembled - what the compiled material actually evaluates
//-----------------------------------------------------------------------------
void MovementBorder_PS(float2 UV, float3 WorldPos, float PixelDepth, float DestDepth,
                       float Time, out float3 OutColor, out float OutAlpha)
{
    OutColor = GetMaterialEmissive();                                   // unlit
    OutAlpha = GetMaterialOpacity(UV, WorldPos, PixelDepth, DestDepth, Time);
}

//-----------------------------------------------------------------------------
// DEAD NODES
//-----------------------------------------------------------------------------
// Four expressions are authored, saved and compiled into the package but wired
// to nothing - EmissiveColor takes VectorParameter_4 directly, so none of this
// is reachable. Recorded because it shows the intended two-colour scheme that
// the shipped material collapses to one flat parameter:
//
//   If_0                 = Color.b > 1 ? Color2 : (== ? Color2 : Color3)
//   LinearInterpolate_1  = lerp(float3(1,0,0), Color.rgb, BorderRelevance)
//   LinearInterpolate_2  = lerp(float3(0,1,0), float3(1,0,0), Color.b)
//   Constant3Vector_0/_1 = float3(1,0,0), float3(0,1,0)
//
// The engine-default fallback material, XComEngineMaterials.MovementGrid_Material,
// carries the same idea live: lerp(Color.rgb, float3(1.0, 0.816, 0.22),
// IndicateUseRestOfMoves) - teal fading to amber as the path eats into the
// second move. Also unreachable there.

//-----------------------------------------------------------------------------
// LINE PROFILE - recovered, and it corrects what this file used to say
//-----------------------------------------------------------------------------
// MovementBorder_Line was written up here as the one asset that could not be
// transcribed, on the grounds that it is BC5. That was wrong, or at least
// needlessly pessimistic: the SDK's exporter decompresses it on the way out.
//
//   XComGame.com batchexport UI_3D Texture2D TGA <out>    -> the pixels
//   XComGame.com batchexport UI_3D Texture2D T3D <out>    -> the properties
//
// Properties, as serialised:
//   64x64, PF_BC5, TC_TwoChannels, TMGS_NoMipmaps, AddressX=TA_Clamp
//   SourceFilePath  3DUI/MovementBorder_Line.psd   (2015-09-15)
//
// The axis convention this file already assumed is confirmed by the pixels: U is
// the ribbon's WIDTH (hence TA_Clamp on X), V is its length. Blue is zero
// throughout - it really is a two-channel mask.
//
// -- G, the standing profile -------------------------------------------------
// Flat along V, so it is one 1-D cross-section. A shoulder over the outer
// quarter each side, plateau across the middle half. Measured, U = 0 .. 15:
//
//   0.035 0.082 0.141 0.204 0.275 0.345 0.424 0.498
//   0.580 0.655 0.729 0.796 0.859 0.918 0.965 1.000
//
// then 1.000 through U = 47, then the mirror image down to 0.000 at U = 63. The
// two ends are half a texel out of step with each other (0.035 at one rim,
// 0.000 at the other) - authored, not symmetric by construction.
//
// This is the SAME curve as CursorTrail_MSK's cross-section, to within a
// thousandth, just laid on the other axis. See ./xcom2_path_ribbon.hlsl, which
// tabulates it and records that it is an S-curve but NOT a smoothstep - the
// closest simple fit found is still 6.5% out at the shoulder. Two materials, two
// textures, one house profile. Use the table.
//
// -- R, the scrolling profile ------------------------------------------------
// The same cross-section gated along V by an exact 50% square wave: 16 texels at
// 1.0, 16 at 0.102, twice across the texture. So TWO dashes per V unit, and V
// tiles once per 96uu tile, therefore two dashes per tile.
//
// Note the gaps are 0.102, not zero - a dark dash train on a continuous line,
// not a row of separate pips. CursorTrail_MSK does the same thing with a floor
// of 0.5; same idea, four times shallower here.
//
// -- what the port actually draws --------------------------------------------
// ../assets/shaders/ribbon.fs.glsl `lineProfile` reconstructs this analytically
// and predates the recovery above, so it is worth stating the differences rather
// than assuming they are settled:
//
//   section width   port  plateau to |e| = 0.24, ink gone by |e| = 0.86
//                   game  plateau to |e| = 0.50, ink to the rim
//                   -> the ported line is roughly half the width of XCOM's, and
//                      the shader comment says so ("widening these two numbers
//                      fattens the ribbon without touching its geometry"), so
//                      this is a look decision that was tuned by eye, not a bug.
//   dash period     port  one dash per tile          game  two
//   dash floor      port  gaps to 0                  game  gaps to 0.102
//
// The dash channel is reached only when BorderRelevance < 1, and the shipped
// value is 1.0, so in the default configuration none of the last two rows is
// visible in either the game or the port.
//
// The port's third output, `core`, has no counterpart in the texture at all - it
// is a port-side invention feeding the glow pass, which is already recorded
// above as not being XCOM's.

//-----------------------------------------------------------------------------
// WHY IT READS AS FLUORESCENT
//-----------------------------------------------------------------------------
// The ribbon looks lit from within. It is not, and there is no bloom involved
// anywhere. Three independent facts, all from the SDK:
//
//  1. BLOOM IS OFF, GLOBALLY.
//       XComGame/Config/XComEngine.ini, [SystemSettings]:  Bloom=False
//     Not a quality tier - [SystemSettings] is the base section, and the only
//     other Bloom line in the file is under [SystemSettingsMobile]. The same
//     block turns off DepthOfField, AmbientOcclusion, SSAO and
//     ScreenSpaceReflections. XCOM 2 is not a bloomy game.
//
//  2. THE MATERIAL NEVER EXCEEDS WHITE. Color is (0.177, 0.666, 0.666). There
//     is no multiply, no HDR scale, nothing above 1.0 anywhere in the emissive
//     chain - it is one parameter straight to the output. Even with a bloom
//     pass switched on, a 0.666 emissive is under any sane threshold.
//
//  3. IT IS DRAWN AFTER THE POST CHAIN ENTIRELY. bIs3DUI, and Firaxis' own
//     comment on it in Engine/Classes/Material.uc is unambiguous:
//       "If true, this material is used with the 3D UI and should be rendered
//        after gamma correction."
//     So it is not tone mapped, not graded, and not available to bloom even in
//     principle.
//
// So where does the halo come from? THE PROFILE TEXTURE. See LINE PROFILE: the
// cross-section is a plateau across the middle half and a soft shoulder over the
// outer QUARTER on each side. Half the ribbon's width is falloff. The soft edge
// is painted into the asset, at authoring time, by an artist - it is not
// computed by anything at runtime.
//
// And the reason a flat 0.666 cyan reads as EMITTING rather than as a painted
// stripe is that every cue which would tie it to the scene's lighting has been
// switched off, one flag at a time:
//
//   MLM_Unlit                     no shading, no gradient - perfectly flat
//   bIs3DUI                       skips tone mapping and colour grading
//   bAllowFog = false             no aerial perspective; it never recedes
//   ForceNoHaveSeenFOW = FullVis  fog of war cannot tint it
//   bDisableDepthTest             nothing in front of it, ever
//
// That is the perceptual mechanism of fluorescence, and it is worth stating
// plainly because it is cheap to reproduce and easy to reach for a bloom pass
// instead: a surface reads as fluorescent when it is MORE SATURATED THAN THE
// ILLUMINATION CAN EXPLAIN. The eye estimates the light from the scene, infers
// the gamut a real surface could occupy under it, and anything outside that
// gamut is read as self-luminous. Everything real in an XCOM frame has been
// through the same tone map and the same grade, which is exactly what
// establishes that gamut - and the ribbon is exempt from all of it. It sits
// outside the frame's gamut by construction while being DIMMER than white.
//
// Not brightness. Purity. It glows the way a hi-vis vest glows, and a hi-vis
// vest does not emit either.
//
// -- the consequence for this project ----------------------------------------
// ../assets/shaders/ribbon_glow.fs.glsl exists because this file used to claim
// the halo was scene bloom: with no bloom chain here, the port re-draws the
// ribbon overbright and blurs it back over the frame to stand in for one. There
// is nothing to stand in for. XCOM's halo is fifteen texels of authored shoulder
// on a 64-wide profile, and the port already reconstructs a shoulder in
// `lineProfile` - narrower than XCOM's, per the table above.
//
// This does not automatically make the glow pass wrong; it makes it OURS, an
// addition rather than a reproduction, and it should be judged by eye on that
// basis. plans/bloom_emissive.md treats the GlowPass as a stopgap awaiting a
// real bloom stage, and that framing needs revisiting too - the thing it is a
// stopgap for does not exist in the game being copied.

//-----------------------------------------------------------------------------
// REPLICATION RECIPE
//-----------------------------------------------------------------------------
// Everything above is analysis. This is the short version: what to actually do
// to get the look, in any renderer, ranked by how much of the effect each step
// carries. Checked against this tree as of writing.
//
//  1. DRAW IT AFTER THE TONE MAPPER, IN DISPLAY COLOUR.  [we already do this]
//     This is the single most important step and the easiest to undo by
//     accident. The authored colour must reach the framebuffer unmodified - not
//     tone mapped, not exposed, not graded. That is what puts it outside the
//     frame's gamut and makes it read as emitting.
//
//     In this tree that is Camera::ScenePhase::Display, and FrameRenderer draws
//     the rings there, after ToneMapPass. It matches XCOM's bIs3DUI exactly.
//
//     CONSEQUENCE FOR plans/bloom_emissive.md: that note's open question - the
//     one it says "needs eyes rather than a header argument", whether the rings
//     should trade their crisp display-colour ink for tone-mapped emissive - now
//     has an evidence-based answer. Tone-mapping them would move AWAY from XCOM
//     and would cost exactly the property this section is about. Keep the ink.
//
//  2. AUTHOR THE SOFT EDGE INTO THE CROSS-SECTION. Plateau across the middle
//     half, shoulder over the outer quarter each side, per the 17 values in
//     LINE PROFILE. The halo is geometry-space, at the ribbon's own scale, and
//     it therefore stays put when the camera moves. A screen-space blur does
//     not, which is the tell that separates this from a bloom.
//
//  3. TURN OFF EVERY SCENE CUE. Unlit, no fog, no fog-of-war tint, no depth
//     test - and do the occlusion in the shader instead, so it stays a soft
//     artistic fade rather than a hardware yes/no. All five are listed above
//     with the flag each one corresponds to.
//
//  4. KEEP THE COLOUR BELOW WHITE. 0.666, not 1.0 and certainly not 4.0. The
//     instinct when something should glow is to overdrive it; XCOM does the
//     opposite and it is why the ribbon never blows out over a bright floor.
//
//  5. DO NOT ADD A BLOOM OR GLOW PASS FOR THIS. Steps 1-4 are the whole effect.
//     If a glow pass is wanted anyway it is a separate, deliberate look of our
//     own - which is exactly what ribbon_glow.fs.glsl currently is.
//
// Known deltas between this tree and XCOM, both recorded in LINE PROFILE above:
// our section is about half XCOM's width, and we have the extra glow pass. Both
// are look decisions, neither is a bug, and both want eyes rather than a table.
