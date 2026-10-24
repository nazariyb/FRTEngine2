#include "Profiler/CpuProfiler.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Containers/String.h"
#include "Platform/Thread.h"
#include "Platform/Timestamp.h"


namespace
{
using namespace frt;
using namespace frt::profiler;

constexpr uint32 NotFound = 0xFFFFFFFFu;
constexpr uint32 RingCapacity = CCpuProfiler::RingCapacity;

/**
 * One thread's closed scopes, written by that thread only and read by the collector.
 *
 * The writer claims an index (Claimed = index + 1), fences, fills the slot index % RingCapacity
 * and publishes it (Published = index + 1, release). The collector reads Published (acquire),
 * copies the slots it has not collected yet, fences, and reads Claimed: a slot whose index the
 * writer has since claimed again (index + RingCapacity < Claimed) may hold a mix of two scopes, so
 * it is dropped. Every field is an atomic accessed relaxed, which on x64 is a plain load or store:
 * the fences order them, and nothing is ever read torn.
 */
struct SThreadRing
{
	struct SSlot
	{
		std::atomic<uint64> NameData;
		std::atomic<uint64> NameLengthAndDepth;   // length in the high half, depth in the low half
		std::atomic<uint64> BeginTicks;
		std::atomic<uint64> EndTicks;
	};

	std::atomic<uint64> Claimed { 0u };
	std::atomic<uint64> Published { 0u };
	std::atomic<bool> bThreadExited { false };
	// Collector only: the next index to collect.
	uint64 Collected = 0u;

	uint64 ThreadId = 0u;
	uint32 ThreadIndex = 0u;
	SSlot Slots[RingCapacity];
};
static_assert(sizeof(SThreadRing::SSlot) == 32u);

// A slot as the collector copied it.
struct SSlotCopy
{
	uint64 NameData = 0u;
	uint64 NameLengthAndDepth = 0u;
	uint64 BeginTicks = 0u;
	uint64 EndTicks = 0u;
};

// The calling thread's ring (created by its first scope) and how many of its scopes are open.
struct SThreadState
{
	std::shared_ptr<SThreadRing> Ring;
	uint32 Depth = 0u;

	~SThreadState ()
	{
		// The registry keeps the ring until the collector has read what is left in it.
		if (Ring)
		{
			Ring->bThreadExited.store(true, std::memory_order_release);
		}
	}
};

thread_local SThreadState ThisThread;

// One scope name in the open frame.
struct SAccumulator
{
	uint64 TotalTicks = 0u;
	uint64 MaxTicks = 0u;
	uint32 Count = 0u;
	uint32 Name = 0u;
};

struct SCollector
{
	SCollector ()
	{
		Open.BeginTicks = platform::ReadTimestamp();
	}

	std::mutex RegistryMutex;
	std::vector<std::shared_ptr<SThreadRing>> Rings;   // under RegistryMutex
	uint32 NextThreadIndex = 0u;                       // under RegistryMutex

	// The collecting thread's alone.
	SCpuFrame Open;
	SCpuFrame Last;
	// Interned names: never removed, and a deque never moves its elements, so views stay valid.
	std::deque<FString> Names;
	// Literal address -> Names index. A module reloaded at the same address may put another literal
	// there, so a hit is checked against the text.
	std::unordered_map<uint64, uint32> NameByAddress;
	std::vector<SAccumulator> Accumulators;            // the open frame's, by first appearance
	std::vector<uint32> AccumulatorOfName;             // Names index -> Accumulators index, or NotFound
	std::vector<std::shared_ptr<SThreadRing>> RingsToCollect;
	std::vector<SSlotCopy> Copies;
};

SCollector& GetCollector ()
{
	static SCollector collector;
	return collector;
}

SThreadRing& RegisterThisThread ()
{
	auto ring = std::make_shared<SThreadRing>();
	ring->ThreadId = platform::GetCurrentThreadId();
	{
		SCollector& collector = GetCollector();
		const std::lock_guard lock(collector.RegistryMutex);
		ring->ThreadIndex = collector.NextThreadIndex++;
		collector.Rings.push_back(ring);
	}
	ThisThread.Ring = std::move(ring);
	return *ThisThread.Ring;
}

// The Names index of Text, added if new.
uint32 InternText (SCollector& Collector, FStringView Text)
{
	for (uint32 i = 0u; i < static_cast<uint32>(Collector.Names.size()); ++i)
	{
		if (Collector.Names[i] == Text)
		{
			return i;
		}
	}
	Collector.Names.emplace_back(Text);
	return static_cast<uint32>(Collector.Names.size() - 1u);
}

uint32 InternName (SCollector& Collector, uint64 Address, FStringView Text)
{
	if (const auto it = Collector.NameByAddress.find(Address); it != Collector.NameByAddress.end()
		&& Collector.Names[it->second] == Text)
	{
		return it->second;
	}

	// A literal not seen at this address: the same text elsewhere (another file or module), or new.
	const uint32 index = InternText(Collector, Text);
	Collector.NameByAddress[Address] = index;
	return index;
}

// Unloaded: when the slot's name lies in that module's image, which is gone, so is the text.
void AddEvent (SCollector& Collector, const SThreadRing& Ring, const SSlotCopy& Slot,
	const platform::SModuleImage* Unloaded)
{
	uint32 name = NotFound;
	if (Unloaded != nullptr && Unloaded->Contains(reinterpret_cast<const void*>(Slot.NameData)))
	{
		name = InternText(Collector, CCpuProfiler::UnloadedModuleName);
	}
	else
	{
		const FStringView literal(reinterpret_cast<const char*>(Slot.NameData),
			static_cast<uint32>(Slot.NameLengthAndDepth >> 32u));
		name = InternName(Collector, Slot.NameData, literal);
	}

	SCpuScopeEvent& event = Collector.Open.Events.emplace_back();
	event.Name = Collector.Names[name];
	event.BeginTicks = Slot.BeginTicks;
	event.EndTicks = Slot.EndTicks;
	event.ThreadId = Ring.ThreadId;
	event.ThreadIndex = Ring.ThreadIndex;
	event.Depth = static_cast<uint32>(Slot.NameLengthAndDepth & 0xFFFFFFFFu);

	if (Collector.AccumulatorOfName.size() <= name)
	{
		Collector.AccumulatorOfName.resize(name + 1u, NotFound);
	}
	uint32& accumulatorIndex = Collector.AccumulatorOfName[name];
	if (accumulatorIndex == NotFound)
	{
		accumulatorIndex = static_cast<uint32>(Collector.Accumulators.size());
		Collector.Accumulators.push_back({ 0u, 0u, 0u, name });
	}
	SAccumulator& accumulator = Collector.Accumulators[accumulatorIndex];
	const uint64 duration = Slot.EndTicks >= Slot.BeginTicks ? Slot.EndTicks - Slot.BeginTicks : 0u;
	accumulator.TotalTicks += duration;
	accumulator.MaxTicks = (std::max)(accumulator.MaxTicks, duration);
	++accumulator.Count;
}

void CollectRing (SCollector& Collector, SThreadRing& Ring, const platform::SModuleImage* Unloaded)
{
	const uint64 published = Ring.Published.load(std::memory_order_acquire);
	uint64 first = Ring.Collected;
	if (published - first > RingCapacity)
	{
		Collector.Open.DroppedEvents += published - RingCapacity - first;
		first = published - RingCapacity;
	}
	if (first == published)
	{
		return;
	}

	Collector.Copies.resize(static_cast<size_t>(published - first));
	for (uint64 index = first; index < published; ++index)
	{
		const SThreadRing::SSlot& slot = Ring.Slots[index % RingCapacity];
		SSlotCopy& copy = Collector.Copies[static_cast<size_t>(index - first)];
		copy.NameData = slot.NameData.load(std::memory_order_relaxed);
		copy.NameLengthAndDepth = slot.NameLengthAndDepth.load(std::memory_order_relaxed);
		copy.BeginTicks = slot.BeginTicks.load(std::memory_order_relaxed);
		copy.EndTicks = slot.EndTicks.load(std::memory_order_relaxed);
	}

	// Indices the writer has claimed again since may have been overwritten while they were copied.
	std::atomic_thread_fence(std::memory_order_acquire);
	const uint64 claimed = Ring.Claimed.load(std::memory_order_relaxed);
	const uint64 intact = claimed > RingCapacity ? (std::clamp)(claimed - RingCapacity, first, published) : first;
	Collector.Open.DroppedEvents += intact - first;
	for (uint64 index = intact; index < published; ++index)
	{
		AddEvent(Collector, Ring, Collector.Copies[static_cast<size_t>(index - first)], Unloaded);
	}
	Ring.Collected = published;
}

void CollectAll (SCollector& Collector, const platform::SModuleImage* Unloaded = nullptr)
{
	{
		const std::lock_guard lock(Collector.RegistryMutex);
		Collector.RingsToCollect = Collector.Rings;
	}

	bool bAnyExited = false;
	for (const std::shared_ptr<SThreadRing>& ring : Collector.RingsToCollect)
	{
		// Read before the ring: a thread seen gone has published everything it ever will.
		bAnyExited |= ring->bThreadExited.load(std::memory_order_acquire);
		CollectRing(Collector, *ring, Unloaded);
	}
	Collector.RingsToCollect.clear();

	if (Unloaded != nullptr)
	{
		// Another module may be mapped there later: its literals are found by text again.
		std::erase_if(Collector.NameByAddress, [Unloaded] (const std::pair<const uint64, uint32>& Entry)
		{
			return Unloaded->Contains(reinterpret_cast<const void*>(Entry.first));
		});
	}

	if (bAnyExited)
	{
		const std::lock_guard lock(Collector.RegistryMutex);
		std::erase_if(Collector.Rings, [] (const std::shared_ptr<SThreadRing>& Ring)
		{
			return Ring->bThreadExited.load(std::memory_order_acquire)
				&& Ring->Collected == Ring->Published.load(std::memory_order_acquire);
		});
	}
}
}


namespace frt::profiler
{
double SCpuFrame::GetDurationMs () const
{
	return EndTicks > BeginTicks ? platform::TimestampToMilliseconds(EndTicks - BeginTicks) : 0.0;
}

void CCpuProfiler::BeginFrame ()
{
	SCollector& collector = GetCollector();
	const uint64 now = platform::ReadTimestamp();
	CollectAll(collector);

	SCpuFrame& frame = collector.Open;
	frame.EndTicks = now;
	frame.Scopes.clear();
	for (const SAccumulator& accumulator : collector.Accumulators)
	{
		SCpuScopeStats& stats = frame.Scopes.emplace_back();
		stats.Name = collector.Names[accumulator.Name];
		stats.TotalMs = platform::TimestampToMilliseconds(accumulator.TotalTicks);
		stats.MaxMs = platform::TimestampToMilliseconds(accumulator.MaxTicks);
		stats.Count = accumulator.Count;
		collector.AccumulatorOfName[accumulator.Name] = NotFound;
	}
	collector.Accumulators.clear();
	std::sort(frame.Scopes.begin(), frame.Scopes.end(), [] (const SCpuScopeStats& A, const SCpuScopeStats& B)
	{
		return A.TotalMs != B.TotalMs ? A.TotalMs > B.TotalMs : A.Name.Compare(B.Name) < 0;
	});

	// The vectors trade places every frame, so their capacity is reused.
	std::swap(collector.Last, collector.Open);
	collector.Open.Index = collector.Last.Index + 1u;
	collector.Open.BeginTicks = now;
	collector.Open.EndTicks = 0u;
	collector.Open.Events.clear();
	collector.Open.Scopes.clear();
	collector.Open.DroppedEvents = 0u;
}

void CCpuProfiler::CollectPending ()
{
	CollectAll(GetCollector());
}

void CCpuProfiler::ReleaseModule (const platform::SModuleImage& Image)
{
	CollectAll(GetCollector(), &Image);
}

const SCpuFrame& CCpuProfiler::GetLastFrame ()
{
	return GetCollector().Last;
}

void CCpuProfiler::RecordScope (FStringLiteral Name, uint64 BeginTicks, uint64 EndTicks, uint32 Depth) noexcept
{
	SThreadRing& ring = ThisThread.Ring ? *ThisThread.Ring : RegisterThisThread();

	const uint64 index = ring.Published.load(std::memory_order_relaxed);
	ring.Claimed.store(index + 1u, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);

	SThreadRing::SSlot& slot = ring.Slots[index % RingCapacity];
	slot.NameData.store(reinterpret_cast<uint64>(Name.GetData()), std::memory_order_relaxed);
	slot.NameLengthAndDepth.store((static_cast<uint64>(Name.Len()) << 32u) | Depth, std::memory_order_relaxed);
	slot.BeginTicks.store(BeginTicks, std::memory_order_relaxed);
	slot.EndTicks.store(EndTicks, std::memory_order_relaxed);
	ring.Published.store(index + 1u, std::memory_order_release);
}

CCpuScope::CCpuScope (FStringLiteral InName) noexcept
	: Name(InName)
	, Depth(ThisThread.Depth++)
{
	// Last, so the bookkeeping above is not timed.
	BeginTicks = platform::ReadTimestamp();
}

CCpuScope::~CCpuScope ()
{
	const uint64 endTicks = platform::ReadTimestamp();
	--ThisThread.Depth;
	CCpuProfiler::RecordScope(Name, BeginTicks, endTicks, Depth);
}
}
