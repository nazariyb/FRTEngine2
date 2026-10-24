#pragma once

#include <utility>
#include <vector>

#include "Containers/String.h"
#include "Log/Log.h"


namespace frt::test
{
/**
 * Takes every message frt::log lets through, instead of printing it, for as long as it exists, and
 * puts the previous hook back afterwards: a test checks what would have been printed, and the test
 * output stays clean. The hook runs under the log's mutex, so threads may log while it is installed;
 * read the entries once they are done.
 */
class CLogCapture
{
public:
	struct SEntry
	{
		FString Category;
		log::ELevel Level = log::ELevel::Info;
		FString Text;
		FString Line;
	};

	CLogCapture () noexcept
	{
		Previous = log::SetHook(&Capture, this, &PreviousContext);
	}

	~CLogCapture ()
	{
		log::SetHook(Previous, PreviousContext);
	}

	CLogCapture (const CLogCapture&) = delete;
	CLogCapture& operator= (const CLogCapture&) = delete;

	[[nodiscard]] const std::vector<SEntry>& GetEntries () const { return Entries; }

	// The lines as they would have been written ("[category] text\n").
	[[nodiscard]] std::vector<FString> GetLines () const
	{
		std::vector<FString> lines;
		for (const SEntry& entry : Entries)
		{
			lines.push_back(entry.Line);
		}
		return lines;
	}

	void Clear () { Entries.clear(); }

private:
	static void Capture (void* Context, const log::SMessage& Message)
	{
		SEntry entry;
		entry.Category = Message.Category;
		entry.Level = Message.Level;
		entry.Text = Message.Text;
		entry.Line = Message.Line;
		static_cast<CLogCapture*>(Context)->Entries.push_back(std::move(entry));
	}

	std::vector<SEntry> Entries;
	log::FHook Previous = nullptr;
	void* PreviousContext = nullptr;
};
}
