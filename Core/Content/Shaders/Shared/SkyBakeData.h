// Sky environment bake constants (Scene/SkyBake.hlsl), shared with CGpuScene.
#ifndef FRT_SKY_BAKE_DATA_H
#define FRT_SKY_BAKE_DATA_H

#include "ShaderInterop.h"

FRT_GPU_NAMESPACE_BEGIN

struct SSkyBakeConstants
{
	float4 Zenith;
	float4 Horizon;
	float4 Ground;
	float Intensity;
	float HorizonSoftness;
	uint Size;                 // output resolution of this dispatch
	float LuminanceFloor;      // keeps the importance pdf > 0 wherever radiance > 0
	uint OutEnvironment;       // RWTexture2D<float4>
	uint OutLuminance;         // RWTexture2D<float>, pyramid level being written
	uint InLuminance;          // RWTexture2D<float>, previous (finer) level
	uint Pad0;
};
FRT_GPU_STATIC_ASSERT_SIZE(SSkyBakeConstants, 80);

FRT_GPU_NAMESPACE_END

#endif
