#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "CoreTypes.h"
#include "Assets/Pipeline/Importers/AssetImporter.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * What the importers of Assets/Pipeline/Importers share and nothing outside them uses: the singletons, the one
 * whole-file read that hashes in the same pass, diagnostics that name the file, and the resolution of a path a
 * source spells relative to its own folder. Internal to Core (not exported).
 */
namespace frt::assets::importers
{
// The singletons, in ImporterTable's order of the available rows.
[[nodiscard]] const IAssetImporter& GetTextureImporter () noexcept;
[[nodiscard]] const IAssetImporter& GetGltfImporter () noexcept;
[[nodiscard]] const IAssetImporter& GetLegacyMaterialImporter () noexcept;
[[nodiscard]] const IAssetImporter& GetLegacyInputImporter () noexcept;
[[nodiscard]] const IAssetImporter& GetRawImporter () noexcept;

// A diagnostic about In's file (Subject its mount path, File its source file), code 0 unless given.
[[nodiscard]] SDiagnostic MakeFileDiagnostic (const SAnalyzeInput& In, ESeverity Severity, FString Message, uint32 Code = 0u);

// The most a legacy text source (.frtmat.yml, .frtinputmap, .frtinputaction) is read whole for: real ones are a
// few KiB; anything bigger is misnamed, and up to 16 workers buffering it at once must not exhaust memory.
inline constexpr uint64 MaxSourceTextBytes = 16ull * 1024u * 1024u;

/**
 * The whole file in one stable pass (platform::ReadFileChunksStable), hashed with BLAKE3-160 as it is read
 * (the registry's content hash, the same as HashFileStable gives). A file larger than MaxBytes is not read: a
 * code-0 error says so (Analyze fails, the record stays). On anything but Stable, adds the error to Out (code 0),
 * sets Out.bSourceChanged for Changed, and returns nullopt.
 */
[[nodiscard]] std::optional<std::vector<uint8>> ReadWholeFileHashed (const SAnalyzeInput& In, SAnalyzeResult& Out, uint64 MaxBytes);

// The text of a file read with ReadWholeFileHashed, decoded as platform::ReadFileText's Detect decodes it (a BOM
// stripped, UTF-16 transcoded); nullopt (with an error in Out) for UTF-16 with an odd byte count.
[[nodiscard]] std::optional<FString> DecodeSourceText (std::vector<uint8>&& Bytes, const SAnalyzeInput& In, SAnalyzeResult& Out);

// Why a reference a source holds cannot become an edge, or nothing (empty) when it can.
enum class EReferenceProblem : uint8
{
	None,
	NotRelative,     // a URI scheme, a drive, a stream name (any ':'), or a leading '/' or '\'
	LeavesMount,     // a ".." climbs above the mount root
};

/**
 * The mount path that Reference, as written in the file at FileMountPath, names relative to the file's folder
 * (ResolveMountPath, '\' a separator too, ".." collapsed). Only folder-relative references resolve: the runtime
 * reads an absolute or drive path as a machine path, which no mount path names, and a URI scheme names no file.
 */
[[nodiscard]] std::optional<FString> ResolveRelativeReference (FStringView FileMountPath, FStringView Reference, EReferenceProblem* OutProblem = nullptr);
// The same against a folder (a mount root included) instead of a file's folder.
[[nodiscard]] std::optional<FString> ResolveFolderReference (FStringView FolderMountPath, FStringView Reference, EReferenceProblem* OutProblem = nullptr);

// "a reference that is not relative to the file's folder" and so on, for diagnostics.
[[nodiscard]] FStringLiteral DescribeReferenceProblem (EReferenceProblem Problem) noexcept;
}
