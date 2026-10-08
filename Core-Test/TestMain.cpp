#include <gtest/gtest.h>


// GoogleTest's own gtest_main is not built: with GoogleTest a DLL, its main() would be a
// definition of a dllimport function.
int main (int ArgCount, char** Args)
{
	::testing::InitGoogleTest(&ArgCount, Args);
	return RUN_ALL_TESTS();
}
