#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"


/**
 * Content migrations, per repository (design §14.8): each step its own commit, recorded in that repository's
 * `Schemas/Migrations.yml` (the project's beside its .frtproject, the engine's at the checkout root), so that a project
 * outside the engine knows which of the engine's steps it has had. The file is tool-written and committed:
 *
 *     FormatVersion: 1
 *     Applied:
 *       - metas
 *
 * through the canonical emitter (§3.7), with the step names sorted and nothing else: no dates, users, machines or engine
 * versions, so two people who migrate the same commit write the same bytes and two branches that each add a step merge as
 * text. A step this engine does not know (the file of a newer engine) is kept as it is when the file is written again.
 *
 * Here: the step table, the file's reading and writing, which required steps a root lacks, and the gate's switch.
 * `migrate` runs the steps (MetaVerbs.h: MigrateMetas); `validate` reports a root that lacks one as MIG001
 * (Validation.h). The gate itself is the hosts' (FindMigrationGap, IsMigrationGateOn): CEngine opens a project behind
 * read-only and logs MIG001, the editor alone asks in a dialog (never in a scripted run), the project manager offers
 * the migration, and AssetTool refuses a writing run of every verb but `migrate` with exit code 1.
 */
namespace frt::config
{
class CConfig;
}

namespace frt::assets
{
class IVcsProvider;

// "Schemas/Migrations.yml", relative to a repository root, as diagnostics name it (File).
inline constexpr FStringLiteral MigrationsFile = "Schemas/Migrations.yml";
inline constexpr uint32 MigrationsFormatVersion = 1u;

/**
 * Every step §14.8 names, in the order `migrate --all` runs them. bAvailable: this engine can run it (and so requires it of
 * every root); the others are named so that `migrate <step>` can say which milestone brings it.
 */
struct SMigrationStep
{
	FStringLiteral Name;
	FStringLiteral Milestone;
	bool bAvailable = false;
};

[[nodiscard]] FRT_CORE_API std::span<const SMigrationStep> GetMigrationSteps () noexcept;
// The step named Name (ASCII case ignored), or nullptr.
[[nodiscard]] FRT_CORE_API const SMigrationStep* FindMigrationStep (FStringView Name) noexcept;

// <Root>/Schemas/Migrations.yml.
[[nodiscard]] FRT_CORE_API std::filesystem::path GetMigrationsPath (const std::filesystem::path& Root);

struct SMigrationsFile
{
	bool bExists = false;
	std::vector<FString> Applied;   // as the file lists them, known or not
};

/**
 * Reads Root's file. A missing file is no error: nothing is applied (bExists false). False, with OutError filled (Code
 * MIG001, File MigrationsFile, Line and Column where it has them), when the file cannot be read or is not the format above.
 */
[[nodiscard]] FRT_CORE_API bool ReadMigrations (const std::filesystem::path& Root, SMigrationsFile& Out, SDiagnostic* OutError = nullptr);

// The file's canonical bytes for Applied (sorted by bytes, duplicates once). False only when the emitter refuses a name.
[[nodiscard]] FRT_CORE_API bool EmitMigrations (std::span<const FString> Applied, FString& Out, SDiagnostic* OutError = nullptr);

// The available steps File lacks, in run order: what makes the root behind the engine (MIG001).
[[nodiscard]] FRT_CORE_API std::vector<FStringLiteral> GetMissingMigrations (const SMigrationsFile& File);

/**
 * Adds Step to Root's file (creating it and Schemas/ when missing) and writes it with platform::WriteFileAtomic, after
 * Provider's CheckOut of an existing file (Perforce keeps it read-only; null: None). Nothing is written when the file
 * already lists Step. The file lies outside every mount, so it is no step of an op (OpJournal.h): a crash before this
 * leaves the root behind, and the step, run again, skips what it did and then records itself. False with OutError.
 */
[[nodiscard]] FRT_CORE_API bool RecordMigration (const std::filesystem::path& Root, FStringView Step, IVcsProvider* Provider,
	FString* OutError = nullptr);

/**
 * Root behind this engine (§14.8): its file is missing, cannot be read, or lacks a step this engine requires. The MIG001
 * the hosts and `validate` report, an error with Subject RootMountPath ("/Project", "/Engine"), File MigrationsFile (Line
 * and Column where the file is malformed), and a message that names the steps and the command that brings it up to date
 * (`FRT.bat assets migrate --all`, with --engine for bEngine). nullopt: Root is up to date.
 */
[[nodiscard]] FRT_CORE_API std::optional<SDiagnostic> FindMigrationGap (const std::filesystem::path& Root, FStringView RootMountPath,
	bool bEngine);

/**
 * The MIG001 gate's switch, [Assets] MigrationGate, MigrationGateDefault when no layer sets it. Off until H4b: the
 * engine and both templates have no Schemas/Migrations.yml before then, so every one of them would open read-only.
 * H4b commits each repository's file and then turns MigrationGateDefault on; -Assets:MigrationGate=1 turns the gate on
 * for one run (the tests of AssetTool's gate), =0 off. Only the hosts read it: `validate` reports MIG001 either way.
 */
inline constexpr bool MigrationGateDefault = false;
[[nodiscard]] FRT_CORE_API bool IsMigrationGateOn (const config::CConfig& Config);
}
