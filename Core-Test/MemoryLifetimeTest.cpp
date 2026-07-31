// CMemoryPool's primary-instance handling. Every case here was a live bug: a dangling
// pointer after destruction, a stale pointer after a move, and a leaked arena on
// move-assignment.
#include <gtest/gtest.h>

#include <utility>

#include "Memory/Memory.h"
#include "Memory/MemoryPool.h"

using namespace frt;
using namespace frt::memory;
using namespace frt::memory::literals;


TEST(MemoryPoolLifetime, DestructionClearsThePrimaryPointer)
{
	{
		CMemoryPool pool(16_Mb);
		pool.MakeThisPrimaryInstance();
		ASSERT_EQ(CMemoryPool::GetPrimaryInstance(), &pool);
	}

	// Leaving it dangling turned every later allocation into a use-after-free that read
	// as random corruption. Nulling it makes the misuse fail immediately instead.
	EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), nullptr);
}

TEST(MemoryPoolLifetime, MoveConstructionCarriesThePrimaryPointer)
{
	CMemoryPool source(16_Mb);
	source.MakeThisPrimaryInstance();
	ASSERT_EQ(CMemoryPool::GetPrimaryInstance(), &source);

	CMemoryPool moved = std::move(source);

	EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), &moved)
		<< "primary must follow the arena, not stay on the moved-from husk";

	void* block = CMemoryPool::GetPrimaryInstance()->Allocate(1024u);
	EXPECT_NE(block, nullptr);
	CMemoryPool::GetPrimaryInstance()->Free(block);
}

TEST(MemoryPoolLifetime, MoveAssignmentCarriesThePrimaryPointer)
{
	CMemoryPool source(16_Mb);
	source.MakeThisPrimaryInstance();

	// Assigning onto a pool that already owns an arena must release it rather than leak
	// it - for GameInstance's pool that would be gigabytes.
	CMemoryPool destination(8_Mb);
	destination = std::move(source);

	EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), &destination);

	void* block = CMemoryPool::GetPrimaryInstance()->Allocate(1024u);
	EXPECT_NE(block, nullptr);
	CMemoryPool::GetPrimaryInstance()->Free(block);
}

TEST(MemoryPoolLifetime, SelfMoveAssignmentLeavesThePoolUsable)
{
	CMemoryPool pool(16_Mb);
	pool.MakeThisPrimaryInstance();

	pool = std::move(pool);

	void* block = CMemoryPool::GetPrimaryInstance()->Allocate(1024u);
	EXPECT_NE(block, nullptr) << "self-assignment must not release its own arena";
	CMemoryPool::GetPrimaryInstance()->Free(block);
}

TEST(MemoryPoolLifetime, PrimaryStartsAndEndsNull)
{
	// Each test above tears its pool down, so nothing should be left claimed. If this
	// fails, some other test leaked a primary instance and the others are unreliable.
	EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), nullptr);
}
