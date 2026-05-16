cbuffer ObjectConstantBuffer : register(b0)
{
	float4x4 gWorld;
	// float4x4 projection;
	// float deltaSeconds;
}

cbuffer MaterialConstantBuffer : register(b1)
{
	float4 gDiffuseAlbedo;
	uint4 gMaterialTextureIndices0;
	uint4 gMaterialTextureIndices1;
	uint4 gMaterialTextureIndices2;
	uint4 gMaterialTextureIndices3;
	uint gMaterialFlags;
	float gRoughness;
	float gMetallic;
	float gEmissiveIntensity;
	float4 gEmissive;
}

cbuffer PassConstantBuffer : register(b2)
{
	float4x4 gView;
	float4x4 gViewInv;
	float4x4 gProj;
	float4x4 gProjInv;
	float4x4 gViewProj;
	float4x4 gViewProjInv;
	float3 gCameraPos;
	float pad1;
	float2 gRenderTargetSize;
	float2 gRenderTargetSizeInv;
	float gNearPlane;
	float gFarPlane;
	float gTotalTime;
	float gDeltaTime;
	uint gFrameIndex;
	uint gRaytracingSampleCount;   // samples per pixel per frame; tunable at runtime without shader recompile
	uint gAccumulationFrameIndex;  // resets to 0 on any camera / scene movement
	uint gRaytracingMaxBounces;    // upper bound on path depth, tunable at runtime

	uint gRaytracingRussianRouletteDepth;  // depth at which RR termination kicks in
	uint gLightCount;                      // entries in gLights buffer; 0 disables NEE
	uint gPortalCount;                     // entries in gPortals buffer; 0 disables portal pre-filter
	uint gPortalPreFilter;                 // toggle: 0 = baseline (no filter), 1 = pre-filter on
}

// NEE light entry. Mirror of frt::graphics::SLight (96 bytes, 16-aligned).
struct SLight
{
	float3 Position;
	uint   Type;          // 0=Point, 1=Directional, 2=AreaQuad
	float3 Direction;     // Directional: from sun toward scene. AreaQuad: face normal.
	float  Pad0;
	float3 Edge1;         // AreaQuad: half-extent right
	float  Pad1;
	float3 Edge2;         // AreaQuad: half-extent up
	float  Area;
	float4 Emission;      // RGB radiance multiplier
	float  Intensity;
	int    InstanceId;
	int    Pad2;
	int    Pad3;
};

// Bound as root SRV in Hit signature. Empty when gLightCount == 0.
StructuredBuffer<SLight> gLights : register(t19);


// Portal quad — used by the Sky-NEE pre-filter. Mirror of frt::graphics::SPortal (64 bytes).
struct SPortal
{
	float3 Center;
	float  Pad0;
	float3 Normal;       // unit; the portal's facing direction
	float  Pad1;
	float3 Edge1;        // half-extent along right (world-space)
	float  Pad2;
	float3 Edge2;        // half-extent along up
	uint   Flags;        // reserved
};

// Bound as root SRV in Hit signature at t20. Empty / dummy when gPortalCount == 0.
StructuredBuffer<SPortal> gPortals : register(t20);

// Sky / sun parameters. Bound to Miss (and any RT shader that needs sky radiance).
// Mirror of frt::graphics::SSkyConstants — keep field order in sync.
cbuffer SkyConstantBuffer : register(b3)
{
	float3 gSunDirection;       // unit vector FROM sun TO scene
	float  gSunIntensity;
	float4 gSunColor;

	float4 gSkyZenithColor;
	float4 gSkyHorizonColor;
	float4 gGroundColor;

	float  gSkyIntensity;
	float  gSkyHorizonSoftness;
	float  gPadSky0;
	float  gPadSky1;
}

// Procedural sky radiance for a world-space ray direction. Shared between Miss (bounce rays
// that escape the scene) and Hit (sky-NEE shadow ray that escapes → returns sky radiance).
// Sun is intentionally excluded — direct sun comes via NEE on a Directional light entry.
float3 EvalSkyRadiance(float3 dir)
{
	const float softness = max(gSkyHorizonSoftness, 1e-4f);
	const float skyT     = smoothstep(-softness, softness, dir.y);   // 0 deep ground, 1 above horizon
	const float zenithT  = saturate(dir.y);
	const float3 skyColor = lerp(gSkyHorizonColor.rgb, gSkyZenithColor.rgb, zenithT);
	return lerp(gGroundColor.rgb, skyColor * gSkyIntensity, skyT);
}

struct VSInput
{
	float3 position : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float3 tangent : TANGENT;
	float3 biTangent : BITANGENT;
	float4 color : COLOR;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
};

struct PSOutput
{
	float4 color : SV_TARGET;
};
