#pragma once

#include <format>
#include <optional>
#include <utility>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


/**
 * The engine's log: one line per message, "[category] message", on stderr and in the debugger's
 * output (platform::OutputDebugText) - the convention every system already prints by hand with
 * std::fprintf(stderr, "[texture] ..."), now behind one function that a level can silence.
 *
 * Not a logging framework (spec §15): no sinks to register, no files, no formatting options, no
 * macros that compile messages out. A category is a static name (FStringLiteral: "assets",
 * "platform"); categories compare by text, so a game module's "assets" is the engine's, and nothing
 * here keeps a pointer into a module that may unload. Each category has a minimum level, settable
 * at run time; categories without their own use the default minimum (Info at start).
 *
 * Thread-safe: any thread may log. One mutex is held while a line is written, so lines never
 * interleave; levels are read under it as well, after a lock-free check that rejects everything more
 * verbose than any minimum (Verbose, by default). The log allocates nothing for its own state, and
 * works before the engine starts and after it stops (static initializers, atexit handlers).
 */
namespace frt::log
{
// Ordered from most to least severe: a minimum level of Warning lets Error and Warning through.
enum class ELevel : uint8
{
	Error,
	Warning,
	Info,
	Verbose,
};

// The categories the engine itself uses. Any other literal works the same way.
inline constexpr FStringLiteral Assets = "assets";
inline constexpr FStringLiteral Platform = "platform";
inline constexpr FStringLiteral Memory = "memory";
inline constexpr FStringLiteral Game = "game";

// Category names longer than this are cut for level lookups (a message still prints in full).
inline constexpr uint32 MaxCategoryLength = 31u;
// Categories with a level of their own at the same time; SetMinLevel refuses one more.
inline constexpr uint32 MaxCategoryLevels = 64u;

// "error", "warning", "info", "verbose".
[[nodiscard]] FRT_CORE_API FStringLiteral GetLevelName (ELevel Level) noexcept;
// A level by its name, IgnoreCaseAscii (config values, command lines); nullopt for anything else.
[[nodiscard]] FRT_CORE_API std::optional<ELevel> ParseLevel (FStringView Name) noexcept;

// Messages of Category less severe than Level are dropped. False when MaxCategoryLevels other
// categories already have a level of their own.
FRT_CORE_API bool SetMinLevel (FStringView Category, ELevel Level) noexcept;
// The category goes back to the default minimum.
FRT_CORE_API void ResetMinLevel (FStringView Category) noexcept;
// The minimum of every category without a level of its own.
FRT_CORE_API void SetDefaultMinLevel (ELevel Level) noexcept;
// Every category back to the default minimum, and that back to Info (tests).
FRT_CORE_API void ResetAllMinLevels () noexcept;
[[nodiscard]] FRT_CORE_API ELevel GetMinLevel (FStringView Category) noexcept;
// Whether a message of Category at Level would be written: check it before building an expensive
// message (the helpers below do).
[[nodiscard]] FRT_CORE_API bool IsEnabled (FStringView Category, ELevel Level) noexcept;

// Writes "[Category] Message\n" to stderr and the debugger, unless the level filters it out. The
// message is one line by convention; text after a line break prints as is.
FRT_CORE_API void Write (FStringView Category, ELevel Level, FStringView Message) noexcept;

// std::format into the message, formatted only when the level lets it through:
//     log::Warning(log::Assets, "{}: unknown field '{}'", Path, Field);
template <typename... TArgs>
void Error (FStringLiteral Category, std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	if (IsEnabled(Category, ELevel::Error))
	{
		Write(Category, ELevel::Error, FString::Format(Fmt, std::forward<TArgs>(Args)...));
	}
}

template <typename... TArgs>
void Warning (FStringLiteral Category, std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	if (IsEnabled(Category, ELevel::Warning))
	{
		Write(Category, ELevel::Warning, FString::Format(Fmt, std::forward<TArgs>(Args)...));
	}
}

template <typename... TArgs>
void Info (FStringLiteral Category, std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	if (IsEnabled(Category, ELevel::Info))
	{
		Write(Category, ELevel::Info, FString::Format(Fmt, std::forward<TArgs>(Args)...));
	}
}

template <typename... TArgs>
void Verbose (FStringLiteral Category, std::format_string<TArgs...> Fmt, TArgs&&... Args)
{
	if (IsEnabled(Category, ELevel::Verbose))
	{
		Write(Category, ELevel::Verbose, FString::Format(Fmt, std::forward<TArgs>(Args)...));
	}
}

// C printf into the message, for lines ported from std::fprintf(stderr, ...) as they are. FString::
// Printf's argument rules: numbers, enums, C strings, void*.
template <typename... TArgs>
void Printf (FStringLiteral Category, ELevel Level, _Printf_format_string_ const char* Fmt, TArgs... Args) noexcept
{
	if (IsEnabled(Category, Level))
	{
		Write(Category, Level, FString::Printf(Fmt, Args...));
	}
}


// ---- Test hook ----

// One message that passed the filters, as Write received it, and the exact line it writes.
struct SMessage
{
	FStringView Category;
	ELevel Level = ELevel::Info;
	FStringView Text;
	FStringView Line;   // "[Category] Text\n"
};

using FHook = void (*) (void* Context, const SMessage& Message);

// For tests: while a hook is set, every message that passes the filters goes to it INSTEAD of stderr
// and the debugger, so a test can check what would be printed without printing it. Called with the
// log's mutex held (one message at a time, from whichever thread logged): it must not log. Null
// restores normal output. Returns the hook it replaced, with its context in OutPreviousContext.
FRT_CORE_API FHook SetHook (FHook Hook, void* Context, void** OutPreviousContext = nullptr) noexcept;
}
