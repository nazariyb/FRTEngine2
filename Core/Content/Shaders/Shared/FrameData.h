// Per-frame constants (root CBV b1), shared by every pass.
#ifndef FRT_FRAME_DATA_H
#define FRT_FRAME_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

FRT_GPU_CONST uint FrameFlag_ReferenceMode = 1u << 0u;      // progressive, unbiased accumulation
FRT_GPU_CONST uint FrameFlag_CountersEnabled = 1u << 1u;
FRT_GPU_CONST uint FrameFlag_HistoryValid = 1u << 2u;       // previous-frame buffers are usable
FRT_GPU_CONST uint FrameFlag_SunEnabled = 1u << 3u;
FRT_GPU_CONST uint FrameFlag_SkyEnabled = 1u << 4u;

struct SFrameConstants
{
	// Camera. "Jittered" matrices include the sub-pixel TAA offset; the others do not.
	float4x4 View;
	float4x4 InvView;
	float4x4 Proj;                    // jittered
	float4x4 InvViewProjNoJitter;     // primary ray generation (jitter is applied explicitly)
	float4x4 ViewProj;                // jittered
	float4x4 InvViewProj;             // jittered
	float4x4 ViewProjNoJitter;
	float4x4 PrevViewProjNoJitter;

	float3 CameraPosition;
	float NearZ;
	float3 PrevCameraPosition;
	float FarZ;
	float3 CameraForward;
	float TanHalfFovY;

	float2 RenderSize;
	float2 InvRenderSize;
	float2 DisplaySize;
	float2 InvDisplaySize;
	float2 Jitter;                    // render pixels, in [-0.5, 0.5]
	float2 PrevJitter;

	float AspectRatio;
	float PixelSpreadAngle;           // ray cone spread of a primary ray (radians)
	float Time;
	float DeltaTime;

	uint FrameIndex;                  // monotonically increasing
	uint AccumulationIndex;           // reference mode: samples-so-far / spp
	uint Flags;                       // FrameFlag_*
	float PreExposure;                // exposure applied before storing HDR history (1 = none)

	// Scene tables (bindless SRV indices; FRT_INVALID_INDEX when absent).
	uint InstanceBuffer;
	uint GeometryBuffer;
	uint MaterialBuffer;
	uint LightBuffer;

	uint LightCount;                  // analytic lights in LightBuffer
	uint EmissiveTriangleBuffer;
	uint EmissiveTriangleCount;
	uint LightAliasTable;             // SAliasEntry[LightCount + EmissiveTriangleCount]

	uint SkyEnvironmentMap;           // octahedral equal-area RGBA16F radiance
	uint SkyLuminancePyramid;         // R32F mips, top level = average luminance
	uint SkyEnvironmentSize;
	uint SkyPyramidMips;

	uint PortalBuffer;
	uint PortalCount;
	uint BlueNoiseTexture;            // 128x128x64 STBN-style vec2 slices (or FRT_INVALID_INDEX)
	uint EnergyCompensationLut;       // RG16F GGX directional albedo (E, Eavg)

	uint RayCounterBuffer;            // RWByteAddressBuffer UAV, FRT_INVALID_INDEX when disabled
	uint SunLightIndex;               // index into LightBuffer of the sun, FRT_INVALID_INDEX if none
	uint SkyLightIndex;
	uint TotalLightCount;             // LightCount + EmissiveTriangleCount

	// Procedural sky (baked into SkyEnvironmentMap; kept for analytic evaluation / debug).
	float4 SkyZenithColor;
	float4 SkyHorizonColor;
	float4 GroundColor;
	float3 SunDirection;              // unit, pointing TO the sun
	float SunCosAngularRadius;
	float3 SunRadiance;               // radiance of the solar disk
	float SkyIntensity;
	float HorizonSoftness;
	float SceneRadius;
	float Pad0;
	float Pad1;
};
FRT_GPU_STATIC_ASSERT_SIZE(SFrameConstants, 816);

FRT_GPU_NAMESPACE_END

#endif // FRT_FRAME_DATA_H
