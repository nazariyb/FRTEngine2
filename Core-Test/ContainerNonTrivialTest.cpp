// TArray with element types that own a resource.
//
// Every case here was silently broken while the array only ever relocated raw bytes:
// growth copied without moving, removal placement-new'd over live objects and moved out
// of destroyed ones, and both assignment operators mishandled the destination. None of it
// is observable with trivially copyable elements, which is why it survived so long.
#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "TestCommon.h"

#include "Containers/Array.h"

using namespace frt;

namespace
{
/** Counts construction and destruction so leaks and double-destroys are visible. */
struct STracked
{
	static inline int32 LiveCount = 0;
	static inline int32 MoveCount = 0;
	static inline int32 CopyCount = 0;

	static void Reset ()
	{
		LiveCount = 0;
		MoveCount = 0;
		CopyCount = 0;
	}

	std::string Value;

	STracked () { ++LiveCount; }
	explicit STracked (std::string InValue) : Value(std::move(InValue)) { ++LiveCount; }

	STracked (const STracked& Other) : Value(Other.Value) { ++LiveCount; ++CopyCount; }
	STracked (STracked&& Other) noexcept : Value(std::move(Other.Value)) { ++LiveCount; ++MoveCount; }

	STracked& operator= (const STracked& Other)
	{
		Value = Other.Value;
		++CopyCount;
		return *this;
	}

	STracked& operator= (STracked&& Other) noexcept
	{
		Value = std::move(Other.Value);
		++MoveCount;
		return *this;
	}

	~STracked () { --LiveCount; }
};

static_assert(!std::is_trivially_copyable_v<STracked>, "STracked must exercise the non-trivial path");
static_assert(std::is_trivially_copyable_v<int>, "int must exercise the trivial path");
}


TEST(TArrayNonTrivial, GrowthPreservesContents)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;

		// Well past the initial capacity, so ReAlloc runs several times.
		for (uint32 i = 0u; i < 200u; ++i)
		{
			arr.Add(STracked("value-" + std::to_string(i)));
		}

		ASSERT_EQ(arr.Count(), 200u);

		for (uint32 i = 0u; i < 200u; ++i)
		{
			EXPECT_EQ(arr[i].Value, "value-" + std::to_string(i))
				<< "element " << i << " did not survive relocation";
		}
	}

	EXPECT_EQ(STracked::LiveCount, 0) << "elements leaked or were destroyed twice";
}

TEST(TArrayNonTrivial, GrowthMovesRatherThanCopies)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	TArray<STracked> arr;
	for (uint32 i = 0u; i < 100u; ++i)
	{
		arr.Add(STracked("x"));
	}

	// Relocation must never copy - that is the whole point of the non-trivial path.
	EXPECT_EQ(STracked::CopyCount, 0);
	EXPECT_GT(STracked::MoveCount, 0);
}

TEST(TArrayNonTrivial, RemoveLastElement)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;
		arr.Add(STracked("a"));
		arr.Add(STracked("b"));

		// The case that used to move out of an object it had just destroyed: with
		// bKeepOrder=false and Index already the last, source and destination are the same
		// slot. This is exactly what TComponentPool does on every removal.
		arr.RemoveAt<false>(1);

		ASSERT_EQ(arr.Count(), 1u);
		EXPECT_EQ(arr[0].Value, "a");
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, SwapAndPopFromMiddle)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;
		arr.Add(STracked("a"));
		arr.Add(STracked("b"));
		arr.Add(STracked("c"));

		arr.RemoveAt<false>(0);

		ASSERT_EQ(arr.Count(), 2u);
		EXPECT_EQ(arr[0].Value, "c") << "tail element should have been moved into the hole";
		EXPECT_EQ(arr[1].Value, "b");
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, OrderedRemovalShiftsAndDestroysExactlyOnce)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;
		for (uint32 i = 0u; i < 5u; ++i)
		{
			arr.Add(STracked(std::to_string(i)));
		}

		arr.RemoveAt<true>(1);

		ASSERT_EQ(arr.Count(), 4u);
		EXPECT_EQ(arr[0].Value, "0");
		EXPECT_EQ(arr[1].Value, "2");
		EXPECT_EQ(arr[2].Value, "3");
		EXPECT_EQ(arr[3].Value, "4");
		EXPECT_EQ(STracked::LiveCount, 4) << "the vacated tail slot was not destroyed";
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, RemoveEveryElement)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;
		for (uint32 i = 0u; i < 50u; ++i)
		{
			arr.Add(STracked(std::to_string(i)));
		}

		while (!arr.IsEmpty())
		{
			arr.RemoveAt<false>(static_cast<int64>(arr.Count() - 1u));
		}

		EXPECT_EQ(arr.Count(), 0u);
		EXPECT_EQ(STracked::LiveCount, 0);
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, CopyAssignment)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> source;
		source.Add(STracked("one"));
		source.Add(STracked("two"));

		TArray<STracked> destination;
		destination.Add(STracked("discarded"));

		// Used to hand ReAlloc the source's element count while the destination's storage
		// had already been cleared, relocating out of destroyed objects.
		destination = source;

		ASSERT_EQ(destination.Count(), 2u);
		EXPECT_EQ(destination[0].Value, "one");
		EXPECT_EQ(destination[1].Value, "two");
		EXPECT_EQ(source.Count(), 2u) << "source must be untouched";
		EXPECT_EQ(source[0].Value, "one");
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, MoveAssignmentDoesNotLeakDestination)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> source;
		source.Add(STracked("kept"));

		TArray<STracked> destination;
		for (uint32 i = 0u; i < 20u; ++i)
		{
			destination.Add(STracked("dropped"));
		}

		destination = std::move(source);

		ASSERT_EQ(destination.Count(), 1u);
		EXPECT_EQ(destination[0].Value, "kept");
		EXPECT_EQ(STracked::LiveCount, 1) << "the destination's old elements were not destroyed";
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, SelfMoveAssignmentIsSafe)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	{
		TArray<STracked> arr;
		arr.Add(STracked("value"));

		arr = std::move(arr);

		ASSERT_EQ(arr.Count(), 1u);
		EXPECT_EQ(arr[0].Value, "value");
	}

	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, ClearDestroysEverything)
{
	FRT_TEST_MEMORY_POOL();
	STracked::Reset();

	TArray<STracked> arr;
	for (uint32 i = 0u; i < 30u; ++i)
	{
		arr.Add(STracked(std::to_string(i)));
	}

	ASSERT_EQ(STracked::LiveCount, 30);

	arr.Clear();

	EXPECT_EQ(arr.Count(), 0u);
	EXPECT_EQ(STracked::LiveCount, 0);
}

TEST(TArrayNonTrivial, TriviallyCopyableElementsStillWork)
{
	FRT_TEST_MEMORY_POOL();

	// The compile-time branch must leave the fast path behaving exactly as before.
	TArray<int> arr;
	for (int i = 0; i < 500; ++i)
	{
		arr.Add(i);
	}

	ASSERT_EQ(arr.Count(), 500u);
	EXPECT_EQ(arr[0], 0);
	EXPECT_EQ(arr[499], 499);

	arr.RemoveAt<false>(0);
	EXPECT_EQ(arr.Count(), 499u);
	EXPECT_EQ(arr[0], 499);

	arr.RemoveAt<true>(0);
	EXPECT_EQ(arr.Count(), 498u);
	EXPECT_EQ(arr[0], 1);
}
