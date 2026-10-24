// TFunctionRef refers to its callable without copying it, and TUniqueFunction owns one: inline when
// small, in a CRT heap block otherwise (never the engine's memory pool), every callable constructed,
// moved and destroyed exactly as often as it should be, and the code it would run visible to
// module release.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <array>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "Functional/FunctionRef.h"
#include "Functional/UniqueFunction.h"
#include "Memory/MemoryPool.h"

using namespace frt;


namespace
{
int Twice (int Value)
{
	return Value * 2;
}

int Negate (int Value)
{
	return -Value;
}

// Counts its constructions, moves and destructions; Payload makes it as large as a test needs.
template <size_t PayloadBytes>
struct TTracked
{
	struct SCounts
	{
		int Constructed = 0;
		int Moved = 0;
		int Destroyed = 0;
		int Called = 0;
	};

	explicit TTracked (SCounts& InCounts) noexcept
		: Counts(&InCounts)
	{
		++Counts->Constructed;
	}

	TTracked (TTracked&& Other) noexcept
		: Counts(Other.Counts)
		, Payload(Other.Payload)
	{
		++Counts->Moved;
	}

	TTracked (const TTracked&) = delete;
	TTracked& operator= (const TTracked&) = delete;
	TTracked& operator= (TTracked&&) = delete;

	~TTracked ()
	{
		++Counts->Destroyed;
	}

	int operator() (int Value)
	{
		++Counts->Called;
		return Value + 1;
	}

	SCounts* Counts;
	std::array<uint8, PayloadBytes> Payload = {};
};

using SmallCallable = TTracked<8u>;     // a pointer and 8 bytes: inline
using LargeCallable = TTracked<128u>;   // heap

// Small enough for the inline buffer, but its move may throw: it must go to the heap, or moving the
// function could fail halfway.
struct SThrowingMove
{
	SThrowingMove () = default;
	SThrowingMove (SThrowingMove&&) noexcept(false) {}
	int operator() (int Value) const { return Value; }
};

// As large as the inline buffer, but more aligned than it.
struct SOverAligned
{
	int operator() (int Value) const
	{
		// The heap block honours the alignment.
		return (reinterpret_cast<uintptr_t>(this) % 32u) == 0u ? Value : -1;
	}

	alignas(32) uint8 Bytes[32] = {};
};

int CallRef (TFunctionRef<int (int)> Function, int Value)
{
	return Function(Value);
}

struct SAddOne
{
	int operator() (int Value) const { return Value + 1; }
};
}


static_assert(sizeof(TFunctionRef<int (int)>) == 2u * sizeof(void*));
static_assert(std::is_trivially_copy_constructible_v<TFunctionRef<int (int)>>);
static_assert(std::is_trivially_destructible_v<TFunctionRef<int (int)>>);
static_assert(!std::is_default_constructible_v<TFunctionRef<int (int)>>);
// Never rebound: assigning a temporary callable would leave the ref pointing at a destroyed object.
static_assert(!std::is_copy_assignable_v<TFunctionRef<int (int)>>);
static_assert(!std::is_move_assignable_v<TFunctionRef<int (int)>>);
static_assert(!std::is_assignable_v<TFunctionRef<int (int)>&, SAddOne>);
static_assert(!std::is_assignable_v<TFunctionRef<int (int)>&, int (*) (int)>);
static_assert(std::is_constructible_v<TFunctionRef<int (int)>, SAddOne&>);
static_assert(!std::is_copy_constructible_v<TUniqueFunction<int (int)>>);
static_assert(std::is_nothrow_move_constructible_v<TUniqueFunction<int (int)>>);
static_assert(TUniqueFunction<int (int)>::IsStoredInline<SmallCallable>);
static_assert(!TUniqueFunction<int (int)>::IsStoredInline<LargeCallable>);
static_assert(!TUniqueFunction<int (int)>::IsStoredInline<SThrowingMove>);
static_assert(!TUniqueFunction<int (int)>::IsStoredInline<SOverAligned>);
// Arguments and results convert as for a call; anything goes for void.
static_assert(std::is_constructible_v<TUniqueFunction<void (int)>, int (*) (int)>);
static_assert(!std::is_constructible_v<TUniqueFunction<int (int)>, void (*) (int)>);
static_assert(!std::is_constructible_v<TFunctionRef<int (int)>, void (*) (int)>);


// ---------------- TFunctionRef ----------------

TEST(FunctionRef, CallsLambdasFunctorsAndFunctions)
{
	int offset = 5;
	EXPECT_EQ(CallRef([offset] (int Value) { return Value + offset; }, 1), 6);
	EXPECT_EQ(CallRef(Twice, 4), 8);
	EXPECT_EQ(CallRef(&Negate, 4), -4);

	struct SFunctor
	{
		int operator() (int Value) const { return Value * 10; }
	};
	EXPECT_EQ(CallRef(SFunctor(), 3), 30);
}

TEST(FunctionRef, RefersToTheCallableInsteadOfCopyingIt)
{
	int calls = 0;
	auto counter = [&calls, local = 0] (int Value) mutable
	{
		++calls;
		return Value + ++local;
	};
	const TFunctionRef<int (int)> ref = counter;
	const TFunctionRef<int (int)> copy = ref;

	EXPECT_EQ(ref(10), 11);
	EXPECT_EQ(copy(10), 12);   // the same lambda: its state moved on
	EXPECT_EQ(counter(10), 13);
	EXPECT_EQ(calls, 3);
}

TEST(FunctionRef, KeepsAFunctionPointerByValue)
{
	// A temporary pointer: stored, not referred to.
	const TFunctionRef<int (int)> ref = &Twice;
	EXPECT_EQ(ref(21), 42);
}

TEST(FunctionRef, ForwardsReferencesAndMoveOnlyArguments)
{
	const auto take = [] (std::unique_ptr<int> Value, int& Out) { Out = *Value; };
	const TFunctionRef<void (std::unique_ptr<int>, int&)> ref = take;
	int out = 0;
	ref(std::make_unique<int>(7), out);
	EXPECT_EQ(out, 7);
}


// ---------------- TUniqueFunction ----------------

TEST(UniqueFunction, StartsEmptyAndResetsToEmpty)
{
	TUniqueFunction<int (int)> function;
	EXPECT_FALSE(function);
	EXPECT_TRUE(function == nullptr);
	EXPECT_EQ(function.GetInvoker(), nullptr);
	EXPECT_EQ(function.GetTarget(), nullptr);
	EXPECT_FALSE(function.IsInline());

	function = [] (int Value) { return Value; };
	EXPECT_TRUE(function);
	function = nullptr;
	EXPECT_FALSE(function);

	// A null function pointer is no function.
	int (*null) (int) = nullptr;
	EXPECT_FALSE(TUniqueFunction<int (int)>(null));
}

TEST(UniqueFunction, StoresASmallCallableInlineAndConstructsItOnce)
{
	SmallCallable::SCounts counts;
	{
		TUniqueFunction<int (int)> function{ SmallCallable(counts) };
		EXPECT_TRUE(function.IsInline());
		EXPECT_EQ(function(1), 2);
		EXPECT_EQ(counts.Called, 1);

		// Moving the function relocates the callable: one move, one destruction of the source.
		TUniqueFunction<int (int)> moved(std::move(function));
		EXPECT_FALSE(function);
		EXPECT_TRUE(moved.IsInline());
		EXPECT_EQ(moved(2), 3);
		EXPECT_EQ(counts.Moved, 2);       // into the function, then into `moved`
		EXPECT_EQ(counts.Destroyed, 2);   // the temporary, then the moved-from inline copy
	}
	EXPECT_EQ(counts.Constructed, 1);
	EXPECT_EQ(counts.Constructed + counts.Moved, counts.Destroyed);
}

TEST(UniqueFunction, StoresALargeCallableOnTheHeapAndNeverMovesIt)
{
	LargeCallable::SCounts counts;
	{
		TUniqueFunction<int (int)> function{ LargeCallable(counts) };
		EXPECT_FALSE(function.IsInline());
		EXPECT_EQ(function(1), 2);

		// The block changes owner; the callable stays where it is.
		TUniqueFunction<int (int)> moved(std::move(function));
		TUniqueFunction<int (int)> assigned;
		assigned = std::move(moved);
		EXPECT_FALSE(function);
		EXPECT_FALSE(moved);
		EXPECT_EQ(assigned(5), 6);
		EXPECT_EQ(counts.Moved, 1);   // only into the block
	}
	EXPECT_EQ(counts.Constructed + counts.Moved, counts.Destroyed);
	EXPECT_EQ(counts.Called, 2);
}

TEST(UniqueFunction, MoveAssignmentDestroysThePreviousCallable)
{
	SmallCallable::SCounts first;
	LargeCallable::SCounts second;
	TUniqueFunction<int (int)> function{ SmallCallable(first) };
	function = TUniqueFunction<int (int)>(LargeCallable(second));
	EXPECT_EQ(first.Constructed + first.Moved, first.Destroyed);
	EXPECT_EQ(function(0), 1);
	EXPECT_EQ(second.Called, 1);
	function.Reset();
	EXPECT_EQ(second.Constructed + second.Moved, second.Destroyed);
}

TEST(UniqueFunction, CallablesWhoseMoveMayThrowOrThatAreOverAlignedGoToTheHeap)
{
	const TUniqueFunction<int (int)> throwing(SThrowingMove{});
	EXPECT_FALSE(throwing.IsInline());
	EXPECT_EQ(throwing(4), 4);

	const TUniqueFunction<int (int)> aligned(SOverAligned{});
	EXPECT_FALSE(aligned.IsInline());
	EXPECT_EQ(aligned(4), 4);
}

TEST(UniqueFunction, HoldsMoveOnlyCallables)
{
	auto owned = std::make_unique<int>(41);
	TUniqueFunction<int ()> function([Owned = std::move(owned)] () { return *Owned + 1; });
	TUniqueFunction<int ()> moved = std::move(function);
	EXPECT_EQ(moved(), 42);
}

TEST(UniqueFunction, ConvertsResultsAndIgnoresThemForVoid)
{
	const TUniqueFunction<double (int)> widened(&Twice);
	EXPECT_DOUBLE_EQ(widened(3), 6.0);

	int calls = 0;
	const TUniqueFunction<void (int)> discarded([&calls] (int Value) { ++calls; return Value; });
	discarded(1);
	EXPECT_EQ(calls, 1);
}

TEST(UniqueFunction, ExposesTheInvokerAndAFunctionPointersTarget)
{
	const TUniqueFunction<int (int)> lambda([] (int Value) { return Value; });
	const TUniqueFunction<int (int)> pointer(&Twice);
	ASSERT_NE(lambda.GetInvoker(), nullptr);
	EXPECT_EQ(lambda.GetTarget(), nullptr);
	EXPECT_EQ(pointer.GetTarget(), reinterpret_cast<const void*>(&Twice));

	// A range around the invoker stands in for the image of the module that built the function.
	const char* invoker = static_cast<const char*>(lambda.GetInvoker());
	EXPECT_TRUE(lambda.RefersToImage({ invoker, invoker + 1 }));
	EXPECT_FALSE(lambda.RefersToImage({ invoker + 1, invoker + 2 }));

	// A pointer to a function in the image refers to it whoever built the function.
	const char* target = reinterpret_cast<const char*>(&Twice);
	EXPECT_TRUE(pointer.RefersToImage({ target, target + 1 }));
	const platform::SModuleImage everything = { nullptr, reinterpret_cast<const void*>(~uintptr_t(0)) };
	EXPECT_FALSE(TUniqueFunction<int (int)>().RefersToImage(everything));
}

TEST(UniqueFunction, NeverAllocatesFromTheMemoryPool)
{
	// No primary pool exists outside FRT_TEST_MEMORY_POOL: anything allocating from TLSF would crash.
	ASSERT_EQ(memory::CMemoryPool::GetPrimaryInstance(), nullptr);

	LargeCallable::SCounts mainCounts;
	{
		TUniqueFunction<int (int)> function{ LargeCallable(mainCounts) };
		EXPECT_EQ(function(1), 2);
	}

	// And workers build and destroy them concurrently (TLSF is single-threaded).
	std::vector<std::thread> workers;
	std::array<int, 8> results = {};
	for (size_t i = 0u; i < results.size(); ++i)
	{
		workers.emplace_back([&results, i] ()
		{
			for (int n = 0; n < 1000; ++n)
			{
				std::array<int, 32> payload = {};
				payload[0] = n;
				TUniqueFunction<int ()> function([payload] () { return payload[0]; });
				TUniqueFunction<int ()> moved = std::move(function);
				results[i] += moved() == n ? 1 : 0;
			}
		});
	}
	for (std::thread& worker : workers)
	{
		worker.join();
	}
	for (const int result : results)
	{
		EXPECT_EQ(result, 1000);
	}
	EXPECT_EQ(mainCounts.Constructed + mainCounts.Moved, mainCounts.Destroyed);
}

TEST(UniqueFunction, ACallableMayEmptyItsOwnFunctionFromItsDestructor)
{
	// Reset clears the function before destroying the callable: a destructor that looks at (or
	// assigns) the function sees it empty, never half-destroyed.
	struct SWatcher
	{
		TUniqueFunction<void ()>* Owner = nullptr;
		bool* bSawEmpty = nullptr;
		SWatcher (TUniqueFunction<void ()>* InOwner, bool* InSawEmpty) noexcept : Owner(InOwner), bSawEmpty(InSawEmpty) {}
		SWatcher (SWatcher&& Other) noexcept : Owner(Other.Owner), bSawEmpty(Other.bSawEmpty) { Other.Owner = nullptr; }
		~SWatcher ()
		{
			if (Owner != nullptr)
			{
				*bSawEmpty = !*Owner;
			}
		}
		void operator() () const {}
	};

	bool bSawEmpty = false;
	TUniqueFunction<void ()> function;
	function = TUniqueFunction<void ()>(SWatcher(&function, &bSawEmpty));
	function.Reset();
	EXPECT_TRUE(bSawEmpty);
}
