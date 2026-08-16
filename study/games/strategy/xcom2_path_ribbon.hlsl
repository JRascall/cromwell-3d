//=============================================================================
// XCOM 2 - UI_3D.CursorSet.CursorRibbon
//
// The material the movement PATH line is drawn with - the band that traces the
// route from the selected unit to the puck, fading in as it leaves the unit and
// out again as it reaches the destination tile. Recovered node-for-node from the
// SDK and written out as readable HLSL.
//
//   package  XComGame/Content/XCOM_2/Packages/UI/UI_3D.upk
//   object   CursorSet.CursorRibbon            (UE3 package ver 845, licensee 105)
//   bound by XComGame/Config/XComGameCore.ini, [XComGame.XComPathingPawn]:
//              PathMaterialNormalName  = UI_3D.CursorSet.CursorRibbon
//              PathMaterialDashingName = UI_3D.CursorSet.CursorRibbon_Dashing
//            and XComPrecomputedPath.uc (the grenade/rocket arc), which hard-codes
//              PathingRibbonMaterialName = "UI_3D.CursorSet.CursorRibbon"
//
// This is the SIBLING of ../xcom2_movement_border.hlsl. That one is the boundary
// around everywhere you *could* go; this one is the line to where you *are*
// going. They are drawn by different components, they are different materials,
// and - the point of reading them together - they solve occlusion two different
// ways. See DIFFERENCES FROM MovementBorder at the bottom.
//
// Everything below is transcribed from the serialised MaterialExpression objects
// under CursorRibbon, obtained with the SDK's own commandlet:
//
//   XComGame.com batchexport UI_3D Material T3D <out>
//
// Node defaults the package leaves unserialised were read from the SDK's own
// UnrealScript, Development/SrcOrig/Engine/Classes/MaterialExpression*.uc. Two
// of them matter here and both are easy to get wrong:
//   MaterialExpressionConstantScale defaults Scale to 0.5, not 1
//   MaterialExpressionConstantClamp defaults Min=0, Max=1
//
// Unlike MovementBorder, this material contains NO Custom nodes - there is no
// hand-written HLSL inside it at all. It is twenty-seven stock nodes, six of
// which are dead.
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
//   bForceNonHalfResTranslucency true               <- MovementBorder does NOT
//                                                      set this. A one-pixel-wide
//                                                      line drawn into a half-res
//                                                      translucency buffer is a
//                                                      staircase; a five-unit-wide
//                                                      boundary band survives it.
//                                                      Confirmed by config, not
//                                                      just reasoning: XComEngine.ini
//                                                      [SystemSettings] sets
//                                                      bAllowDownsampledTranslucency
//                                                      =True with a 1500 distance
//                                                      threshold, so the buffer this
//                                                      opts out of is really there.
//   bIs3DUI                      true
//   bUsedWithMovementGrid        true
//   bUsedWithParticleSprites     true               } both set because the ribbon
//   bUsedWithBeamTrails          true               } mesh is built trail-style
//   ForceNoHaveSeenFOW           EHVF_FullVisible
//   WorldPositionOffset          (unconnected)      <- see DIFFERENCES, below
//
// MLM_Unlit means no lighting term is ever evaluated: the pixel shader returns
// EmissiveColor and the translucent blend does the rest. As with the boundary,
// there is no bloom on this anywhere: [SystemSettings] in XComEngine.ini sets
// Bloom=False globally, the emissive never exceeds 0.666, and bIs3DUI means it
// is drawn after gamma correction and so after the post chain entirely. The
// halo is authored into the mask's shoulder. See ./xcom2_movement_border.hlsl,
// § WHY IT READS AS FLUORESCENT, which works this through once for both.
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Parameters (MaterialExpressionVectorParameter_20 default)
//-----------------------------------------------------------------------------
float4 Color = float4(0.18, 0.67, 0.67, 0.0);   // teal, group "Values"

// The only thing any variant changes. All three shipped skins are
// MaterialInstanceConstants over this one material, overriding "Color" and
// nothing else:
//
//   CursorRibbon              (the parent's own default)  0.18,  0.67,  0.67   teal
//   CursorRibbon_Dashing                                  1.0,   0.7,   0.2    amber
//   MIC_CursorRibbon_NoAccess                             0.797, 0.038, 0.010  red
//
// XComPathingPawn.uc swaps between the first two live as the cursor crosses the
// first move's range (RenderablePath.SetMaterial(PathMaterialDashing)), which is
// why the line turns amber mid-drag rather than at the start of it.

Texture2D    CursorTrail_MSK;   // 64x64, PF_A8R8G8B8, TC_TwoChannels, no mips,
SamplerState TrailSampler;      // TEXTUREGROUP_UI3D, AddressX/Y both TA_Wrap.
                                // Source art: 3DUI/MoveLines.psd (2015-02-02).
                                // Two profiles packed as R and G; see TRAIL MASK.

//-----------------------------------------------------------------------------
// Baked constants, with the expression each one came from
//-----------------------------------------------------------------------------
static const float FADE_BIAS     = 0.5;    // Constant_20        added to 1-vertexAlpha
static const float DASH_SCALE    = 0.25;   // ConstantScale_0.Scale
static const float OCCLUDED_MUL  = 0.5;    // ConstantScale_11.Scale (the DEFAULT)
static const float DASH_TILING   = 2.0;    // TextureCoordinate_0.UTiling
static const float DASH_SPEED_U  = -1.0;   // Panner_0.SpeedX, UV/second

// Related gameplay-side constants, from the UnrealScript defaultproperties.
// These shape the ribbon MESH, not the material, but the material's UVs only
// mean anything alongside them.
//
//   XComPathingPawn.uc, Begin Object Class=XComRenderablePathComponent:
//     fRibbonWidth        2.0     the movement path line
//     iPathLengthOffset  -2       ribbon stops short of the spline's end
//     fEmitterTimeStep   10.0
//
//   XComPrecomputedPath.uc (grenade/rocket arc), same component, retuned:
//     fRibbonWidth        1.25    a thinner line for a thrown arc
//     fEmitterTimeStep    0.03
//
//   X2GrapplePuck.uc: 2.0 / -2 / 10 - identical to the move path.
//
// [inferred] The unit of fRibbonWidth is not stated. MovementBorderWidth, the
// comparable knob on the boundary component, is 5.0 unreal units, so this is
// most likely uu as well - i.e. the path line is deliberately THINNER than the
// boundary band it sits inside.
//
// iPathLengthOffset = -2 is worth its own line, because it is half the answer to
// "how does it fade out at the destination": it does not entirely. The geometry
// is built two units short of the spline's end, and the move puck is drawn on
// the destination tile over the gap.

//-----------------------------------------------------------------------------
// PIXEL: EmissiveColor
//-----------------------------------------------------------------------------
// EmissiveColor <- VectorParameter_20 "Color", masked to RGB. One parameter,
// straight to the output - the same shape as MovementBorder. Everything
// expressive lives in Opacity.
float3 GetMaterialEmissive()
{
    return Color.rgb;
}

//-----------------------------------------------------------------------------
// PIXEL: Opacity
//-----------------------------------------------------------------------------
// If_7(A = DestDepth, B = PixelDepth,
//      A > B  -> ConstantClamp_0,      A == B -> ConstantClamp_0,
//      A < B  -> ConstantClamp_13)
//
// DestDepth is the depth already in the buffer, PixelDepth this fragment's. So
// A > B means the world behind this pixel is FURTHER than the ribbon - the
// ribbon is in front, unoccluded. A < B means something is in front of it.
//
// Two entirely separate looks hang off that branch, which is the whole design of
// the material: visible path is a solid band, occluded path is a faint scrolling
// dash. You can follow a route behind a wall without it ever competing with the
// route in front of one.
float GetMaterialOpacity(float2 UV, float4 VertexColor,
                         float PixelDepth, float DestDepth, float Time)
{
    // -- the fade, and it is a fade to HALF -----------------------------------
    // Add_34 = OneMinus_10(VertexColor.a) + 0.5
    //
    // Vertex alpha is written per-vertex by the native ribbon builder; the
    // material's only job is to read it. The arithmetic tells us the range even
    // though the ramp itself is not recoverable (see NOT RECOVERED): the term
    // spans [0.5, 1.5] and is then clamped by the profile multiply into
    // [0.5, 1.0]. The ends of the ribbon go to HALF opacity, never to zero.
    //
    // That matters for anyone porting it. The vanishing at the destination is
    // NOT this term - it is geometric (iPathLengthOffset = -2) plus the move
    // puck covering the stub. Reproducing the fade as alpha -> 0 gives you a
    // ribbon that tapers into nothing and reads as weaker than XCOM's.
    float fade = (1.0 - VertexColor.a) + FADE_BIAS;

    // -- cross-section profile ------------------------------------------------
    // TextureSample_2: CursorTrail_MSK at the DEFAULT texture coordinate - no
    // tiling node, no panner - taking .g. G is flat along U, so this sample is
    // effectively one-dimensional: it is the soft-shouldered profile across the
    // width of the ribbon and nothing else.
    float profile = CursorTrail_MSK.Sample(TrailSampler, UV).g;

    // Multiply_1, then ConstantClamp_0(0,1). This is the unoccluded look in full.
    float visible = clamp(fade * profile, 0.0, 1.0);

    // -- the dash, only ever seen through geometry ----------------------------
    // Panner_0(TextureCoordinate_0(UTiling=2), SpeedX=-1) -> TextureSample_1.r
    //
    // The R channel is the same cross-section profile gated by a hard square
    // wave along the length. UTiling 2 doubles it to four dashes per unit of U,
    // and the panner slides the whole pattern one texture width per second, so
    // the dashes crawl toward the unit at two cycles a second.
    float2 dashUV = float2(UV.x * DASH_TILING + Time * DASH_SPEED_U, UV.y);
    float  dash   = CursorTrail_MSK.Sample(TrailSampler, dashUV).r * DASH_SCALE;

    // Multiply_4 takes Multiply_1 UNCLAMPED (not ConstantClamp_0 - the graph
    // forks before the clamp), scales by 0.5, and clamps that. Transcribed as
    // authored; with dash <= 0.25 the final clamp can never engage.
    float occluded = clamp(fade * profile * dash * OCCLUDED_MUL, 0.0, 1.0);

    // Peak occluded opacity is 1.0 * 0.25 * 0.5 = 0.125 - one eighth. Behind a
    // wall the path is a suggestion, not a line.
    return (DestDepth >= PixelDepth) ? visible : occluded;
}

//-----------------------------------------------------------------------------
// Assembled - what the compiled material actually evaluates
//-----------------------------------------------------------------------------
void CursorRibbon_PS(float2 UV, float4 VertexColor, float PixelDepth, float DestDepth,
                     float Time, out float3 OutColor, out float OutAlpha)
{
    OutColor = GetMaterialEmissive();                                     // unlit
    OutAlpha = GetMaterialOpacity(UV, VertexColor, PixelDepth, DestDepth, Time);
}

//-----------------------------------------------------------------------------
// DEAD NODES
//-----------------------------------------------------------------------------
// Nine expressions form one complete chain that is authored, saved and compiled
// into the package and wired to nothing - Add_12's output goes nowhere:
//
//   ScreenPosition_6 (ScreenAlign=true)
//     -> ConstantScale_10 (x200)
//       -> Add_13 ( + Time_6 )
//         -> ComponentMask_12 (.g)
//           -> Sine_6
//             -> ConstantClamp_12 (0,1)
//               -> Add_12 ( + Constant_15 = 0.2 )    <- unreachable
//
// That is a screen-space horizontal scanline shimmer: 200 cycles down the
// screen, scrolling with time, clamped to the positive half of the sine and
// floored at 0.2. A CRT/hologram pass over the path line, tried and abandoned.
//
// Recorded for the same reason MovementBorder's dead nodes are: it is the only
// evidence of what was tried. Note that it is SCREEN-space, not path-space - the
// shimmer would have stayed put while the ribbon moved under it, which is very
// likely why it is not connected.

//-----------------------------------------------------------------------------
// TRAIL MASK - CursorTrail_MSK, and this one IS recovered
//-----------------------------------------------------------------------------
// MovementBorder's profile texture is BC5 and the boundary study reconstructs it
// analytically. This one is uncompressed PF_A8R8G8B8, so it reads out exactly.
// 64x64, blue channel zero throughout, two meaningful channels:
//
//   V (across the ribbon's width) carries a symmetric edge falloff, identical in
//   both channels: 0 at both edges, flat 1.0 across the middle half, ramping
//   over the outer quarter each side. Measured, at 1/64 steps from the edge:
//
//     0.000 0.035 0.082 0.137 0.204 0.271 0.349 0.424 0.498
//     0.576 0.655 0.729 0.800 0.859 0.914 0.965 1.000
//
//   then flat to v = 0.75, then the mirror image. It is an S-curve but NOT a
//   smoothstep - it is noticeably fuller at the shoulders. Best simple fit found
//   was smoothstep(0, 0.26, v)^0.8, still 6.5% off at worst; plain
//   smoothstep(0, 0.25, v) is 8.2% off. Prefer the table above; it is 17 values.
//
//   U (along the ribbon's length) is where the channels differ:
//     G  flat 1.0 everywhere - the pure width profile, which is why the
//        unoccluded branch samples G at untiled UVs.
//     R  the same profile gated by an exact 50% square wave, 16 texels on and
//        16 off, twice across the texture. Verified as a clean ratio: R/G is
//        exactly 1.0 for x in [0,16) and [32,48), exactly 0.502 for the other
//        two runs.
//
// Note the dash is not on/off, it is 1.0/0.5 - the gaps are half-lit, not empty.
// Combined with the 0.25 and 0.5 scales, the occluded ribbon oscillates between
// 12.5% and 6.3% opacity. It reads as a crawl, not a chase of separate pips.
//
// Also note the axis convention is the OPPOSITE of MovementBorder_Line, where U
// is the width and V the length. Same studio, same package, same week; do not
// assume one from the other.

//-----------------------------------------------------------------------------
// DIFFERENCES FROM MovementBorder
//-----------------------------------------------------------------------------
// Both are MLM_Unlit translucent 3D-UI materials with bDisableDepthTest and a
// flat Color parameter for emissive. Everything else diverges, and each
// divergence is a decision:
//
//  1. OCCLUSION. MovementBorder does a SOFT depth fade - a 10uu dead zone, 10uu
//     of falloff, then gone - because the boundary lies flat on the floor and
//     needs to dissolve gracefully where a stair rises through it. CursorRibbon
//     does a HARD branch and keeps a dim animated version behind geometry,
//     because a route you cannot see the far end of is useless.
//
//  2. WorldPositionOffset. MovementBorder pushes every vertex 8uu along the eye
//     ray so a 4uu-lifted floor strip survives grazing angles. CursorRibbon has
//     none - it does not lie flat on the floor, it rides a spline through the
//     air, so there is nothing to lift it out of.
//
//  3. HEIGHT FADE. MovementBorder's whole Custom node ("Height Fading") hides
//     the band above the storey you are looking at. CursorRibbon has no such
//     term - the path is allowed to cross storeys, and does, whenever the route
//     climbs.
//
//  4. ANIMATION. MovementBorder can scroll its profile but ships with
//     BorderRelevance = 1.0, i.e. the static line; the scroll is opted into.
//     CursorRibbon's scroll is unconditional but confined to the occluded
//     branch, so in the common case neither material moves.
//
//  5. bForceNonHalfResTranslucency, set here and not there. See settings above.

//-----------------------------------------------------------------------------
// NOT RECOVERED
//-----------------------------------------------------------------------------
// The vertex colour ramp. XComRenderablePathComponent is native - the SDK ships
// its declaration (fRibbonWidth, iPathLengthOffset, fEmitterTimeStep, and
// UpdatePathRenderData(InterpCurveVector Spline, float PathLength,
// XComPathingPawn InPawn, vector CameraLocation)) but no C++ body, so how alpha
// is distributed along the ribbon is not readable here. What the material proves
// about it, and it is most of what a port needs:
//
//   - alpha is per-vertex, so the ramp is in the mesh, not the shader
//   - the material inverts it, so HIGH vertex alpha means a DIMMER ribbon
//   - the resulting opacity spans [0.5, 1.0] of the profile - the ends dim by
//     half, they do not disappear
//
// The mesh build itself - how the spline is sampled, how fEmitterTimeStep of 10
// against the precomputed path's 0.03 changes anything, and where the ribbon
// gets its UVs - is likewise native and unread.
//
// The sibling that IS still unrecovered as a whole: UI_3D.Tile.MovementTile, the
// per-tile dots material (MovementGridDotsMaterialName). Its T3D exports cleanly
// by the command at the top of this file; nobody has read it yet.
