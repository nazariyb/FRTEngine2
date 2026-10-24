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

// A hash of every field but Subject, which is equal within one subject. The lengths go in with the
// numbers, so text moved from one string field to the next changes it. Collisions only cost a full
// comparison (SameWithinSubject).
uint64 Fingerprint (const SDiagnostic& Diagnostic) noexcept
{
	const uint32 numbers[] = {
		static_cast<uint32>(Diagnostic.Severity), Diagnostic.Code, Diagnostic.Line, Diagnostic.Column,
		Diagnostic.File.Len(), Diagnostic.FieldPath.Len(), Diagnostic.Message.Len(),
	};
	uint64 fingerprint = hash::HashBytes(numbers, sizeof(numbers));
	fingerprint = hash::HashBytes(Diagnostic.File.GetData(), Diagnostic.File.Len(), fingerprint);
	fingerprint = hash::HashBytes(Diagnostic.FieldPath.GetData(), Diagnostic.FieldPath.Len(), fingerprint);
	return hash::HashBytes(Diagnostic.Message.GetData(), Diagnostic.Message.Len(), fingerprint);
}

// operator== for two diagnostics of the same subject: the numbers first, the strings last.
bool SameWithinSubject (const SDiagnostic& A, const SDiagnostic& B) noexcept
{
	return A.Code == B.Code && A.Line == B.Line && A.Column == B.Column && A.Severity == B.Severity
		&& A.File == B.File && A.FieldPath == B.FieldPath && A.Message == B.Message;
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
	const uint64 fingerprint = Fingerprint(Diagnostic);

	{
		const std::scoped_lock lock(Mutex);
		SSubjectDiagnostics& subject = BySubject[Diagnostic.Subject];
		const auto [first, last] = subject.ByFingerprint.equal_range(fingerprint);
		for (auto it = first; it != last; ++it)
		{
			if (SameWithinSubject(subject.Diagnostics[it->second], Diagnostic))
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
		subject.ByFingerprint.emplace(fingerprint, static_cast<uint32>(subject.Diagnostics.size()));
		subject.Diagnostics.push_back(std::move(Diagnostic));
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
	return found != BySubject.end() ? found->second.Diagnostics : std::vector<SDiagnostic>();
}

uint32 CDiagnosticStore::ClearSubject (FStringView Subject)
{
	SSubjectDiagnostics removed;
	{
		const std::scoped_lock lock(Mutex);
		const auto found = BySubject.find(Subject);
		if (found == BySubject.end())
		{
			return 0u;
		}
		removed = std::move(found->second);
		BySubject.erase(found);
		Count -= static_cast<uint32>(removed.Diagnostics.size());
		++Revision;
	}
	// Freed outside the lock.
	return static_cast<uint32>(removed.Diagnostics.size());
}

void CDiagnosticStore::Clear ()
{
	std::map<FString, SSubjectDiagnostics, SSubjectLess> bySubject;
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
		for (const SDiagnostic& diagnostic : entry.second.Diagnostics)
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
		for (const SDiagnostic& diagnostic : entry.second.Diagnostics)
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
