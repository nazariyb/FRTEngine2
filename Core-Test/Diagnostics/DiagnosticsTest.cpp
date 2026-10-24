// Diagnostic codes pack into the same uint32 in every build and unpack to the spec's text; the store
// keeps each subject's current diagnostics and a bounded history, prints each new one once as
// "[assets] <subject>: <message>", and takes appends from many threads.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "Log/LogCapture.h"

#include <initializer_list>
#include <optional>
#include <thread>
#include <vector>

#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Log/Log.h"

using namespace frt;


namespace
{
SDiagnostic MakeDiagnostic (FStringView Subject, FStringView Message, ESeverity Severity = ESeverity::Error,
	FStringLiteral Code = "META001")
{
	SDiagnostic diagnostic;
	diagnostic.Severity = Severity;
	diagnostic.Code = EncodeDiagnosticCode(Code).value_or(0u);
	diagnostic.Subject = Subject;
	diagnostic.Message = Message;
	return diagnostic;
}

std::vector<FString> MessagesOf (const std::vector<SDiagnostic>& Diagnostics)
{
	std::vector<FString> messages;
	for (const SDiagnostic& diagnostic : Diagnostics)
	{
		messages.push_back(diagnostic.Message);
	}
	return messages;
}

class DiagnosticStoreTest : public ::testing::Test
{
protected:
	void SetUp () override { log::ResetAllMinLevels(); }
	void TearDown () override { log::ResetAllMinLevels(); }

	// The store's echo goes here instead of stderr.
	test::CLogCapture Capture;
};
}


// ---------------- Codes ----------------

// Frozen: a packed code is stored data (build state, reports) and must never change.
static_assert(DiagnosticCode("META001") == 0x03F7E101u);
static_assert(DiagnosticCode("A000") == 0x00000100u);
static_assert(DiagnosticCode("ZZZZZ255") == ((((((26u * 27u + 26u) * 27u + 26u) * 27u + 26u) * 27u + 26u) << 8u) | 255u));
static_assert(EncodeDiagnosticCode("BUILD003").has_value());

TEST(DiagnosticCode, EveryCodeOfTheSpecRoundTrips)
{
	// The codes the design names (AssetManager-Design.md), every family and the highest numbers.
	constexpr FStringLiteral codes[] = {
		"BUILD001", "BUILD002", "BUILD003", "COOK001", "COOK004", "IMP010", "IMP011", "IMP012", "LOAD001", "LOAD007",
		"META001", "META009", "MIG001", "MIG002", "NAME001", "NAME002", "PATH001", "PATH003", "REF001", "REF010",
		"ROW001", "RULE001", "RULE002", "TYPE001", "TYPE003", "VCS001", "VCS004",
	};
	std::vector<uint32> packed;
	for (const FStringLiteral code : codes)
	{
		const std::optional<uint32> value = EncodeDiagnosticCode(code);
		ASSERT_TRUE(value.has_value()) << code.CStr();
		EXPECT_TRUE(IsValidDiagnosticCode(*value)) << code.CStr();
		EXPECT_EQ(FormatDiagnosticCode(*value), code);
		packed.push_back(*value);
	}
	// Distinct codes, distinct values.
	for (size_t i = 0u; i < packed.size(); ++i)
	{
		for (size_t j = i + 1u; j < packed.size(); ++j)
		{
			EXPECT_NE(packed[i], packed[j]) << codes[i].CStr() << " " << codes[j].CStr();
		}
	}
}

TEST(DiagnosticCode, OnlyCapitalLettersAndThreeDigitsUpTo255Pack)
{
	for (const FStringView text : { "", "META", "001", "META01", "META0001", "meta001", "Meta001", "ABCDEF001",
		"META00A", "META 001", " META001", "META256", "META999", "1META001", "MÉTA001" })
	{
		EXPECT_FALSE(EncodeDiagnosticCode(text).has_value()) << FString(text).CStr();
	}
	EXPECT_EQ(EncodeDiagnosticCode("A255").value_or(0u), (1u << 8u) | 255u);
	EXPECT_EQ(EncodeDiagnosticCode("ABCDE000").value_or(0u), (((((1u * 27u + 2u) * 27u + 3u) * 27u + 4u) * 27u + 5u) << 8u));
}

TEST(DiagnosticCode, ZeroAndInvalidValuesFormatAsNothing)
{
	EXPECT_FALSE(IsValidDiagnosticCode(0u));
	EXPECT_EQ(FormatDiagnosticCode(0u), "");
	EXPECT_FALSE(IsValidDiagnosticCode(7u));                 // a number without letters
	EXPECT_FALSE(IsValidDiagnosticCode(27u << 8u));          // a zero base-27 digit is no letter
	EXPECT_FALSE(IsValidDiagnosticCode(0xFFFFFFFFu));        // six letters' worth
	EXPECT_EQ(FormatDiagnosticCode(0xFFFFFFFFu), "");

	FString text("code ");
	AppendDiagnosticCode(text, DiagnosticCode("VCS004"));
	EXPECT_EQ(text, "code VCS004");
}

TEST(DiagnosticSeverity, NamesAndOrder)
{
	EXPECT_EQ(GetSeverityName(ESeverity::Info), "info");
	EXPECT_EQ(GetSeverityName(ESeverity::Warning), "warning");
	EXPECT_EQ(GetSeverityName(ESeverity::Error), "error");
	EXPECT_LT(ESeverity::Info, ESeverity::Warning);
	EXPECT_LT(ESeverity::Warning, ESeverity::Error);
}


// ---------------- CDiagnosticStore ----------------

TEST_F(DiagnosticStoreTest, KeepsEachSubjectsDiagnosticsInAppendOrder)
{
	CDiagnosticStore store;
	EXPECT_TRUE(store.Append(MakeDiagnostic("Content/Rock.png", "first")));
	EXPECT_TRUE(store.Append(MakeDiagnostic("Content/Tree.png", "other")));
	EXPECT_TRUE(store.Append(MakeDiagnostic("Content/Rock.png", "second", ESeverity::Warning)));

	EXPECT_EQ(MessagesOf(store.Query("Content/Rock.png")), (std::vector<FString>{ "first", "second" }));
	EXPECT_EQ(MessagesOf(store.Query("Content/Tree.png")), (std::vector<FString>{ "other" }));
	EXPECT_TRUE(store.Query("Content/Nothing.png").empty());
	EXPECT_EQ(store.GetCount(), 3u);
	EXPECT_EQ(store.GetCount(ESeverity::Error), 2u);
	EXPECT_EQ(store.GetCount(ESeverity::Warning), 1u);
	EXPECT_EQ(store.GetSubjects(), (std::vector<FString>{ "Content/Rock.png", "Content/Tree.png" }));

	std::vector<FString> visited;
	store.ForEach([&visited] (const SDiagnostic& Diagnostic) { visited.push_back(Diagnostic.Message); });
	EXPECT_EQ(visited, (std::vector<FString>{ "first", "second", "other" }));
}

TEST_F(DiagnosticStoreTest, AnEqualDiagnosticIsStoredAndPrintedOnce)
{
	CDiagnosticStore store;
	SDiagnostic diagnostic = MakeDiagnostic("Content/Rock.png", "bad field");
	diagnostic.File = "Content/Rock.png.meta";
	diagnostic.Line = 4u;
	diagnostic.Column = 9u;
	diagnostic.FieldPath = "Settings.Srgb";

	EXPECT_TRUE(store.Append(diagnostic));
	EXPECT_FALSE(store.Append(diagnostic));
	// Anything different is another diagnostic.
	SDiagnostic elsewhere = diagnostic;
	elsewhere.Line = 5u;
	EXPECT_TRUE(store.Append(elsewhere));

	EXPECT_EQ(store.GetCount(), 2u);
	EXPECT_EQ(store.GetRecent().size(), 2u);
	EXPECT_EQ(Capture.GetEntries().size(), 2u);
}

TEST_F(DiagnosticStoreTest, PrintsEachNewDiagnosticAsSubjectAndMessage)
{
	CDiagnosticStore store;
	store.Append(MakeDiagnostic("Content/Rock.png", "missing meta", ESeverity::Error));
	store.Append(MakeDiagnostic("Content/Tree.png", "unknown field 'Srgbb'", ESeverity::Warning));
	store.Append(MakeDiagnostic("", "scan finished with 2 problems", ESeverity::Info));

	const std::vector<test::CLogCapture::SEntry>& entries = Capture.GetEntries();
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].Line, "[assets] Content/Rock.png: missing meta\n");
	EXPECT_EQ(entries[0].Level, log::ELevel::Error);
	EXPECT_EQ(entries[1].Line, "[assets] Content/Tree.png: unknown field 'Srgbb'\n");
	EXPECT_EQ(entries[1].Level, log::ELevel::Warning);
	EXPECT_EQ(entries[2].Line, "[assets] scan finished with 2 problems\n");
	EXPECT_EQ(entries[2].Level, log::ELevel::Info);

	// Another category, if the owner says so.
	CDiagnosticStore platformStore(8u, log::Platform);
	platformStore.Append(MakeDiagnostic("C:/Temp", "case-sensitive directory"));
	EXPECT_EQ(entries.back().Line, "[platform] C:/Temp: case-sensitive directory\n");
}

TEST_F(DiagnosticStoreTest, ASilencedCategoryStillStores)
{
	log::SetMinLevel(log::Assets, log::ELevel::Error);
	CDiagnosticStore store;
	store.Append(MakeDiagnostic("Content/Rock.png", "a warning", ESeverity::Warning));
	store.Append(MakeDiagnostic("Content/Rock.png", "an error", ESeverity::Error));
	EXPECT_EQ(store.GetCount(), 2u);
	EXPECT_EQ(Capture.GetLines(), (std::vector<FString>{ "[assets] Content/Rock.png: an error\n" }));
}

TEST_F(DiagnosticStoreTest, ClearSubjectForgetsOneSubjectButNotTheHistory)
{
	CDiagnosticStore store;
	store.Append(MakeDiagnostic("Content/Rock.png", "first"));
	store.Append(MakeDiagnostic("Content/Rock.png", "second"));
	store.Append(MakeDiagnostic("Content/Tree.png", "other"));
	const uint64 revision = store.GetRevision();

	EXPECT_EQ(store.ClearSubject("Content/Rock.png"), 2u);
	EXPECT_EQ(store.ClearSubject("Content/Rock.png"), 0u);
	EXPECT_GT(store.GetRevision(), revision);
	EXPECT_TRUE(store.Query("Content/Rock.png").empty());
	EXPECT_EQ(store.GetCount(), 1u);
	EXPECT_EQ(store.GetSubjects(), (std::vector<FString>{ "Content/Tree.png" }));
	EXPECT_EQ(MessagesOf(store.GetRecent()), (std::vector<FString>{ "first", "second", "other" }));

	// A re-check that finds the same problem reports it again: it is current again.
	EXPECT_TRUE(store.Append(MakeDiagnostic("Content/Rock.png", "first")));
	EXPECT_EQ(Capture.GetEntries().size(), 4u);

	store.Clear();
	EXPECT_EQ(store.GetCount(), 0u);
	EXPECT_TRUE(store.GetRecent().empty());
	EXPECT_TRUE(store.GetSubjects().empty());
}

TEST_F(DiagnosticStoreTest, TheRingKeepsTheMostRecentOldestFirst)
{
	CDiagnosticStore store(3u);
	EXPECT_EQ(store.GetRecentCapacity(), 3u);
	for (int i = 0; i < 5; ++i)
	{
		store.Append(MakeDiagnostic("Content/Rock.png", FString::Format("message {}", i)));
	}
	EXPECT_EQ(MessagesOf(store.GetRecent()), (std::vector<FString>{ "message 2", "message 3", "message 4" }));
	EXPECT_EQ(store.GetCount(), 5u);

	CDiagnosticStore noHistory(0u);
	noHistory.Append(MakeDiagnostic("Content/Rock.png", "kept"));
	EXPECT_TRUE(noHistory.GetRecent().empty());
	EXPECT_EQ(noHistory.GetCount(), 1u);
}

TEST_F(DiagnosticStoreTest, ManyThreadsAppendAtOnce)
{
	constexpr int threads = 8;
	constexpr int perThread = 250;
	CDiagnosticStore store(64u);
	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t)
	{
		workers.emplace_back([&store, t] ()
		{
			const FString subject = FString::Format("Content/Thread{}.png", t);
			for (int i = 0; i < perThread; ++i)
			{
				store.Append(MakeDiagnostic(subject, FString::Format("problem {}", i), ESeverity::Warning));
				// Readers at the same time.
				(void)store.GetCount();
			}
		});
	}
	for (std::thread& worker : workers)
	{
		worker.join();
	}

	EXPECT_EQ(store.GetCount(), static_cast<uint32>(threads * perThread));
	EXPECT_EQ(store.GetRecent().size(), 64u);
	EXPECT_EQ(Capture.GetEntries().size(), static_cast<size_t>(threads * perThread));
	for (int t = 0; t < threads; ++t)
	{
		const std::vector<SDiagnostic> diagnostics = store.Query(FString::Format("Content/Thread{}.png", t));
		ASSERT_EQ(diagnostics.size(), static_cast<size_t>(perThread));
		for (int i = 0; i < perThread; ++i)
		{
			EXPECT_EQ(diagnostics[static_cast<size_t>(i)].Message, FString::Format("problem {}", i));
		}
	}
}
