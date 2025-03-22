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


TEST(MemoryAllocation, BasicAllocation)
{
    //TODO:
}
