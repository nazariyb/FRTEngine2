// AssetTool's command line as scripts see it (AssetTool/Source, design §13.1): the exit codes - 0 for
// success, 2 for every usage error (unknown options too), 3 when the writer lock cannot be taken for a
// reason other than an owner, 4 when a run that writes meets a mount another process owns - the shape of
// the --json lines (one object per line, stable field names), help's list of verbs, newguid's GUIDs, the
// asset registry's verbs (scan, status, query, refs, referencers, explain, list-rules, dump registry: only scan
// writes, and only under the lock; their answers, exit codes and positioned query errors), the
// writer lock of /Project and, with --engine, of /Engine: a read-only run never takes it, a second writer runs as a
// read-only guest only when the project's Safety.GuestProcesses says so, and AssetTool's
// own lock verb keeps every other writer out until its input ends. A verb that writes nothing runs beside a
// working directory's project it cannot use. The ddc verbs (§7.4, §13.2) count, trim and check the store
// FRT_DDC_DIR or --ddc names, never create one, keep what every BuildState log of the machine names (the
// project's, the engine's, the recent projects') without writing any of them, and with --ci trim nothing.
// Runs the AssetTool executable beside Core-Test on a scratch project in a directory with a non-ASCII name,
// and a copy of it in a scratch engine checkout for /Engine. Release builds no AssetTool: skipped there.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include "BuildConfig.h"
#include "EnginePaths.h"
#include "Assets/AssetId.h"
#include "Assets/BuildState.h"
#include "Assets/DerivedDataCache.h"
#include "Assets/MountPaths.h"
#include "Assets/OwnerLock.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Platform/FileLock.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Project/Project.h"
#include "Project/UserRegistry.h"
#include "Reflect/DataTreeJson.h"

using namespace frt;


namespace
{
// The C++ initializer newguid prints declares a constant: SGuid is an aggregate.
constexpr assets::SGuid InitializerExample
	{ { 0x9f, 0xc0, 0xd4, 0x01, 0x0b, 0xbf, 0x28, 0xb4, 0x59, 0x40, 0x72, 0xe7, 0x2b, 0x86, 0x55, 0xab } };
static_assert(InitializerExample.IsValid() && InitializerExample.GetVersion() == 2u);

// What the build appends to the names of this configuration's binaries: Core-Test-<config>.exe gives
// "-<config>" ("-Development-Default"); Release's plain names give "".
FString GetConfigurationSuffix ()
{
	const FString stem = FString::FromPath(platform::GetExecutablePath().stem());
	constexpr FStringLiteral prefix = "Core-Test";
	return FString(stem.View().RightChop(stem.StartsWith(prefix) ? prefix.Len() : stem.Len()));
}

// AssetTool beside this executable, named as the build names it: Core-Test-<config>.exe runs
// AssetTool-<config>.exe.
std::filesystem::path GetAssetToolPath ()
{
	return platform::GetExecutablePath().parent_path() / ("AssetTool" + GetConfigurationSuffix() + ".exe").ToPath();
}

std::vector<FStringView> SplitLines (FStringView Text)
{
	std::vector<FStringView> lines;
	for (const FStringView line : Text.Lines())
	{
		lines.push_back(line);
	}
	return lines;
}

bool IsLowerHex (FStringView Text)
{
	for (const char c : Text)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}
	return true;
}

// "SGuid { { 0x9f, 0xc0, ... } }" back to the bytes it initializes; nullopt if it is not that shape.
std::optional<assets::SGuid> ParseInitializer (FStringView Text)
{
	constexpr FStringLiteral Prefix = "SGuid { { ";
	constexpr FStringLiteral Suffix = " } }";
	if (!Text.StartsWith(Prefix) || !Text.EndsWith(Suffix))
	{
		return std::nullopt;
	}
	assets::SGuid guid;
	uint32 count = 0u;
	for (const FStringView byte : Text.Mid(Prefix.Len(), Text.Len() - Prefix.Len() - Suffix.Len()).Split(FStringView(", ")))
	{
		if (count == assets::SGuid::Size || byte.Len() != 4u || !byte.StartsWith("0x") || !IsLowerHex(byte.RightChop(2u)))
		{
			return std::nullopt;
		}
		guid.Bytes[count++] = static_cast<uint8>(byte.RightChop(2u).ToUInt32(16u).value_or(0u));
	}
	return count == assets::SGuid::Size ? std::optional<assets::SGuid>(guid) : std::nullopt;
}

// The number after "Field": in a --json line; nullopt without one.
std::optional<uint64> JsonUInt (FStringView Line, FStringView Field)
{
	const FString prefix = "\"" + Field + "\":";
	const uint32 at = Line.Find(prefix);
	if (at == FStringView::NotFound)
	{
		return std::nullopt;
	}
	const FStringView rest = Line.RightChop(at + prefix.Len());
	uint32 digits = 0u;
	while (digits < rest.Len() && rest[digits] >= '0' && rest[digits] <= '9')
	{
		++digits;
	}
	return rest.Left(digits).ToUInt64();
}

// Bytes no compressor shrinks, different for each seed: values the DDC keeps as files of their own.
std::vector<uint8> NoiseBytes (size_t Size, uint32 Seed)
{
	std::vector<uint8> bytes(Size);
	uint64 state = 0x9e3779b97f4a7c15ull * (Seed + 1u);
	for (uint8& byte : bytes)
	{
		state ^= state << 13u;
		state ^= state >> 7u;
		state ^= state << 17u;
		byte = static_cast<uint8>(state >> 32u);
	}
	return bytes;
}

// A record key of the scratch store, from its number.
hash::SContentHash ScratchKey (uint32 Number)
{
	const uint8 bytes[] = { static_cast<uint8>(Number), static_cast<uint8>(Number >> 8u), 'k', 'e', 'y' };
	return hash::HashContent(bytes);
}

// Count records at Root, each with a 100 KiB value of its own (stored as a file of its own), written in
// order with time between them, so the oldest is the least recently used. Their keys, in that order.
std::vector<hash::SContentHash> FillDdc (const std::filesystem::path& Root, uint32 Count)
{
	assets::CDerivedDataCache ddc;
	FString error;
	EXPECT_TRUE(ddc.Open(Root, &error)) << error.CStr();
	std::vector<hash::SContentHash> keys;
	for (uint32 i = 0u; i < Count; ++i)
	{
		assets::SDdcRecord record;
		record.Builder = "Test";
		record.Version = assets::SGuid { { 0x9f, 0xc0, 0xd4, 0x01, 0x0b, 0xbf, 0x48, 0xb4, 0x99, 0x40, 0x72, 0xe7, 0x2b, 0x86, 0x55, 0xab } };
		assets::SDdcValue& value = record.Values.emplace_back();
		value.Name = "V";
		value.Bytes = NoiseBytes(100u * 1024u, i);
		keys.push_back(ScratchKey(i));
		EXPECT_TRUE(ddc.PutRecord("Test", keys.back(), record, &error)) << error.CStr();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return keys;
}

// A newguid GUID: 32 lowercase hex digits that parse, version 4 with the RFC variant.
void ExpectFreshGuid (FStringView Text)
{
	EXPECT_EQ(Text.Len(), assets::SGuid::TextLength) << FString(Text).CStr();
	EXPECT_TRUE(IsLowerHex(Text)) << FString(Text).CStr();
	const std::optional<assets::SGuid> guid = assets::SGuid::Parse(Text);
	ASSERT_TRUE(guid.has_value()) << FString(Text).CStr();
	EXPECT_EQ(guid->GetVersion(), 4u);
	EXPECT_TRUE(guid->HasRfcVariant());
	EXPECT_FALSE(guid->HasNumericText());
}


class AssetToolTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
#if FRT_BUILD_RELEASE
		GTEST_SKIP() << "Release builds no AssetTool";
#else
		Exe = GetAssetToolPath();
		ASSERT_TRUE(std::filesystem::exists(Exe)) << FString::FromPath(Exe).CStr();

		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Ассети 资产 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ProjectDir = Dir / FStringView("Гра 游戏").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(ProjectDir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		ProjectFile = ProjectDir / "AssetToolTest.frtproject";
		// The engine by its directory: the scratch project is not inside it.
		ASSERT_TRUE(platform::WriteFileText(ProjectFile, FString::Format("[Project]\nName=AssetToolTest\nEngine={}\n",
			FString::FromPath(paths::GetEngineDir()))));

		// Nothing AssetTool or the hosts write per user lands in the user's own directory, and the ddc verbs
		// find the machine's store in a scratch directory (whatever FRT_DDC_DIR a verify run set).
		PreviousUserDataDir = platform::GetEnvironmentPath("FRT_USER_DATA_DIR");
		ASSERT_TRUE(platform::SetEnvironmentPath("FRT_USER_DATA_DIR", Dir / "UserData"));
		PreviousDdcDir = platform::GetEnvironmentPath("FRT_DDC_DIR");
		DdcDir = Dir / FStringView("Кеш DDC").ToPath();
		ASSERT_TRUE(platform::SetEnvironmentPath("FRT_DDC_DIR", DdcDir));
#endif
	}

	void TearDown () override
	{
		if (bPathChanged)
		{
			platform::SetEnvironmentPath("PATH", PreviousPath);
		}
		if (!Dir.empty())
		{
			platform::SetEnvironmentPath("FRT_USER_DATA_DIR", PreviousUserDataDir);
			platform::SetEnvironmentPath("FRT_DDC_DIR", PreviousDdcDir);
			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}
	}

	// An engine checkout of its own in the scratch directory, for --engine runs, which lock the engine
	// AssetTool belongs to and must not lock this checkout: an Engine.frtengine with a copy of AssetTool
	// and Core's DLL in Binaries/Win64. Core finds its engine from where it is loaded (EnginePaths.h). The
	// other libraries Core loads stay where they are, found through PATH, which TearDown puts back.
	// Returns the copy of AssetTool; Engine is the checkout's directory.
	std::filesystem::path MakeScratchEngine (const std::filesystem::path& Engine)
	{
		const std::filesystem::path binaries = Engine / "Binaries" / "Win64";
		std::error_code ec;
		EXPECT_TRUE(std::filesystem::create_directories(binaries, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		EXPECT_TRUE(platform::WriteFileText(Engine / project::EngineFileName.ToPath(), "[Engine]\nName=AssetToolTestEngine\n"));
		const std::filesystem::path core = ("Core" + GetConfigurationSuffix() + ".dll").ToPath();
		for (const std::filesystem::path& file : { Exe.filename(), core })
		{
			std::filesystem::copy_file(Exe.parent_path() / file, binaries / file, ec);
			EXPECT_FALSE(ec) << FString::FromPath(file).CStr() << ": " << FString::FromUtf8Lossy(ec.message()).CStr();
		}

		if (!bPathChanged)
		{
			PreviousPath = platform::GetEnvironmentPath("PATH");
			bPathChanged = true;
		}
		EXPECT_TRUE(platform::SetEnvironmentPath("PATH", (FString::FromPath(Exe.parent_path()) + ";" +
			FString::FromPath(PreviousPath)).ToPath()));
		return binaries / Exe.filename();
	}

	// AssetTool with Arguments, in WorkingDir (default: the scratch directory, which holds no project).
	test::SProcessResult Run (const std::vector<FString>& Arguments, const std::filesystem::path& WorkingDir = {}) const
	{
		return test::RunProcess(Exe, Arguments, WorkingDir.empty() ? Dir : WorkingDir);
	}

	[[nodiscard]] FString ProjectArgument () const
	{
		return "-Project=" + FString::FromPath(ProjectFile);
	}

	std::filesystem::path Exe;
	std::filesystem::path Dir;
	std::filesystem::path ProjectDir;
	std::filesystem::path ProjectFile;
	std::filesystem::path PreviousUserDataDir;
	std::filesystem::path DdcDir;
	std::filesystem::path PreviousDdcDir;
	std::filesystem::path PreviousPath;
	bool bPathChanged = false;
};
}


TEST_F(AssetToolTest, HelpListsEveryVerb)
{
	const test::SProcessResult help = Run({ "help" });
	ASSERT_EQ(help.ExitCode, 0) << help.Errors.CStr();
	EXPECT_TRUE(help.Output.Contains("help [verb]:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("newguid [n]:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("lock:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("ddc stats|gc [--max-gb=<n>]|verify [--ddc=<dir>]:")) << help.Output.CStr();
	EXPECT_TRUE(help.Output.Contains("Exit codes: 0 success, 1 problems found, 2 usage error, 3 internal error or crash, 4 the mount"))
		<< help.Output.CStr();
	EXPECT_EQ(Run({ "--help" }).Output, help.Output);

	// --json: one object per verb, in table order, with stable fields.
	const test::SProcessResult json = Run({ "help", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(json.Output);
	ASSERT_EQ(lines.size(), 17u) << json.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"verb\":\"help\",\"usage\":\"help [verb]: ")) << json.Output.CStr();
	EXPECT_TRUE(lines[0].EndsWith("\",\"writes\":false}")) << json.Output.CStr();
	EXPECT_TRUE(lines[1].StartsWith("{\"verb\":\"newguid\",\"usage\":\"newguid [n]: ")) << json.Output.CStr();
	EXPECT_TRUE(lines[2].StartsWith("{\"verb\":\"lock\",")) << json.Output.CStr();
	EXPECT_TRUE(lines[2].EndsWith("\",\"writes\":true}")) << json.Output.CStr();
	EXPECT_TRUE(lines[3].StartsWith("{\"verb\":\"ddc\",")) << json.Output.CStr();
	EXPECT_TRUE(lines[3].EndsWith("\",\"writes\":false}")) << json.Output.CStr();
	// The registry's verbs: only scan writes.
	constexpr FStringLiteral RegistryVerbs[] = { "scan", "status", "query", "refs", "referencers", "explain", "list-rules", "dump" };
	for (uint32 i = 0u; i < std::size(RegistryVerbs); ++i)
	{
		EXPECT_TRUE(lines[4u + i].StartsWith("{\"verb\":\"" + FString(RegistryVerbs[i]) + "\",")) << json.Output.CStr();
		EXPECT_EQ(lines[4u + i].EndsWith("\"writes\":true}"), i == 0u) << json.Output.CStr();
	}
	// The meta verbs (M4): every one takes the writer lock unless read-only.
	constexpr FStringLiteral MetaVerbs[] = { "validate", "fix", "new-meta", "migrate", "resave" };
	for (uint32 i = 0u; i < std::size(MetaVerbs); ++i)
	{
		EXPECT_TRUE(lines[12u + i].StartsWith("{\"verb\":\"" + FString(MetaVerbs[i]) + "\",")) << json.Output.CStr();
		EXPECT_TRUE(lines[12u + i].EndsWith("\"writes\":true}")) << json.Output.CStr();
	}

	const test::SProcessResult one = Run({ "help", "NewGuid" });
	ASSERT_EQ(one.ExitCode, 0) << one.Errors.CStr();
	EXPECT_EQ(SplitLines(one.Output).size(), 1u) << one.Output.CStr();
	EXPECT_TRUE(one.Output.Contains("newguid [n]:")) << one.Output.CStr();
}

TEST_F(AssetToolTest, NewGuidPrintsFreshGuidsAsMetaTextAndCppInitializers)
{
	const test::SProcessResult text = Run({ "newguid" });
	ASSERT_EQ(text.ExitCode, 0) << text.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(text.Output);
	ASSERT_EQ(lines.size(), 1u) << text.Output.CStr();
	const SStringSplit guidAndInitializer = lines[0].SplitFirst(FStringView("  "));
	ASSERT_TRUE(guidAndInitializer) << text.Output.CStr();
	ExpectFreshGuid(guidAndInitializer.Left);
	EXPECT_EQ(ParseInitializer(guidAndInitializer.Right), assets::SGuid::Parse(guidAndInitializer.Left)) << text.Output.CStr();

	// --json, the -Run= spelling, any case: {"guid":"<32 hex>","cpp":"SGuid { { ... } }"} per line, all different.
	const test::SProcessResult json = Run({ "-Run=NewGuid", "5", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<FStringView> objects = SplitLines(json.Output);
	ASSERT_EQ(objects.size(), 5u) << json.Output.CStr();
	std::set<FString> seen;
	for (const FStringView object : objects)
	{
		constexpr FStringLiteral Start = "{\"guid\":\"";
		constexpr FStringLiteral Middle = "\",\"cpp\":\"";
		constexpr FStringLiteral End = "\"}";
		ASSERT_TRUE(object.StartsWith(Start) && object.EndsWith(End)) << FString(object).CStr();
		const FStringView guid = object.Mid(Start.Len(), assets::SGuid::TextLength);
		ExpectFreshGuid(guid);
		const FStringView rest = object.RightChop(Start.Len() + assets::SGuid::TextLength);
		ASSERT_TRUE(rest.StartsWith(Middle)) << FString(object).CStr();
		const FStringView initializer = rest.Mid(Middle.Len(), rest.Len() - Middle.Len() - End.Len());
		EXPECT_EQ(ParseInitializer(initializer), assets::SGuid::Parse(guid)) << FString(object).CStr();
		EXPECT_TRUE(seen.insert(FString(guid)).second) << "twice: " << FString(guid).CStr();
	}
	EXPECT_TRUE(json.Errors.IsEmpty()) << json.Errors.CStr();
}

TEST_F(AssetToolTest, UsageErrorsExitWithTwo)
{
	const test::SProcessResult noVerb = Run({});
	EXPECT_EQ(noVerb.ExitCode, 2);
	EXPECT_TRUE(noVerb.Errors.Contains("No verb")) << noVerb.Errors.CStr();
	EXPECT_TRUE(noVerb.Output.IsEmpty()) << noVerb.Output.CStr();

	const test::SProcessResult unknown = Run({ "frobnicate" });
	EXPECT_EQ(unknown.ExitCode, 2);
	EXPECT_TRUE(unknown.Errors.Contains("No verb 'frobnicate'")) << unknown.Errors.CStr();

	// With --json the error is an object on stdout too, for a script that reads only that.
	const test::SProcessResult unknownJson = Run({ "frobnicate", "--json" });
	EXPECT_EQ(unknownJson.ExitCode, 2);
	EXPECT_TRUE(unknownJson.Output.StartsWith("{\"error\":\"No verb 'frobnicate'")) << unknownJson.Output.CStr();
	EXPECT_TRUE(unknownJson.Output.Contains("\"exitCode\":2}")) << unknownJson.Output.CStr();

	const std::vector<std::vector<FString>> badArguments =
	{
		{ "newguid", "abc" }, { "newguid", "0" }, { "newguid", "10001" }, { "newguid", "1", "2" }, { "help", "nosuch" },
		{ "help", "a", "b" }, { ProjectArgument(), "lock", "extra" },
		{ "ddc" }, { "ddc", "frob" }, { "ddc", "stats", "extra" }, { "ddc", "stats", "--max-gb=1" }, { "ddc", "gc", "--max-gb=abc" },
		{ "ddc", "gc", "--max-gb=-1" }, { "ddc", "verify", "--ddc=" }, { "ddc", "stats", "--max-size=1" },
		// Options nobody reads: a typo, a negative number, a host option in the wrong form.
		{ "newguid", "--jsno" }, { "--cii", ProjectArgument(), "lock" }, { "newguid", "-3" }, { "newguid", "--json=1" },
		{ "-Run", "newguid" }, { "newguid", "-Assets:ReadOnly" }, { "help", "-" },
	};
	for (const std::vector<FString>& arguments : badArguments)
	{
		const test::SProcessResult result = Run(arguments);
		EXPECT_EQ(result.ExitCode, 2) << arguments.back().CStr() << ": " << result.Errors.CStr();
		EXPECT_FALSE(result.Errors.IsEmpty()) << arguments.back().CStr();
		EXPECT_TRUE(result.Output.IsEmpty()) << arguments.back().CStr() << ": " << result.Output.CStr();
	}

	const test::SProcessResult negative = Run({ "newguid", "-3", "--json" });
	EXPECT_EQ(negative.ExitCode, 2);
	EXPECT_TRUE(negative.Errors.Contains("newguid: no option '-3'")) << negative.Errors.CStr();
	EXPECT_TRUE(negative.Output.StartsWith("{\"error\":\"newguid: no option '-3'")) << negative.Output.CStr();

	// A verb that writes needs a mount: no project here, no --engine.
	const test::SProcessResult noProject = Run({ "lock" });
	EXPECT_EQ(noProject.ExitCode, 2);
	EXPECT_TRUE(noProject.Errors.Contains("no project")) << noProject.Errors.CStr();

	// A project that is not there; --engine together with a project, in each way of naming one.
	const test::SProcessResult missing = Run({ "-Project=" + FString::FromPath(Dir / "Немає.frtproject"), "newguid" });
	EXPECT_EQ(missing.ExitCode, 2);
	EXPECT_TRUE(missing.Errors.Contains("Немає.frtproject")) << missing.Errors.CStr();
	for (const FString& project : { ProjectArgument(), "--project=" + FString::FromPath(ProjectFile), FString::FromPath(ProjectFile),
		FString("-Project=") })
	{
		const test::SProcessResult both = Run({ "--engine", project, "newguid" });
		EXPECT_EQ(both.ExitCode, 2) << project.CStr();
		EXPECT_TRUE(both.Errors.Contains("--engine acts on /Engine without a project")) << project.CStr() << ": " << both.Errors.CStr();
	}

	// Verbs that write nothing need no project.
	EXPECT_EQ(Run({ "newguid" }).ExitCode, 0);
	EXPECT_EQ(Run({ "--engine", "newguid" }).ExitCode, 0);
}

// <verb> --help says what the verb does, and does not run it.
TEST_F(AssetToolTest, HelpWithAVerbDoesNotRunIt)
{
	const test::SProcessResult help = Run({ "newguid", "--help" });
	ASSERT_EQ(help.ExitCode, 0) << help.Errors.CStr();
	EXPECT_EQ(help.Output, Run({ "help", "newguid" }).Output);
	EXPECT_EQ(SplitLines(help.Output).size(), 1u) << help.Output.CStr();

	// lock --help takes no lock, so it needs no project.
	const test::SProcessResult lock = Run({ "-Run=lock", "-?" });
	EXPECT_EQ(lock.ExitCode, 0) << lock.Errors.CStr();
	EXPECT_TRUE(lock.Output.Contains("lock:")) << lock.Output.CStr();
}

// help and newguid work wherever they are run: a project in the working directory that cannot be used - its
// file does not load, its engine is not there, it is another engine's - is passed over with a warning on stderr.
// A run that writes, or that names the project, still fails on it.
TEST_F(AssetToolTest, AVerbThatWritesNothingRunsBesideAProjectItCannotUse)
{
	std::error_code ec;
	const std::filesystem::path otherEngine = Dir / FStringView("Інший рушій").ToPath();
	ASSERT_TRUE(std::filesystem::create_directories(otherEngine, ec));
	ASSERT_TRUE(platform::WriteFileText(otherEngine / project::EngineFileName.ToPath(), "[Engine]\nName=OtherEngine\n"));

	struct SCase
	{
		FStringView Directory;
		FString Descriptor;
		FStringView Why;   // in the warning
	};
	const SCase cases[] =
	{
		{ "Без рушія", "[Project]\nName=NoEngine\nEngine=C:\\NoSuchEngine\n", "no engine 'C:\\NoSuchEngine'" },
		{ "Без імені", "[Project]\nEngine=" + FString::FromPath(paths::GetEngineDir()) + "\n", "has no [Project] Name" },
		{ "Чужий рушій", "[Project]\nName=OtherEngine\nEngine=" + FString::FromPath(otherEngine) + "\n", "uses the engine in" },
	};
	for (const SCase& testCase : cases)
	{
		SCOPED_TRACE(FString(testCase.Directory).CStr());
		const std::filesystem::path dir = Dir / testCase.Directory.ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(dir, ec));
		const std::filesystem::path file = dir / "Other.frtproject";
		ASSERT_TRUE(platform::WriteFileText(file, testCase.Descriptor));

		const test::SProcessResult help = Run({ "help" }, dir);
		EXPECT_EQ(help.ExitCode, 0) << help.Errors.CStr();
		EXPECT_TRUE(help.Output.Contains("newguid [n]:")) << help.Output.CStr();
		EXPECT_TRUE(help.Errors.StartsWith("warning: the working directory's project cannot be used, so help runs without one: "))
			<< help.Errors.CStr();
		EXPECT_TRUE(help.Errors.Contains(testCase.Why)) << help.Errors.CStr();

		// The warning stays on stderr: --json stdout is the GUIDs alone.
		const test::SProcessResult guids = Run({ "newguid", "2", "--json" }, dir);
		EXPECT_EQ(guids.ExitCode, 0) << guids.Errors.CStr();
		EXPECT_EQ(SplitLines(guids.Output).size(), 2u) << guids.Output.CStr();
		EXPECT_TRUE(guids.Errors.StartsWith("warning: ")) << guids.Errors.CStr();

		// It is needed: the verb writes, or the project was named.
		const test::SProcessResult lock = Run({ "lock" }, dir);
		EXPECT_EQ(lock.ExitCode, 2) << lock.Errors.CStr();
		EXPECT_FALSE(lock.Errors.Contains("warning")) << lock.Errors.CStr();
		EXPECT_EQ(Run({ "-Project=" + FString::FromPath(file), "newguid" }).ExitCode, 2);
		EXPECT_EQ(Run({ FString::FromPath(file), "newguid" }).ExitCode, 2);
	}
}

// The writer lock cannot be taken for a reason that is no other owner - here Local is a file, so Local/Assets
// cannot be made: an internal error, exit code 3, with the reason, and no owner in the JSON object.
TEST_F(AssetToolTest, AFailureToTakeTheLockIsAnInternalError)
{
	ASSERT_TRUE(platform::WriteFileText(ProjectDir / "Local", "not a directory"));

	const test::SProcessResult text = Run({ ProjectArgument(), "lock" });
	EXPECT_EQ(text.ExitCode, 3) << text.Errors.CStr();
	EXPECT_TRUE(text.Errors.StartsWith("lock: cannot take the writer lock of /Project: ")) << text.Errors.CStr();
	EXPECT_TRUE(text.Output.IsEmpty()) << text.Output.CStr();

	const test::SProcessResult json = Run({ ProjectArgument(), "lock", "--json" });
	EXPECT_EQ(json.ExitCode, 3) << json.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(json.Output);
	ASSERT_EQ(lines.size(), 1u) << json.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"error\":\"lock: cannot take the writer lock of /Project: ")) << json.Output.CStr();
	EXPECT_TRUE(lines[0].EndsWith(",\"exitCode\":3}")) << json.Output.CStr();
	EXPECT_FALSE(lines[0].Contains("\"owner\"")) << json.Output.CStr();

	// A run that does not write never asks for the lock.
	EXPECT_EQ(Run({ ProjectArgument(), "newguid" }).ExitCode, 0);
}

// --engine: the run acts on /Engine as its owner, with the engine's own Local/Assets/Owner.lock - of the
// engine this AssetTool belongs to, here a scratch checkout - and is refused while another process owns it,
// with the design's message naming /Engine. A project in the working directory makes no difference.
TEST_F(AssetToolTest, TheEnginesOwnerTakesTheEnginesLock)
{
	const std::filesystem::path engine = Dir / FStringView("Рушій 引擎").ToPath();
	const std::filesystem::path engineTool = MakeScratchEngine(engine);
	ASSERT_FALSE(HasFailure());
	const std::filesystem::path lockFile = assets::GetOwnerLockPath(engine);

	const test::SProcessResult locked = test::RunProcess(engineTool, std::vector<FString> { "--engine", "lock", "--json" }, ProjectDir);
	ASSERT_EQ(locked.ExitCode, 0) << locked.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(locked.Output);
	ASSERT_EQ(lines.size(), 2u) << locked.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"locked\":\"/Engine\",\"lockFile\":\"")) << locked.Output.CStr();
	EXPECT_TRUE(lines[0].Contains(FStringView("Рушій 引擎\\\\Local\\\\Assets\\\\Owner.lock\""))) << locked.Output.CStr();
	EXPECT_EQ(lines[1], "{\"unlocked\":\"/Engine\"}");
	EXPECT_FALSE(std::filesystem::exists(lockFile));

	FString error;
	const std::optional<platform::CExclusiveFileLock> owned = assets::TryTakeOwnerLock(engine, nullptr, &error);
	ASSERT_TRUE(owned.has_value()) << error.CStr();
	const FString owner = assets::GetThisProcessAsOwner().Describe();

	const test::SProcessResult refused = test::RunProcess(engineTool, std::vector<FString> { "--engine", "lock" }, ProjectDir);
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(owner + " owns /Engine: close it, or run with --ci")) << refused.Errors.CStr();

	const test::SProcessResult refusedJson = test::RunProcess(engineTool, std::vector<FString> { "--engine", "lock", "--json" });
	EXPECT_EQ(refusedJson.ExitCode, 4) << refusedJson.Errors.CStr();
	EXPECT_TRUE(refusedJson.Output.Contains("\"exitCode\":4,\"owner\":\"" + owner + "\",\"mount\":\"/Engine\"}")) << refusedJson.Output.CStr();

	// A project's lock is another one: free. (This checkout's AssetTool: the project names this engine.)
	EXPECT_EQ(Run({ ProjectArgument(), "lock" }).ExitCode, 0);
}

// The design's message and code for a second writer: this test process owns the project, AssetTool's
// lock verb (which writes, as far as the lock is concerned) is refused and names it. Read-only runs
// never get that far, and verbs that write nothing never ask.
TEST_F(AssetToolTest, AWritingRunExitsWithFourWhileAnotherProcessOwnsTheProject)
{
	FString error;
	const std::optional<platform::CExclusiveFileLock> owned = assets::TryTakeOwnerLock(ProjectDir, nullptr, &error);
	ASSERT_TRUE(owned.has_value()) << error.CStr();
	const FString owner = assets::GetThisProcessAsOwner().Describe();

	const test::SProcessResult refused = Run({ ProjectArgument(), "lock" });
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(owner + " owns /Project: close it, or run with --ci")) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Output.IsEmpty()) << refused.Output.CStr();

	const test::SProcessResult refusedJson = Run({ ProjectArgument(), "lock", "--json" });
	EXPECT_EQ(refusedJson.ExitCode, 4);
	const std::vector<FStringView> lines = SplitLines(refusedJson.Output);
	ASSERT_EQ(lines.size(), 1u) << refusedJson.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"error\":\"")) << refusedJson.Output.CStr();
	EXPECT_TRUE(lines[0].Contains("\"exitCode\":4,\"owner\":\"" + owner + "\",\"mount\":\"/Project\"}")) << refusedJson.Output.CStr();

	// Read-only by request: no lock, so no 4 (lock itself has nothing to do then).
	const test::SProcessResult ci = Run({ ProjectArgument(), "lock", "--ci" });
	EXPECT_EQ(ci.ExitCode, 2) << ci.Errors.CStr();
	EXPECT_TRUE(ci.Errors.Contains("read-only")) << ci.Errors.CStr();
	EXPECT_EQ(Run({ ProjectArgument(), "lock", "-Assets:ReadOnly=1" }).ExitCode, 2);

	// newguid writes nothing: it runs beside the owner.
	EXPECT_EQ(Run({ ProjectArgument(), "newguid" }).ExitCode, 0);
}

// lock holds the project until its input ends: a second writer (AssetTool again) is refused meanwhile and
// names it, and once it let go the next one runs. The project is found from the working directory.
TEST_F(AssetToolTest, LockKeepsOtherWritersOutUntilItsInputEnds)
{
	test::CTestProcess holder;
	const FString arguments[] = { FString("lock") };
	FString startError;
	ASSERT_TRUE(holder.Start(Exe, arguments, true, ProjectDir, &startError)) << startError.CStr();
	ASSERT_TRUE(test::WaitUntil([&] { return holder.ReadOutput().Contains("is locked"); }))
		<< "out: " << holder.ReadOutput().CStr() << " err: " << holder.ReadErrors().CStr();
	EXPECT_TRUE(holder.ReadOutput().StartsWith("/Project is locked by this process")) << holder.ReadOutput().CStr();
	EXPECT_TRUE(std::filesystem::exists(assets::GetOwnerLockPath(ProjectDir)));

	const test::SProcessResult refused = Run({ "lock" }, ProjectDir);
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(FString::Format(", pid {}, started ", holder.GetProcessId()))) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains("AssetTool")) << refused.Errors.CStr();
	EXPECT_TRUE(holder.IsRunning());

	holder.CloseInput();
	EXPECT_EQ(holder.Wait(), 0) << holder.ReadErrors().CStr();
	EXPECT_TRUE(holder.ReadOutput().Contains("/Project is unlocked")) << holder.ReadOutput().CStr();
	EXPECT_FALSE(std::filesystem::exists(assets::GetOwnerLockPath(ProjectDir)));

	// Its input ends at once: it locks and lets go.
	const test::SProcessResult next = Run({ "lock", "--json" }, ProjectDir);
	EXPECT_EQ(next.ExitCode, 0) << next.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(next.Output);
	ASSERT_EQ(lines.size(), 2u) << next.Output.CStr();
	EXPECT_TRUE(lines[0].StartsWith("{\"locked\":\"/Project\",\"lockFile\":\"")) << next.Output.CStr();
	EXPECT_EQ(lines[1], "{\"unlocked\":\"/Project\"}");
}

// A killed lock holder leaves nothing behind that keeps the next writer out.
TEST_F(AssetToolTest, AKilledLockHolderLeavesTheProjectFree)
{
	test::CTestProcess holder;
	const FString arguments[] = { ProjectArgument(), FString("lock") };
	ASSERT_TRUE(holder.Start(Exe, arguments, true));
	ASSERT_TRUE(test::WaitUntil([&] { return holder.ReadOutput().Contains("is locked"); })) << holder.ReadErrors().CStr();
	holder.Kill();

	const test::SProcessResult next = Run({ ProjectArgument(), "lock" });
	EXPECT_EQ(next.ExitCode, 0) << next.Errors.CStr();
}

// Where there is no store, the ddc verbs say so and succeed - and create none to find that out.
TEST_F(AssetToolTest, DdcVerbsFindNoStoreWhereThereIsNoneAndCreateNone)
{
	constexpr FStringLiteral Actions[] = { "stats", "gc", "verify" };
	for (const FStringLiteral action : Actions)
	{
		SCOPED_TRACE(action.CStr());
		const test::SProcessResult text = Run({ "ddc", FString(action) });
		EXPECT_EQ(text.ExitCode, 0) << text.Errors.CStr();
		EXPECT_TRUE(text.Output.StartsWith("No derived-data cache at " + FString::FromPath(DdcDir) + ": nothing to ")) << text.Output.CStr();

		const test::SProcessResult json = Run({ "ddc", FString(action), "--json" });
		EXPECT_EQ(json.ExitCode, 0) << json.Errors.CStr();
		const std::vector<FStringView> lines = SplitLines(json.Output);
		ASSERT_EQ(lines.size(), 1u) << json.Output.CStr();
		EXPECT_TRUE(lines[0].StartsWith("{\"ddc\":\"")) << json.Output.CStr();
		EXPECT_TRUE(lines[0].EndsWith("\",\"exists\":false}")) << json.Output.CStr();
	}
	EXPECT_FALSE(std::filesystem::exists(DdcDir));
}

// ddc stats counts what the store holds, per kind - FRT_DDC_DIR's store, or the one --ddc names - and, like
// every ddc verb, runs beside a process that owns the project: the store needs no writer lock.
TEST_F(AssetToolTest, DdcStatsCountsTheStoreItIsPointedAt)
{
	const std::vector<hash::SContentHash> keys = FillDdc(DdcDir, 2u);
	ASSERT_FALSE(HasFailure());
	assets::CDerivedDataCache ddc;
	ASSERT_TRUE(ddc.Open(DdcDir));
	const assets::SDdcStats stats = ddc.GetStats();
	ddc.Close();

	FString error;
	const std::optional<platform::CExclusiveFileLock> owned = assets::TryTakeOwnerLock(ProjectDir, nullptr, &error);
	ASSERT_TRUE(owned.has_value()) << error.CStr();

	const test::SProcessResult json = Run({ ProjectArgument(), "ddc", "stats", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<FStringView> lines = SplitLines(json.Output);
	ASSERT_EQ(lines.size(), 1u) << json.Output.CStr();
	EXPECT_TRUE(lines[0].Contains("\"exists\":true")) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "recordFiles"), 2u) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "valueFiles"), 2u) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "recordBytes"), stats.Records.Bytes) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "valueBytes"), stats.Values.Bytes) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "contentBytes"), stats.GetContentBytes()) << json.Output.CStr();
	EXPECT_EQ(JsonUInt(lines[0], "manifestFiles"), 0u) << json.Output.CStr();

	const test::SProcessResult text = Run({ "ddc", "stats" });
	ASSERT_EQ(text.ExitCode, 0) << text.Errors.CStr();
	const std::vector<FStringView> textLines = SplitLines(text.Output);
	ASSERT_FALSE(textLines.empty());
	EXPECT_EQ(textLines[0], "Derived-data cache: " + FString::FromPath(DdcDir));
	EXPECT_TRUE(text.Output.Contains(FString::Format("  {:<12} {:>8} file(s)", "records", 2))) << text.Output.CStr();

	// --ddc wins over FRT_DDC_DIR.
	const std::filesystem::path other = Dir / FStringView("Інший кеш").ToPath();
	const test::SProcessResult elsewhere = Run({ "ddc", "stats", "--json", "--ddc=" + FString::FromPath(other) });
	EXPECT_EQ(elsewhere.ExitCode, 0) << elsewhere.Errors.CStr();
	EXPECT_TRUE(elsewhere.Output.Contains("\"exists\":false")) << elsewhere.Output.CStr();
	FillDdc(other, 1u);
	const test::SProcessResult filled = Run({ "ddc", "stats", "--json", "--ddc=" + FString::FromPath(other) });
	EXPECT_EQ(JsonUInt(filled.Output, "recordFiles"), 1u) << filled.Output.CStr();

	// The root it prints has one separator style and no detour, however --ddc spelled it.
	const FString spelled = FString::FromPath(other).Replace("\\", "/") + "/../" + FString::FromPath(other.filename());
	const test::SProcessResult normalized = Run({ "ddc", "stats", "--json", "--ddc=" + spelled });
	EXPECT_EQ(JsonUInt(normalized.Output, "recordFiles"), 1u) << normalized.Output.CStr();
	EXPECT_TRUE(normalized.Output.Contains("{\"ddc\":\"" + FString::FromPath(other).Replace("\\", "\\\\") + "\""))
		<< normalized.Output.CStr();
}

// ddc gc trims the content to the budget, least recently used first, keeping every record the project's
// BuildState names (and its value); with --ci it is a dry run that reports the same and deletes nothing.
TEST_F(AssetToolTest, DdcGcTrimsToTheBudgetKeepsTheBuildStateAndIsADryRunWithCi)
{
	const std::vector<hash::SContentHash> keys = FillDdc(DdcDir, 3u);
	ASSERT_FALSE(HasFailure());
	{
		// The project's BuildState names the oldest record, which LRU order would evict first.
		assets::CBuildStateLog log;
		FString error;
		ASSERT_TRUE(log.Open(assets::GetBuildStatePath(ProjectDir), &error)) << error.CStr();
		assets::SBuildStateEntry entry;
		entry.Step = assets::SBuildStepId{ "Asset", "Test", "", "" };
		entry.StaticKey = ScratchKey(100u);
		entry.Key = keys[0];
		ASSERT_TRUE(log.Append(entry, &error)) << error.CStr();
	}
	assets::CDerivedDataCache ddc;
	ASSERT_TRUE(ddc.Open(DdcDir));
	const uint64 before = ddc.GetStats().GetContentBytes();

	// A budget of nothing, read-only: what would go, and nothing goes. The pinned record keeps the content
	// over the budget: a problem, exit 1.
	const test::SProcessResult dry = Run({ ProjectArgument(), "ddc", "gc", "--max-gb=0", "--ci", "--json" });
	EXPECT_EQ(dry.ExitCode, 1) << dry.Errors.CStr();
	EXPECT_TRUE(dry.Output.Contains("\"dryRun\":true")) << dry.Output.CStr();
	EXPECT_EQ(JsonUInt(dry.Output, "recordsEvicted"), 2u) << dry.Output.CStr();
	EXPECT_EQ(JsonUInt(dry.Output, "valuesEvicted"), 2u) << dry.Output.CStr();
	EXPECT_EQ(JsonUInt(dry.Output, "pinnedKeys"), 1u) << dry.Output.CStr();
	EXPECT_EQ(JsonUInt(dry.Output, "contentBytesBefore"), before) << dry.Output.CStr();
	EXPECT_TRUE(dry.Output.Contains("\"withinBudget\":false")) << dry.Output.CStr();
	EXPECT_TRUE(dry.Errors.Contains("stays over the budget")) << dry.Errors.CStr();
	EXPECT_EQ(ddc.GetStats().GetContentBytes(), before);
	EXPECT_EQ(Run({ ProjectArgument(), "ddc", "gc", "--max-gb=0", "-Assets:ReadOnly=1" }).ExitCode, 1);
	EXPECT_EQ(ddc.GetStats().GetContentBytes(), before);

	// For real: the same two go; the pinned record and its value stay.
	const test::SProcessResult real = Run({ ProjectArgument(), "ddc", "gc", "--max-gb=0", "--json" });
	EXPECT_EQ(real.ExitCode, 1) << real.Errors.CStr();
	EXPECT_TRUE(real.Output.Contains("\"dryRun\":false")) << real.Output.CStr();
	EXPECT_EQ(JsonUInt(real.Output, "recordsEvicted"), 2u) << real.Output.CStr();
	EXPECT_TRUE(ddc.GetRecord("Test", keys[0]).has_value());
	EXPECT_FALSE(std::filesystem::exists(ddc.GetRecordPath("Test", keys[1])));
	EXPECT_FALSE(std::filesystem::exists(ddc.GetRecordPath("Test", keys[2])));

	// Within the budget: nothing to do, exit 0.
	const test::SProcessResult fits = Run({ ProjectArgument(), "ddc", "gc", "--max-gb=1" });
	EXPECT_EQ(fits.ExitCode, 0) << fits.Errors.CStr();
	EXPECT_TRUE(fits.Output.Contains("evicted 0 record(s)")) << fits.Output.CStr();

	// Without the project (and with no recent projects) its BuildState is not known here: everything goes.
	// The engine's own log, if this checkout has one, names none of the scratch keys.
	const std::optional<std::vector<hash::SContentHash>> engineKeys =
		assets::CBuildStateLog::ReadKeys(assets::GetBuildStatePath(paths::GetEngineDir()));
	const test::SProcessResult all = Run({ "ddc", "gc", "--max-gb=0", "--json" });
	EXPECT_EQ(all.ExitCode, 0) << all.Errors.CStr() << all.Output.CStr();
	EXPECT_EQ(JsonUInt(all.Output, "pinnedKeys"), engineKeys ? engineKeys->size() : 0u) << all.Output.CStr();
	EXPECT_EQ(JsonUInt(all.Output, "buildStateLogs"), engineKeys ? 1u : 0u) << all.Output.CStr();
	EXPECT_EQ(ddc.GetStats().GetContentBytes(), 0u);
}

// The store is machine-wide, so ddc gc keeps what every BuildState log of the machine names, not only the
// mount's: the engine's and those of the user's recent projects (the editor adds each project it opens),
// so an editor open on another project keeps its values. Each log is read once and none is written: a
// recent project without a log gets none, and a foreign file is left as it is (and reported). Run from
// anywhere, even without a project, the same records stay. The note and the JSON count the logs read.
TEST_F(AssetToolTest, DdcGcKeepsWhatEveryBuildStateOfTheMachineNames)
{
	const std::vector<hash::SContentHash> keys = FillDdc(DdcDir, 4u);
	ASSERT_FALSE(HasFailure());
	const auto writeBuildState = [] (const std::filesystem::path& Project, const hash::SContentHash& Key, uint32 Number)
	{
		assets::CBuildStateLog log;
		FString error;
		ASSERT_TRUE(log.Open(assets::GetBuildStatePath(Project), &error)) << error.CStr();
		assets::SBuildStateEntry entry;
		entry.Step = assets::SBuildStepId{ FString::Format("Asset{}", Number), "Test", "", "" };
		entry.StaticKey = ScratchKey(100u + Number);
		entry.Key = Key;
		ASSERT_TRUE(log.Append(entry, &error)) << error.CStr();
	};
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::create_directories(Dir / "UserData", ec)) << FString::FromUtf8Lossy(ec.message()).CStr();

	// This project's log names the oldest record, another recent project's the next: LRU order would
	// evict both first.
	writeBuildState(ProjectDir, keys[0], 1u);
	const std::filesystem::path other = Dir / FStringView("Інша гра 另一个").ToPath();
	writeBuildState(other, keys[1], 2u);
	ASSERT_FALSE(HasFailure());
	project::AddRecentProject(other / "Other.frtproject");
	project::AddRecentProject(ProjectFile);   // the mount itself in the list too: read once
	// A recent project whose log is a foreign file, and one with no Local/ at all.
	const std::filesystem::path foreign = assets::GetBuildStatePath(Dir / "Foreign");
	ASSERT_TRUE(platform::WriteFileText(foreign, "not a build state log"));
	project::AddRecentProject(Dir / "Foreign" / "Foreign.frtproject");
	project::AddRecentProject(Dir / "Bare" / "Bare.frtproject");
	ASSERT_EQ(project::GetRecentProjects().size(), 4u);

	const std::optional<std::vector<hash::SContentHash>> engineKeys =
		assets::CBuildStateLog::ReadKeys(assets::GetBuildStatePath(paths::GetEngineDir()));
	const uint64 logs = 2u + (engineKeys ? 1u : 0u);
	const uint64 pinnedKeys = 2u + (engineKeys ? engineKeys->size() : 0u);

	// Read-only from outside any project: the same logs, nothing deleted.
	const test::SProcessResult dry = Run({ "ddc", "gc", "--max-gb=0", "--ci" });
	EXPECT_EQ(dry.ExitCode, 1) << dry.Errors.CStr();   // the kept records stay over a budget of nothing
	EXPECT_TRUE(dry.Output.Contains(FString::Format("{} key(s) of {} BuildState log(s) kept", pinnedKeys, logs))) << dry.Output.CStr();
	EXPECT_TRUE(dry.Output.Contains("would evict 2 record(s)")) << dry.Output.CStr();
	EXPECT_TRUE(dry.Errors.Contains("warning: the BuildState log " + FString::FromPath(foreign))) << dry.Errors.CStr();

	const test::SProcessResult gc = Run({ ProjectArgument(), "ddc", "gc", "--max-gb=0", "--json" });
	EXPECT_EQ(gc.ExitCode, 1) << gc.Errors.CStr();
	EXPECT_EQ(JsonUInt(gc.Output, "buildStateLogs"), logs) << gc.Output.CStr();
	EXPECT_EQ(JsonUInt(gc.Output, "buildStateLogsUnreadable"), 1u) << gc.Output.CStr();
	EXPECT_EQ(JsonUInt(gc.Output, "pinnedKeys"), pinnedKeys) << gc.Output.CStr();
	EXPECT_EQ(JsonUInt(gc.Output, "recordsEvicted"), 2u) << gc.Output.CStr();
	assets::CDerivedDataCache ddc;
	ASSERT_TRUE(ddc.Open(DdcDir));
	EXPECT_TRUE(ddc.GetRecord("Test", keys[0]).has_value());
	EXPECT_TRUE(ddc.GetRecord("Test", keys[1]).has_value());
	EXPECT_FALSE(std::filesystem::exists(ddc.GetRecordPath("Test", keys[2])));
	EXPECT_FALSE(std::filesystem::exists(ddc.GetRecordPath("Test", keys[3])));

	// Nothing written where it read: the foreign file as it was, no log (or Local/) for the bare project.
	EXPECT_EQ(platform::ReadFileText(foreign), FString("not a build state log"));
	EXPECT_FALSE(std::filesystem::exists(Dir / "Bare"));
}

// ddc verify re-checks every file, deletes the corrupt ones and exits with 1 when it found any.
TEST_F(AssetToolTest, DdcVerifyDeletesCorruptFilesAndSaysSo)
{
	const std::vector<hash::SContentHash> keys = FillDdc(DdcDir, 2u);
	ASSERT_FALSE(HasFailure());
	assets::CDerivedDataCache ddc;
	ASSERT_TRUE(ddc.Open(DdcDir));
	const std::filesystem::path record = ddc.GetRecordPath("Test", keys[1]);
	ASSERT_TRUE(platform::WriteFileBytes(record, std::vector<uint8>(std::filesystem::file_size(record), 0u)));

	const test::SProcessResult damaged = Run({ "ddc", "verify", "--json" });
	EXPECT_EQ(damaged.ExitCode, 1) << damaged.Errors.CStr();
	EXPECT_EQ(JsonUInt(damaged.Output, "recordsChecked"), 2u) << damaged.Output.CStr();
	EXPECT_EQ(JsonUInt(damaged.Output, "corruptRecords"), 1u) << damaged.Output.CStr();
	EXPECT_EQ(JsonUInt(damaged.Output, "valuesChecked"), 2u) << damaged.Output.CStr();
	EXPECT_FALSE(std::filesystem::exists(record));

	const test::SProcessResult clean = Run({ "ddc", "verify" });
	EXPECT_EQ(clean.ExitCode, 0) << clean.Errors.CStr();
	EXPECT_TRUE(clean.Output.Contains("every file is intact")) << clean.Output.CStr();
}


// ---- The asset registry's verbs ----

namespace
{
// A PNG's header as stb_image's info reads it: signature, IHDR (8-bit RGBA), an empty IDAT, IEND.
std::vector<uint8> MakeRegistryPng (uint32 Width, uint32 Height)
{
	std::vector<uint8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	const auto u32 = [&] (uint32 Value)
	{
		png.insert(png.end(), { uint8(Value >> 24u), uint8(Value >> 16u), uint8(Value >> 8u), uint8(Value) });
	};
	const auto chunk = [&] (const char (&Type)[5], std::initializer_list<uint8> Data, std::initializer_list<uint32> Words)
	{
		u32(static_cast<uint32>(Data.size() + Words.size() * 4u));
		png.insert(png.end(), Type, Type + 4);
		for (const uint32 word : Words)
		{
			u32(word);
		}
		png.insert(png.end(), Data);
		u32(0u);   // stb never checks the CRC
	};
	chunk("IHDR", { 8u, 6u, 0u, 0u, 0u }, { Width, Height });
	chunk("IDAT", {}, {});
	chunk("IEND", {}, {});
	return png;
}

// Every line of a --json stream parsed (a failure for a line that is no JSON object).
std::vector<reflect::CDataTree> ParseJsonLines (FStringView Output)
{
	std::vector<reflect::CDataTree> objects;
	for (const FStringView line : Output.Lines())
	{
		if (line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		reflect::CDataTree tree;
		SDiagnostic error;
		EXPECT_TRUE(reflect::ParseJson(line, tree, &error)) << error.Message.CStr() << ": " << FString(line).CStr();
		objects.push_back(std::move(tree));
	}
	return objects;
}

FString JsonText (const reflect::CDataTree& Tree, FStringView Path)
{
	return FString(Tree.Find(Path).AsText().value_or(FStringView()));
}

int64 JsonInt (const reflect::CDataTree& Tree, FStringView Path)
{
	return Tree.Find(Path).AsInt64().value_or(-1);
}

class AssetToolRegistryTest : public AssetToolTest
{
protected:
	void SetUp () override
	{
		AssetToolTest::SetUp();
		if (IsSkipped() || HasFatalFailure())
		{
			return;
		}
		const std::filesystem::path content = ProjectDir / "Content";
		WritePng(content / FStringView("Textures/Камінь.png").ToPath(), 64u);
		WritePng(content / FStringView("Textures/Трава.png").ToPath(), 2048u);
		WriteText(content / FStringView("Textures/Assets.yml").ToPath(), "FormatVersion: 1\nRules:\n  - { Match: \"*.png\", Labels: [ Stone ] }\n");
		WriteText(content / FStringView("Materials/Камінь.frtmat.yml").ToPath(), "version: 2\nname: Камінь\n\nalbedo:\n  texture: ../Textures/Камінь.png\n");
		WriteText(content / FStringView("Notes/нотатки.txt").ToPath(), "notes");
		WriteText(ProjectDir / "Config" / "Assets.yml", "FormatVersion: 1\n");
	}

	static void WritePng (const std::filesystem::path& Path, uint32 Size)
	{
		std::error_code ec;
		std::filesystem::create_directories(Path.parent_path(), ec);
		ASSERT_TRUE(platform::WriteFileBytes(Path, MakeRegistryPng(Size, Size))) << FString::FromPath(Path).CStr();
	}

	static void WriteText (const std::filesystem::path& Path, FStringView Text)
	{
		std::error_code ec;
		std::filesystem::create_directories(Path.parent_path(), ec);
		ASSERT_TRUE(platform::WriteFileText(Path, Text)) << FString::FromPath(Path).CStr();
	}

	// A registry verb on the scratch project, run in its directory.
	test::SProcessResult RunOnProject (std::vector<FString> Arguments) const
	{
		Arguments.insert(Arguments.begin(), ProjectArgument());
		return Run(Arguments, ProjectDir);
	}

	[[nodiscard]] bool WroteLocalAssets () const
	{
		return std::filesystem::exists(assets::GetMountAssetsLocalDir(ProjectDir));
	}
};
}

// scan under the lock writes the project's snapshot, ProjectId and Rules.cache, and a warm scan loads it; scan --ci
// and every read verb write nothing at all.
TEST_F(AssetToolRegistryTest, OnlyAScanUnderTheLockWritesAndAWarmScanLoadsItsSnapshot)
{
	for (const std::vector<FString>& readOnly : std::vector<std::vector<FString>> { { "scan", "--ci" }, { "status" }, { "query", "type:Texture" },
		{ "refs", "/Project/Materials/Камінь.frtmat.yml" }, { "referencers", "/Project/Textures/Камінь.png" },
		{ "explain", "/Project/Textures/Трава.png" }, { "list-rules" }, { "dump", "registry" } })
	{
		const test::SProcessResult result = RunOnProject(readOnly);
		EXPECT_EQ(result.ExitCode, 0) << readOnly.front().CStr() << ": " << result.Errors.CStr();
		EXPECT_FALSE(WroteLocalAssets()) << readOnly.front().CStr();
	}

	const test::SProcessResult cold = RunOnProject({ "scan", "--json" });
	ASSERT_EQ(cold.ExitCode, 0) << cold.Errors.CStr();
	const std::vector<reflect::CDataTree> coldLines = ParseJsonLines(cold.Output);
	ASSERT_EQ(coldLines.size(), 1u) << cold.Output.CStr();
	EXPECT_EQ(JsonText(coldLines[0], "verb"), "scan");
	EXPECT_EQ(JsonText(coldLines[0], "owner"), "Project");
	EXPECT_EQ(JsonText(coldLines[0], "mounts.1"), "/Project");
	EXPECT_TRUE(coldLines[0].Find("writer").AsBool().value_or(false));
	EXPECT_GE(JsonInt(coldLines[0], "records"), 3);
	EXPECT_GE(JsonInt(coldLines[0], "analyzed"), 3);
	EXPECT_EQ(JsonInt(coldLines[0], "errors"), 0);
	EXPECT_TRUE(std::filesystem::exists(assets::GetRegistrySnapshotPath(ProjectDir)));
	EXPECT_TRUE(std::filesystem::exists(assets::GetProjectIdPath(ProjectDir)));
	EXPECT_TRUE(std::filesystem::exists(assets::GetRulesCachePath(ProjectDir)));
	EXPECT_EQ(FString::FromPath(assets::GetRegistrySnapshotPath(ProjectDir)), JsonText(coldLines[0], "snapshot"));
	EXPECT_FALSE(std::filesystem::exists(ProjectDir / FStringView("Content/Textures/Камінь.png.meta").ToPath())) << "scan never writes a meta";

	// Warm: the project's records come from its snapshot (Engine.snapshot is not this verb's to write).
	const test::SProcessResult warm = RunOnProject({ "scan" });
	ASSERT_EQ(warm.ExitCode, 0) << warm.Errors.CStr();
	EXPECT_TRUE(warm.Output.Contains("AssetRegistry.snapshot: Loaded")) << warm.Output.CStr();
	EXPECT_TRUE(warm.Output.Contains("wrote ")) << warm.Output.CStr();

	// The same dump from the warm registry as from a cold read-only one.
	const test::SProcessResult warmDump = RunOnProject({ "dump", "registry", "--mount=Project" });
	ASSERT_EQ(warmDump.ExitCode, 0) << warmDump.Errors.CStr();
	std::error_code ec;
	std::filesystem::remove_all(assets::GetMountAssetsLocalDir(ProjectDir), ec);
	const test::SProcessResult coldDump = RunOnProject({ "dump", "registry", "--mount=Project" });
	EXPECT_EQ(warmDump.Output, coldDump.Output);
	EXPECT_FALSE(WroteLocalAssets());
}

// A second writer (§5.9): while this test process owns the project, scan exits with 4 naming it - until the project's
// Safety.GuestProcesses lets it run as a read-only guest, which says so, scans, and writes no snapshot. lock is never a
// guest: holding the lock is its point.
TEST_F(AssetToolRegistryTest, AScanBesideTheOwnerRunsAsAReadOnlyGuestOnlyWhenTheProjectSaysSo)
{
	FString error;
	const std::optional<platform::CExclusiveFileLock> owned = assets::TryTakeOwnerLock(ProjectDir, nullptr, &error);
	ASSERT_TRUE(owned.has_value()) << error.CStr();
	const FString owner = assets::GetThisProcessAsOwner().Describe();

	const test::SProcessResult refused = RunOnProject({ "scan" });
	EXPECT_EQ(refused.ExitCode, 4) << refused.Errors.CStr();
	EXPECT_TRUE(refused.Errors.Contains(owner + " owns /Project: close it, or run with --ci")) << refused.Errors.CStr();

	WriteText(ProjectDir / "Config" / "Assets.yml", "FormatVersion: 1\nSafety:\n  GuestProcesses: true\n");
	const test::SProcessResult guest = RunOnProject({ "scan" });
	EXPECT_EQ(guest.ExitCode, 0) << guest.Errors.CStr();
	EXPECT_TRUE(guest.Errors.Contains(owner + " owns /Project: scan runs as a read-only guest (Safety.GuestProcesses)")) << guest.Errors.CStr();
	EXPECT_TRUE(guest.Output.Contains("read-only: wrote nothing")) << guest.Output.CStr();
	EXPECT_FALSE(std::filesystem::exists(assets::GetRegistrySnapshotPath(ProjectDir)));

	const test::SProcessResult lock = RunOnProject({ "lock" });
	EXPECT_EQ(lock.ExitCode, 4) << lock.Errors.CStr();
	EXPECT_TRUE(lock.Errors.Contains(owner + " owns /Project")) << lock.Errors.CStr();
}

// query: the records in the query engine's order with the columns asked for, a limit, and positioned errors.
TEST_F(AssetToolRegistryTest, QueryPrintsTheMatchingRecordsAndPositionsItsErrors)
{
	const test::SProcessResult json = RunOnProject({ "query", "type:Texture", "--columns=$Path,Width,$Labels", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<reflect::CDataTree> lines = ParseJsonLines(json.Output);
	ASSERT_EQ(lines.size(), 3u) << json.Output.CStr();
	EXPECT_EQ(JsonText(lines[0], "path"), "/Project/Textures/Камінь.png");
	EXPECT_EQ(JsonInt(lines[0], "Width"), 64);
	EXPECT_EQ(JsonText(lines[0], "labels.0"), "Stone");
	EXPECT_EQ(JsonText(lines[1], "path"), "/Project/Textures/Трава.png");
	EXPECT_EQ(JsonInt(lines[2], "matched"), 2);
	EXPECT_EQ(JsonInt(lines[2], "shown"), 2);

	// Several arguments are one query text.
	const test::SProcessResult limited = RunOnProject({ "query", "type:Texture", "Width>=1024", "--limit=1" });
	ASSERT_EQ(limited.ExitCode, 0) << limited.Errors.CStr();
	EXPECT_TRUE(limited.Output.Contains("/Project/Textures/Трава.png")) << limited.Output.CStr();
	EXPECT_TRUE(limited.Output.Contains("1 of 1 record(s)")) << limited.Output.CStr();

	const test::SProcessResult parse = RunOnProject({ "query", "type:Texture (Width>1" });
	EXPECT_EQ(parse.ExitCode, 2) << parse.Errors.CStr();
	EXPECT_TRUE(parse.Errors.Contains("query: column ")) << parse.Errors.CStr();
	EXPECT_TRUE(parse.Errors.Contains("^")) << parse.Errors.CStr();
	const test::SProcessResult compile = RunOnProject({ "query", "type:Texture Wdth>1", "--json" });
	EXPECT_EQ(compile.ExitCode, 2) << compile.Errors.CStr();
	EXPECT_TRUE(compile.Errors.Contains("query: column 14: ")) << compile.Errors.CStr();
	EXPECT_TRUE(compile.Output.Contains("\"exitCode\":2,\"column\":14}")) << compile.Output.CStr();
	EXPECT_EQ(RunOnProject({ "query", "type:Texture", "--columns=Nope" }).ExitCode, 2);
	EXPECT_EQ(RunOnProject({ "query", "type:Texture", "--limit=0" }).ExitCode, 2);

	// A negated term is text, not an unknown option: as an argument of its own, at the start of one quoted text, or after --,
	// which ends the options. Of /Project's records only the material has no Stone label.
	for (const std::vector<FString>& negated : std::vector<std::vector<FString>> { { "query", "-label:Stone", "dir:/Project" },
		{ "query", "-label:Stone dir:/Project" }, { "query", "--ci", "--", "-label:Stone", "dir:/Project" }, { "-label:Stone", "query", "dir:/Project" } })
	{
		const test::SProcessResult result = RunOnProject(negated);
		ASSERT_EQ(result.ExitCode, 0) << negated[1].CStr() << ": " << result.Errors.CStr();
		EXPECT_TRUE(result.Output.Contains("/Project/Materials/Камінь.frtmat.yml")) << result.Output.CStr();
		EXPECT_TRUE(result.Output.Contains("1 of 1 record(s)")) << result.Output.CStr();
	}
	// After --, even a host option's spelling is the query's text.
	const test::SProcessResult text = RunOnProject({ "query", "--", "--json" });
	EXPECT_EQ(text.ExitCode, 2) << text.Output.CStr();
	EXPECT_TRUE(text.Errors.Contains("query: column ")) << text.Errors.CStr();
	EXPECT_FALSE(text.Output.StartsWith("{")) << text.Output.CStr();
	// Two dashes still make an option, so a typo of one is refused.
	const test::SProcessResult typo = RunOnProject({ "query", "type:Texture", "--limt=1" });
	EXPECT_EQ(typo.ExitCode, 2);
	EXPECT_TRUE(typo.Errors.Contains("query: no option '--limt=1'")) << typo.Errors.CStr();

	const test::SProcessResult rows = RunOnProject({ "query", "type:Texture", "--rows" });
	EXPECT_EQ(rows.ExitCode, 0) << rows.Errors.CStr();
	EXPECT_TRUE(rows.Errors.Contains("M5")) << rows.Errors.CStr();
	EXPECT_TRUE(rows.Output.Contains("0 of 0 record(s)")) << rows.Output.CStr();

	// No mount: no project here, no --engine.
	const test::SProcessResult noProject = Run({ "query", "type:Texture" });
	EXPECT_EQ(noProject.ExitCode, 2);
	EXPECT_TRUE(noProject.Errors.Contains("query: no project")) << noProject.Errors.CStr();
}

// refs and referencers list edges with their flags and field paths; an asset that names no record, or several, is a
// usage error.
TEST_F(AssetToolRegistryTest, RefsAndReferencersListEdgesWithTheirFieldPaths)
{
	const test::SProcessResult refs = RunOnProject({ "refs", "/Project/Materials/Камінь.frtmat.yml", "--json" });
	ASSERT_EQ(refs.ExitCode, 0) << refs.Errors.CStr();
	const std::vector<reflect::CDataTree> edges = ParseJsonLines(refs.Output);
	ASSERT_EQ(edges.size(), 1u) << refs.Output.CStr();
	EXPECT_EQ(JsonText(edges[0], "from"), "/Project/Materials/Камінь.frtmat.yml");
	EXPECT_EQ(JsonText(edges[0], "target"), "/Project/Textures/Камінь.png");
	EXPECT_EQ(JsonText(edges[0], "resolved"), "/Project/Textures/Камінь.png");
	EXPECT_EQ(JsonText(edges[0], "field"), "albedo.texture");
	EXPECT_EQ(JsonText(edges[0], "state"), "Resolved");
	EXPECT_EQ(JsonText(edges[0], "flags.0"), "Build");

	const test::SProcessResult direct = RunOnProject({ "referencers", "/Project/Textures/Камінь.png" });
	ASSERT_EQ(direct.ExitCode, 0) << direct.Errors.CStr();
	EXPECT_TRUE(direct.Output.Contains("/Project/Materials/Камінь.frtmat.yml -> /Project/Textures/Камінь.png")) << direct.Output.CStr();
	EXPECT_TRUE(direct.Output.Contains("field=albedo.texture")) << direct.Output.CStr();
	const test::SProcessResult transitive = RunOnProject({ "referencers", "/Project/Textures/Камінь.png", "--transitive", "--json" });
	ASSERT_EQ(transitive.ExitCode, 0) << transitive.Errors.CStr();
	const std::vector<reflect::CDataTree> reached = ParseJsonLines(transitive.Output);
	ASSERT_EQ(reached.size(), 1u) << transitive.Output.CStr();
	EXPECT_EQ(JsonText(reached[0], "type"), "frt.Material");

	// By a unique name, as the search box resolves ref:X: "Трава" is one texture's stem; "Камінь" is the stem of the
	// texture and of the material.
	EXPECT_EQ(RunOnProject({ "referencers", "Трава" }).ExitCode, 0);
	const test::SProcessResult ambiguous = RunOnProject({ "refs", "Камінь" });
	EXPECT_EQ(ambiguous.ExitCode, 2);
	EXPECT_TRUE(ambiguous.Errors.Contains("names 2 records")) << ambiguous.Errors.CStr();
	EXPECT_EQ(RunOnProject({ "refs", "/Project/Nothing.png" }).ExitCode, 2);
	EXPECT_EQ(RunOnProject({ "refs" }).ExitCode, 2);
}

// explain: every effective value with the rule file and line that set it, for a mount path or a disk path.
TEST_F(AssetToolRegistryTest, ExplainNamesTheRuleFileAndLineOfEveryValue)
{
	const test::SProcessResult text = RunOnProject({ "explain", "/Project/Textures/Камінь.png" });
	ASSERT_EQ(text.ExitCode, 0) << text.Errors.CStr();
	EXPECT_TRUE(text.Output.Contains("importer  Texture")) << text.Output.CStr();
	EXPECT_TRUE(text.Output.Contains("Content/Textures/Assets.yml:3 (rule 1)")) << text.Output.CStr();
	EXPECT_TRUE(text.Output.Contains("Core/Config/Assets.yml")) << text.Output.CStr();

	// A disk path relative to the working directory names the same file.
	const test::SProcessResult relative = RunOnProject({ "explain", "Content/Textures/Камінь.png" });
	ASSERT_EQ(relative.ExitCode, 0) << relative.Errors.CStr();
	EXPECT_EQ(relative.Output, text.Output);

	const test::SProcessResult json = RunOnProject({ "explain", "Content/Notes/нотатки.txt", "--json" });
	ASSERT_EQ(json.ExitCode, 0) << json.Errors.CStr();
	const std::vector<reflect::CDataTree> lines = ParseJsonLines(json.Output);
	ASSERT_EQ(lines.size(), 1u) << json.Output.CStr();
	EXPECT_EQ(JsonText(lines[0], "class"), "Unclaimed");
	EXPECT_EQ(JsonText(lines[0], "importer"), "");
	EXPECT_FALSE(lines[0].Find("record").AsBool().value_or(true));

	EXPECT_EQ(RunOnProject({ "explain", "/Nowhere/x.png" }).ExitCode, 2);
	EXPECT_EQ(RunOnProject({ "explain", FString::FromPath(Dir) }).ExitCode, 2);
}

// status and scan exit with 1 on an error-severity problem (a malformed rule file's RULE001); list-rules --json is
// Rules.cache's lines; dump --json has a record for every record line of the text dump, and --mount filters.
TEST_F(AssetToolRegistryTest, StatusListRulesAndDumpReportWhatTheRegistryHolds)
{
	const test::SProcessResult clean = RunOnProject({ "status", "--json" });
	ASSERT_EQ(clean.ExitCode, 0) << clean.Errors.CStr();
	bool bUnclaimed = false;
	for (const reflect::CDataTree& line : ParseJsonLines(clean.Output))
	{
		bUnclaimed = bUnclaimed || (JsonText(line, "kind") == "unclaimed" && JsonText(line, "folder") == "/Project/Notes" && JsonText(line, "extension") == ".txt");
	}
	EXPECT_TRUE(bUnclaimed) << clean.Output.CStr();

	const test::SProcessResult rules = RunOnProject({ "list-rules", "--json" });
	ASSERT_EQ(rules.ExitCode, 0) << rules.Errors.CStr();
	const std::vector<reflect::CDataTree> ruleLines = ParseJsonLines(rules.Output);
	ASSERT_GE(ruleLines.size(), 4u) << rules.Output.CStr();
	EXPECT_EQ(JsonText(ruleLines[0], "format"), "frt.rules.cache");
	EXPECT_TRUE(rules.Output.Contains("\"file\":\"Content/Textures/Assets.yml\"")) << rules.Output.CStr();

	const test::SProcessResult dumpText = RunOnProject({ "dump", "registry", "--text", "--mount=/Project" });
	const test::SProcessResult dumpJson = RunOnProject({ "dump", "registry", "--json", "--mount=Project" });
	ASSERT_EQ(dumpText.ExitCode, 0) << dumpText.Errors.CStr();
	ASSERT_EQ(dumpJson.ExitCode, 0) << dumpJson.Errors.CStr();
	EXPECT_TRUE(dumpText.Output.StartsWith("mount /Project")) << dumpText.Output.CStr();
	EXPECT_FALSE(dumpText.Output.Contains("mount /Engine")) << dumpText.Output.CStr();
	uint32 textRecords = 0u;
	for (const FStringView line : dumpText.Output.View().Lines())
	{
		textRecords += line.StartsWith("    record ") ? 1u : 0u;
	}
	uint32 jsonRecords = 0u;
	for (const reflect::CDataTree& line : ParseJsonLines(dumpJson.Output))
	{
		jsonRecords += JsonText(line, "kind") == "record" ? 1u : 0u;
	}
	EXPECT_EQ(jsonRecords, textRecords);
	EXPECT_GE(textRecords, 3u);

	// The two machine-readable listings of the rule files name each the same way: a dump's rules line by the folder and
	// level list-rules (Rules.cache) gives, with no file name of a different spelling. Of every mount, /Engine included,
	// whose file is Core/Config/Assets.yml.
	const test::SProcessResult dumpAll = RunOnProject({ "dump", "registry", "--json" });
	ASSERT_EQ(dumpAll.ExitCode, 0) << dumpAll.Errors.CStr();
	std::set<std::pair<FString, FString>> listed;
	for (const reflect::CDataTree& line : ruleLines)
	{
		if (JsonText(line, "kind") == "file")
		{
			listed.emplace(JsonText(line, "folder"), JsonText(line, "level"));
		}
	}
	uint32 dumpedRuleFiles = 0u;
	for (const reflect::CDataTree& line : ParseJsonLines(dumpAll.Output))
	{
		if (JsonText(line, "kind") != "rules")
		{
			continue;
		}
		++dumpedRuleFiles;
		EXPECT_FALSE(line.Find("file").IsValid()) << dumpAll.Output.CStr();
		EXPECT_TRUE(listed.contains({ JsonText(line, "folder"), JsonText(line, "level") }))
			<< JsonText(line, "folder").CStr() << " " << JsonText(line, "level").CStr() << ": " << rules.Output.CStr();
	}
	EXPECT_EQ(dumpedRuleFiles, listed.size()) << dumpAll.Output.CStr();
	EXPECT_TRUE(listed.contains({ "/Engine", "Mount" })) << rules.Output.CStr();
	EXPECT_TRUE(listed.contains({ "/Project/Textures", "Folder" })) << rules.Output.CStr();
	EXPECT_EQ(RunOnProject({ "dump", "registry", "--mount=Nope" }).ExitCode, 2);
	EXPECT_EQ(RunOnProject({ "dump", "asset" }).ExitCode, 2);

	// A malformed folder rule file: RULE001, an error, so status and scan exit with 1.
	WriteText(ProjectDir / "Content" / "Textures" / "Assets.yml", "FormatVersion: 1\nScan:\n  Bogus: 1\n");
	const test::SProcessResult broken = RunOnProject({ "status" });
	EXPECT_EQ(broken.ExitCode, 1) << broken.Output.CStr();
	EXPECT_TRUE(broken.Output.Contains("RULE001")) << broken.Output.CStr();
	EXPECT_EQ(RunOnProject({ "scan", "--ci" }).ExitCode, 1);
}

// The meta verbs end to end through the executable, on the scratch project (their logic is MetaVerbsTest's, in-process):
// validate --ci reports what has no meta and the missing Schemas/Migrations.yml and writes nothing; new-meta creates one
// meta by its path on disk; migrate --all creates the others, skips that one and records itself; validate as the owner is
// then clean of errors; fix restores a lost meta byte for byte from the verified copy the last writer's snapshot holds;
// resave --metas rewrites a hand-edited meta; status names the provider and the metas pending; explain shows the meta.
TEST_F(AssetToolRegistryTest, TheMetaVerbsCreateValidateRestoreAndExplainMetasThroughTheTool)
{
	const std::filesystem::path stone = ProjectDir / FStringView("Content/Textures/Камінь.png.meta").ToPath();
	const std::filesystem::path grass = ProjectDir / FStringView("Content/Textures/Трава.png.meta").ToPath();
	const std::filesystem::path migrations = ProjectDir / "Schemas" / "Migrations.yml";
	const auto read = [] (const std::filesystem::path& Path) { return platform::ReadFileText(Path).value_or(FString()); };
	const auto count = [] (FStringView Output, FStringView Kind, FStringView Code = {})
	{
		uint32 found = 0u;
		for (const reflect::CDataTree& line : ParseJsonLines(Output))
		{
			found += JsonText(line, "kind") == Kind && (Code.IsEmpty() || JsonText(line, "code") == Code) ? 1u : 0u;
		}
		return found;
	};

	const test::SProcessResult report = RunOnProject({ "validate", "--ci", "--json" });
	EXPECT_EQ(report.ExitCode, 1) << report.Output.CStr() << report.Errors.CStr();
	EXPECT_EQ(count(report.Output, "problem", "META001"), 3u) << "two textures and a material: " << report.Output.CStr();
	EXPECT_EQ(count(report.Output, "problem", "MIG001"), 1u) << report.Output.CStr();
	EXPECT_FALSE(std::filesystem::exists(stone));
	EXPECT_FALSE(WroteLocalAssets()) << "a read-only validate writes nothing";

	const test::SProcessResult created = RunOnProject({ "new-meta", "Content/Textures/Камінь.png", "--json" });
	ASSERT_EQ(created.ExitCode, 0) << created.Output.CStr() << created.Errors.CStr();
	ASSERT_EQ(count(created.Output, "meta"), 1u) << created.Output.CStr();
	const FString stoneMeta = read(stone);
	EXPECT_TRUE(stoneMeta.StartsWith("FormatVersion: 1\nGuid: ")) << stoneMeta.CStr();

	const test::SProcessResult migrated = RunOnProject({ "migrate", "--all", "--json" });
	ASSERT_EQ(migrated.ExitCode, 0) << migrated.Output.CStr() << migrated.Errors.CStr();
	EXPECT_EQ(count(migrated.Output, "meta"), 2u) << "the other texture and the material: " << migrated.Output.CStr();
	EXPECT_EQ(read(stone), stoneMeta) << "a meta that exists is skipped";
	EXPECT_EQ(read(migrations), "FormatVersion: 1\nApplied:\n  - metas\n");
	EXPECT_FALSE(std::filesystem::exists(ProjectDir / FStringView("Content/Notes/нотатки.txt.meta").ToPath())) << "an unclaimed file takes none";

	const test::SProcessResult owner = RunOnProject({ "validate", "--json" });
	EXPECT_EQ(owner.ExitCode, 0) << "warnings pass without --ci: " << owner.Output.CStr() << owner.Errors.CStr();
	EXPECT_EQ(count(owner.Output, "problem", "META001") + count(owner.Output, "problem", "MIG001"), 0u) << owner.Output.CStr();

	// Lost, then fixed from the verified copy; hand-edited, then resaved.
	const FString grassMeta = read(grass);
	std::error_code ec;
	std::filesystem::remove(stone, ec);
	WriteText(grass, grassMeta.Replace("FormatVersion: 1\n", "") + "FormatVersion: 1\n");
	const test::SProcessResult fixed = RunOnProject({ "fix", "--json" });
	EXPECT_EQ(fixed.ExitCode, 0) << fixed.Output.CStr() << fixed.Errors.CStr();
	EXPECT_EQ(read(stone), stoneMeta) << "restored byte for byte: " << fixed.Output.CStr();
	EXPECT_EQ(count(fixed.Output, "problem", "META005"), 1u) << "what remains: " << fixed.Output.CStr();
	const test::SProcessResult resaved = RunOnProject({ "resave", "--metas", "--json" });
	EXPECT_EQ(resaved.ExitCode, 0) << resaved.Output.CStr() << resaved.Errors.CStr();
	EXPECT_EQ(read(grass), grassMeta);

	const test::SProcessResult status = RunOnProject({ "status", "--json" });
	ASSERT_EQ(status.ExitCode, 0) << status.Errors.CStr();
	bool bMount = false;
	for (const reflect::CDataTree& line : ParseJsonLines(status.Output))
	{
		if (JsonText(line, "kind") == "mount" && JsonText(line, "mount") == "/Project")
		{
			bMount = JsonText(line, "provider") == "None" && JsonInt(line, "pendingMetas") == 0;
		}
	}
	EXPECT_TRUE(bMount) << "a project outside any repository: " << status.Output.CStr();

	const test::SProcessResult explained = RunOnProject({ "explain", "/Project/Textures/Камінь.png", "--json" });
	ASSERT_EQ(explained.ExitCode, 0) << explained.Errors.CStr();
	const std::vector<reflect::CDataTree> lines = ParseJsonLines(explained.Output);
	ASSERT_EQ(lines.size(), 1u) << explained.Output.CStr();
	EXPECT_EQ(JsonText(lines[0], "meta.file"), "Content/Textures/Камінь.png.meta");
	EXPECT_EQ(JsonText(lines[0], "meta.state"), "Paired");
	EXPECT_TRUE(stoneMeta.Contains("Guid: " + JsonText(lines[0], "meta.guid") + "\n")) << explained.Output.CStr();
}

// scan --engine is the engine's owner: it writes Engine.snapshot under its own engine checkout, never a project's.
TEST_F(AssetToolRegistryTest, ScanWithEngineWritesTheEnginesSnapshot)
{
	const std::filesystem::path engine = Dir / FStringView("Рушій 引擎").ToPath();
	const std::filesystem::path engineTool = MakeScratchEngine(engine);
	ASSERT_FALSE(HasFailure());
	WritePng(engine / "Core" / "Content" / "Textures" / "Default.png", 4u);
	const test::SProcessResult scanned = test::RunProcess(engineTool, std::vector<FString> { "--engine", "scan", "--json" }, Dir);
	ASSERT_EQ(scanned.ExitCode, 0) << scanned.Errors.CStr();
	const std::vector<reflect::CDataTree> lines = ParseJsonLines(scanned.Output);
	ASSERT_EQ(lines.size(), 1u) << scanned.Output.CStr();
	EXPECT_EQ(JsonText(lines[0], "owner"), "Engine");
	EXPECT_EQ(JsonInt(lines[0], "records"), 1);
	EXPECT_TRUE(std::filesystem::exists(assets::GetEngineSnapshotPath(engine)));
	EXPECT_TRUE(std::filesystem::exists(assets::GetProjectIdPath(engine)));
	EXPECT_FALSE(WroteLocalAssets());

	// A read verb with --engine reads /Engine alone.
	const test::SProcessResult dump = test::RunProcess(engineTool, std::vector<FString> { "--engine", "dump", "registry" }, Dir);
	ASSERT_EQ(dump.ExitCode, 0) << dump.Errors.CStr();
	EXPECT_TRUE(dump.Output.Contains("file /Engine/Textures/Default.png Asset")) << dump.Output.CStr();
	EXPECT_FALSE(dump.Output.Contains("mount /Project")) << dump.Output.CStr();
}
