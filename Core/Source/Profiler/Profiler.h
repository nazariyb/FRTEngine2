#pragma once

#include <d3d12.h>

#include "Core.h"
#include "CoreTypes.h"


// Compile-time backend toggles. Override per-config in build system if needed.
//
// FRT_PROFILE_PIX        : emit cmdList->BeginEvent/EndEvent calls (visible in PIX captures).
// FRT_PROFILE_TIMESTAMPS : emit D3D12 timestamp queries; results readable via CGpuProfiler.
//
// Both default ON in non-shipping builds. Shipping/headless builds typically disable both.

#ifndef FRT_PROFILE_TIMESTAMPS
	#if defined(FRT_HEADLESS)
		#define FRT_PROFILE_TIMESTAMPS 0
	#else
		#define FRT_PROFILE_TIMESTAMPS 1
	#endif
#endif

#ifndef FRT_PROFILE_PIX
	#define FRT_PROFILE_PIX FRT_PROFILE_TIMESTAMPS
#endif

#define FRT_PROFILE_ANY (FRT_PROFILE_PIX || FRT_PROFILE_TIMESTAMPS)


namespace frt::profiler
{
class CGpuProfiler;

// RAII scope that brackets a region of GPU work with both a PIX event (if enabled)
// and a pair of timestamp queries (if enabled). Construct on the command list;
// destructor closes both. Compiles to nothing when both backends are off.
class FRT_CORE_API CGpuScope
{
public:
	CGpuScope (ID3D12GraphicsCommandList* CmdList, const char* Name);
	~CGpuScope ();

	CGpuScope (const CGpuScope&) = delete;
	CGpuScope& operator= (const CGpuScope&) = delete;

private:
	ID3D12GraphicsCommandList* CommandList;
	int32 ScopeIndex;
};
}


#if FRT_PROFILE_ANY
	#define FRT_GPU_SCOPE_CAT2(a, b) a##b
	#define FRT_GPU_SCOPE_CAT(a, b) FRT_GPU_SCOPE_CAT2(a, b)
	#define FRT_GPU_SCOPE(CmdList, Name) \
		::frt::profiler::CGpuScope FRT_GPU_SCOPE_CAT(_gpuScope_, __LINE__)((CmdList), (Name))
#else
	#define FRT_GPU_SCOPE(CmdList, Name) ((void)0)
#endif
