#include "Assets/Meta/Validation.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/Migrations.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Pipeline/Rules.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/StringMap.h"
#include "Platform/FileSystem.h"
#include "Reflect/DataTree.h"


namespace frt::assets
{
namespace
{
// The hashed FRT text formats a source can be (§13.3), besides metas (the lifecycle's) and rule files (read apart).
constexpr FStringLiteral HashedTextExtensions[] = { ".yml", ".frt", ".csv", ".ini", ".frtinputmap", ".frtinputaction" };

[[nodiscard]] bool IsHashedText (FStringView MountPath) noexcept
{
	const FStringView leaf = GetMountPathLeaf(MountPath);
	return std::any_of(std::begin(HashedTextExtensions), std::end(HashedTextExtensions),
		[&] (FStringLiteral Extension) { return leaf.Len() > Extension.Len() && leaf.EndsWith<ESearchCase::IgnoreCase>(Extension); });
}

[[nodiscard]] bool HasCrLf (FStringView Bytes) noexcept
{
	return Bytes.Find("\r\n") != FStringView::NotFound;
}

// A file's bytes from one unchanged version of it; nullopt when it cannot be read or changed meanwhile (the next run sees it).
[[nodiscard]] std::optional<FString> ReadStable (const std::filesystem::path& Path)
{
	FString bytes;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&bytes] (std::span<const uint8> Chunk)
	{
		bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), static_cast<uint32>(Chunk.size()));
	});
	return read == platform::EStableRead::Stable ? std::optional<FString>(std::move(bytes)) : std::nullopt;
}

// A meta's source, any other path itself: a meta and its asset are one subject for the paths a run is limited to.
[[nodiscard]] FStringView GetAssetOf (FStringView Path) noexcept
{
	return GetMetaSource(Path).value_or(Path);
}

[[nodiscard]] bool IsTracked (const SVcsStatus& Status) noexcept
{
	return Status.State != EVcsState::Untracked;
}

class CValidation
{
public:
	CValidation (const CRegistryView& InView, const SValidateOptions& InOptions)
		: View(InView)
		, Options(InOptions)
		, Mounts(*InOptions.Mounts)
	{
		const std::span<const SMount> mounts = Mounts.GetMounts();
		InScopeMount.assign(mounts.size(), Options.Scope.empty() ? 1u : 0u);
		for (const MountId mount : Options.Scope)
		{
			if (mount < InScopeMount.size())
			{
				InScopeMount[mount] = 1u;
			}
		}
		Providers.assign(mounts.size(), nullptr);
		for (const auto& [mount, provider] : Options.Providers)
		{
			if (mount < Providers.size() && provider != nullptr && provider->GetName() != "None")
			{
				Providers[mount] = provider;
			}
		}
		if (Options.Paths.has_value())
		{
			for (const FString& path : *Options.Paths)
			{
				FoldedPaths.push_back(FString(GetAssetOf(path)).FoldCase());
			}
		}
	}

	void Run (std::vector<SDiagnostic>& Out)
	{
		std::vector<SDiagnostic> found;
		View.ForEachProblem([&] (FStringView Subject, EProblemSource, const SDiagnostic& Diagnostic)
		{
			const FStringView subject = Diagnostic.Subject.IsEmpty() ? Subject : Diagnostic.Subject.View();
			if (IsInScope(subject))
			{
				found.push_back(Diagnostic);
				found.back().Subject = FString(subject);
			}
		});

		std::vector<SPair> pairs;
		View.ForEachFile([&] (uint32 Row)
		{
			SFileData file;
			if (!View.GetFile(Row, file) || !IsInScope(file.MountPath))
			{
				return;
			}
			if (file.Class == EFileClass::Meta)
			{
				CheckMeta(file);
			}
			else
			{
				if (IsHashedText(file.MountPath) && (!Options.bQuick || Options.Paths.has_value()))
				{
					CheckText(file.MountPath, file.MountPath);
				}
				if (GetsMeta(file.Class) && (file.MetaState == EMetaState::Paired || file.MetaState == EMetaState::Broken))
				{
					AddPair(file, pairs);
				}
			}
		});
		CheckRuleFiles();
		CheckTracking(pairs);
		CheckMigrations();
		ApplyValidation();

		// The registry's first: a V-only rule's finding of what the lifecycle already reported (VCS001 of a meta, VCS003 of a
		// meta it created) is the same problem.
		found.insert(found.end(), std::make_move_iterator(Own.begin()), std::make_move_iterator(Own.end()));
		FStringSet seen;
		for (SDiagnostic& problem : found)
		{
			FString key = FString::Format("{:08x}\x1F{}\x1F{}", problem.Code, problem.Subject, problem.FieldPath);
			if (problem.Code == 0u)
			{
				key += '\x1F';
				key += problem.Message;
			}
			if (seen.insert(std::move(key)).second)
			{
				Out.push_back(std::move(problem));
			}
		}
		std::stable_sort(Out.begin(), Out.end(), [] (const SDiagnostic& A, const SDiagnostic& B)
		{
			if (A.Subject != B.Subject)
			{
				return LessPortableThenBytes(A.Subject, B.Subject);
			}
			if (A.Code != B.Code)
			{
				return A.Code < B.Code;
			}
			return A.FieldPath.View() < B.FieldPath.View();
		});
	}

private:
	struct SPair
	{
		MountId Mount = InvalidMountId;
		FString Source;    // mount paths
		FString Meta;
	};

	const CRegistryView& View;
	const SValidateOptions& Options;
	const CMountTable& Mounts;
	std::vector<uint8> InScopeMount;
	std::vector<IVcsProvider*> Providers;
	std::vector<FString> FoldedPaths;
	std::vector<SDiagnostic> Own;

	[[nodiscard]] const SMount* MountOf (FStringView Subject) const noexcept
	{
		return Mounts.Find(GetMountNameOf(Subject));
	}

	[[nodiscard]] bool IsInMountScope (FStringView Subject) const noexcept
	{
		const SMount* mount = MountOf(Subject);
		return mount != nullptr && mount->Id < InScopeMount.size() && InScopeMount[mount->Id] != 0u;
	}

	[[nodiscard]] bool IsInPaths (FStringView Subject) const
	{
		if (!Options.Paths.has_value())
		{
			return true;
		}
		const FString folded = FString(GetAssetOf(Subject)).FoldCase();
		return std::any_of(FoldedPaths.begin(), FoldedPaths.end(), [&] (const FString& Path) { return IsMountPathAtOrBelow(folded, Path, true); });
	}

	[[nodiscard]] bool IsInScope (FStringView Subject) const
	{
		return IsInMountScope(Subject) && IsInPaths(Subject);
	}

	// A V-only problem about Subject (a file's or folder's mount path), File per the diagnostics convention.
	SDiagnostic& Add (ESeverity Severity, uint32 Code, FStringView Subject, FString Message, FStringView FieldPath = {})
	{
		SDiagnostic& problem = Own.emplace_back();
		problem.Severity = Severity;
		problem.Code = Code;
		problem.Subject = FString(Subject);
		problem.FieldPath = FString(FieldPath);
		problem.Message = std::move(Message);
		FStringView mountName;
		FStringView relative;
		if (const SMount* mount = MountOf(Subject); mount != nullptr && SplitMountPath(Subject, mountName, relative) && !relative.IsEmpty())
		{
			problem.File = mount->GetSourceFile(relative);
		}
		return problem;
	}

	// ---- Metas ----

	void CheckMeta (const SFileData& Meta)
	{
		const std::optional<FStringView> source = GetMetaSource(Meta.MountPath);
		if (!source.has_value())
		{
			return;
		}
		std::optional<SFileData> sourceFile;
		if (const std::optional<uint32> row = View.FindFile(*source); row.has_value())
		{
			SFileData data;
			if (View.GetFile(*row, data) && data.Class != EFileClass::Meta)
			{
				sourceFile = data;
			}
		}
		if (sourceFile.has_value() && !GetsMeta(sourceFile->Class))
		{
			Add(ESeverity::Warning, Meta007, Meta.MountPath, FString::Format("'{}' is the meta of a {} file, which takes no meta: delete the meta, "
				"or claim the file in a rule", ToDisplayText(Meta.MountPath), enum_::ToString(sourceFile->Class)));
		}
		else if (!sourceFile.has_value())
		{
			const std::optional<std::filesystem::path> path = Mounts.FromMountPath(*source);
			const std::optional<platform::SFileStat> stat = path.has_value() ? platform::Stat(*path) : std::nullopt;
			if (stat.has_value() && !stat->bDirectory)
			{
				Add(ESeverity::Warning, Meta007, Meta.MountPath, FString::Format("'{}' is the meta of a file the rules exclude: delete the meta, "
					"or include the file", ToDisplayText(Meta.MountPath)));
			}
			return;   // an orphan otherwise: META002 is the lifecycle's
		}

		const std::optional<std::filesystem::path> path = Mounts.FromMountPath(Meta.MountPath);
		const std::optional<FString> bytes = path.has_value() ? ReadStable(*path) : std::nullopt;
		if (!bytes.has_value())
		{
			return;
		}
		SMetaParseResult parsed;
		ParseMeta(*bytes, parsed);
		if (parsed.State != EMetaParseState::Ok)
		{
			return;   // META004, the lifecycle's
		}
		if (!parsed.bCanonical)
		{
			Add(ESeverity::Warning, Meta005, Meta.MountPath, FString::Format("'{}' is not canonical: `resave --metas` rewrites it",
				ToDisplayText(Meta.MountPath)));
		}
		for (const SDiagnostic& diagnostic : parsed.Diagnostics)
		{
			if (diagnostic.Code == 0u)
			{
				SDiagnostic& problem = Add(ESeverity::Warning, 0u, Meta.MountPath, diagnostic.Message, diagnostic.FieldPath);
				problem.Line = diagnostic.Line;
				problem.Column = diagnostic.Column;
			}
		}
		if (sourceFile.has_value())
		{
			CheckSettings(Meta.MountPath, *sourceFile, parsed.Meta);
		}
	}

	// What ResolveFile leaves out of a meta: settings the effective importer has no field for or that are no value of
	// theirs, and a pin this build does not know.
	void CheckSettings (FStringView MetaPath, const SFileData& Source, const SAssetMeta& Meta)
	{
		if (!Meta.Importer.IsEmpty() && FindImporter(Meta.Importer) == nullptr)
		{
			Add(ESeverity::Warning, 0u, MetaPath, FString::Format("Importer: {} is no importer this build knows: the rules decide",
				Meta.Importer), "Importer");
		}
		const SImporterDesc* importer = Source.Record.IsValid() ? FindImporter(View.GetImporterName(Source.Record)) : nullptr;
		if (importer == nullptr)
		{
			return;
		}
		const auto check = [&] (reflect::HDataNode Map, FStringView Prefix)
		{
			if (!Map.IsMap())
			{
				return;
			}
			for (const reflect::SDataChild& child : Map.GetChildren())
			{
				const FString fieldPath = FString::Format("{}.{}", Prefix, child.Key);
				const int32 field = importer->Settings != nullptr ? FindSettingsField(*importer->Settings, child.Key) : -1;
				if (field < 0)
				{
					SDiagnostic& problem = Add(ESeverity::Warning, 0u, MetaPath, FString::Format("{} is no setting of the importer {}: it is ignored",
						fieldPath, importer->Name), fieldPath);
					problem.Line = child.KeyLine;
					continue;
				}
				const SSettingsField& definition = importer->Settings->Fields[static_cast<uint32>(field)];
				if (!NormalizeMetaSettingValue(child.Value, definition).has_value())
				{
					SDiagnostic& problem = Add(ESeverity::Warning, 0u, MetaPath, FString::Format("{}: '{}' is no value of {}'s {}: the rules' value "
						"stands", fieldPath, child.Value.GetText(), importer->Name, definition.Name), fieldPath);
					problem.Line = child.KeyLine;
				}
			}
		};
		if (!Meta.Settings.IsEmpty())
		{
			check(Meta.Settings.GetRoot(), "Settings");
		}
		if (!Meta.Targets.IsEmpty() && Meta.Targets.GetRoot().IsMap())
		{
			for (const reflect::SDataChild& target : Meta.Targets.GetRoot().GetChildren())
			{
				check(target.Value, FString::Format("Targets.{}", target.Key));
			}
		}
	}

	// ---- Text: CRLF in what the lifecycle does not read ----

	void CheckText (FStringView Subject, FStringView MountPath)
	{
		const std::optional<std::filesystem::path> path = Mounts.FromMountPath(MountPath);
		const std::optional<FString> bytes = path.has_value() ? ReadStable(*path) : std::nullopt;
		if (bytes.has_value() && HasCrLf(*bytes))
		{
			Add(ESeverity::Error, Vcs001, Subject, FString::Format("'{}' has CRLF line endings: its content hash would differ between checkouts. "
				"Give it LF (FRT.bat setup normalises a Git checkout)", ToDisplayText(MountPath)));
		}
	}

	void CheckRuleFiles ()
	{
		View.ForEachRuleFile([&] (uint32 Row)
		{
			SRuleFileData rule;
			if (!View.GetRuleFile(Row, rule))
			{
				return;
			}
			const SMount* mount = Mounts.Find(rule.Mount);
			if (mount == nullptr || rule.Mount >= InScopeMount.size() || InScopeMount[rule.Mount] == 0u)
			{
				return;
			}
			if (rule.Folder == InvalidRow)
			{
				// The mount's Config/Assets.yml lies outside its content: named by the mount root, read when not limited to paths.
				if (Options.Paths.has_value())
				{
					return;
				}
				const std::optional<FString> bytes = ReadStable(mount->RulesFile);
				if (bytes.has_value() && HasCrLf(*bytes))
				{
					SDiagnostic& problem = Add(ESeverity::Error, Vcs001, mount->GetRootMountPath(), FString::Format("'{}' has CRLF line endings: "
						"rule files are hashed. Give it LF (FRT.bat setup normalises a Git checkout)", ToDisplayText(FString::FromPath(mount->RulesFile))));
					problem.File = platform::GetRelativePath(mount->RulesFile, mount->OwnerRoot).value_or(FString::FromPath(mount->RulesFile));
				}
				return;
			}
			const FString subject = MakeChildMountPath(rule.FolderMountPath, RuleFileName);
			if (IsInPaths(subject))
			{
				CheckText(subject, subject);
			}
		});
	}

	// ---- VCS003: one StatusOf per provider over every pair in scope ----

	void AddPair (const SFileData& Source, std::vector<SPair>& Pairs) const
	{
		if (Source.Mount >= Providers.size() || Providers[Source.Mount] == nullptr)
		{
			return;
		}
		FString meta = GetMetaPath(Source.MountPath);
		if (const std::optional<uint32> row = View.FindFile(meta); row.has_value())
		{
			SFileData data;
			if (View.GetFile(*row, data))
			{
				meta = FString(data.MountPath);   // as spelled on disk
			}
		}
		Pairs.push_back(SPair { Source.Mount, FString(Source.MountPath), std::move(meta) });
	}

	void CheckTracking (std::span<const SPair> Pairs)
	{
		std::map<MountId, std::vector<const SPair*>> byMount;
		for (const SPair& pair : Pairs)
		{
			byMount[pair.Mount].push_back(&pair);
		}
		std::map<IVcsProvider*, std::vector<const SPair*>> byProvider;
		for (const auto& [mount, pairs] : byMount)
		{
			std::vector<const SPair*>& list = byProvider[Providers[mount]];
			list.insert(list.end(), pairs.begin(), pairs.end());
		}
		for (const auto& [provider, pairs] : byProvider)
		{
			std::vector<FString> files;
			std::vector<const SPair*> asked;
			for (const SPair* pair : pairs)
			{
				const std::optional<std::filesystem::path> source = Mounts.FromMountPath(pair->Source);
				const std::optional<std::filesystem::path> meta = Mounts.FromMountPath(pair->Meta);
				if (source.has_value() && meta.has_value())
				{
					files.push_back(FString::FromPath(*source));
					files.push_back(FString::FromPath(*meta));
					asked.push_back(pair);
				}
			}
			std::vector<SVcsStatus> status;
			const SVcsResult result = provider->StatusOf(files, status);
			if (result.Error != EVcsError::None || status.size() != files.size())
			{
				continue;   // nothing is known: no finding
			}
			for (size_t i = 0u; i < asked.size(); ++i)
			{
				const bool bSource = IsTracked(status[2u * i]);
				const bool bMeta = IsTracked(status[2u * i + 1u]);
				if (bSource == bMeta)
				{
					continue;
				}
				const FString command = provider->GetAddCommand(files[2u * i + (bSource ? 1u : 0u)]);
				const FString hint = command.IsEmpty() ? FString() : FString::Format(" ({})", command);
				SDiagnostic& problem = Add(ESeverity::Warning, Vcs003, asked[i]->Source, bSource
					? FString::Format("generated locally: commit it. '{}' is tracked and its meta is not{}", ToDisplayText(asked[i]->Source), hint)
					: FString::Format("'{}' is not tracked and its meta is: add it{}", ToDisplayText(asked[i]->Source), hint));
				// File names the half the VCS does not track, the one to add (the problems panel's fix-it).
				FStringView mountName;
				FStringView relative;
				const FStringView untracked = bSource ? asked[i]->Meta.View() : asked[i]->Source.View();
				if (const SMount* mount = MountOf(untracked); mount != nullptr && SplitMountPath(untracked, mountName, relative))
				{
					problem.File = mount->GetSourceFile(relative);
				}
			}
		}
	}

	// ---- MIG001: each repository root of the scope ----

	void CheckMigrations ()
	{
		std::vector<const SMount*> roots;   // the first mount of each owner root, /Project before its plugins
		for (const SMount& mount : Mounts.GetMounts())
		{
			if (InScopeMount[mount.Id] == 0u)
			{
				continue;
			}
			const bool bKnown = std::any_of(roots.begin(), roots.end(), [&] (const SMount* Root) { return Root->OwnerRoot == mount.OwnerRoot; });
			if (!bKnown)
			{
				roots.push_back(&mount);
			}
		}
		if (Options.Paths.has_value())
		{
			// Limited (--staged, or paths named): a root is behind for a run that asks for something of its own mounts. A commit
			// of code alone stages no content, so the hook's check 6 lets it through; content into a root behind is refused.
			std::erase_if(roots, [&] (const SMount* Root)
			{
				return std::none_of(Options.Paths->begin(), Options.Paths->end(), [&] (const FString& Path)
				{
					const SMount* mount = MountOf(Path);
					return mount != nullptr && mount->OwnerRoot == Root->OwnerRoot && IsInMountScope(Path);
				});
			});
		}
		for (const SMount* mount : roots)
		{
			// The hosts' gate reads the same (Migrations.h), so `validate` and a refused run say one thing.
			std::optional<SDiagnostic> gap = FindMigrationGap(mount->OwnerRoot, mount->GetRootMountPath(), mount->Kind == EMountKind::Engine);
			if (!gap.has_value())
			{
				continue;
			}
			SDiagnostic& problem = Add(gap->Severity, gap->Code, gap->Subject, std::move(gap->Message));
			problem.File = std::move(gap->File);
			problem.Line = gap->Line;
			problem.Column = gap->Column;
		}
	}

	// ---- The rule files' Validation: severities, per folder ----

	void ApplyValidation ()
	{
		if (Options.Rules == nullptr)
		{
			return;
		}
		TStringMap<std::shared_ptr<const SScanScope>> scopes;
		for (SDiagnostic& problem : Own)
		{
			if (problem.Code == 0u)
			{
				continue;
			}
			const FStringView asset = GetAssetOf(problem.Subject);
			const FStringView parent = GetMountPathParent(asset);
			const FString folder(parent.IsEmpty() ? asset : parent);
			auto found = scopes.find(folder);
			if (found == scopes.end())
			{
				std::vector<SDiagnostic> ignored;
				found = scopes.emplace(folder, Options.Rules->OpenFolderScope(folder, ignored)).first;
			}
			if (found->second != nullptr)
			{
				Options.Rules->ApplyValidation(*found->second, problem);
			}
		}
	}
};
}


void RunValidation (const CRegistryView& View, const SValidateOptions& Options, std::vector<SDiagnostic>& Out)
{
	if (Options.Mounts == nullptr)
	{
		return;
	}
	CValidation(View, Options).Run(Out);
}

SValidationCounts CountValidation (std::span<const SDiagnostic> Problems) noexcept
{
	SValidationCounts counts;
	for (const SDiagnostic& problem : Problems)
	{
		switch (problem.Severity)
		{
		case ESeverity::Error: ++counts.Errors; break;
		case ESeverity::Warning: ++counts.Warnings; break;
		case ESeverity::Info: ++counts.Infos; break;
		}
	}
	return counts;
}
}
