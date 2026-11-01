#pragma once

#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/Discovery/Mounts.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


/**
 * The small rules every part of the meta lifecycle must apply alike: the scanner, the writer, the lifecycle, `migrate
 * metas` and the tests. One owner, because identity depends on them: if `migrate metas` and CreateMeta differed by one
 * byte of input, two clones of one commit would write different GUIDs and the promise of §4.2 would silently fail.
 *
 * Both GUID derivations are frozen, like everything in AssetId.h: Core-Test pins known answers, and a change is a format
 * change. An automatic meta's Input is the UTF-8 of FString::FoldCase(mount path), then the 20 bytes of the content's
 * SContentHash, under the generator "auto-meta". A duplicate's loser has the 16 bytes of its old GUID, then its folded
 * mount path, under "dedupe". Folding the path is what lets a case-insensitive and a case-sensitive checkout agree.
 */
namespace frt::assets
{
inline constexpr FStringLiteral MetaFileSuffix = ".meta";
inline constexpr FStringLiteral EngineAssetSalt = "FRT.Engine";
inline constexpr FStringLiteral AutoMetaGenerator = "auto-meta";
inline constexpr FStringLiteral DedupeGenerator = "dedupe";

// "/Project/Rock.png" -> "/Project/Rock.png.meta". Also works on an absolute path as text, or on a bare name.
[[nodiscard]] inline FString GetMetaPath (FStringView SourcePath)
{
	FString path;
	path.Reserve(SourcePath.Len() + MetaFileSuffix.Len());
	path += SourcePath;
	path += FStringView(MetaFileSuffix);
	return path;
}

/**
 * "/Project/Rock.png.meta" -> "/Project/Rock.png", viewing MetaPath. nullopt when it does not end in ".meta" or nothing
 * would be left of its last segment (a file named ".meta", a dot-name). The suffix matches in any case, as the built-in
 * exclude does (Discovery/BuiltinExcludes.h): "Rock.png.META" is still Rock.png's meta, spelled wrong (META006).
 */
[[nodiscard]] inline std::optional<FStringView> GetMetaSource (FStringView MetaPath) noexcept
{
	if (!MetaPath.EndsWith<ESearchCase::IgnoreCase>(MetaFileSuffix))
	{
		return std::nullopt;
	}
	const FStringView source = MetaPath.LeftChop(MetaFileSuffix.Len());
	if (source.IsEmpty() || source.EndsWith('/'))
	{
		return std::nullopt;
	}
	return source;
}

// What a mount's automatic GUIDs are salted with: "FRT.Engine" for /Engine; for /Project and every plugin, the project's
// SProjectDescriptor::GetAssetSalt() ([Project] AssetSalt, else its Name).
[[nodiscard]] inline FString GetMountAssetSalt (const SMount& Mount, FStringView ProjectSaltOrName)
{
	return FString(Mount.Kind == EMountKind::Engine ? FStringView(EngineAssetSalt) : ProjectSaltOrName);
}

namespace detail
{
[[nodiscard]] inline std::vector<uint8> MakeGuidInput (std::span<const uint8> Head, std::span<const uint8> Tail)
{
	std::vector<uint8> input;
	input.reserve(Head.size() + Tail.size());
	input.insert(input.end(), Head.begin(), Head.end());
	input.insert(input.end(), Tail.begin(), Tail.end());
	return input;
}
}

// IsTaken(const SGuid&) -> bool: true re-rolls (a live GUID has it). The lifecycle's facts carry the live set, and it adds
// each GUID it proposes before asking for the next.
template <typename TIsTaken>
	requires std::is_invocable_r_v<bool, TIsTaken&, const SGuid&>
[[nodiscard]] SGuid MakeCreateMetaGuid (FStringView Salt, FStringView MountPath, const hash::SContentHash& Content, TIsTaken&& IsTaken)
{
	const FString folded = FString(MountPath).FoldCase();
	const std::vector<uint8> input = detail::MakeGuidInput(folded.View().AsBytes(), Content.Bytes);
	return MakeAutoGuid(Salt, AutoMetaGenerator, input, IsTaken);
}

[[nodiscard]] inline SGuid MakeCreateMetaGuid (FStringView Salt, FStringView MountPath, const hash::SContentHash& Content)
{
	return MakeCreateMetaGuid(Salt, MountPath, Content, [] (const SGuid&) { return false; });
}

template <typename TIsTaken>
	requires std::is_invocable_r_v<bool, TIsTaken&, const SGuid&>
[[nodiscard]] SGuid MakeDedupeGuid (FStringView Salt, const SGuid& OldGuid, FStringView LoserMountPath, TIsTaken&& IsTaken)
{
	const FString folded = FString(LoserMountPath).FoldCase();
	const std::vector<uint8> input = detail::MakeGuidInput(OldGuid.Bytes, folded.View().AsBytes());
	return MakeAutoGuid(Salt, DedupeGenerator, input, IsTaken);
}

[[nodiscard]] inline SGuid MakeDedupeGuid (FStringView Salt, const SGuid& OldGuid, FStringView LoserMountPath)
{
	return MakeDedupeGuid(Salt, OldGuid, LoserMountPath, [] (const SGuid&) { return false; });
}
}
