#pragma once

#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/StringView.h"
#include "Platform/ModuleImage.h"


/**
 * CPU profiling scopes (AssetManager-Design I7): FRT_CPU_SCOPE("Assets.Finalize") times the rest of
 * the enclosing block on whatever thread runs it, and the Stats panel lists the costliest scope
 * names of the last frame.
 *
 * Each thread records closed scopes into a ring of its own (thread_local, created on its first
 * scope and listed in a registry under a mutex), so recording takes no lock and shares no cache
 * line: two timestamp reads (platform::ReadTimestamp, QueryPerformanceCounter) and a handful of
 * plain stores, published with a release store of the ring's count. The ring is a seqlock-style
 * single-producer buffer (Boehm, "Can Seqlocks Get Along with Programming Language Memory Models?",
 * MSPC 2012): the collector reads slots without stopping the writer, and drops any slot the writer
 * may have overwritten meanwhile instead of reading a torn one. A thread that records more than
 * RingCapacity scopes between two collections loses the oldest; the frame counts them.
 *
 * Collection runs on one thread, the main thread: CEngine::BeginFrame calls BeginFrame, which
 * drains every ring into the frame that is ending (a scope belongs to the frame in which it was
 * collected, so nothing is counted twice or lost to a race at the boundary), aggregates it per
 * name and makes it GetLastFrame. Names are copied (interned) when collected, so results never
 * point into a module's image. Until then a ring holds only the literal's address, so a module's
 * unload takes two calls: CollectPending before it, which keeps the real names of everything the
 * module recorded so far, and ReleaseModule after it, for the scopes its last code recorded while
 * it unloaded (DLL_PROCESS_DETACH, static destructors) - their names are gone with the image.
 *
 * Compiled into every build. PIX CPU markers would need the binary WinPixEventRuntime, so they
 * are left for later (AssetManager-Design §15, I7).
 */
namespace frt::profiler
{
// One closed scope, as collected.
struct SCpuScopeEvent
{
	FStringView Name;           // interned by the profiler: valid for the life of the process
	uint64 BeginTicks = 0u;     // platform::ReadTimestamp
	uint64 EndTicks = 0u;
	uint64 ThreadId = 0u;       // platform::GetCurrentThreadId of the recording thread
	uint32 ThreadIndex = 0u;    // 0 for the first thread that ever recorded a scope, then 1, 2, ...
	uint32 Depth = 0u;          // scopes open on that thread around this one
};

// One scope name over a frame. Inclusive times: a nested scope counts in its parent as well.
struct SCpuScopeStats
{
	FStringView Name;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
	uint32 Count = 0u;
};

// What was collected between two frame boundaries.
struct SCpuFrame
{
	uint64 Index = 0u;                      // frames closed before this one
	uint64 BeginTicks = 0u;                 // the boundary that opened it
	uint64 EndTicks = 0u;                   // the boundary that closed it
	// Thread by thread in ThreadIndex order, each thread's scopes in the order they closed (so a
	// nested scope comes before the scope around it).
	std::vector<SCpuScopeEvent> Events;
	// By name, the largest TotalMs first.
	std::vector<SCpuScopeStats> Scopes;
	// Scopes overwritten in a full ring before they could be collected.
	uint64 DroppedEvents = 0u;

	[[nodiscard]] FRT_CORE_API double GetDurationMs () const;
};

class FRT_CORE_API CCpuProfiler
{
public:
	// Scopes one thread can record between two collections (32 bytes each); older ones are
	// overwritten.
	static constexpr uint32 RingCapacity = 4096u;

	// The frame boundary, on the collecting (main) thread: collects every ring, closes the frame
	// that is ending (GetLastFrame) and opens the next.
	static void BeginFrame ();
	// Collects every ring into the open frame without closing it: before a module whose code records
	// scopes is unloaded.
	static void CollectPending ();
	// The same, right after the module whose image was Image unloaded: a scope whose name pointed into
	// it is collected as UnloadedModuleName, without reading the name. Nothing of the module can
	// record a scope after its unload returned, so this collects the last of them.
	static void ReleaseModule (const platform::SModuleImage& Image);
	static constexpr FStringLiteral UnloadedModuleName = "<unloaded module>";
	// The last closed frame; empty before the first boundary. Collecting thread only, until its next
	// BeginFrame.
	[[nodiscard]] static const SCpuFrame& GetLastFrame ();

	// FRT_CPU_SCOPE's end: one closed scope on the calling thread.
	static void RecordScope (FStringLiteral Name, uint64 BeginTicks, uint64 EndTicks, uint32 Depth) noexcept;
};

// Times its own lifetime. Use FRT_CPU_SCOPE rather than naming one.
class FRT_CORE_API CCpuScope
{
public:
	// Name: a literal, "Area.Step" by convention ("Assets.Finalize", "Engine.RenderFrame").
	explicit CCpuScope (FStringLiteral InName) noexcept;
	~CCpuScope ();

	CCpuScope (const CCpuScope&) = delete;
	CCpuScope& operator= (const CCpuScope&) = delete;

private:
	FStringLiteral Name;
	uint64 BeginTicks = 0u;
	uint32 Depth = 0u;
};
}


#define FRT_CPU_SCOPE_CAT2(a, b) a##b
#define FRT_CPU_SCOPE_CAT(a, b) FRT_CPU_SCOPE_CAT2(a, b)
// FRT_CPU_SCOPE("Area.Step"): the rest of the enclosing block, on the calling thread. One per line.
#define FRT_CPU_SCOPE(Name) ::frt::profiler::CCpuScope FRT_CPU_SCOPE_CAT(_cpuScope_, __LINE__)(Name)
