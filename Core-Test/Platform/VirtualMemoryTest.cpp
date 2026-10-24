// Platform/VirtualMemory.h: a reservation is address space only, commits bring zeroed pages exactly
// where asked, a decommit gives them back without losing the addresses, and a release ends it all.
// The page states are checked with VirtualQuery, the OS's own view.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <bit>
#include <cstring>

#include "Platform/VirtualMemory.h"

#include <Windows.h>

using namespace frt;


namespace
{
// The OS's state of the page holding Address: MEM_FREE, MEM_RESERVE or MEM_COMMIT.
DWORD PageState (const void* Address)
{
	MEMORY_BASIC_INFORMATION info = {};
	if (VirtualQuery(Address, &info, sizeof(info)) != sizeof(info))
	{
		return 0u;
	}
	return info.State;
}

bool AllZero (const uint8* Bytes, uint64 Count)
{
	for (uint64 i = 0u; i < Count; ++i)
	{
		if (Bytes[i] != 0u)
		{
			return false;
		}
	}
	return true;
}
}


TEST(VirtualMemoryTest, PageSizeAndGranularity)
{
	const uint64 page = platform::GetPageSize();
	const uint64 granularity = platform::GetAllocationGranularity();
	EXPECT_TRUE(std::has_single_bit(page));
	EXPECT_TRUE(std::has_single_bit(granularity));
	EXPECT_GE(page, 4096u);
	EXPECT_EQ(granularity % page, 0u);
}

TEST(VirtualMemoryTest, ReserveCommitDecommitRelease)
{
	const uint64 page = platform::GetPageSize();
	constexpr uint64 Reserved = 64ull << 20u;
	auto* base = static_cast<uint8*>(platform::ReserveVirtual(Reserved));
	ASSERT_NE(base, nullptr);
	EXPECT_EQ(reinterpret_cast<uint64>(base) % platform::GetAllocationGranularity(), 0u);
	EXPECT_EQ(PageState(base), static_cast<DWORD>(MEM_RESERVE));

	// Three pages and a byte, from a page boundary 1 MiB in: four whole pages.
	uint8* range = base + (1ull << 20u);
	ASSERT_TRUE(platform::CommitVirtual(range, 3u * page + 1u));
	EXPECT_EQ(PageState(range), static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(PageState(range + 3u * page), static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(PageState(range + 4u * page), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(PageState(range - 1u), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_TRUE(AllZero(range, 4u * page));

	std::memset(range, 0xAB, 4u * page);
	// Committing again keeps what is there.
	ASSERT_TRUE(platform::CommitVirtual(range, page));
	EXPECT_EQ(range[0], 0xABu);

	// Only whole pages can be given back.
	EXPECT_FALSE(platform::DecommitVirtual(range + 1u, page));
	EXPECT_FALSE(platform::DecommitVirtual(range, page + 1u));
	ASSERT_TRUE(platform::DecommitVirtual(range, 2u * page));
	EXPECT_EQ(PageState(range), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(PageState(range + 2u * page), static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(range[2u * page], 0xABu);

	// Back as zeros.
	ASSERT_TRUE(platform::CommitVirtual(range, 2u * page));
	EXPECT_TRUE(AllZero(range, 2u * page));

	ASSERT_TRUE(platform::ReleaseVirtual(base, Reserved));
	EXPECT_EQ(PageState(base), static_cast<DWORD>(MEM_FREE));
	EXPECT_EQ(PageState(range), static_cast<DWORD>(MEM_FREE));
}

// Reserving costs no memory: a terabyte of address space, one page of it used.
TEST(VirtualMemoryTest, ReservesFarMoreThanMemory)
{
	constexpr uint64 Reserved = 1ull << 40u;
	auto* base = static_cast<uint8*>(platform::ReserveVirtual(Reserved));
	ASSERT_NE(base, nullptr);
	uint8* last = base + Reserved - platform::GetPageSize();
	ASSERT_TRUE(platform::CommitVirtual(last, 1u));
	last[0] = 42u;
	EXPECT_EQ(last[0], 42u);
	EXPECT_TRUE(platform::ReleaseVirtual(base, Reserved));
}

TEST(VirtualMemoryTest, RejectsWhatIsNotReserved)
{
	EXPECT_EQ(platform::ReserveVirtual(0u), nullptr);
	EXPECT_FALSE(platform::CommitVirtual(nullptr, 4096u));
	EXPECT_FALSE(platform::ReleaseVirtual(nullptr, 0u));

	// Addresses whose reservation has ended.
	const uint64 page = platform::GetPageSize();
	void* base = platform::ReserveVirtual(page);
	ASSERT_NE(base, nullptr);
	ASSERT_TRUE(platform::ReleaseVirtual(base, page));
	EXPECT_FALSE(platform::CommitVirtual(base, page));
	EXPECT_FALSE(platform::DecommitVirtual(base, page));
	EXPECT_FALSE(platform::ReleaseVirtual(base, page));
}
