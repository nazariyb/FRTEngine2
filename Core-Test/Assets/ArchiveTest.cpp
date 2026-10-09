// IWriteArchive's overload set. Write(bool) is virtual, so it cannot be the constrained template R2
// asks for: pointer -> bool is a standard conversion and beats every user-defined route to FStringView,
// so text of every spelling needs its own overload, and any other pointer must not compile.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <cstddef>
#include <string>

#include "Assets/Archive.h"
#include "Containers/String.h"

using namespace frt;


namespace
{
// What the last Write received, and as which type.
class CRecordingWriteArchive final : public assets::IWriteArchive
{
public:
	using IWriteArchive::Write;
	void Write (FStringLiteral, bool V) override { Last = V ? "bool:true" : "bool:false"; }
	void Write (FStringLiteral, int32 V) override { Last = FString::Format("int32:{}", V); }
	void Write (FStringLiteral, uint32 V) override { Last = FString::Format("uint32:{}", V); }
	void Write (FStringLiteral, float V) override { Last = FString::Format("float:{}", V); }
	void Write (FStringLiteral, FStringView V) override { Last = FString::Format("text:{}", V); }
	void BeginGroup (FStringLiteral) override {}
	void EndGroup () override {}

	FString Last;
};

template <typename TArchive, typename TValue>
concept CanWrite = requires (TArchive& Ar, TValue Value) { Ar.Write("key", Value); };
}


static_assert(CanWrite<assets::IWriteArchive, const char*>);
static_assert(CanWrite<assets::IWriteArchive, char*>);
static_assert(CanWrite<assets::IWriteArchive, const char8_t*>);
static_assert(CanWrite<assets::IWriteArchive, FStringView>);
static_assert(CanWrite<assets::IWriteArchive, bool>);
static_assert(!CanWrite<assets::IWriteArchive, const wchar_t*>);
static_assert(!CanWrite<assets::IWriteArchive, const char16_t*>);
static_assert(!CanWrite<assets::IWriteArchive, const char32_t*>);
static_assert(!CanWrite<assets::IWriteArchive, const void*>);
static_assert(!CanWrite<assets::IWriteArchive, const FString*>);
static_assert(!CanWrite<assets::IWriteArchive, std::string*>);
static_assert(!CanWrite<assets::IWriteArchive, std::nullptr_t>);

TEST(Archive, TextOfEverySpellingIsWrittenAsText)
{
	CRecordingWriteArchive ar;

	ar.Write("k", "literal");
	EXPECT_EQ(ar.Last, "text:literal");

	const char* const pointer = "pointer";
	ar.Write("k", pointer);
	EXPECT_EQ(ar.Last, "text:pointer");

	// u8"" is how section 7 of the string design spells a non-ASCII literal.
	ar.Write("k", u8"Матеріал");
	EXPECT_EQ(ar.Last, "text:Матеріал");

	const char8_t* const utf8Pointer = u8"Цегла";
	ar.Write("k", utf8Pointer);
	EXPECT_EQ(ar.Last, "text:Цегла");

	ar.Write("k", std::string("std::string"));
	EXPECT_EQ(ar.Last, "text:std::string");

	ar.Write("k", FString("FString"));
	EXPECT_EQ(ar.Last, "text:FString");

	ar.Write("k", true);
	EXPECT_EQ(ar.Last, "bool:true");
}
