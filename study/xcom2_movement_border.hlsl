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
// the serialised MaterialExpression objects in that package. The only thing
// that is a reconstruction rather than a transcription is MovementBorder_Line,
// the 64x64 BC5 profile texture; see LINE PROFILE at the bottom.
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
// returns EmissiveColor and the translucent blend does the rest. The ribbon's
// glow is that emissive value carried into the scene's bloom - the material
// itself contains no glow, falloff or halo of any kind.
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
// LINE PROFILE
//-----------------------------------------------------------------------------
// MovementBorder_Line is the one asset not transcribed here. It is a 64x64 BC5
// two-channel texture holding two 1-D cross-sections of the line, addressed
// clamped across U (the ribbon's width) and tiling along V (its length, once per
// 96uu tile). R scrolls, G does not.
//
// The port reconstructs both analytically instead of shipping the game's texture:
// a soft-shouldered profile across the width, and for the scrolling channel that
// same profile gated by a dash pattern along the length. See
// ../assets/shaders/ribbon.fs.glsl, `lineProfile`. This is the single place the
// port deviates from the material, and it is a deviation of asset, not of maths.
