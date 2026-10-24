#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Log/Log.h"


/**
 * Structured problems found in content: a missing meta, a bad field, a failed build (spec §7.9). They
 * are data, not log lines: the content browser badge, the validation panel, `assets status` and
 * `--json` output all read the same records, filtered by severity, code and subject (spec §12.8).
 * Until those exist, every new diagnostic is also printed once, through frt::log, as
 * "[assets] <subject>: <message>", like the rest of the engine's output.
 */
namespace frt
{
enum class ESeverity : uint8
{
	Info,
	Warning,
	Error,
};

// "info", "warning", "error".
[[nodiscard]] FRT_CORE_API FStringLiteral GetSeverityName (ESeverity Severity) noexcept;

/**
 * One problem. Subject names what it is about - an asset's path or its id as text - and is what the
 * store groups by; File, Line, Column and FieldPath say where in the source (Line and Column count
 * from 1; 0 is unknown). Code is a packed textual code (EncodeDiagnosticCode: "META001"), 0 for none.
 *
 * The spec's `SAssetId Asset` and `SFixIt Fix` members arrive with the asset registry (M3/M4) and the
 * validation panel's fix-its (M4); until then Subject carries the asset.
 */
struct SDiagnostic
{
	ESeverity Severity = ESeverity::Error;
	uint32 Code = 0u;
	FString Subject;
	FString File;
	uint32 Line = 0u;
	uint32 Column = 0u;
	FString FieldPath;
	FString Message;

	[[nodiscard]] bool operator== (const SDiagnostic& Other) const = default;
};


// ---- Codes ----
//
// The spec writes codes as up to five capital letters and three digits: META001, PATH002, BUILD003.
// They are packed into a uint32 so a diagnostic stays small and compares in one instruction, and
// unpacked for every text form. The packing is arithmetic, not a table, so a game module can use
// codes of its own ("GAME001") and the value of a code is the same in every build:
//     bits 31..8  the letters, base 27, first letter most significant, A = 1 ... Z = 26 (27^5 < 2^24)
//     bits  7..0  the number, 000-255
// 0 is no code (no letters). Codes past 255 do not exist: the spec's highest is 012.

inline constexpr uint32 MaxDiagnosticCodeLetters = 5u;
inline constexpr uint32 MaxDiagnosticCodeNumber = 255u;

// Packs "META001": one to five letters A-Z, then exactly three digits, nothing else. nullopt for
// anything else (lowercase, a missing digit, a number past 255).
[[nodiscard]] constexpr std::optional<uint32> EncodeDiagnosticCode (FStringView Text) noexcept
{
	uint32 letters = 0u;
	uint32 numLetters = 0u;
	while (numLetters < Text.Len() && Text.GetData()[numLetters] >= 'A' && Text.GetData()[numLetters] <= 'Z')
	{
		if (numLetters == MaxDiagnosticCodeLetters)
		{
			return std::nullopt;
		}
		letters = letters * 27u + static_cast<uint32>(Text.GetData()[numLetters] - 'A' + 1);
		++numLetters;
	}
	if (numLetters == 0u || Text.Len() != numLetters + 3u)
	{
		return std::nullopt;
	}
	uint32 number = 0u;
	for (uint32 i = numLetters; i < Text.Len(); ++i)
	{
		const char digit = Text.GetData()[i];
		if (digit < '0' || digit > '9')
		{
			return std::nullopt;
		}
		number = number * 10u + static_cast<uint32>(digit - '0');
	}
	if (number > MaxDiagnosticCodeNumber)
	{
		return std::nullopt;
	}
	return (letters << 8u) | number;
}

namespace detail
{
// Not constexpr: reaching it during constant evaluation is the compile error DiagnosticCode reports.
inline void InvalidDiagnosticCode () noexcept
{}
}

// A code constant, checked at compile time: `inline constexpr uint32 Meta001 = DiagnosticCode("META001");`.
consteval uint32 DiagnosticCode (FStringLiteral Text) noexcept
{
	const std::optional<uint32> code = EncodeDiagnosticCode(Text);
	if (!code)
	{
		detail::InvalidDiagnosticCode();
	}
	return code.value_or(0u);
}

// A packed code with one to five letters and a number up to 255 (what EncodeDiagnosticCode makes).
[[nodiscard]] FRT_CORE_API bool IsValidDiagnosticCode (uint32 Code) noexcept;
// Appends the text form, "META001"; nothing for 0 or an invalid code.
FRT_CORE_API FString& AppendDiagnosticCode (FString& Out, uint32 Code) noexcept;
[[nodiscard]] FRT_CORE_API FString FormatDiagnosticCode (uint32 Code) noexcept;


// ---- The store ----

/**
 * The diagnostics currently known, grouped by subject, plus a bounded ring of the most recent ones.
 *
 * The store holds the current problems of each subject: whoever re-checks a subject (a re-import, a
 * validator run) clears it first and appends what it finds. A diagnostic equal to one already stored
 * for its subject is not stored or printed again, so running the same check twice reports once. The
 * ring keeps the last RecentCapacity diagnostics appended, whatever became of their subjects since:
 * the history a log panel shows.
 *
 * Every diagnostic Append stores is printed once through frt::log, at the level of its severity, as
 * "[<category>] <subject>: <message>" (the category is "assets" unless the constructor says
 * otherwise; without a subject, only the message). Levels apply: a category silenced to Error still
 * stores warnings.
 *
 * Thread-safe: workers append (Analyze, builds) while the main thread queries; one mutex guards the
 * store, held only for the copy in or out. Everything is std containers and FString (CRT heap), never
 * the engine's memory pool. Queries return copies; ForEach visits under the lock and must not call
 * back into the store. GetRevision changes with every change, for immediate-mode panels that poll
 * (spec §5.9).
 */
class FRT_CORE_API CDiagnosticStore
{
public:
	static constexpr uint32 DefaultRecentCapacity = 256u;

	explicit CDiagnosticStore (uint32 RecentCapacity = DefaultRecentCapacity, FStringLiteral LogCategory = log::Assets);
	CDiagnosticStore (const CDiagnosticStore&) = delete;
	CDiagnosticStore& operator= (const CDiagnosticStore&) = delete;

	// Stores the diagnostic under its subject and in the ring, and prints it. False (nothing stored,
	// nothing printed) when an equal diagnostic is already stored for the subject.
	bool Append (SDiagnostic Diagnostic);

	// The subject's diagnostics, in the order they were appended; empty for an unknown subject.
	[[nodiscard]] std::vector<SDiagnostic> Query (FStringView Subject) const;
	// Forgets the subject's diagnostics (the ring keeps them); returns how many there were.
	uint32 ClearSubject (FStringView Subject);
	// Forgets everything, the ring included.
	void Clear ();

	// Every subject with diagnostics, in byte order.
	[[nodiscard]] std::vector<FString> GetSubjects () const;
	// Every stored diagnostic, by subject (byte order), then in append order.
	void ForEach (TFunctionRef<void (const SDiagnostic&)> Visit) const;
	[[nodiscard]] uint32 GetCount () const;
	[[nodiscard]] uint32 GetCount (ESeverity Severity) const;

	// The ring, oldest first: at most GetRecentCapacity() diagnostics.
	[[nodiscard]] std::vector<SDiagnostic> GetRecent () const;
	[[nodiscard]] uint32 GetRecentCapacity () const { return RecentCapacity; }

	[[nodiscard]] uint64 GetRevision () const;

private:
	struct SSubjectLess
	{
		using is_transparent = void;
		bool operator() (FStringView A, FStringView B) const noexcept { return A < B; }
	};

	const uint32 RecentCapacity;
	const FStringLiteral LogCategory;

	mutable std::mutex Mutex;
	std::map<FString, std::vector<SDiagnostic>, SSubjectLess> BySubject;
	std::vector<SDiagnostic> Recent;   // a ring once full: RecentHead is the oldest
	uint32 RecentHead = 0u;
	uint32 Count = 0u;
	uint64 Revision = 0u;
};
}
