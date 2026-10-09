#include <gtest/gtest.h>

#include "TestCommon.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Core.h"
#include "Containers/String.h"
#include "Containers/WideString.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::literals;


// Assets/Yaml/YamlString.h declares it, but needs yaml-cpp's headers, which only Core has. The
// function itself is plain: declared here exactly as there.
namespace frt::assets
{
[[nodiscard]] FRT_CORE_API bool IsYamlStorable (FStringView Text) noexcept;
}


namespace
{
// Narrow text never becomes a path: the deleted overloads catch every narrow spelling.
template <typename T>
concept CanReadFileText = requires (const T& Path) { { platform::ReadFileText(Path) }; };

template <typename T>
concept CanReadFileBytes = requires (const T& Path) { { platform::ReadFileBytes(Path) }; };

template <typename T>
concept CanWriteFileText = requires (const T& Path) { { platform::WriteFileText(Path, "text") }; };

static_assert(CanReadFileText<std::filesystem::path>);
static_assert(!CanReadFileText<const char*>);
static_assert(!CanReadFileText<std::string>);
static_assert(!CanReadFileText<std::string_view>);
static_assert(!CanReadFileText<FString>);
static_assert(!CanReadFileText<FStringView>);
static_assert(CanReadFileBytes<std::filesystem::path>);
static_assert(!CanReadFileBytes<const char*>);
static_assert(!CanReadFileBytes<std::string>);
static_assert(!CanReadFileBytes<std::string_view>);
static_assert(CanWriteFileText<std::filesystem::path>);
static_assert(!CanWriteFileText<const char*>);
static_assert(!CanWriteFileText<std::string>);
static_assert(!CanWriteFileText<std::string_view>);


// The file a Windows tool writes as "Unicode": a BOM, then each UTF-16 unit in the byte order asked for.
FString MakeUtf16File (std::wstring_view Units, bool bBigEndian)
{
	FString bytes;
	const auto appendUnit = [&bytes, bBigEndian] (uint32 Unit)
	{
		const char high = static_cast<char>((Unit >> 8u) & 0xFFu);
		const char low = static_cast<char>(Unit & 0xFFu);
		bytes.AppendChar(bBigEndian ? high : low);
		bytes.AppendChar(bBigEndian ? low : high);
	};

	appendUnit(0xFEFFu);
	for (const wchar_t unit : Units)
	{
		appendUnit(static_cast<uint32>(unit));
	}
	return bytes;
}

// The file's text, or a marker no test expects when it cannot be read.
FString ReadText (const std::filesystem::path& Path, ETextFileEncoding Encoding = ETextFileEncoding::Detect)
{
	std::optional<FString> text = platform::ReadFileText(Path, Encoding);
	return text ? std::move(*text) : FString("<unreadable>");
}

bool HasConsole ()
{
	return GetConsoleWindow() != nullptr || GetConsoleOutputCP() != 0u;
}

// PlatformTest.ProcessCommandLine run as a child: the argument it is given, and the file it writes
// that argument to as it arrived.
constexpr FStringLiteral ChildArgumentName = "FRT-Test-Argument";
constexpr FStringLiteral ChildReportName = "FRT-Test-Report";


// Everything touching disk works in a directory with a non-ASCII name (every test exercises the path
// conversions) under a parent of this process's own: test runs of several configurations may share
// the temp directory, and a shared parent could be removed by one run between another's creating it
// and its subdirectory.
class PlatformFileTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT String Тест 漢字 😀 {}", GetCurrentProcessId()).ToPath()
			/ "Platform";
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		// The parent is shared with the string tests' scratch directories (StringConversionTest.cpp).
		std::filesystem::remove(Dir.parent_path(), ec);
	}

	std::filesystem::path Dir;
};
}


// Core/Resources/Utf8CodePage.manifest, merged into every engine executable (Premake/common.lua):
// narrow text the CRT and third-party code decode with the ANSI code page is UTF-8.
TEST(PlatformTest, ProcessCodePageIsUtf8)
{
	EXPECT_EQ(GetACP(), static_cast<UINT>(CP_UTF8));
}


// ---------------- Environment ----------------

TEST(PlatformTest, EnvironmentValueRoundTrip)
{
	constexpr FStringLiteral Name = "FRT_TEST_ВАР";
	ASSERT_TRUE(platform::SetEnvironmentValue(Name, "значення 😀"));

	const std::optional<FString> value = platform::GetEnvironmentValue(Name);
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(*value, "значення 😀");

	// The CRT's copy of the environment was set too.
	wchar_t* crtValue = nullptr;
	size_t crtLength = 0u;
	ASSERT_EQ(_wdupenv_s(&crtValue, &crtLength, ToWide(Name).CStr()), 0);
	ASSERT_NE(crtValue, nullptr);
	EXPECT_EQ(FString::FromWide(crtValue), "значення 😀");
	std::free(crtValue);

	// An empty value removes the variable.
	ASSERT_TRUE(platform::SetEnvironmentValue(Name, ""));
	EXPECT_FALSE(platform::GetEnvironmentValue(Name).has_value());
}

TEST(PlatformTest, EnvironmentUnsetIsNotEmpty)
{
	EXPECT_FALSE(platform::GetEnvironmentValue("FRT_TEST_NEVER_SET_Ж").has_value());

	// Windows keeps variables set to an empty value (the CRT cannot make one: it removes instead).
	const FWideString name(FStringView("FRT_TEST_EMPTY_Ж"));
	ASSERT_TRUE(SetEnvironmentVariableW(name.CStr(), L""));
	const std::optional<FString> empty = platform::GetEnvironmentValue("FRT_TEST_EMPTY_Ж");
	SetEnvironmentVariableW(name.CStr(), nullptr);
	ASSERT_TRUE(empty.has_value());
	EXPECT_TRUE(empty->IsEmpty());
}

TEST(PlatformTest, EnvironmentRejectsUnsettableNames)
{
	EXPECT_FALSE(platform::SetEnvironmentValue("", "x"));
	// "A=B" with value "x" would set A to "B=x".
	EXPECT_FALSE(platform::SetEnvironmentValue("FRT_TEST_A=B", "x"));
	EXPECT_FALSE(platform::GetEnvironmentValue("FRT_TEST_A").has_value());
	EXPECT_FALSE(platform::SetEnvironmentPath("", std::filesystem::path(L"C:\\x")));
}

TEST(PlatformTest, EnvironmentPathRoundTrip)
{
	// Exactly, unpaired surrogate included: the path is never text on the way.
	const std::filesystem::path path(L"C:\\Тест\\x\xD800" L"y");
	ASSERT_TRUE(platform::SetEnvironmentPath("FRT_TEST_PATH_Ш", path));
	EXPECT_EQ(platform::GetEnvironmentPath("FRT_TEST_PATH_Ш").native(), path.native());

	// An empty path removes the variable.
	ASSERT_TRUE(platform::SetEnvironmentPath("FRT_TEST_PATH_Ш", std::filesystem::path()));
	EXPECT_TRUE(platform::GetEnvironmentPath("FRT_TEST_PATH_Ш").empty());
	EXPECT_FALSE(platform::GetEnvironmentValue("FRT_TEST_PATH_Ш").has_value());
}


// ---------------- Command line ----------------

TEST(PlatformTest, CommandLineValues)
{
	const std::vector<FString> arguments =
	{
		"-Project=A", "--project=B", "/PROJECT=C", "-Define=X=1", "-Empty=", "-ПРОЕКТ=D", "Project=E", "-Projects=F",
	};
	const std::span<const FString> all(arguments);

	// Every prefix, the name IgnoreCaseAscii; the last one wins.
	EXPECT_EQ(platform::FindCommandLineValue(all.first(1u), "Project").value_or("<none>"), "A");
	EXPECT_EQ(platform::FindCommandLineValue(all.subspan(1u, 1u), "Project").value_or("<none>"), "B");
	EXPECT_EQ(platform::FindCommandLineValue(all, "Project").value_or("<none>"), "C");
	EXPECT_EQ(platform::FindCommandLineValue(all, "PROJECT").value_or("<none>"), "C");

	// The first '=' ends the name.
	EXPECT_EQ(platform::FindCommandLineValue(all, "Define").value_or("<none>"), "X=1");
	EXPECT_FALSE(platform::FindCommandLineValue(all, "Define=X").has_value());

	const std::optional<FString> empty = platform::FindCommandLineValue(all, "Empty");
	ASSERT_TRUE(empty.has_value());
	EXPECT_TRUE(empty->IsEmpty());

	// Non-ASCII names match exactly: ASCII case rules, as for INI keys.
	EXPECT_EQ(platform::FindCommandLineValue(all, "ПРОЕКТ").value_or("<none>"), "D");
	EXPECT_FALSE(platform::FindCommandLineValue(all, "проект").has_value());

	EXPECT_FALSE(platform::FindCommandLineValue(all, "Missing").has_value());
}

TEST(PlatformTest, CommandLineSwitches)
{
	const std::vector<FString> arguments = { "-Verbose", "--fast", "/Q", "Plain", "-Name=Value", "-Ж" };

	EXPECT_TRUE(platform::HasCommandLineSwitch(arguments, "verbose"));
	EXPECT_TRUE(platform::HasCommandLineSwitch(arguments, "FAST"));
	EXPECT_TRUE(platform::HasCommandLineSwitch(arguments, "q"));
	EXPECT_TRUE(platform::HasCommandLineSwitch(arguments, "Ж"));
	EXPECT_FALSE(platform::HasCommandLineSwitch(arguments, "ж"));
	EXPECT_FALSE(platform::HasCommandLineSwitch(arguments, "Plain"));   // no prefix: not a switch
	EXPECT_FALSE(platform::HasCommandLineSwitch(arguments, "Name"));    // a value, not a switch
	EXPECT_FALSE(platform::HasCommandLineSwitch(arguments, "Verb"));
}

TEST(PlatformTest, ProcessCommandLine)
{
	// main() received the same command line, parsed by the CRT into UTF-8 (the process code page). Its
	// copy, not __argv: GoogleTest has taken its flags out of that by now.
	const std::vector<FString> arguments = platform::GetCommandLineArguments();
	const std::vector<FString>& mainArguments = test::GetMainArguments();
	ASSERT_EQ(arguments.size(), mainArguments.size());
	for (size_t i = 0u; i < arguments.size(); ++i)
	{
		EXPECT_EQ(arguments[i], mainArguments[i]);
	}

	// Started by PlatformFileTest.ProcessCommandLineNonAscii: tell it what arrived.
	const std::optional<FString> report = platform::FindCommandLineValue(ChildReportName);
	if (report.has_value())
	{
		const std::optional<FString> argument = platform::FindCommandLineValue(ChildArgumentName);
		ASSERT_TRUE(argument.has_value());
		ASSERT_TRUE(platform::WriteFileText(report->ToPath(), *argument));
	}

	EXPECT_FALSE(platform::HasCommandLineSwitch("FRT-No-Such-Switch-Ж"));
	EXPECT_FALSE(platform::FindCommandLineValue("FRT-No-Such-Value-Ж").has_value());
}

// The comparison above on a command line no test run has by default: this executable again, started
// with Cyrillic, CJK and an emoji in its arguments, running only that test.
TEST_F(PlatformFileTest, ProcessCommandLineNonAscii)
{
	constexpr FStringLiteral Argument = "Тест 漢字 😀";
	const std::filesystem::path report = Dir / FStringView("звіт.txt").ToPath();
	FWideString commandLine(FString::Format("\"{}\" --gtest_filter=PlatformTest.ProcessCommandLine \"-{}={}\" \"-{}={}\"",
		FString::FromPath(platform::GetExecutablePath()), ChildArgumentName, Argument, ChildReportName, FString::FromPath(report)).View());

	// A console of its own that is never shown: the child's output must not mix into this run's.
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	ASSERT_TRUE(CreateProcessW(nullptr, commandLine.GetData(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
		&startup, &process)) << platform::GetSystemErrorMessage(GetLastError()).CStr();

	const DWORD waited = WaitForSingleObject(process.hProcess, 60'000u);
	if (waited != WAIT_OBJECT_0)
	{
		TerminateProcess(process.hProcess, 1u);
	}
	DWORD exitCode = 1u;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);

	ASSERT_EQ(waited, WAIT_OBJECT_0);
	EXPECT_EQ(exitCode, 0u);
	EXPECT_EQ(ReadText(report), Argument);
}


// ---------------- Files ----------------

TEST_F(PlatformFileTest, WriteAndReadRoundTrip)
{
	// Bytes, never validated: ill-formed UTF-8 and NULs come back as written.
	const FStringView text = "Привіт 漢字 😀\r\n" "\xFF\x80" " \0end"_sv;
	const std::filesystem::path file = Dir / FStringView("Підтека 子目录").ToPath() / FStringView("файл 😀.txt").ToPath();
	ASSERT_TRUE(platform::WriteFileText(file, text));   // creates the directory

	EXPECT_EQ(ReadText(file), text);
	EXPECT_EQ(ReadText(file, ETextFileEncoding::Raw), text);

	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(file);
	ASSERT_TRUE(bytes.has_value());
	EXPECT_EQ(FStringView(reinterpret_cast<const char*>(bytes->data()), bytes->size()), text);

	// Writing again replaces the file, never appends.
	ASSERT_TRUE(platform::WriteFileText(file, "short"));
	EXPECT_EQ(ReadText(file), "short");
}

TEST_F(PlatformFileTest, EmptyFile)
{
	const std::filesystem::path file = Dir / "empty.txt";
	ASSERT_TRUE(platform::WriteFileText(file, ""));

	const std::optional<FString> text = platform::ReadFileText(file);
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->IsEmpty());

	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(file);
	ASSERT_TRUE(bytes.has_value());
	EXPECT_TRUE(bytes->empty());
}

TEST_F(PlatformFileTest, UnreadableFiles)
{
	EXPECT_FALSE(platform::ReadFileText(Dir / "missing.txt").has_value());
	EXPECT_FALSE(platform::ReadFileBytes(Dir / "missing.bin").has_value());
	EXPECT_FALSE(platform::ReadFileText(Dir).has_value());   // a directory is no file

	// Without bCreateDirectories a missing directory is a failure, not created.
	const std::filesystem::path file = Dir / "Missing" / "a.txt";
	EXPECT_FALSE(platform::WriteFileText(file, "x", false));
	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(file.parent_path(), ec));
}

TEST_F(PlatformFileTest, Utf8BomStrippedByDetectKeptByRaw)
{
	const std::filesystem::path file = Dir / "bom.txt";
	ASSERT_TRUE(platform::WriteFileText(file, "\xEF\xBB\xBF" "ключ=значення\n"));

	EXPECT_EQ(ReadText(file), "ключ=значення\n");
	EXPECT_EQ(ReadText(file, ETextFileEncoding::Raw), "\xEF\xBB\xBF" "ключ=значення\n");
}

TEST_F(PlatformFileTest, TextWithoutBomUnchanged)
{
	const std::filesystem::path file = Dir / "plain.txt";
	// A lone FF or FE is no BOM; neither is a BOM-like sequence later in the file.
	for (const FStringView text : { FStringView("ключ 漢字\n"), FStringView("\xFF"), FStringView("\xFE"),
		FStringView("a\xFF\xFE" "b"), FStringView("a\xEF\xBB\xBF" "b") })
	{
		ASSERT_TRUE(platform::WriteFileText(file, text));
		EXPECT_EQ(ReadText(file), text);
	}
}

TEST_F(PlatformFileTest, Utf16FilesAreTranscoded)
{
	// An unpaired surrogate becomes its WTF-8 form, a pair one 4-byte sequence.
	const std::wstring_view units = L"Тест \xD800" L"x 😀\r\n";
	const FStringView expected = "Тест \xED\xA0\x80" "x 😀\r\n";

	const std::filesystem::path littleEndian = Dir / "utf16le.txt";
	const FString littleEndianBytes = MakeUtf16File(units, false);
	ASSERT_TRUE(platform::WriteFileText(littleEndian, littleEndianBytes));
	EXPECT_EQ(ReadText(littleEndian), expected);
	EXPECT_EQ(ReadText(littleEndian, ETextFileEncoding::Raw), littleEndianBytes);

	const std::filesystem::path bigEndian = Dir / "utf16be.txt";
	ASSERT_TRUE(platform::WriteFileText(bigEndian, MakeUtf16File(units, true)));
	EXPECT_EQ(ReadText(bigEndian), expected);

	// A BOM alone is an empty text.
	ASSERT_TRUE(platform::WriteFileText(bigEndian, MakeUtf16File(L"", true)));
	const std::optional<FString> empty = platform::ReadFileText(bigEndian);
	ASSERT_TRUE(empty.has_value());
	EXPECT_TRUE(empty->IsEmpty());
}

TEST_F(PlatformFileTest, OddLengthUtf16IsNoText)
{
	const std::filesystem::path file = Dir / "odd.txt";
	const FStringView bytes = "\xFF\xFE" "A\0B"_sv;
	ASSERT_TRUE(platform::WriteFileText(file, bytes));

	EXPECT_FALSE(platform::ReadFileText(file).has_value());
	EXPECT_EQ(ReadText(file, ETextFileEncoding::Raw), bytes);
}


// ---------------- Tool output ----------------

TEST(PlatformTest, DecodeToolOutput)
{
	DWORD oemCodePage = 0u;
	GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
		reinterpret_cast<LPWSTR>(&oemCodePage), static_cast<int>(sizeof(oemCodePage) / sizeof(wchar_t)));
	if (oemCodePage == 0u || oemCodePage == CP_UTF8)
	{
		GTEST_SKIP() << "the system OEM code page is UTF-8: every line is UTF-8";
	}

	// A word the OEM code page encodes, as bytes that are not UTF-8 by accident: Cyrillic for 866
	// and its neighbours, Latin letters with diacritics for 437, 850 and most others.
	FString word;
	FString oemBytes;
	for (const FStringView candidate : { FStringView("Тест"), FStringView("Ünïcödé"), FStringView("café") })
	{
		const FWideString wide(candidate);
		const int length = WideCharToMultiByte(oemCodePage, 0u, wide.CStr(), static_cast<int>(wide.Len()), nullptr, 0, nullptr, nullptr);
		if (length <= 0)
		{
			continue;
		}
		FString bytes;
		bytes.ResizeUninitialized(static_cast<uint32>(length));
		BOOL bUsedDefault = FALSE;
		WideCharToMultiByte(oemCodePage, 0u, wide.CStr(), static_cast<int>(wide.Len()), bytes.GetData(), length, nullptr, &bUsedDefault);
		if (!bUsedDefault && !bytes.IsValidUtf8())
		{
			word = FString(candidate);
			oemBytes = std::move(bytes);
			break;
		}
	}
	if (word.IsEmpty())
	{
		GTEST_SKIP() << "no test word is encodable in OEM code page " << oemCodePage;
	}

	const FString utf8Line = "ok ✓ 漢字\n";
	const FString oemLine = oemBytes + "\r\n";
	EXPECT_EQ(platform::DecodeToolOutput(utf8Line), utf8Line);
	EXPECT_EQ(platform::DecodeToolOutput(oemLine), word + "\r\n");

	// Line by line; the last one without a terminator.
	const FString mixed = utf8Line + oemLine + utf8Line + oemBytes;
	EXPECT_EQ(platform::DecodeToolOutput(mixed), utf8Line + word + "\r\n" + utf8Line + word);
	EXPECT_TRUE(platform::DecodeToolOutput("").IsEmpty());
}


// ---------------- Console, system text, modules ----------------

TEST(PlatformTest, ConsoleCodePagesRestored)
{
	if (!HasConsole())
	{
		GTEST_SKIP() << "no console";
	}

	// Back to the pages main() found, then a known starting point that is not UTF-8, so the restore
	// shows even on a console that already uses UTF-8.
	platform::RestoreConsoleCodePages();
	const UINT originalOutput = GetConsoleOutputCP();
	const UINT originalInput = GetConsoleCP();
	if (!SetConsoleOutputCP(437u) || !SetConsoleCP(437u))
	{
		platform::InitConsoleUtf8();
		GTEST_SKIP() << "the console's code page cannot be changed";
	}

	platform::InitConsoleUtf8();
	platform::InitConsoleUtf8();   // must not save the UTF-8 the first call set
	EXPECT_EQ(GetConsoleOutputCP(), static_cast<UINT>(CP_UTF8));
	EXPECT_EQ(GetConsoleCP(), static_cast<UINT>(CP_UTF8));

	platform::RestoreConsoleCodePages();
	EXPECT_EQ(GetConsoleOutputCP(), 437u);
	EXPECT_EQ(GetConsoleCP(), 437u);

	// Leave the console as main() set it up: its own pages saved, UTF-8 active.
	SetConsoleOutputCP(originalOutput);
	SetConsoleCP(originalInput);
	platform::InitConsoleUtf8();
}

TEST(PlatformTest, SystemErrorMessage)
{
	const FString message = platform::GetSystemErrorMessage(ERROR_FILE_NOT_FOUND);
	EXPECT_FALSE(message.IsEmpty());
	EXPECT_TRUE(message.IsValidUtf8());
	EXPECT_EQ(message.View().TrimEnd(), message.View());   // no trailing "\r\n"

	// The same error as an HRESULT.
	EXPECT_FALSE(platform::GetSystemErrorMessage(static_cast<uint32>(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))).IsEmpty());
}

TEST(PlatformTest, ModulePaths)
{
	const std::filesystem::path executable = platform::GetExecutablePath();
	ASSERT_FALSE(executable.empty());
	EXPECT_TRUE(executable.is_absolute());
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::is_regular_file(executable, ec));
	EXPECT_TRUE(FString::FromPath(executable.filename()).StartsWith<ESearchCase::IgnoreCaseAscii>("Core-Test"));
	EXPECT_EQ(platform::GetModulePath(), executable);

	const std::filesystem::path kernel = platform::GetModulePath(GetModuleHandleW(L"kernel32.dll"));
	EXPECT_TRUE(FString::FromPath(kernel.filename()).Equals<ESearchCase::IgnoreCaseAscii>("kernel32.dll"));
}


// ---------------- YAML ----------------

TEST(PlatformTest, YamlStorableText)
{
	EXPECT_TRUE(assets::IsYamlStorable(""));
	EXPECT_TRUE(assets::IsYamlStorable("Матеріал 漢字 😀 \t\x01 \"quoted\""));
	EXPECT_TRUE(assets::IsYamlStorable("\xEF\xBF\xBD"));               // U+FFFD itself is text

	EXPECT_FALSE(assets::IsYamlStorable("bad \xFF byte"));
	EXPECT_FALSE(assets::IsYamlStorable("cut \xE2\x82"));
	EXPECT_FALSE(assets::IsYamlStorable("overlong \xC0\xAF"));
	EXPECT_FALSE(assets::IsYamlStorable("surrogate \xED\xA0\x80"));    // WTF-8: a path with an unpaired surrogate
	EXPECT_FALSE(assets::IsYamlStorable("\xEF\xB7\x90"));              // U+FDD0
	EXPECT_FALSE(assets::IsYamlStorable("\xEF\xBF\xBE"));              // U+FFFE
	EXPECT_FALSE(assets::IsYamlStorable("\xF0\x9F\xBF\xBF"));          // U+1FFFF: xxFFFF in every plane
}
