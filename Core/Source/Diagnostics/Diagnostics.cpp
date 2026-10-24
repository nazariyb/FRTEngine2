#include "Diagnostics/Diagnostics.h"

#include <utility>


namespace frt
{
namespace
{
log::ELevel ToLogLevel (ESeverity Severity) noexcept
{
	switch (Severity)
	{
	case ESeverity::Info:
		return log::ELevel::Info;
	case ESeverity::Warning:
		return log::ELevel::Warning;
	case ESeverity::Error:
		return log::ELevel::Error;
	}
	return log::ELevel::Error;
}

// The letters of a packed code, last first; 0 when the code has none or more than five.
uint32 CountCodeLetters (uint32 Letters) noexcept
{
	uint32 count = 0u;
	while (Letters != 0u)
	{
		if (Letters % 27u == 0u || count == MaxDiagnosticCodeLetters)
		{
			return 0u;   // a zero digit is no letter: not a code EncodeDiagnosticCode makes
		}
		Letters /= 27u;
		++count;
	}
	return count;
}
}


FStringLiteral GetSeverityName (ESeverity Severity) noexcept
{
	switch (Severity)
	{
	case ESeverity::Info:
		return "info";
	case ESeverity::Warning:
		return "warning";
	case ESeverity::Error:
		return "error";
	}
	return "unknown";
}


// ---------------- Codes ----------------

bool IsValidDiagnosticCode (uint32 Code) noexcept
{
	return CountCodeLetters(Code >> 8u) != 0u && (Code & 0xFFu) <= MaxDiagnosticCodeNumber;
}

FString& AppendDiagnosticCode (FString& Out, uint32 Code) noexcept
{
	uint32 letters = Code >> 8u;
	const uint32 numLetters = CountCodeLetters(letters);
	if (numLetters == 0u)
	{
		return Out;
	}

	char text[MaxDiagnosticCodeLetters + 3u] = {};
	for (uint32 i = numLetters; i > 0u; --i)
	{
		text[i - 1u] = static_cast<char>('A' + letters % 27u - 1u);
		letters /= 27u;
	}
	const uint32 number = Code & 0xFFu;
	text[numLetters] = static_cast<char>('0' + number / 100u);
	text[numLetters + 1u] = static_cast<char>('0' + number / 10u % 10u);
	text[numLetters + 2u] = static_cast<char>('0' + number % 10u);
	return Out.Append(FStringView(text, numLetters + 3u));
}

FString FormatDiagnosticCode (uint32 Code) noexcept
{
	FString text;
	AppendDiagnosticCode(text, Code);
	return text;
}


// ---------------- CDiagnosticStore ----------------

CDiagnosticStore::CDiagnosticStore (uint32 InRecentCapacity, FStringLiteral InLogCategory)
	: RecentCapacity(InRecentCapacity)
	, LogCategory(InLogCategory)
{}

bool CDiagnosticStore::Append (SDiagnostic Diagnostic)
{
	// The echo, built before the lock and written after it: printing is the slow part.
	FString line;
	if (!Diagnostic.Subject.IsEmpty())
	{
		line.Append(Diagnostic.Subject).Append(": ");
	}
	line.Append(Diagnostic.Message);
	const log::ELevel level = ToLogLevel(Diagnostic.Severity);

	{
		const std::scoped_lock lock(Mutex);
		std::vector<SDiagnostic>& stored = BySubject[Diagnostic.Subject];
		for (const SDiagnostic& existing : stored)
		{
			if (existing == Diagnostic)
			{
				return false;
			}
		}

		if (RecentCapacity > 0u)
		{
			if (Recent.size() < RecentCapacity)
			{
				Recent.push_back(Diagnostic);
			}
			else
			{
				Recent[RecentHead] = Diagnostic;
				RecentHead = (RecentHead + 1u) % RecentCapacity;
			}
		}
		stored.push_back(std::move(Diagnostic));
		++Count;
		++Revision;
	}

	log::Write(LogCategory, level, line);
	return true;
}

std::vector<SDiagnostic> CDiagnosticStore::Query (FStringView Subject) const
{
	const std::scoped_lock lock(Mutex);
	const auto found = BySubject.find(Subject);
	return found != BySubject.end() ? found->second : std::vector<SDiagnostic>();
}

uint32 CDiagnosticStore::ClearSubject (FStringView Subject)
{
	std::vector<SDiagnostic> removed;
	{
		const std::scoped_lock lock(Mutex);
		const auto found = BySubject.find(Subject);
		if (found == BySubject.end())
		{
			return 0u;
		}
		removed = std::move(found->second);
		BySubject.erase(found);
		Count -= static_cast<uint32>(removed.size());
		++Revision;
	}
	// Freed outside the lock.
	return static_cast<uint32>(removed.size());
}

void CDiagnosticStore::Clear ()
{
	std::map<FString, std::vector<SDiagnostic>, SSubjectLess> bySubject;
	std::vector<SDiagnostic> recent;
	{
		const std::scoped_lock lock(Mutex);
		bySubject.swap(BySubject);
		recent.swap(Recent);
		RecentHead = 0u;
		Count = 0u;
		++Revision;
	}
}

std::vector<FString> CDiagnosticStore::GetSubjects () const
{
	const std::scoped_lock lock(Mutex);
	std::vector<FString> subjects;
	subjects.reserve(BySubject.size());
	for (const auto& entry : BySubject)
	{
		subjects.push_back(entry.first);
	}
	return subjects;
}

void CDiagnosticStore::ForEach (TFunctionRef<void (const SDiagnostic&)> Visit) const
{
	const std::scoped_lock lock(Mutex);
	for (const auto& entry : BySubject)
	{
		for (const SDiagnostic& diagnostic : entry.second)
		{
			Visit(diagnostic);
		}
	}
}

uint32 CDiagnosticStore::GetCount () const
{
	const std::scoped_lock lock(Mutex);
	return Count;
}

uint32 CDiagnosticStore::GetCount (ESeverity Severity) const
{
	const std::scoped_lock lock(Mutex);
	uint32 count = 0u;
	for (const auto& entry : BySubject)
	{
		for (const SDiagnostic& diagnostic : entry.second)
		{
			count += diagnostic.Severity == Severity ? 1u : 0u;
		}
	}
	return count;
}

std::vector<SDiagnostic> CDiagnosticStore::GetRecent () const
{
	const std::scoped_lock lock(Mutex);
	std::vector<SDiagnostic> recent;
	recent.reserve(Recent.size());
	for (size_t i = 0u; i < Recent.size(); ++i)
	{
		recent.push_back(Recent[(RecentHead + i) % Recent.size()]);
	}
	return recent;
}

uint64 CDiagnosticStore::GetRevision () const
{
	const std::scoped_lock lock(Mutex);
	return Revision;
}
}
