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
	uint gPadPCB0;
	uint gPadPCB1;
	uint gPadPCB2;
}

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
