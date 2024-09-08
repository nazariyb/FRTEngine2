#include "pch.h"

#include "Memory/Memory.h"
#include "Memory/PoolAllocator.h"


struct TestStruct
{
  int a = 1;
  int b = 2;
  TestStruct() : a(1), b(2) {}
};

struct TestStruct2
{
  int a = 3;
  int b = 4;
  TestStruct2() : a(3), b(4) {}
};


TEST(MemoryAllocation, BasicAllocation)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);

  TMemoryHandle<TestStruct, PoolAllocator> allocated = New<TestStruct, PoolAllocator>(&testAllocator);
  EXPECT_EQ(allocated->a, 1);
}

TEST(MemoryAllocation, MultipleAllocations)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  auto allocated1 = New<TestStruct, PoolAllocator>(&testAllocator);
  EXPECT_EQ(allocated1->a, 1);

  auto allocated2 = New<TestStruct2, PoolAllocator>(&testAllocator);
  EXPECT_EQ(allocated2->a, 3);
}

TEST(MemoryAllocation, Deallocation)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);

  {
    auto allocated1 = New<TestStruct, PoolAllocator>(&testAllocator);
    EXPECT_EQ(allocated1->a, 1);
    EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);
  }
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  auto allocated2 = New<TestStruct2, PoolAllocator>(&testAllocator);
  EXPECT_EQ(allocated2->a, 3);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);
}

TEST(MemoryAllocation, Release)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);

  TMemoryHandle<TestStruct, PoolAllocator> allocatedCopy;
  auto allocated = New<TestStruct, PoolAllocator>(&testAllocator);
  allocatedCopy = allocated;
  EXPECT_TRUE(allocatedCopy.Get() == allocated.Get());
  allocated.Release();

  EXPECT_FALSE(allocatedCopy);
}

TEST(MemoryAllocation, CopyHandleOperator)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  auto allocated = New<TestStruct, PoolAllocator>(&testAllocator);
  EXPECT_TRUE(allocated);
  EXPECT_EQ(allocated->a, 1);
  EXPECT_EQ(allocated->b, 2);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);

  auto allocatedCopy = allocated;
  // check that the old object hasn't changed
  EXPECT_TRUE(allocated);
  EXPECT_EQ(allocated->a, 1);
  EXPECT_EQ(allocated->b, 2);

  // check the new object has valid data
  EXPECT_TRUE(allocatedCopy);
  EXPECT_EQ(allocatedCopy->a, 1);
  EXPECT_EQ(allocatedCopy->b, 2);

  // check allocated memory hasn't changed
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);
}

TEST(MemoryAllocation, MoveHandleOperator)
{
  using namespace frt::memory;

  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  auto allocated = New<TestStruct>(&testAllocator);
  EXPECT_TRUE(allocated);
  EXPECT_EQ(allocated->a, 1);
  EXPECT_EQ(allocated->b, 2);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);

  TMemoryHandle<TestStruct, PoolAllocator> allocatedMove;
  allocatedMove = std::move(allocated);
  // check that the old object got invalidated
  EXPECT_FALSE(allocated);

  // check the new object has valid data
  EXPECT_TRUE(allocatedMove);
  EXPECT_EQ(allocatedMove->a, 1);
  EXPECT_EQ(allocatedMove->b, 2);

  // check allocated memory hasn't changed
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16);
}

TEST(MemoryAllocation, FindEmptySpace)
{
  using namespace frt::memory;
  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  auto allocated1 = New<TestStruct>(&testAllocator);
  EXPECT_TRUE(allocated1);
  EXPECT_EQ(allocated1->a, 1);
  EXPECT_EQ(allocated1->b, 2);
  auto allocated2 = New<TestStruct>(&testAllocator);
  EXPECT_TRUE(allocated2);
  EXPECT_EQ(allocated2->a, 1);
  EXPECT_EQ(allocated2->b, 2);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16+16);

  const uint64 oldPtr = reinterpret_cast<uint64>(allocated1.Get());
  allocated1.Release();

  auto allocated3 = New<TestStruct2>(&testAllocator);
  EXPECT_TRUE(allocated3);
  EXPECT_EQ(allocated3->a, 3);
  EXPECT_EQ(allocated3->b, 4);

  // check this didn't get corrupted
  EXPECT_TRUE(allocated2);
  EXPECT_EQ(allocated2->a, 1);
  EXPECT_EQ(allocated2->b, 2);

  EXPECT_EQ(testAllocator.GetMemoryUsed(), 16+16);

  const uint64 newPtr = reinterpret_cast<uint64>(allocated3.Get());
  EXPECT_EQ(oldPtr, newPtr);
}

TEST(MemoryAllocation, ArrayWithInit)
{
  using namespace frt::memory;
  PoolAllocator testAllocator(100 * MegaByte);
  EXPECT_EQ(testAllocator.AlignmentSize, 8);
  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);

  { 
    auto allocatedArray = NewArray<TestStruct, DefaultAllocator, true>(5, &testAllocator);
    EXPECT_TRUE(allocatedArray);
    EXPECT_EQ(allocatedArray.GetNum(), 5);
    EXPECT_EQ(allocatedArray.GetSize(), 5 * sizeof(TestStruct));
    EXPECT_EQ(testAllocator.GetMemoryUsed(), 5 * sizeof(TestStruct) + 8);

    EXPECT_EQ(allocatedArray[0].a, 1);
    EXPECT_EQ(allocatedArray[0].b, 2);
    EXPECT_EQ(allocatedArray[4].a, 1);
    EXPECT_EQ(allocatedArray[4].b, 2);
  }

  EXPECT_EQ(testAllocator.GetMemoryUsed(), 0);
}
