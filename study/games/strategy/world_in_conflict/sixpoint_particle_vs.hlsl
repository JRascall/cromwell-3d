// SixPointLightParticleVertexShader — World in Conflict (Massive Entertainment, 2007)
//
// PROVENANCE. This is not a file that ships on disk. WiC compiles its vertex
// shaders at runtime from HLSL held as string literals inside wic.exe, and the
// compiler split each one across hundreds of separate literals — so the bytes
// recovered from the executable arrive as an ordered run of fragments, one
// declaration or statement per fragment. What follows is that run reflowed into
// compilable form: every token, identifier, constant register and expression is
// Massive's, transcribed in order; only the whitespace and the brace/semicolon
// placement that the literal boundaries had eaten are ours. Nothing is
// paraphrased and nothing is filled in — where the recovered text is ambiguous
// the note says so rather than inventing.
//
// Recovered from: E:\World in Conflict\wic.exe, v1.0.1.0 (build dated 2009-06-10),
// ASCII string region; the sibling NormalMappedParticleVertexShader sits directly
// after it and shares the constant layout.
//
// WHAT IT IS. The vertex half of six-point (a.k.a. six-way) particle lighting.
// The pixel half is particle_sixpointlight.sur, beside this file. Together they
// light a smoke billboard from six baked directional channels rather than
// treating it as a flat unlit sprite — see world_in_conflict_particles.md §2.
//
// The two ideas worth taking, both visible below:
//
//  1. The six-direction basis is the BILLBOARD'S OWN FRAME (±right, ±up,
//     ±front), rebuilt per vertex from the camera. It is not a world-fixed
//     basis. That is what lets one authored texture pair light correctly from
//     any sun angle at any camera angle, and it is why the baking tool can
//     render the six channels once, orthographically, and be done.
//
//  2. sun0.w carries TWO things multiplied together: the normalisation of the
//     six weights, and a plume-scale self-shadowing term derived from where
//     this particle sits relative to the effect's origin (myPePos). The second
//     is the "cluster lighting" of CLUSTERLIGHTING / CLUSTERLIGHTINGBONE in the
//     .pe files. It costs one dot product and it is most of why WiC's smoke
//     reads as a lit volume instead of a pile of lit sprites.

float4x4 myProjection        : register(c0);
float4   myFogConstants      : register(c4);
float4   myFogConstants2     : register(c5);
float4   mySunVector         : register(c6);
float4   myPePos             : register(c7);   // world origin of the whole effect
float4   myUseClusterLight   : register(c8);   // .x = scale, .y = bias; (0,1) disables
float4x4 myWorldToCam        : register(c9);
float2   myInvMapSize        : register(c13);

struct VSINPUT
{
    float3 myPos    : POSITION;
    float4 diffuse  : COLOR0;
    float4 myUV     : TEXCOORD0;
    float4 myUV2    : TEXCOORD1;   // .xy = billboard up vector (fixed 1/32767)
                                   // .z  = per-particle scalar /255
                                   // .w  = V-flip flag (1 or 2 -> flipV 0 or 1)
};

struct VSOUT
{
    float4 oPosition : POSITION;
    float  fog       : FOG;
    float4 color     : COLOR0;
    float4 sun1      : COLOR1;     // .xyz = weights for -right,-up,-front
                                   // .w   = per-particle scalar
    float3 t0        : TEXCOORD0;  // .xy = UV, .z = saturated fog
    float2 t1        : TEXCOORD1;
    float2 t2        : TEXCOORD2;
    float4 sun0      : TEXCOORD3;  // .xyz = weights for +right,+up,+front
                                   // .w   = 1/sum(all six) * cluster term
    float4 t4        : TEXCOORD4;  // .xyz = world position, .w = depth * 1e-4
    float2 t5        : TEXCOORD5;  // world XZ normalised to map — heightmap lookup
    float2 t6        : TEXCOORD6;  // detail-texture UV
};

// Exponential-ish fog folded into one lit() instruction — the standard ps_1_1-era
// trick for getting a pow() without a pow().
float fog(float dist, float2 fogC1, float3 thresInvThresExp)
{
    float temp = dist * fogC1.x + fogC1.y;
    temp = lit(temp, temp, thresInvThresExp.z).z;
    return temp * thresInvThresExp.y + thresInvThresExp.x;
}

VSOUT main(VSINPUT v)
{
    VSOUT Out;

    float4 worldPos = float4(v.myPos, 1);
    float4 pos = mul(worldPos, myWorldToCam);
    Out.oPosition = mul(pos, myProjection);

    float l2     = mul(pos, pos);
    float invlen = rsqrt(l2);
    float len    = 1.0f / invlen;
    Out.fog  = fog(len, myFogConstants, myFogConstants2);
    Out.t0.z = saturate(Out.fog);

    Out.t0.xy = v.myUV.xy * (1.f / 0x0100);
    Out.t1    = v.myUV.xy * (1.f / 0x0100);
    Out.t2    = v.myUV.xy * (1.f / 0x0100);
    Out.t6    = v.myUV.zw * (1.f / 0x0100);

    // The billboard frame, rebuilt per vertex. 'front' is the normalised
    // view vector, so the basis follows the camera; 'up' comes packed in the
    // vertex so a particle can be rolled or world-aligned without a new shader.
    float3 front = pos.xyz * invlen;
    float3 up    = float3(v.myUV2.x * (1.0 / 32767), v.myUV2.y * (1.0 / 32767), 0);
    float3 right = cross(front, up);
    right = normalize(right);
    up    = cross(right, front);

    // RANDOMFLIPV mirrors the sprite in V to hide repetition across a plume.
    // A mirrored sprite has a mirrored left/right lighting response, so the
    // sign of the 'right' weights has to flip with it or the lighting detaches
    // from the image. This is the whole cost of that correctness: one multiply.
    float flipV = v.myUV2.w - 1.0;

    Out.sun0.x = saturate(dot( right, -mySunVector) * flipV);
    Out.sun0.y = saturate(dot( up,     mySunVector));
    Out.sun0.z = saturate(dot( front, -mySunVector));
    Out.sun1.x = saturate(dot(-right, -mySunVector) * flipV);
    Out.sun1.y = saturate(dot(-up,     mySunVector));
    Out.sun1.z = saturate(dot(-front, -mySunVector));

    // Normalise: exactly one hemisphere's worth of weight is ever non-zero, but
    // the split between the three axes varies, so divide by the total to keep
    // brightness constant as the sun swings around.
    Out.sun0.w = 1 / (dot(Out.sun0.xyz, float3(1,1,1)) + dot(Out.sun1.xyz, float3(1,1,1)));

    // Cluster lighting. The vector from the effect's origin to this particle,
    // dotted with the sun, darkens particles on the far side of the plume and
    // brightens the near side — a sphere's worth of self-shadowing for one dot
    // product, with no sorting, no depth pass and no per-particle occlusion query.
    Out.sun0.w *= dot(normalize(pos - myPePos), mySunVector) * myUseClusterLight.x
                + myUseClusterLight.y;

    Out.sun1.w = v.myUV2.z / 255;

    Out.t4.xyz = worldPos;
    Out.t4.w   = Out.oPosition.w * 0.0001;   // depth, rescaled for the DX10 z-feather
    Out.t5     = worldPos.xz * myInvMapSize.xy;  // DX9 z-feather: terrain heightmap UV
    Out.color  = v.diffuse;

    return Out;
}
