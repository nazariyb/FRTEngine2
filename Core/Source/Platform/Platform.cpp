#include "Platform/Platform.h"

#include <cstdlib>
#include <system_error>

#include <Windows.h>
#include <shellapi.h>

#include "Containers/WideString.h"


namespace frt::platform
{
namespace
{
// Longest environment variable name or value the CRT accepts (_MAX_ENV, UTF-16 units). Longer ones
// reach its invalid parameter handler, which ends the process.
constexpr uint32 MaxEnvironmentUnits = 32767u;

// Windows paths are at most 32767 UTF-16 units, whatever the long-path setting.
constexpr size_t MaxPathUnits = 32768u;

// Longest file the read functions return: the most an FString holds.
constexpr int64 MaxFileBytes = FString::MaxLength;


// ---------------- Console ----------------

struct SConsoleState
{
	UINT OutputCodePage = 0u;
	UINT InputCodePage = 0u;
	bool bSaved = false;
	bool bAtExitRegistered = false;
};

// A lock without a constructor or destructor: InitConsoleUtf8 may run from a static initializer,
// RestoreConsoleCodePages from the atexit handler, after other statics are gone.
SRWLOCK ConsoleLock = SRWLOCK_INIT;
SConsoleState ConsoleState;

// A process started from Explorer or the IDE without a console has nothing to set: the code page
// queries return 0 there.
bool HasConsole () noexcept
{
	return GetConsoleWindow() != nullptr || GetConsoleOutputCP() != 0u;
}

void __cdecl RestoreConsoleCodePagesAtExit ()
{
	RestoreConsoleCodePages();
}


// ---------------- Command line ----------------

// "--Name", "-Name" or "/Name" -> "Name"; anything else is not a switch.
bool StripSwitchPrefix (FStringView Argument, FStringView& OutBody) noexcept
{
	if (Argument.StartsWith("--"))
	{
		OutBody = Argument.RightChop(2u);
		return true;
	}
	if (Argument.StartsWith('-') || Argument.StartsWith('/'))
	{
		OutBody = Argument.RightChop(1u);
		return true;
	}
	return false;
}


// ---------------- Environment ----------------

// What _wputenv_s accepts without reaching the CRT's invalid parameter handler, and without
// misreading the name: "A=B" with value "C" would set A to "B=C".
bool IsSettableEnvironmentName (FStringView Name) noexcept
{
	return !Name.IsEmpty() && Name.Find('=') == FStringView::NotFound && Name.Len() < MaxEnvironmentUnits;
}

// The variable's value, terminator excluded. False when it is not set (or cannot be read).
bool ReadEnvironmentVariable (const wchar_t* Name, std::vector<wchar_t>& OutValue)
{
	OutValue.resize(256u);
	while (true)
	{
		SetLastError(ERROR_SUCCESS);
		const DWORD length = GetEnvironmentVariableW(Name, OutValue.data(), static_cast<DWORD>(OutValue.size()));
		if (length == 0u)
		{
			OutValue.clear();
			// 0 is also the length of a variable set to an empty value: only the error code tells
			// "unset" (ERROR_ENVVAR_NOT_FOUND) apart.
			return GetLastError() == ERROR_SUCCESS;
		}
		if (length < OutValue.size())
		{
			OutValue.resize(length);
			return true;
		}
		// Too small: length is the size needed, terminator included. Loop, since another thread may
		// grow the value before the next call.
		OutValue.resize(length);
	}
}


// ---------------- Files ----------------

void SetBufferLength (FString& Buffer, uint32 Length) noexcept
{
	// Not Resize: its shrinking form asserts a code-point boundary, and these are raw bytes.
	Buffer.ResizeUninitialized(Length);
}

void SetBufferLength (std::vector<uint8>& Buffer, uint32 Length)
{
	Buffer.resize(Length);
}

void* GetWritableData (FString& Buffer) noexcept
{
	return Buffer.GetData();
}

void* GetWritableData (std::vector<uint8>& Buffer) noexcept
{
	return Buffer.data();
}

// The whole file into Out. Shares read, write and delete access as the C++ streams do, so a file
// another process holds open for writing (an editor saving it) can still be read.
template <typename TBuffer>
bool ReadWholeFile (const std::filesystem::path& Path, TBuffer& Out)
{
	const HANDLE file = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	LARGE_INTEGER size = {};
	bool bRead = GetFileSizeEx(file, &size) != 0 && size.QuadPart <= MaxFileBytes;
	if (bRead)
	{
		const uint32 total = static_cast<uint32>(size.QuadPart);
		SetBufferLength(Out, total);

		uint32 done = 0u;
		while (done < total)
		{
			DWORD read = 0u;
			if (!ReadFile(file, static_cast<char*>(GetWritableData(Out)) + done, total - done, &read, nullptr))
			{
				bRead = false;
				break;
			}
			if (read == 0u)
			{
				break;   // the file shrank since its size was taken: what is there is the file
			}
			done += read;
		}
		SetBufferLength(Out, done);
	}

	CloseHandle(file);
	return bRead;
}

// The bytes after a UTF-16 BOM, as canonical WTF-8. nullopt for an odd byte count: not UTF-16 at
// all, or cut off.
std::optional<FString> DecodeUtf16File (FStringView Bytes, bool bBigEndian)
{
	if (Bytes.Len() % 2u != 0u)
	{
		return std::nullopt;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(Bytes.GetData());
	std::vector<wchar_t> units(Bytes.Len() / 2u);
	for (size_t i = 0u; i < units.size(); ++i)
	{
		const uint32 first = bytes[2u * i];
		const uint32 second = bytes[2u * i + 1u];
		units[i] = static_cast<wchar_t>(bBigEndian ? (first << 8u) | second : (second << 8u) | first);
	}
	return FString::FromWide(std::wstring_view(units.data(), units.size()));
}


// ---------------- System text ----------------

// The system's OEM code page: what console tools write when nobody told them otherwise. Asked of
// the locale, not GetOEMCP(), which may report the UTF-8 the process manifest asks for.
UINT GetSystemOemCodePage () noexcept
{
	DWORD codePage = 0u;
	const int written = GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
		reinterpret_cast<LPWSTR>(&codePage), static_cast<int>(sizeof(codePage) / sizeof(wchar_t)));
	return written > 0 && codePage != 0u ? codePage : CP_OEMCP;
}

FString FormatSystemMessage (uint32 ErrorCode)
{
	wchar_t* buffer = nullptr;
	// Language 0: neutral, then the thread's, the user's and the system's language, then English.
	const DWORD length = FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, ErrorCode, 0u, reinterpret_cast<LPWSTR>(&buffer), 0u, nullptr);
	if (length == 0u || buffer == nullptr)
	{
		return FString();
	}

	FString message = FString::FromWide(buffer, length);
	LocalFree(buffer);
	message.TrimEndInline();   // messages end in "\r\n"
	return message;
}
}


// ---------------- Console ----------------

void InitConsoleUtf8 () noexcept
{
	if (!HasConsole())
	{
		return;
	}

	AcquireSRWLockExclusive(&ConsoleLock);
	// Only the first call saves: a second one would save the UTF-8 the first one set.
	if (!ConsoleState.bSaved)
	{
		ConsoleState.OutputCodePage = GetConsoleOutputCP();
		ConsoleState.InputCodePage = GetConsoleCP();
		ConsoleState.bSaved = true;
		if (!ConsoleState.bAtExitRegistered)
		{
			ConsoleState.bAtExitRegistered = std::atexit(&RestoreConsoleCodePagesAtExit) == 0;
		}
	}
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	ReleaseSRWLockExclusive(&ConsoleLock);
}

void RestoreConsoleCodePages () noexcept
{
	AcquireSRWLockExclusive(&ConsoleLock);
	if (ConsoleState.bSaved)
	{
		// 0 is what the queries returned on failure: nothing worth putting back.
		if (ConsoleState.OutputCodePage != 0u)
		{
			SetConsoleOutputCP(ConsoleState.OutputCodePage);
		}
		if (ConsoleState.InputCodePage != 0u)
		{
			SetConsoleCP(ConsoleState.InputCodePage);
		}
		ConsoleState.bSaved = false;
	}
	ReleaseSRWLockExclusive(&ConsoleLock);
}


// ---------------- Command line ----------------

std::vector<FString> GetCommandLineArguments ()
{
	std::vector<FString> arguments;
	int count = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
	if (argv == nullptr)
	{
		return arguments;
	}

	arguments.reserve(count > 1 ? static_cast<size_t>(count - 1) : 0u);
	for (int i = 1; i < count; ++i)
	{
		arguments.push_back(FString::FromWide(argv[i]));
	}
	LocalFree(argv);
	return arguments;
}

bool HasCommandLineSwitch (FStringView Name)
{
	const std::vector<FString> arguments = GetCommandLineArguments();
	return HasCommandLineSwitch(arguments, Name);
}

bool HasCommandLineSwitch (std::span<const FString> Arguments, FStringView Name)
{
	frt_assert(!Name.IsEmpty());
	for (const FString& argument : Arguments)
	{
		FStringView body;
		if (StripSwitchPrefix(argument.View(), body) && body.Equals<ESearchCase::IgnoreCaseAscii>(Name))
		{
			return true;
		}
	}
	return false;
}

std::optional<FString> FindCommandLineValue (FStringView Name)
{
	const std::vector<FString> arguments = GetCommandLineArguments();
	return FindCommandLineValue(arguments, Name);
}

std::optional<FString> FindCommandLineValue (std::span<const FString> Arguments, FStringView Name)
{
	frt_assert(!Name.IsEmpty());
	std::optional<FString> value;
	for (const FString& argument : Arguments)
	{
		FStringView body;
		if (!StripSwitchPrefix(argument.View(), body))
		{
			continue;
		}
		// The first '=' ends the name: a value may hold more of them (-Define=A=1).
		const SStringSplit nameAndValue = body.SplitFirst('=');
		if (nameAndValue && nameAndValue.Left.Equals<ESearchCase::IgnoreCaseAscii>(Name))
		{
			value = FString(nameAndValue.Right);
		}
	}
	return value;
}


// ---------------- Environment ----------------

std::optional<FString> GetEnvironmentValue (FStringView Name)
{
	std::vector<wchar_t> value;
	if (!ReadEnvironmentVariable(ToWide(Name).CStr(), value))
	{
		return std::nullopt;
	}
	return FString::FromWide(std::wstring_view(value.data(), value.size()));
}

bool SetEnvironmentValue (FStringView Name, FStringView Value)
{
	if (!IsSettableEnvironmentName(Name))
	{
		return false;
	}
	const FWideString value(Value);
	if (value.Len() >= MaxEnvironmentUnits)
	{
		return false;
	}
	return _wputenv_s(ToWide(Name).CStr(), value.CStr()) == 0;
}

std::filesystem::path GetEnvironmentPath (FStringView Name)
{
	std::vector<wchar_t> value;
	if (!ReadEnvironmentVariable(ToWide(Name).CStr(), value))
	{
		return {};
	}
	return std::filesystem::path(std::wstring_view(value.data(), value.size()));
}

bool SetEnvironmentPath (FStringView Name, const std::filesystem::path& Value)
{
	if (!IsSettableEnvironmentName(Name) || Value.native().size() >= MaxEnvironmentUnits)
	{
		return false;
	}
	return _wputenv_s(ToWide(Name).CStr(), Value.c_str()) == 0;
}


// ---------------- Diagnostics ----------------

void OutputDebugText (FStringView Text) noexcept
{
	OutputDebugStringW(ToWide(Text).CStr());
}

void ShowErrorMessage (FStringView Title, FStringView Text) noexcept
{
	MessageBoxW(nullptr, ToWide(Text).CStr(), ToWide(Title).CStr(), MB_OK | MB_ICONERROR);
}

FString GetSystemErrorMessage (uint32 ErrorCode)
{
	FString message = FormatSystemMessage(ErrorCode);

	// An HRESULT wrapping a Win32 error (HRESULT_FROM_WIN32: severity bit, FACILITY_WIN32) is not
	// always in the message table under its own value; its low 16 bits are the Win32 code.
	const bool bWin32Result = (ErrorCode & 0x80000000u) != 0u && ((ErrorCode >> 16u) & 0x1FFFu) == FACILITY_WIN32;
	if (message.IsEmpty() && bWin32Result)
	{
		message = FormatSystemMessage(ErrorCode & 0xFFFFu);
	}
	return message;
}

std::filesystem::path GetModulePath (void* Module)
{
	std::vector<wchar_t> buffer(MAX_PATH);
	while (true)
	{
		const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(Module), buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0u)
		{
			return {};
		}
		if (length < buffer.size())
		{
			return std::filesystem::path(std::wstring_view(buffer.data(), length));
		}
		// Truncated: a long path. Retry with room for it.
		if (buffer.size() >= MaxPathUnits)
		{
			return {};
		}
		buffer.resize(buffer.size() * 2u);
	}
}

std::filesystem::path GetExecutablePath ()
{
	return GetModulePath(nullptr);
}

FString DecodeToolOutput (FStringView Bytes)
{
	FString result;
	result.Reserve(Bytes.Len());

	UINT codePage = 0u;   // looked up at the first line that needs it
	std::vector<wchar_t> units;
	FStringView rest = Bytes;
	while (!rest.IsEmpty())
	{
		const uint32 newLine = rest.Find('\n');
		const uint32 lineLength = newLine == FStringView::NotFound ? rest.Len() : newLine + 1u;
		const FStringView line = rest.Left(lineLength);
		rest.RemovePrefix(lineLength);

		if (line.IsValidUtf8())
		{
			result.Append(line);
			continue;
		}

		if (codePage == 0u)
		{
			codePage = GetSystemOemCodePage();
		}
		const int length = MultiByteToWideChar(codePage, 0u, line.GetData(), static_cast<int>(line.Len()), nullptr, 0);
		if (length <= 0)
		{
			result.Append(FString::FromUtf8Lossy(line));   // the code page is unusable: still show something
			continue;
		}
		units.resize(static_cast<size_t>(length));
		MultiByteToWideChar(codePage, 0u, line.GetData(), static_cast<int>(line.Len()), units.data(), length);
		result.AppendWide(std::wstring_view(units.data(), units.size()));
	}
	return result;
}


// ---------------- Files ----------------

std::optional<FString> ReadFileText (const std::filesystem::path& Path, ETextFileEncoding Encoding)
{
	FString text;
	if (!ReadWholeFile(Path, text))
	{
		return std::nullopt;
	}
	if (Encoding == ETextFileEncoding::Raw)
	{
		return text;
	}

	if (text.RemoveFromStart(Utf8Bom))
	{
		return text;
	}
	const FStringView bytes = text.View();
	if (bytes.StartsWith("\xFF\xFE"))
	{
		return DecodeUtf16File(bytes.RightChop(2u), false);
	}
	if (bytes.StartsWith("\xFE\xFF"))
	{
		return DecodeUtf16File(bytes.RightChop(2u), true);
	}
	return text;
}

std::optional<std::vector<uint8>> ReadFileBytes (const std::filesystem::path& Path)
{
	std::vector<uint8> bytes;
	if (!ReadWholeFile(Path, bytes))
	{
		return std::nullopt;
	}
	return bytes;
}

bool WriteFileText (const std::filesystem::path& Path, FStringView Text, bool bCreateDirectories)
{
	if (bCreateDirectories && Path.has_parent_path())
	{
		// A failure shows as CreateFileW failing below.
		std::error_code ec;
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	const HANDLE file = CreateFileW(Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	bool bWritten = true;
	const char* data = Text.GetData();
	uint32 remaining = Text.Len();
	while (remaining > 0u)
	{
		DWORD written = 0u;
		if (!WriteFile(file, data, remaining, &written, nullptr) || written == 0u)
		{
			bWritten = false;
			break;
		}
		data += written;
		remaining -= written;
	}

	// Closing can still fail (a full disk, a network share): that is a failed write too.
	const bool bClosed = CloseHandle(file) != 0;
	return bWritten && bClosed;
}
}
