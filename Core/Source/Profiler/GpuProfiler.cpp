#include "Profiler/GpuProfiler.h"
#include "Profiler/Profiler.h"

#include <cstring>


namespace frt::profiler
{
// ----- Singleton-ish current instance -----
// Set on Init, cleared on Shutdown. CGpuScope dispatches through it.

static CGpuProfiler* gCurrent = nullptr;

CGpuProfiler* CGpuProfiler::GetCurrent () { return gCurrent; }


// ----- CGpuScope ctor/dtor -----

CGpuScope::CGpuScope (ID3D12GraphicsCommandList* CmdList, const char* Name)
	: CommandList(CmdList)
	, ScopeIndex(-1)
{
#if FRT_PROFILE_PIX
	// Minimal PIX-compatible ANSI event format. Visible in PIX captures
	// without requiring WinPixEventRuntime as a build dependency.
	static constexpr UINT PIX_EVENT_ANSI_VERSION = 1;
	const UINT byteCount = static_cast<UINT>(std::strlen(Name)) + 1u;
	CommandList->BeginEvent(PIX_EVENT_ANSI_VERSION, Name, byteCount);
#endif

#if FRT_PROFILE_TIMESTAMPS
	if (CGpuProfiler* profiler = CGpuProfiler::GetCurrent())
	{
		ScopeIndex = profiler->BeginScope(CmdList, Name);
	}
#endif
}

CGpuScope::~CGpuScope ()
{
#if FRT_PROFILE_TIMESTAMPS
	if (ScopeIndex >= 0)
	{
		if (CGpuProfiler* profiler = CGpuProfiler::GetCurrent())
		{
			profiler->EndScope(CommandList, ScopeIndex);
		}
	}
#endif

#if FRT_PROFILE_PIX
	CommandList->EndEvent();
#endif
}


// ----- CGpuProfiler -----

bool CGpuProfiler::Init (
	ID3D12Device* Device,
	ID3D12CommandQueue* DirectQueue,
	uint32 InMaxScopesPerFrame,
	uint32 InFrameCount)
{
	if (!Device || !DirectQueue || InMaxScopesPerFrame == 0u || InFrameCount == 0u)
	{
		return false;
	}

	MaxScopesPerFrame = InMaxScopesPerFrame;
	FrameCount = InFrameCount;
	// Pre-decrement so first BeginFrame() lands on slot 0.
	CurrentFrame = FrameCount - 1u;
	bWarmedUp = false;

	if (FAILED(DirectQueue->GetTimestampFrequency(&TimestampFrequency)) || TimestampFrequency == 0u)
	{
		return false;
	}

	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	// 2 timestamps (begin + end) per scope, per frame slot.
	heapDesc.Count = 2u * MaxScopesPerFrame * FrameCount;
	heapDesc.NodeMask = 0;
	if (FAILED(Device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(QueryHeap.GetAddressOf()))))
	{
		return false;
	}

	D3D12_HEAP_PROPERTIES readbackHeap = {};
	readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = sizeof(uint64) * 2u * MaxScopesPerFrame;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ReadbackBuffers.Clear();
	Frames.Clear();
	for (uint32 i = 0; i < FrameCount; ++i)
	{
		Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
		const HRESULT hr = Device->CreateCommittedResource(
			&readbackHeap, D3D12_HEAP_FLAG_NONE,
			&bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(buffer.GetAddressOf()));
		if (FAILED(hr))
		{
			Shutdown();
			return false;
		}
		ReadbackBuffers.Add(buffer);

		SFrameData frame;
		Frames.Add(std::move(frame));
	}

	gCurrent = this;
	return true;
}

void CGpuProfiler::Shutdown ()
{
	if (gCurrent == this)
	{
		gCurrent = nullptr;
	}
	QueryHeap.Reset();
	ReadbackBuffers.Clear();
	Frames.Clear();
	LastResults.Clear();
}

void CGpuProfiler::BeginFrame ()
{
	if (FrameCount == 0u)
	{
		return;
	}

	// Cycle to next slot. The slot we land on holds results from FrameCount frames ago,
	// which the GPU is guaranteed to have finished by now.
	CurrentFrame = (CurrentFrame + 1u) % FrameCount;

	if (bWarmedUp)
	{
		ReadbackFrame(CurrentFrame);
	}
	bWarmedUp = bWarmedUp || (CurrentFrame + 1u == FrameCount);

	SFrameData& frame = Frames[CurrentFrame];
	frame.ScopeCount = 0u;
	frame.bRecorded = false;
	frame.ScopeNames.Clear();
}

int32 CGpuProfiler::BeginScope (ID3D12GraphicsCommandList* CmdList, const char* Name)
{
	if (!CmdList || !QueryHeap)
	{
		return -1;
	}

	SFrameData& frame = Frames[CurrentFrame];
	if (frame.ScopeCount >= MaxScopesPerFrame)
	{
		return -1;
	}

	const int32 scopeIndex = static_cast<int32>(frame.ScopeCount);
	frame.ScopeNames.Add(Name);
	++frame.ScopeCount;

	const uint32 slot = (CurrentFrame * MaxScopesPerFrame + static_cast<uint32>(scopeIndex)) * 2u;
	CmdList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
	return scopeIndex;
}

void CGpuProfiler::EndScope (ID3D12GraphicsCommandList* CmdList, int32 ScopeIndex)
{
	if (!CmdList || !QueryHeap || ScopeIndex < 0)
	{
		return;
	}

	const uint32 slot = (CurrentFrame * MaxScopesPerFrame + static_cast<uint32>(ScopeIndex)) * 2u + 1u;
	CmdList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
}

void CGpuProfiler::ResolveFrame (ID3D12GraphicsCommandList* CmdList)
{
	if (!CmdList || !QueryHeap)
	{
		return;
	}

	SFrameData& frame = Frames[CurrentFrame];
	if (frame.ScopeCount == 0u)
	{
		return;
	}

	const uint32 slotBase = CurrentFrame * MaxScopesPerFrame * 2u;
	CmdList->ResolveQueryData(
		QueryHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		slotBase,
		frame.ScopeCount * 2u,
		ReadbackBuffers[CurrentFrame].Get(),
		0u);

	frame.bRecorded = true;
}

void CGpuProfiler::ReadbackFrame (uint32 FrameSlot)
{
	SFrameData& frame = Frames[FrameSlot];
	if (!frame.bRecorded || frame.ScopeCount == 0u)
	{
		LastResults.Clear();
		return;
	}

	const D3D12_RANGE readRange = { 0u, sizeof(uint64) * 2u * frame.ScopeCount };
	void* mapped = nullptr;
	if (FAILED(ReadbackBuffers[FrameSlot]->Map(0, &readRange, &mapped)) || !mapped)
	{
		return;
	}

	const uint64* timestamps = static_cast<const uint64*>(mapped);
	const double msPerTick = 1000.0 / static_cast<double>(TimestampFrequency);

	LastResults.Clear();
	for (uint32 i = 0; i < frame.ScopeCount; ++i)
	{
		const uint64 begin = timestamps[i * 2u];
		const uint64 end = timestamps[i * 2u + 1u];
		const uint64 ticks = (end > begin) ? (end - begin) : 0u;

		SGpuScopeResult& r = LastResults.Add();
		r.Name = frame.ScopeNames[i];
		r.DurationMs = static_cast<double>(ticks) * msPerTick;
	}

	const D3D12_RANGE writeRange = { 0u, 0u };
	ReadbackBuffers[FrameSlot]->Unmap(0, &writeRange);
}
}
