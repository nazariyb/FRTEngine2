#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Memory/Ref.h"

namespace
{
struct SDestructionFlag
{
	bool* Destroyed = nullptr;
	explicit SDestructionFlag (bool* InDestroyed) : Destroyed(InDestroyed) {}
	~SDestructionFlag () { *Destroyed = true; }
};

// The shape of every legacy entity: optional components held by shared refs, most empty.
struct SHolder
{
	frt::memory::TRefShared<int> Optional;
	frt::memory::TRefWeak<int> Observer;
};
}

TEST(Ref, CopyingEmptySharedAndWeakYieldsEmpty)
{
	FRT_TEST_MEMORY_POOL();
	using namespace frt::memory;

	TRefShared<int> emptyShared;
	TRefShared<int> sharedCopy(emptyShared);
	EXPECT_FALSE(sharedCopy);

	TRefWeak<int> emptyWeak;
	TRefWeak<int> weakCopy(emptyWeak);
	EXPECT_FALSE(weakCopy);
	EXPECT_FALSE(emptyShared.GetWeak());

	SHolder holder;
	SHolder holderCopy = holder;
	EXPECT_FALSE(holderCopy.Optional);

	TRefShared<int> assigned = NewShared<int>(7);
	assigned = emptyShared;
	EXPECT_FALSE(assigned);
}

TEST(Ref, SharedCopyKeepsObjectAliveUntilLastRelease)
{
	FRT_TEST_MEMORY_POOL();
	using namespace frt::memory;

	bool bDestroyed = false;
	TRefShared<SDestructionFlag> first = NewShared<SDestructionFlag>(&bDestroyed);
	{
		TRefShared<SDestructionFlag> second = first;
		first.Release();
		EXPECT_FALSE(bDestroyed);
	}
	EXPECT_TRUE(bDestroyed);
}

TEST(Ref, WeakExpiresWithTheObject)
{
	FRT_TEST_MEMORY_POOL();
	using namespace frt::memory;

	TRefShared<int> shared = NewShared<int>(3);
	TRefWeak<int> weak = shared.GetWeak();
	ASSERT_TRUE(weak);
	EXPECT_EQ(*weak.Lock(), 3);

	shared.Release();
	EXPECT_FALSE(weak);
	EXPECT_FALSE(weak.Lock());

	// Copying a weak ref to an expired object is legal and stays expired.
	TRefWeak<int> weakCopy = weak;
	EXPECT_FALSE(weakCopy);
}

TEST(Ref, UniqueMoveAssignReleasesThePreviousObject)
{
	FRT_TEST_MEMORY_POOL();
	using namespace frt::memory;

	bool bFirstDestroyed = false;
	bool bSecondDestroyed = false;
	TRefUnique<SDestructionFlag> unique = NewUnique<SDestructionFlag>(&bFirstDestroyed);
	unique = NewUnique<SDestructionFlag>(&bSecondDestroyed);
	EXPECT_TRUE(bFirstDestroyed);
	EXPECT_FALSE(bSecondDestroyed);

	TRefUnique<SDestructionFlag> empty;
	EXPECT_FALSE(empty);
	unique = std::move(empty);
	EXPECT_TRUE(bSecondDestroyed);
	EXPECT_FALSE(unique);
}
