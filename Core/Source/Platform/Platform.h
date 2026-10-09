#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * The operating system's text interfaces, in UTF-8.
 *
 * Win32 speaks UTF-16 (the *W functions) or the ANSI code page (the *A ones). Engine text is
 * UTF-8 (Containers/String.h), so every call here converts at the boundary, through the *W
 * function: nothing goes through the ANSI code page, whatever it is. Text the OS hands back is
 * canonical WTF-8 (FString::FromWide), so even names with unpaired surrogates survive the round
 * trip. Paths stay std::filesystem::path, which is UTF-16 already.
 *
 * Everything here works before the engine's memory pool exists (std::vector, never TArray):
 * hosts call it first thing.
 */
namespace frt::platform
{
// SetConsoleOutputCP/SetConsoleCP(CP_UTF8), so UTF-8 printed with printf/fputs shows as such. The
// console may be the parent cmd.exe or FRT.bat window, which outlives this process: the first call
// saves the code pages it found and registers an std::atexit handler restoring them. Idempotent;
// a no-op without a console. Every host's main calls it first thing (Launcher.cpp, EditorMain.cpp,
// Core-Test's TestMain.cpp); a new host must too.
FRT_CORE_API void InitConsoleUtf8 () noexcept;
// What the atexit handler calls; also for paths that end the process without running it. Puts back
// the pages the first InitConsoleUtf8 saved (a later InitConsoleUtf8 saves them anew).
FRT_CORE_API void RestoreConsoleCodePages () noexcept;

// argv[1..] of this process in UTF-8 (CommandLineToArgvW + FromWide). A std::vector: hosts read it
// before the memory pool exists.
[[nodiscard]] FRT_CORE_API std::vector<FString> GetCommandLineArguments ();
// An argument "-Name", "--Name" or "/Name", the name compared IgnoreCaseAscii (the rules of INI
// keys and config overrides). Name without the dash. The first overload reads this process's
// arguments; the second any list of them.
[[nodiscard]] FRT_CORE_API bool HasCommandLineSwitch (FStringView Name);
[[nodiscard]] FRT_CORE_API bool HasCommandLineSwitch (std::span<const FString> Arguments, FStringView Name);
// The value of the last "-Name=", "--Name=" or "/Name=" argument (later ones override earlier
// ones, like a later config layer), the name compared IgnoreCaseAscii. nullopt when there is none;
// "-Name=" alone is an empty value.
[[nodiscard]] FRT_CORE_API std::optional<FString> FindCommandLineValue (FStringView Name);
[[nodiscard]] FRT_CORE_API std::optional<FString> FindCommandLineValue (std::span<const FString> Arguments, FStringView Name);

// The process environment (GetEnvironmentVariableW). nullopt when the variable is not set; a
// variable set to an empty value is an empty string.
[[nodiscard]] FRT_CORE_API std::optional<FString> GetEnvironmentValue (FStringView Name);
// _wputenv_s: the OS environment block (what child processes inherit and GetEnvironmentVariableW
// reads) and the CRT's copy (getenv) together. An empty Value removes the variable. False for a
// name that is empty or holds '=', or a name or value too long for the CRT (32767 UTF-16 units).
FRT_CORE_API bool SetEnvironmentValue (FStringView Name, FStringView Value);
// A variable holding a path, read and written as UTF-16 through native(), so no text conversion
// can touch it. Get: an empty path when unset. Set: an empty path removes the variable.
[[nodiscard]] FRT_CORE_API std::filesystem::path GetEnvironmentPath (FStringView Name);
FRT_CORE_API bool SetEnvironmentPath (FStringView Name, const std::filesystem::path& Value);

// OutputDebugStringW: shows in the debugger's output whatever the console's code page.
FRT_CORE_API void OutputDebugText (FStringView Text) noexcept;
// MessageBoxW with MB_OK | MB_ICONERROR, no owner window. Blocks until it is dismissed.
FRT_CORE_API void ShowErrorMessage (FStringView Title, FStringView Text) noexcept;
// The system's text for a Win32 error code or an HRESULT (FormatMessageW, in the user's language),
// trailing whitespace and line breaks trimmed. Empty when the system has no text for the code.
[[nodiscard]] FRT_CORE_API FString GetSystemErrorMessage (uint32 ErrorCode);
// The file a loaded module (an HMODULE) was loaded from; nullptr is the executable. Long paths are
// handled (the buffer grows). Empty if the module is not loaded.
[[nodiscard]] FRT_CORE_API std::filesystem::path GetModulePath (void* Module = nullptr);
[[nodiscard]] FRT_CORE_API std::filesystem::path GetExecutablePath ();

// Output of a child tool (a compiler, a build) as UTF-8. Split on '\n' (terminators kept), each line
// that is valid UTF-8 is kept as is; any other line is decoded with the system's OEM code page,
// which is what console tools write by default (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
// LOCALE_IDEFAULTCODEPAGE): GetOEMCP() may report the UTF-8 the process manifest asks for). Call it
// on whole lines (each up to and including its '\n') or on the whole output, never per ReadFile
// chunk: a sequence split across two reads would become two U+FFFD.
[[nodiscard]] FRT_CORE_API FString DecodeToolOutput (FStringView Bytes);

// A whole file as text. Detect: a UTF-8 BOM (EF BB BF) is stripped; FF FE / FE FF mark UTF-16LE /
// UTF-16BE, transcoded to canonical WTF-8 (an odd byte count after the BOM is no UTF-16: nullopt);
// anything else is returned unchanged - bytes are never validated or replaced here. Raw: the bytes
// as stored. nullopt if the file cannot be read (missing, a directory, locked, over 2 GB).
[[nodiscard]] FRT_CORE_API std::optional<FString> ReadFileText (const std::filesystem::path& Path, ETextFileEncoding Encoding = ETextFileEncoding::Detect);
// A whole file as bytes (fonts, binary assets). A std::vector: usable before the memory pool
// exists. nullopt as for ReadFileText.
[[nodiscard]] FRT_CORE_API std::optional<std::vector<uint8>> ReadFileBytes (const std::filesystem::path& Path);
// Replaces the file with Text's bytes: binary, so UTF-8 without a BOM and line endings as given.
// Creates the parent directories first when bCreateDirectories. False if anything failed.
FRT_CORE_API bool WriteFileText (const std::filesystem::path& Path, FStringView Text, bool bCreateDirectories = true);

// Narrow text never becomes a path implicitly: std::filesystem::path(const char*) decodes with the
// ANSI code page. Convert explicitly (FStringView::ToPath).
std::optional<FString> ReadFileText (const char*, ETextFileEncoding = ETextFileEncoding::Detect) = delete;
std::optional<FString> ReadFileText (const std::string&, ETextFileEncoding = ETextFileEncoding::Detect) = delete;
std::optional<FString> ReadFileText (std::string_view, ETextFileEncoding = ETextFileEncoding::Detect) = delete;
std::optional<std::vector<uint8>> ReadFileBytes (const char*) = delete;
std::optional<std::vector<uint8>> ReadFileBytes (const std::string&) = delete;
std::optional<std::vector<uint8>> ReadFileBytes (std::string_view) = delete;
bool WriteFileText (const char*, FStringView, bool = true) = delete;
bool WriteFileText (const std::string&, FStringView, bool = true) = delete;
bool WriteFileText (std::string_view, FStringView, bool = true) = delete;
}
