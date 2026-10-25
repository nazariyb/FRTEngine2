// The builder source guard at run time (Assets/BuilderGuard.h, design §7.3, BUILD003): a builder whose
// sources match the hash committed with its version keys by that version alone; one whose sources changed
// keys with its current source hash as a local-only discriminator - so its key, and so its DDC entry,
// differs from the committed code's and is never shared - and Safety.BuilderSourceGuard off turns that
// off. The table holds the M1 builder, and what GetBuilderKeyVersion answers follows the hashes this very
// binary was built from (Intermediate/Generated/BuilderSourceHashes.gen.h, written by every build).
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "BuildConfig.h"
#include "Assets/BuilderGuard.h"
#include "Assets/KeyWriter.h"
#include "Containers/String.h"

using namespace frt;
using namespace frt::assets;


namespace
{
constexpr SGuid SomeVersion { { 0x93, 0xa4, 0x77, 0xe6, 0x29, 0x87, 0x49, 0xed, 0x92, 0xc0, 0x2e, 0xf9, 0x1e, 0xea, 0x79, 0xbd } };

bool IsLowerHex (FStringView Text)
{
	for (const char c : Text)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}
	return true;
}

// A step's StaticKey as the guard's usage comment writes it.
hash::SContentHash MakeKey (const SBuilderKeyVersion& Version)
{
	CKeyWriter key;
	key.WriteStr("Builder", "Shader.Dxil");
	key.WriteGuid("Version", Version.Version);
	if (Version.bLocalOnly)
	{
		key.BeginRecord("Discriminators");
		key.WriteStr(LocalSourceDiscriminator, Version.LocalDiscriminator);
		key.EndRecord();
	}
	FString error;
	const std::optional<std::vector<uint8>> blob = key.Finish(&error);
	EXPECT_TRUE(blob.has_value()) << error.CStr();
	return blob ? MakeStaticKey(*blob) : hash::SContentHash();
}

// Restores the switch whatever a test does with it.
class BuilderGuardTest : public ::testing::Test
{
protected:
	void TearDown () override
	{
		SetBuilderSourceGuardEnabled(true);
	}
};
}


TEST_F(BuilderGuardTest, TheTableHoldsTheShaderBuilderWithAVersionAndItsGlobs)
{
	ASSERT_FALSE(GetBuilders().empty());
	const SBuilderInfo* shader = FindBuilder("Shader.Dxil");
	ASSERT_NE(shader, nullptr);
	EXPECT_TRUE(shader->Version.IsValid());
	EXPECT_EQ(shader->CommittedSourceHash.Len(), 16u);
	EXPECT_TRUE(IsLowerHex(shader->CommittedSourceHash)) << shader->CommittedSourceHash.CStr();
	EXPECT_EQ(shader->CurrentSourceHash.Len(), 16u);
	EXPECT_TRUE(IsLowerHex(shader->CurrentSourceHash)) << shader->CurrentSourceHash.CStr();
	EXPECT_TRUE(FStringView(shader->Globs).Contains("Core/Source/Graphics/Render/RHI/ShaderCompiler.*")) << shader->Globs.CStr();
	EXPECT_TRUE(FStringView(shader->Globs).Contains("Core/Source/Assets/KeyWriter.*")) << shader->Globs.CStr();

	// Names are key content: exact.
	EXPECT_EQ(FindBuilder("shader.dxil"), nullptr);
	EXPECT_EQ(FindBuilder("Shader"), nullptr);
	for (const SBuilderInfo& builder : GetBuilders())
	{
		EXPECT_EQ(FindBuilder(builder.Name), &builder);
	}
}

TEST_F(BuilderGuardTest, MatchingSourcesKeyByTheVersionAlone)
{
	const SBuilderKeyVersion version = ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "0123456789abcdef", true);
	EXPECT_EQ(version.Version, SomeVersion);
	EXPECT_FALSE(version.bLocalOnly);
	EXPECT_TRUE(version.LocalDiscriminator.IsEmpty());

	// Hex digits in either case are the same hash.
	EXPECT_FALSE(ResolveBuilderKeyVersion(SomeVersion, "0123456789ABCDEF", "0123456789abcdef", true).bLocalOnly);
}

TEST_F(BuilderGuardTest, ChangedSourcesMakeTheOutputsLocalOnlyWithTheCurrentHashAsDiscriminator)
{
	const SBuilderKeyVersion committed = ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "0123456789abcdef", true);
	const SBuilderKeyVersion edited = ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "fedcba9876543210", true);
	EXPECT_EQ(edited.Version, SomeVersion);
	EXPECT_TRUE(edited.bLocalOnly);
	EXPECT_EQ(edited.LocalDiscriminator, "fedcba9876543210");

	// The edited builder's key differs from the committed code's - no stale hit either way - and two
	// different edits differ from each other.
	const SBuilderKeyVersion otherEdit = ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "00000000ffffffff", true);
	EXPECT_NE(MakeKey(edited), MakeKey(committed));
	EXPECT_NE(MakeKey(edited), MakeKey(otherEdit));
	EXPECT_EQ(MakeKey(edited), MakeKey(ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "fedcba9876543210", true)));
}

TEST_F(BuilderGuardTest, TheSwitchTurnsTheGuardOff)
{
	const SBuilderKeyVersion version = ResolveBuilderKeyVersion(SomeVersion, "0123456789abcdef", "fedcba9876543210", false);
	EXPECT_FALSE(version.bLocalOnly);
	EXPECT_TRUE(version.LocalDiscriminator.IsEmpty());

	EXPECT_TRUE(IsBuilderSourceGuardEnabled());
	SetBuilderSourceGuardEnabled(false);
	EXPECT_FALSE(IsBuilderSourceGuardEnabled());
	EXPECT_FALSE(GetBuilderKeyVersion("Shader.Dxil").bLocalOnly);
	SetBuilderSourceGuardEnabled(true);
	EXPECT_TRUE(IsBuilderSourceGuardEnabled());
}

// What every builder's keys use in this build: local-only exactly when the sources this binary was built
// from differ from the committed hash (always matching in Release, which compares nothing).
TEST_F(BuilderGuardTest, KeyVersionsFollowTheSourcesThisBuildWasMadeFrom)
{
	for (const SBuilderInfo& builder : GetBuilders())
	{
		SCOPED_TRACE(builder.Name.CStr());
		const SBuilderKeyVersion version = GetBuilderKeyVersion(builder.Name);
		EXPECT_EQ(version.Version, builder.Version);
		const bool bChanged = !FStringView(builder.CurrentSourceHash).Equals<ESearchCase::IgnoreCaseAscii>(builder.CommittedSourceHash);
		EXPECT_EQ(version.bLocalOnly, bChanged);
		EXPECT_EQ(version.LocalDiscriminator, bChanged ? FStringView(builder.CurrentSourceHash) : FStringView());
		// Asked again (the log line is once per builder; the answer is the same every time).
		EXPECT_EQ(GetBuilderKeyVersion(builder.Name), version);
#if !FRT_WITH_DEV_TOOLS
		EXPECT_FALSE(version.bLocalOnly);
#endif
	}
}
