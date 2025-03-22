#include <gtest/gtest.h>

#include "Containers/Array.h"


using uint32 = uint32_t;
using int64 = int64_t;

using namespace frt;
using namespace frt::memory;
using namespace frt::memory::literals;

#define PREPARE_ALLOCATOR()\
    CMemoryPool testAllocator(100_Mb);\
    testAllocator.MakeThisPrimaryInstance();


TEST(TArrayTest, DefaultConstructor)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    EXPECT_EQ(arr.GetSize(), 0);
    EXPECT_TRUE(arr.IsEmpty());
}

TEST(TArrayTest, AddElements)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    int& elem1 = arr.Add();
    elem1 = 10;
    EXPECT_EQ(arr.GetSize(), 1);
    EXPECT_EQ(arr[0], 10);

    int& elem2 = arr.Add(20);
    EXPECT_EQ(arr.GetSize(), 2);
    EXPECT_EQ(arr[1], 20);
}

TEST(TArrayTest, AddUniqueElements)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.AddUnique(5);
    EXPECT_EQ(arr.GetSize(), 1);
    arr.AddUnique(5);
    EXPECT_EQ(arr.GetSize(), 1);
    arr.AddUnique(10);
    EXPECT_EQ(arr.GetSize(), 2);
}

TEST(TArrayTest, InsertElements)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(1);
    arr.Add(3);
    arr.Insert(2, 1);
    EXPECT_EQ(arr.GetSize(), 3);
    EXPECT_EQ(arr[0], 1);
    EXPECT_EQ(arr[1], 2);
    EXPECT_EQ(arr[2], 3);
}

TEST(TArrayTest, RemoveElement)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(1);
    arr.Add(2);
    arr.Add(3);
    bool removed = arr.Remove(2);
    EXPECT_TRUE(removed);
    EXPECT_EQ(arr.GetSize(), 2);
    EXPECT_FALSE(arr.Contains(2));
}

TEST(TArrayTest, RemoveAllElements)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(4);
    arr.Add(4);
    arr.Add(4);
    uint32 removedCount = arr.RemoveAll(4);
    EXPECT_EQ(removedCount, 3);
    EXPECT_EQ(arr.GetSize(), 0);
}

TEST(TArrayTest, RemoveAtElement)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(10);
    arr.Add(20);
    arr.Add(30);
    arr.RemoveAt<true>(1);
    EXPECT_EQ(arr.GetSize(), 2);
    EXPECT_EQ(arr[0], 10);
    EXPECT_EQ(arr[1], 30);
}

TEST(TArrayTest, ClearArray)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(100);
    arr.Add(200);
    arr.Clear();
    EXPECT_EQ(arr.GetSize(), 0);
    EXPECT_TRUE(arr.IsEmpty());
}

TEST(TArrayTest, CopyConstructor)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(5);
    arr.Add(15);
    TArray<int> copy(arr);
    EXPECT_EQ(copy.GetSize(), arr.GetSize());
    EXPECT_EQ(copy[0], arr[0]);
    EXPECT_EQ(copy[1], arr[1]);
}

TEST(TArrayTest, MoveConstructor)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(7);
    arr.Add(14);
    TArray<int> moved(std::move(arr));
    EXPECT_EQ(moved.GetSize(), 2);
    EXPECT_EQ(moved[0], 7);
    EXPECT_EQ(moved[1], 14);
}

TEST(TArrayTest, CopyAssignmentOperator)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(10);
    arr.Add(20);
    TArray<int> copy;
    copy = arr;
    EXPECT_EQ(copy.GetSize(), 2);
    EXPECT_EQ(copy[0], 10);
    EXPECT_EQ(copy[1], 20);
}

TEST(TArrayTest, MoveAssignmentOperator)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(5);
    arr.Add(15);
    TArray<int> moved;
    moved = std::move(arr);
    EXPECT_EQ(moved.GetSize(), 2);
    EXPECT_EQ(moved[0], 5);
    EXPECT_EQ(moved[1], 15);
}

TEST(TArrayTest, FirstAndLastElements)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(11);
    arr.Add(22);
    arr.Add(33);
    EXPECT_EQ(arr.First(), 11);
    EXPECT_EQ(arr.Last(), 33);
}

TEST(TArrayTest, IteratorFunctionality)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    for (int i = 0; i < 5; i++)
    {
        arr.Add(i);
    }
    int expected = 0;
    for (auto value : arr)
    {
        EXPECT_EQ(value, expected++);
    }
}

TEST(TArrayTest, SetSizeWithoutInitializer)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.SetSize<true>(5);
    EXPECT_EQ(arr.GetSize(), 5);
}

TEST(TArrayTest, SetSizeWithInitializer)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.SetSize<true>(5, 42);
    EXPECT_EQ(arr.GetSize(), 5);
    for (uint32 i = 0; i < arr.GetSize(); i++)
    {
        EXPECT_EQ(arr[i], 42);
    }
}

TEST(TArrayTest, AppendArray)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(1);
    arr.Add(2);
    TArray<int> other;
    other.Add(3);
    other.Add(4);
    arr.Append(other);
    EXPECT_EQ(arr.GetSize(), 4);
    EXPECT_EQ(arr[0], 1);
    EXPECT_EQ(arr[1], 2);
    EXPECT_EQ(arr[2], 3);
    EXPECT_EQ(arr[3], 4);
}

TEST(TArrayTest, FindElement)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(100);
    arr.Add(200);
    arr.Add(300);
    uint32 index = arr.Find(200);
    EXPECT_EQ(index, 1);
    index = arr.Find(400);
    EXPECT_EQ(index, arr.Count());
}

TEST(TArrayTest, IsIndexValidTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(10);
    EXPECT_TRUE(arr.IsIndexValid(0));
    EXPECT_FALSE(arr.IsIndexValid(1));

    EXPECT_TRUE(arr.IsIndexValid<TArray<int>::IndexStrategy::IS_CircularClamped>(-1));
    EXPECT_FALSE(arr.IsIndexValid<TArray<int>::IndexStrategy::IS_CircularClamped>(-2));
}

TEST(TArrayTest, GetDataTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(10);
    arr.Add(20);
    int* data = arr.GetData();
    EXPECT_EQ(data[0], 10);
    EXPECT_EQ(data[1], 20);
}

TEST(TArrayTest, GetByIndex)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(10);
    arr.Add(20);
    EXPECT_EQ(arr.Get<TArray<int>::IndexStrategy::IS_CircularClamped>(-2), 10);
    EXPECT_EQ(arr.Get<TArray<int>::IndexStrategy::IS_CircularClamped>(-1), 20);
    EXPECT_EQ(arr.Get<TArray<int>::IndexStrategy::IS_Circular>(-3), 20);
}

TEST(TArrayTest, OperatorIndexTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(5);
    arr.Add(15);
    EXPECT_EQ(arr[0], 5);
    EXPECT_EQ(arr[1], 15);
    arr[0] = 50;
    EXPECT_EQ(arr[0], 50);
}

TEST(TArrayTest, ReAllocAndFreeTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(1);
    arr.Add(2);
    arr.ReAlloc(10);
    EXPECT_GE(arr.GetCapacity(), 10);
    arr.Free();
    EXPECT_EQ(arr.GetSize(), 0);
}

TEST(TArrayTest, SetCapacityAndShrinkToFitTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.SetCapacity(10);
    EXPECT_GE(arr.GetCapacity(), 10);
    for (int i = 0; i < 10; i++)
    {
        arr.Add(i);
    }
    arr.ShrinkToFit();
    EXPECT_EQ(arr.GetCapacity(), arr.GetSize());
}

TEST(TArrayTest, EmplaceAndInsertEmplaceTest)
{
    PREPARE_ALLOCATOR()

    TArray<std::pair<int, int>> arr;
    auto& p1 = arr.Emplace(1, 100);
    EXPECT_EQ(p1.first, 1);
    EXPECT_EQ(p1.second, 100);
    arr.InsertEmplace(0, 2, 200);
    EXPECT_EQ(arr[0].first, 2);
    EXPECT_EQ(arr[0].second, 200);
}

TEST(TArrayTest, InsertWithMoveTest)
{
    PREPARE_ALLOCATOR()

    TArray<int> arr;
    arr.Add(1);
    int value = 2;
    arr.Insert(std::move(value), 1);
    EXPECT_EQ(arr.GetSize(), 2);
    EXPECT_EQ(arr[1], 2);
}
