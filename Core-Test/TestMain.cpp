#include <gtest/gtest.h>

#include <vector>

#include "TestCommon.h"

#include "Containers/String.h"
#include "Platform/Platform.h"


namespace
{
std::vector<frt::FString> gMainArguments;
}

const std::vector<frt::FString>& frt::test::GetMainArguments ()
{
	return gMainArguments;
}


// GoogleTest's own gtest_main is not built: with GoogleTest a DLL, its main() would be a
// definition of a dllimport function.
int main (int ArgCount, char** Args)
{
	// Test names, values and failure messages are UTF-8: show them as such on the console the
	// tests run in (its code pages are given back at exit).
	frt::platform::InitConsoleUtf8();

	// Before InitGoogleTest takes its flags out of Args (see GetMainArguments).
	for (int i = 1; i < ArgCount; ++i)
	{
		gMainArguments.emplace_back(Args[i]);
	}

	::testing::InitGoogleTest(&ArgCount, Args);
	return RUN_ALL_TESTS();
}
