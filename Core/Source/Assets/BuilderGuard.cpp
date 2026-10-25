#include "Assets/BuilderGuard.h"

#include <atomic>
#include <iterator>

#include "BuildConfig.h"
#include "Log/Log.h"


namespace frt::assets
{
namespace
{
// Reaching these during constant evaluation is the compile error: a row of BuilderVersions.inl that
// is not what the table's header comment says, or a builder the generated hashes do not list.
inline void InvalidBuilderVersionGuid () noexcept {}
inline void InvalidBuilderSourceHash () noexcept {}
inline void BuilderMissingFromGeneratedHashes () noexcept {}

consteval bool IsHexDigit (char C) noexcept
{
	return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
}

consteval uint8 HexValue (char C) noexcept
{
	return static_cast<uint8>(C <= '9' ? C - '0' : (C <= 'F' ? C - 'A' + 10 : C - 'a' + 10));
}

// The table's VersionGuid: 32 hex digits, not all zero.
consteval SGuid GuidFromTable (FStringLiteral Text) noexcept
{
	SGuid guid;
	if (Text.Len() != SGuid::TextLength)
	{
		InvalidBuilderVersionGuid();
		return guid;
	}
	for (uint32 i = 0u; i < SGuid::Size; ++i)
	{
		const char high = Text.GetData()[2u * i];
		const char low = Text.GetData()[2u * i + 1u];
		if (!IsHexDigit(high) || !IsHexDigit(low))
		{
			InvalidBuilderVersionGuid();
		}
		guid.Bytes[i] = static_cast<uint8>(HexValue(high) << 4u | HexValue(low));
	}
	if (!guid.IsValid())
	{
		InvalidBuilderVersionGuid();
	}
	return guid;
}

// A source hash, committed or generated: 16 hex digits (FNV-1a 64, Premake/tools.lua).
inline constexpr uint32 SourceHashLength = 16u;

consteval FStringLiteral CheckedSourceHash (FStringLiteral Text) noexcept
{
	if (Text.Len() != SourceHashLength)
	{
		InvalidBuilderSourceHash();
	}
	for (const char c : Text)
	{
		if (!IsHexDigit(c))
		{
			InvalidBuilderSourceHash();
		}
	}
	return Text;
}

consteval bool SameText (FStringLiteral A, FStringLiteral B) noexcept
{
	if (A.Len() != B.Len())
	{
		return false;
	}
	for (uint32 i = 0u; i < A.Len(); ++i)
	{
		if (A.GetData()[i] != B.GetData()[i])
		{
			return false;
		}
	}
	return true;
}

#if FRT_WITH_DEV_TOOLS
// What FRT.bat builder-hashes wrote before this build (Intermediate/Generated, on Core's include
// path): the hash of each builder's sources as they are now.
struct SGeneratedSourceHash
{
	FStringLiteral Name;
	FStringLiteral Hash;
};

constexpr SGeneratedSourceHash GeneratedSourceHashes[] =
{
#define FRT_BUILDER_SOURCE_HASH(Name, Hash) SGeneratedSourceHash{ FStringLiteral(Name), CheckedSourceHash(FStringLiteral(Hash)) },
#include "BuilderSourceHashes.gen.h"
#undef FRT_BUILDER_SOURCE_HASH
};

// The generated hash of Name. Every row has one: both come from the same table in the same build, so
// a missing one means the prebuild step did not run (or the table and the generator disagree).
consteval FStringLiteral CurrentSourceHash (FStringLiteral Name, FStringLiteral) noexcept
{
	for (const SGeneratedSourceHash& generated : GeneratedSourceHashes)
	{
		if (SameText(generated.Name, Name))
		{
			return generated.Hash;
		}
	}
	BuilderMissingFromGeneratedHashes();
	return {};
}
#else
// Release compares nothing (BuilderGuard.h): the build is what was committed.
consteval FStringLiteral CurrentSourceHash (FStringLiteral, FStringLiteral Committed) noexcept
{
	return Committed;
}
#endif

constexpr SBuilderInfo Builders[] =
{
#define FRT_BUILDER(Name, VersionGuid, CommittedSourceHash, Globs) SBuilderInfo{ FStringLiteral(Name), GuidFromTable(FStringLiteral(VersionGuid)),\
	CheckedSourceHash(FStringLiteral(CommittedSourceHash)), CurrentSourceHash(FStringLiteral(Name), FStringLiteral(CommittedSourceHash)),\
	FStringLiteral(Globs) },
#include "Assets/BuilderVersions.inl"
#undef FRT_BUILDER
};

consteval bool AreNamesUnique () noexcept
{
	for (size_t i = 0u; i < std::size(Builders); ++i)
	{
		for (size_t j = i + 1u; j < std::size(Builders); ++j)
		{
			if (SameText(Builders[i].Name, Builders[j].Name))
			{
				return false;
			}
		}
	}
	return true;
}
static_assert(AreNamesUnique(), "Assets/BuilderVersions.inl names a builder twice");

// Safety.BuilderSourceGuard, and whether each builder's local-only state was logged in this process.
// Not pipeline code (Local/Build/PipelineSources.txt): process state is what this file is for.
std::atomic<bool> bGuardEnabled = true;
std::atomic<bool> bLoggedLocalOnly[std::size(Builders)] = {};
}


std::span<const SBuilderInfo> GetBuilders () noexcept
{
	return std::span<const SBuilderInfo>(Builders, std::size(Builders));
}

const SBuilderInfo* FindBuilder (FStringView Name) noexcept
{
	for (const SBuilderInfo& builder : Builders)
	{
		if (Name == FStringView(builder.Name))
		{
			return &builder;
		}
	}
	return nullptr;
}

SBuilderKeyVersion ResolveBuilderKeyVersion (const SGuid& Version, FStringView CommittedSourceHash, FStringView CurrentSourceHash,
	bool bGuard)
{
	SBuilderKeyVersion result;
	result.Version = Version;
	if (bGuard && !CurrentSourceHash.Equals<ESearchCase::IgnoreCaseAscii>(CommittedSourceHash))
	{
		result.bLocalOnly = true;
		result.LocalDiscriminator = CurrentSourceHash;
	}
	return result;
}

SBuilderKeyVersion GetBuilderKeyVersion (FStringView Builder)
{
	const SBuilderInfo* const builder = FindBuilder(Builder);
	frt_assert(builder != nullptr)
	if (builder == nullptr)
	{
		SBuilderKeyVersion unknown;
		unknown.bLocalOnly = true;
		unknown.LocalDiscriminator = "unregistered builder";
		return unknown;
	}

	SBuilderKeyVersion result = ResolveBuilderKeyVersion(builder->Version, builder->CommittedSourceHash, builder->CurrentSourceHash,
		IsBuilderSourceGuardEnabled());
	if (result.bLocalOnly && !bLoggedLocalOnly[builder - Builders].exchange(true, std::memory_order_relaxed))
	{
		log::Info(log::Assets, "BUILD003 builder {}: its sources changed (hash {}, committed with its version: {}), so its outputs are "
			"local-only until its version GUID is bumped (FRT.bat assets newguid, then FRT.bat check-builders --update)",
			builder->Name, builder->CurrentSourceHash, builder->CommittedSourceHash);
	}
	return result;
}

void SetBuilderSourceGuardEnabled (bool bEnabled) noexcept
{
	bGuardEnabled.store(bEnabled, std::memory_order_relaxed);
}

bool IsBuilderSourceGuardEnabled () noexcept
{
	return bGuardEnabled.load(std::memory_order_relaxed);
}
}
