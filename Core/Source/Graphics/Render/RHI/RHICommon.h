#pragma once

// Shared includes and small helpers for the D3D12 backend layer.
//
// Everything under Graphics/Render/RHI is a thin, explicit wrapper over D3D12: it owns
// lifetimes (ComPtr + fence-keyed deferred release), bindless descriptor slots and
// resource-state bookkeeping, and nothing else. Passes still talk D3D12 directly.

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

#include "Asserts.h"
#include "Core.h"
#include "CoreTypes.h"
#include "Exception.h"


namespace frt::graphics::rhi
{
using Microsoft::WRL::ComPtr;

// Sentinel for "no bindless descriptor". Shaders test against the same value.
inline constexpr uint32 InvalidDescriptorIndex = 0xFFFFFFFFu;

// Frames the CPU may record ahead of the GPU. Every per-frame ring is sized by this.
inline constexpr uint32 MaxFramesInFlight = 2u;

inline void SetDebugName (ID3D12Object* Object, std::wstring_view Name)
{
	if (Object && !Name.empty())
	{
		Object->SetName(std::wstring(Name).c_str());
	}
}

inline std::wstring Widen (std::string_view Text)
{
	std::wstring out;
	out.reserve(Text.size());
	for (const char c : Text)
	{
		out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
	}
	return out;
}

constexpr uint64 AlignUp (uint64 Value, uint64 Alignment)
{
	return (Value + Alignment - 1u) & ~(Alignment - 1u);
}

constexpr uint32 DivideRoundUp (uint32 Value, uint32 Divisor)
{
	return (Value + Divisor - 1u) / Divisor;
}

// Capabilities queried once at device creation. Passes read these to pick permutations.
struct SDeviceCaps
{
	D3D_SHADER_MODEL HighestShaderModel = D3D_SHADER_MODEL_6_0;
	D3D12_RAYTRACING_TIER RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
	D3D12_RESOURCE_BINDING_TIER ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
	uint32 WaveLaneCountMin = 32u;
	uint32 WaveLaneCountMax = 32u;
	uint32 AgilitySdkVersion = 0u;
	bool bNative16BitOps = false;
	bool bInt64Atomics = false;
	bool bEnhancedBarriers = false;
	bool bShaderExecutionReordering = false;   // SM 6.9 HitObject available
	bool bSerActuallyReorders = false;         // driver reorders (vs. accepts and ignores)
	bool bOpacityMicromaps = false;
	bool bTypedUavLoadAdditionalFormats = false;

	bool SupportsShaderModel (D3D_SHADER_MODEL Model) const { return HighestShaderModel >= Model; }
};
}
