#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "CoreUtils.h"
#include "Enum.h"
#include "Containers/Array.h"
#include "Graphics/SColor.h"
#include "Math/Math.h"



namespace frt::graphics
{
using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
struct SVertex
{
	Vector3f Position;
	Vector2f Uv;
	Vector3f Normal;
	Vector3f Tangent;
	Vector3f Bitangent;
	DirectX::XMFLOAT4 Color;
};
#pragma pack(pop)

// Sky / sun parameters as the editor exposes them. CGpuScene converts them into the baked sky
// environment, the sun light and the frame constants.
struct SSkyConstants
{
	Vector3f SunDirection      = { -0.30f, -0.70f, 0.60f }; // unit vector pointing FROM sun TO scene
	float    SunIntensity      = 5.0f;

	SColor   SunColor          = { 1.0f, 0.95f, 0.85f, 1.0f };

	SColor   SkyZenithColor    = { 0.10f, 0.20f, 0.45f, 1.0f };
	SColor   SkyHorizonColor   = { 0.40f, 0.35f, 0.30f, 1.0f };
	SColor   GroundColor       = { 0.05f, 0.04f, 0.03f, 1.0f };

	float    SkyIntensity      = 1.0f;
	float    HorizonSoftness   = 0.20f;  // smoothstep width around dir.y = 0
	float    PadSky0           = 0.0f;
	float    PadSky1           = 0.0f;
};


// Time-of-day → sky parameters helper. t in [0,1] where 0=midnight, 0.5=noon, 1=midnight again.
// Drives sun azimuth + elevation, sun color (warm at sunrise/sunset), sky intensity (dim at night),
// and horizon tint. Hand-tuned, not physical.
FRT_CORE_API void ComputeSkyFromTimeOfDay (float TimeOfDay, SSkyConstants& Out);


// CPU-side light description produced by scene extraction (ECS Comp_Light) and converted
// to gpu::SLightData by Sys_MeshRenderer.
enum class ELightType : uint32
{
	Point       = 0u,
	Directional = 1u,
	AreaQuad    = 2u,
	Sky         = 3u, // hemispherical environment; shadow ray escapes → unblocked
};

struct SLight
{
	Vector3f Position    = { 0.f, 0.f, 0.f }; // Point: origin. Directional: unused. AreaQuad: center.
	uint32   Type        = 0u;                // ELightType

	Vector3f Direction   = { 0.f, -1.f, 0.f }; // Directional: from sun toward scene. AreaQuad: face normal.
	float    Pad0        = 0.f;

	Vector3f Edge1       = { 1.f, 0.f, 0.f };  // AreaQuad: half-extent along right (world-space)
	float    Pad1        = 0.f;

	Vector3f Edge2       = { 0.f, 1.f, 0.f };  // AreaQuad: half-extent along up (world-space)
	float    Area        = 0.f;                // 4 * |Edge1| * |Edge2| for AreaQuad

	SColor   Emission    = { 0.f, 0.f, 0.f, 0.f }; // RGB radiance multiplier (HDR)
	float    Intensity   = 0.f;                    // additional scalar multiplier
	int32    InstanceId  = -1;                     // for MIS reverse lookup; -1 = not a surface light
	int32    Pad2        = -1;
	int32    Pad3        = 0;
};


// CPU-side sky portal (window / door opening), render space. Converted to gpu::SPortalData.
struct SPortal
{
	Vector3f Center    = { 0.f, 0.f, 0.f };
	float    Pad0      = 0.f;

	Vector3f Normal    = { 0.f, 0.f, 1.f }; // unit; pre-filter uses sign of dot(Normal, dir)
	float    Pad1      = 0.f;

	Vector3f Edge1     = { 1.f, 0.f, 0.f }; // half-extent along right (world-space)
	float    Pad2      = 0.f;

	Vector3f Edge2     = { 0.f, 1.f, 0.f }; // half-extent along up (world-space)
	uint32   Flags     = 0u;                // bit0: 0 = rect, 1 = ellipse (inscribed in the rect)
};

namespace portal_flags
{
	static constexpr uint32 Ellipse = 1u << 0u;
}


enum class ERenderMode : int32
{
	Raster = 0,
	Raytracing,
	Hybrid,
	Count,
};


}


FRT_DECLARE_ENUM_REFLECTION(
	frt::graphics::ERenderMode,
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Raster),
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Raytracing),
	FRT_ENUM_ENTRY(frt::graphics::ERenderMode, Hybrid));