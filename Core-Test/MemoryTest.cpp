#include <gtest/gtest.h>

#include "Memory/MemoryPool.h"

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

struct TestDestruct
{
    bool* Flag = nullptr;
    explicit TestDestruct(bool* InFlag) : Flag(InFlag) { if (Flag) *Flag = false; }
    ~TestDestruct() { if (Flag) *Flag = true; }
};


TEST(MemoryAllocation, BasicAllocation)
{
    using namespace frt::memory;
    using namespace frt::memory::literals;

    CMemoryPool pool(1_Kb);

    TestStruct* obj1 = pool.NewUnmanaged<TestStruct>();
    TestStruct2* obj2 = pool.NewUnmanaged<TestStruct2>();

    EXPECT_EQ(obj1->a, 1);
    EXPECT_EQ(obj1->b, 2);
    EXPECT_EQ(obj2->a, 3);
    EXPECT_EQ(obj2->b, 4);

    obj1->~TestStruct();
    obj2->~TestStruct2();
    pool.DeleteUnmanaged(obj1);
    pool.DeleteUnmanaged(obj2);
}

TEST(MemoryAllocation, PrimaryInstance)
{
    using namespace frt::memory;
    using namespace frt::memory::literals;

    CMemoryPool pool(1_Kb);
    pool.MakeThisPrimaryInstance();
    EXPECT_EQ(CMemoryPool::GetPrimaryInstance(), &pool);

    TestStruct* obj = memory::NewUnmanaged<TestStruct>();
    EXPECT_EQ(obj->a, 1);
    obj->~TestStruct();
    pool.DeleteUnmanaged(obj);
}

TEST(MemoryAllocation, UniqueAllocation)
{
    using namespace frt::memory;
    using namespace frt::memory::literals;

    CMemoryPool pool(1_Kb);
    pool.MakeThisPrimaryInstance();

    bool destroyed = false;
    {
        auto uniquePtr = pool.NewUnique<TestDestruct>(&destroyed);
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed);
}

TEST(MemoryAllocation, SharedAllocation)
{
    using namespace frt::memory;
    using namespace frt::memory::literals;

    CMemoryPool pool(1_Kb);
    pool.MakeThisPrimaryInstance();

    bool destroyed = false;
    {
        auto shared1 = pool.NewShared<TestDestruct>(&destroyed);
        {
            auto shared2 = shared1;
            EXPECT_FALSE(destroyed);
        }
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed);
}

TEST(MemoryAllocation, ReallocateMemory)
{
    using namespace frt::memory;
    using namespace frt::memory::literals;

    CMemoryPool pool(1_Kb);

    char* data = static_cast<char*>(pool.Allocate(16));
    for (int i = 0; i < 16; ++i)
    {
        data[i] = static_cast<char>(i);
    }

    data = static_cast<char*>(pool.ReAllocate(data, 32));

    for (int i = 0; i < 16; ++i)
    {
        EXPECT_EQ(data[i], i);
    }

    pool.Free(data);
}
