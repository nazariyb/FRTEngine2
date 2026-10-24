#include <gtest/gtest.h>

#include <cstdio>
#include <span>
#include <vector>

#include "TestCommon.h"

#include "Containers/String.h"
#include "Platform/Platform.h"


namespace
{
std::vector<frt::FString> gMainArguments;

struct SChildMode
{
	frt::FString Name;
	frt::test::FChildMain Main = nullptr;
};

// A function-local static: test files register from their own static initializers, in any order
// relative to this file's.
std::vector<SChildMode>& GetChildModes ()
{
	static std::vector<SChildMode> modes;
	return modes;
}

// "--frt-test-child=<Name> ...": the registered function's result, or 2 for an unknown name.
int RunChildMode (frt::FStringView Name, std::span<const frt::FString> Arguments)
{
	for (const SChildMode& mode : GetChildModes())
	{
		if (mode.Name == Name)
		{
			return mode.Main(Arguments);
		}
	}
	std::fprintf(stderr, "Core-Test: no child mode '%s'\n", frt::FString(Name).CStr());
	return 2;
}
}

const std::vector<frt::FString>& frt::test::GetMainArguments ()
{
	return gMainArguments;
}

bool frt::test::RegisterChildMode (FStringView Name, FChildMain Main)
{
	GetChildModes().push_back({ FString(Name), Main });
	return true;
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

	// A child mode, never mixed with a test run (see TestCommon.h).
	if (!gMainArguments.empty() && gMainArguments[0].StartsWith(frt::test::ChildModeSwitch))
	{
		const frt::FStringView name = gMainArguments[0].View().RightChop(frt::test::ChildModeSwitch.Len());
		return RunChildMode(name, std::span<const frt::FString>(gMainArguments).subspan(1u));
	}

	::testing::InitGoogleTest(&ArgCount, Args);
	return RUN_ALL_TESTS();
}
