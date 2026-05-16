#include "GraphicsCoreTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Exception.h"
#include "Memory/Memory.h"


namespace frt::graphics
{
SFrameResources::SFrameResources (
	ID3D12Device* Device,
	uint32 PassCount,
	uint32 ObjectCount,
	uint32 MaterialCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	Init(Device, PassCount, ObjectCount, MaterialCount, BufferArena, DescriptorHeap);
}

SFrameResources::~SFrameResources ()
{}

void SFrameResources::Init (
	ID3D12Device* Device,
	uint32 PassCount,
	uint32 ObjectCount,
	uint32 MaterialCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	THROW_IF_FAILED(
		Device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(CommandListAllocator.GetAddressOf())));

	UploadArena = DX12_UploadArena(Device, 200 * memory::MegaByte);

	PassCB = SConstantBuffer<SPassConstants>(Device, BufferArena, DescriptorHeap, PassCount);
	SkyCB = SConstantBuffer<SSkyConstants>(Device, BufferArena, DescriptorHeap, PassCount);
	ObjectCB = SConstantBuffer<SObjectConstants>(Device, BufferArena, DescriptorHeap, ObjectCount);
	MaterialCB = SConstantBuffer<SMaterialConstants>(Device, BufferArena, DescriptorHeap, MaterialCount);
}


// ---- Time-of-day → sky parameters ----

static float Saturate (float V) { return V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }
static float Lerp (float A, float B, float T) { return A + (B - A) * T; }
static SColor LerpColor (const SColor& A, const SColor& B, float T)
{
	return SColor(Lerp(A.R, B.R, T), Lerp(A.G, B.G, T), Lerp(A.B, B.B, T), Lerp(A.A, B.A, T));
}

void ComputeSkyFromTimeOfDay (float TimeOfDay, SSkyConstants& Out)
{
	const float t = Saturate(TimeOfDay);

	// Sun arc: at t=0.25 sun is at horizon east (rising), t=0.5 zenith, t=0.75 horizon west (setting),
	// t=0/1 below horizon. Elevation = sin(pi * (2t - 0.5)) clamped, azimuth simple linear sweep.
	const float kPi = 3.14159265359f;
	const float arcAngle = kPi * (2.0f * t - 0.5f);   // -pi/2 at t=0, +pi/2 at t=0.5, +3pi/2 at t=1
	const float elevation = std::sin(arcAngle);       // [-1, 1]
	const float azimuth = kPi * (2.0f * t);           // 0..2pi sweep across the day

	const float horizontalLen = std::sqrt(math::Max(0.0f, 1.0f - elevation * elevation));
	// Sun direction points FROM sun TO scene → invert vertical.
	Out.SunDirection = Vector3f(
		horizontalLen * std::cos(azimuth),
		-elevation,
		horizontalLen * std::sin(azimuth));

	// Day-night blend: 0 at night, 1 around midday.
	const float dayness = Saturate(elevation);

	// Sun color: warm orange at horizon, neutral at zenith.
	const SColor warmSun = SColor(1.00f, 0.55f, 0.30f);
	const SColor noonSun = SColor(1.00f, 0.95f, 0.85f);
	const float horizonness = 1.0f - Saturate(elevation * 2.0f);
	Out.SunColor = LerpColor(noonSun, warmSun, horizonness);
	Out.SunIntensity = Lerp(0.5f, 8.0f, dayness);

	// Sky tint: deep night at low elevation, blue-ish at noon, warm at sunrise/sunset.
	const SColor nightZenith = SColor(0.01f, 0.01f, 0.04f);
	const SColor noonZenith  = SColor(0.10f, 0.20f, 0.45f);
	Out.SkyZenithColor = LerpColor(nightZenith, noonZenith, dayness);

	const SColor nightHorizon = SColor(0.02f, 0.02f, 0.05f);
	const SColor duskHorizon  = SColor(0.55f, 0.30f, 0.15f);
	const SColor noonHorizon  = SColor(0.40f, 0.45f, 0.55f);
	const float duskness = horizonness * math::Max(0.0f, math::Min(1.0f, dayness * 4.0f));
	Out.SkyHorizonColor = LerpColor(nightHorizon, LerpColor(noonHorizon, duskHorizon, duskness), dayness);

	Out.GroundColor = SColor(0.04f, 0.04f, 0.04f);
	Out.SkyIntensity = Lerp(0.05f, 1.0f, dayness);
	Out.HorizonSoftness = 0.20f;
}

void SFrameResources::EnsureObjectCapacity (
	ID3D12Device* Device,
	uint32 ObjectCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	if (ObjectCount <= ObjectCB.ObjectCount)
	{
		return;
	}

	ObjectCB = SConstantBuffer<SObjectConstants>(Device, BufferArena, DescriptorHeap, ObjectCount);
}

// ---- CLightList ----

void CLightList::Clear () { Lights.Clear(); }

void CLightList::Add (const SLight& Light) { Lights.Add(Light); }

uint32 CLightList::Count () const { return Lights.Count(); }

// ---- CPortalList ----

void CPortalList::Clear () { Portals.Clear(); }
void CPortalList::Add (const SPortal& Portal) { Portals.Add(Portal); }
uint32 CPortalList::Count () const { return Portals.Count(); }

D3D12_GPU_VIRTUAL_ADDRESS CPortalList::UploadToArena (DX12_UploadArena& Arena) const
{
	const uint32 count = math::Max(Portals.Count(), 1u);
	const uint64 byteCount = static_cast<uint64>(count) * sizeof(SPortal);

	uint64 offset = 0u;
	uint8* dst = Arena.Allocate(byteCount, &offset);
	if (!dst)
	{
		return 0u;
	}

	if (!Portals.IsEmpty())
	{
		std::memcpy(dst, Portals.GetData(), Portals.Count() * sizeof(SPortal));
	}
	else
	{
		std::memset(dst, 0, sizeof(SPortal));
	}
	return Arena.GetGPUBuffer()->GetGPUVirtualAddress() + offset;
}


D3D12_GPU_VIRTUAL_ADDRESS CLightList::UploadToArena (DX12_UploadArena& Arena) const
{
	// Always allocate at least one slot so the SBT root-SRV pointer is valid even when
	// the list is empty (DXR rejects null root SRVs at dispatch time).
	// Shaders gate access on gLightCount; the dummy slot is never read.
	const uint32 count = math::Max(Lights.Count(), 1u);
	const uint64 byteCount = static_cast<uint64>(count) * sizeof(SLight);

	uint64 offset = 0u;
	uint8* dst = Arena.Allocate(byteCount, &offset);
	if (!dst)
	{
		return 0u;
	}

	if (!Lights.IsEmpty())
	{
		std::memcpy(dst, Lights.GetData(), Lights.Count() * sizeof(SLight));
	}
	else
	{
		std::memset(dst, 0, sizeof(SLight));
	}
	return Arena.GetGPUBuffer()->GetGPUVirtualAddress() + offset;
}


void SFrameResources::EnsureMaterialCapacity (
	ID3D12Device* Device,
	uint32 MaterialCount,
	DX12_Arena& BufferArena,
	DX12_DescriptorHeap& DescriptorHeap)
{
	if (MaterialCount <= MaterialCB.ObjectCount)
	{
		return;
	}

	MaterialCB = SConstantBuffer<SMaterialConstants>(Device, BufferArena, DescriptorHeap, MaterialCount);
}
}
