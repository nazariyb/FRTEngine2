#include "Assets/Meta/ProblemList.h"

#include <algorithm>
#include <map>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Registry/QueryEngine.h"
#include "Assets/Registry/RegistryRecords.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
namespace
{
// The mount of a mount path, by its name.
[[nodiscard]] const SMount* FindMountOf (const CMountTable& Mounts, FStringView MountPath) noexcept
{
	FStringView mountName;
	FStringView relative;
	return SplitMountPath(MountPath, mountName, relative) ? Mounts.Find(mountName) : nullptr;
}

// A query value in double quotes, as AssetQuery reads it back: \" and \\ escaped.
void AppendQuoted (FString& Out, FStringView Text)
{
	Out += '"';
	for (const char ch : Text)
	{
		if (ch == '"' || ch == '\\')
		{
			Out += '\\';
		}
		Out += ch;
	}
	Out += '"';
}

// The asset of Subject in View: a meta's source; a sub-entry's file ("<file>#<label>", when the whole is no file of View and
// the part before its last '#' is); else Subject.
[[nodiscard]] FStringView FindAsset (const CRegistryView& View, FStringView Subject)
{
	const FStringView asset = GetProblemAsset(Subject);
	if (View.FindFile(asset).has_value())
	{
		return asset;
	}
	const uint32 hash = asset.FindLast('#');
	if (hash != FStringView::NotFound && hash > 0u && View.FindFile(asset.Left(hash)).has_value())
	{
		return asset.Left(hash);
	}
	return asset;
}

struct SPortableThenBytes
{
	bool operator() (const FString& A, const FString& B) const noexcept { return LessPortableThenBytes(A, B); }
};

[[nodiscard]] bool FoldEqual (FStringView A, FStringView B) noexcept
{
	return SStringEqualIgnoreCase {}(A, B);
}

[[nodiscard]] FString DescribeGuids (std::span<const SGuid> Guids)
{
	FString text;
	for (const SGuid& guid : Guids)
	{
		text += text.IsEmpty() ? "{" : ", {";
		text += guid.ToString();
		text += '}';
	}
	return text;
}

[[nodiscard]] FString DisplayLeaf (FStringView MountPath)
{
	return unicode::ToDisplayText(GetMountPathLeaf(MountPath));
}

// What a CreateMeta gives its file, from the identity the registry stored for it (not from the problem's code: a refused
// create's VCS002 asks the same).
enum class ECreateKind : uint8
{
	First,     // the file never had a meta-given GUID (none, PendingMeta, provisional): META001, or its refusal retried
	Again,     // the identity it had: a lost meta (META008)
	NewGuid,   // another identity: a meta swap's old source, whose GUID now lives in another meta (META008)
};

[[nodiscard]] ECreateKind ClassifyCreate (const SLifecycleAction& Action, const CRegistryView* View)
{
	const std::optional<SStoredFile> stored = View != nullptr ? FindStoredFile(*View, Action.Path) : std::nullopt;
	if (!stored.has_value() || !HasMetaGivenGuid(*stored, Action.Path))
	{
		return ECreateKind::First;
	}
	return Action.Guid.IsValid() && stored->Guid == Action.Guid ? ECreateKind::Again : ECreateKind::NewGuid;
}

// The button of a lifecycle action, and what it does.
void DescribeAction (const SLifecycleAction& Action, const SDiagnostic& Problem, const CRegistryView* View, SProblemFixIt& Out)
{
	switch (Action.Kind)
	{
	case ELifecycleAction::CreateMeta:
	{
		const ECreateKind kind = Problem.Code == Meta001 ? ECreateKind::First : ClassifyCreate(Action, View);
		const FString guid = Action.Guid.IsValid() ? FString::Format("GUID {{{}}}", Action.Guid.ToString()) : FString("a random GUID");
		if (kind == ECreateKind::First)
		{
			Out.Label = "Create its meta";
			Out.Text = FString::Format("writes {}.meta with {}", DisplayLeaf(Action.Path), guid);
		}
		else if (kind == ECreateKind::Again)
		{
			Out.Label = "Create its meta again";
			Out.Text = FString::Format("writes a new {}.meta with its own {}: the settings the lost meta held are gone", DisplayLeaf(Action.Path), guid);
		}
		else
		{
			Out.Label = "Give it a new GUID";
			Out.Text = FString::Format("writes a new {}.meta with {}: a new identity, since its old meta now belongs to another file",
				DisplayLeaf(Action.Path), guid);
		}
		break;
	}
	case ELifecycleAction::RestoreMeta:
		Out.Label = "Restore the verified copy";
		Out.Text = FString::Format("writes the registry's verified copy of {}.meta back, byte for byte", DisplayLeaf(Action.Path));
		break;
	case ELifecycleAction::MoveMeta:
		Out.Label = FString::Format("Relink {}", DisplayLeaf(Action.Path));
		Out.Text = FString::Format("moves {} to {}: the file takes that meta's identity", unicode::ToDisplayText(Action.Path),
			unicode::ToDisplayText(Action.ToPath));
		break;
	case ELifecycleAction::FixMetaCase:
		Out.Label = "Rename the meta to its source's case";
		Out.Text = FString::Format("renames {} to {}", unicode::ToDisplayText(Action.Path), unicode::ToDisplayText(Action.ToPath));
		break;
	case ELifecycleAction::ReGuid:
	{
		const SGuid old = Action.Remap.empty() ? SGuid {} : Action.Remap.front().first;
		Out.Label = "Give the copy a new GUID";
		Out.Text = FString::Format("re-GUIDs every copy of {{{}}} as one set, their references to each other remapped; {} gets {{{}}}",
			old.ToString(), unicode::ToDisplayText(Action.Path), Action.Guid.IsValid() ? Action.Guid.ToString() : FString("a random GUID"));
		break;
	}
	case ELifecycleAction::KeepAliasB:
		Out.Label = FString::Format("Keep {{{}}}", Action.Guid.ToString());
		Out.Text = FString::Format("rewrites {} with GUID {{{}}}, and {} as its aliases", unicode::ToDisplayText(Action.Path),
			Action.Guid.ToString(), DescribeGuids(Action.Aliases));
		break;
	}
	if (Problem.Code == Vcs002)
	{
		Out.Label = "Retry: " + Out.Label;
	}
}

// The command that fixes /Engine from its own checkout, for a process that does not write it (edit [14]).
[[nodiscard]] FString MakeEngineFixCommand (const SMount& Engine)
{
	return FString::Format("\"{}\" assets fix --engine", FString::FromPath((Engine.OwnerRoot / "FRT.bat").make_preferred()));
}

// A META008 whose meta was lost, as the lifecycle's own fix-its say: it brings the identity back (the verified copy, or a
// meta created with the stored GUID). A meta swap (its GUID lives in another meta now) and a new file an orphan matches get
// neither, and a VCS restore would duplicate a GUID or name a file the VCS never had.
[[nodiscard]] bool IsLostMeta (std::span<const SProblemFixIt> FixIts, const CRegistryView* View)
{
	return std::any_of(FixIts.begin(), FixIts.end(), [&] (const SProblemFixIt& Fix)
	{
		return Fix.Kind == EProblemFixKind::Apply && (Fix.Action.Kind == ELifecycleAction::RestoreMeta
			|| (Fix.Action.Kind == ELifecycleAction::CreateMeta && ClassifyCreate(Fix.Action, View) == ECreateKind::Again));
	});
}

[[nodiscard]] const SProblemEntry* FindRefusalFor (std::span<const SProblemEntry> Entries, FStringView Asset) noexcept
{
	const auto found = std::find_if(Entries.begin(), Entries.end(), [&] (const SProblemEntry& Entry)
	{
		return Entry.Problem.Code == Vcs002 && FoldEqual(Entry.Asset, Asset);
	});
	return found != Entries.end() ? &*found : nullptr;
}
}


FStringView GetProblemAsset (FStringView Subject) noexcept
{
	return GetMetaSource(Subject).value_or(Subject);
}

SValidateOptions MakeProblemListOptions (CAssetRegistry& Registry)
{
	SValidateOptions options;
	options.Mounts = &Registry.GetMounts();
	options.Rules = &Registry.GetRules();
	if (const CAssetRepairs* repairs = Registry.GetRepairs(); repairs != nullptr)
	{
		for (const SMount& mount : options.Mounts->GetMounts())
		{
			if (IVcsProvider* provider = repairs->GetProvider(mount.Id); provider != nullptr)
			{
				options.Providers.emplace_back(mount.Id, provider);
			}
		}
	}
	return options;
}

std::vector<SProblemEntry> ListAssetProblems (const CRegistryView& View, const SValidateOptions& Options)
{
	std::vector<SDiagnostic> problems;
	RunValidation(View, Options, problems);
	std::vector<SProblemEntry> entries;
	entries.reserve(problems.size());
	for (SDiagnostic& problem : problems)
	{
		SProblemEntry& entry = entries.emplace_back();
		entry.Asset = FString(FindAsset(View, problem.Subject));
		entry.bFolder = View.FindFolder(entry.Asset).has_value();
		if (!entry.bFolder)
		{
			if (const std::optional<SRecordId> record = View.FindByPath(entry.Asset); record.has_value())
			{
				entry.Type = FormatColumnValues(View, *record, "$Type");
			}
		}
		entry.Problem = std::move(problem);
	}
	return entries;
}

bool SProblemFilter::Matches (const SProblemEntry& Entry) const
{
	const ESeverity severity = Entry.Problem.Severity;
	if ((severity == ESeverity::Error && !bErrors) || (severity == ESeverity::Warning && !bWarnings) || (severity == ESeverity::Info && !bInfos))
	{
		return false;
	}
	if (!Code.IsEmpty() && !FormatDiagnosticCode(Entry.Problem.Code).View().StartsWith<ESearchCase::IgnoreCase>(Code.View().TrimStartAndEnd()))
	{
		return false;
	}
	if (!Type.IsEmpty() && !FoldEqual(Entry.Type, Type))
	{
		return false;
	}
	if (!Folder.IsEmpty())
	{
		FStringView folder = Folder.View().TrimStartAndEnd();
		while (folder.Len() > 1u && folder.EndsWith('/'))
		{
			folder = folder.LeftChop(1u);
		}
		if (!IsMountPathAtOrBelow(Entry.Asset.FoldCase(), FString(folder).FoldCase()))
		{
			return false;
		}
	}
	return true;
}

std::vector<SProblemGroup> GroupProblems (std::span<const SProblemEntry> Entries, const SProblemFilter& Filter)
{
	// Keyed by the asset as spelled: two spellings of one path under the fold are two files on a case-sensitive mount.
	std::map<FString, SProblemGroup, SPortableThenBytes> groups;
	for (uint32 i = 0u; i < Entries.size(); ++i)
	{
		const SProblemEntry& entry = Entries[i];
		if (!Filter.Matches(entry))
		{
			continue;
		}
		SProblemGroup& group = groups[entry.Asset];
		if (group.Entries.empty())
		{
			group.Asset = entry.Asset;
			group.Type = entry.Type;
			group.Worst = entry.Problem.Severity;
		}
		group.Worst = std::max(group.Worst, entry.Problem.Severity);
		group.Entries.push_back(i);
	}
	std::vector<SProblemGroup> out;
	out.reserve(groups.size());
	for (auto& [asset, group] : groups)
	{
		out.push_back(std::move(group));
	}
	return out;
}

std::vector<FString> GetProblemTypes (std::span<const SProblemEntry> Entries)
{
	std::vector<FString> types;
	for (const SProblemEntry& entry : Entries)
	{
		if (!entry.Type.IsEmpty() && std::none_of(types.begin(), types.end(), [&] (const FString& Type) { return FoldEqual(Type, entry.Type); }))
		{
			types.push_back(entry.Type);
		}
	}
	std::sort(types.begin(), types.end(), [] (const FString& A, const FString& B) { return LessPortableThenBytes(A, B); });
	return types;
}

FString MakeGoToQuery (const SProblemEntry& Entry)
{
	FString query;
	if (Entry.bFolder)
	{
		query = "dir:";
		AppendQuoted(query, Entry.Asset);
	}
	else if (!Entry.Type.IsEmpty())
	{
		query = "$Path=";
		AppendQuoted(query, Entry.Asset);
	}
	else
	{
		const FStringView parent = GetMountPathParent(Entry.Asset);
		query = "dir=";
		AppendQuoted(query, parent.IsEmpty() ? Entry.Asset.View() : parent);
	}
	return query;
}

std::optional<std::filesystem::path> GetProblemFilePath (const SDiagnostic& Problem, const CMountTable& Mounts)
{
	const SMount* mount = FindMountOf(Mounts, Problem.Subject);
	if (mount != nullptr && !Problem.File.IsEmpty())
	{
		return (mount->OwnerRoot / Problem.File.ToPath()).lexically_normal();
	}
	return Mounts.FromMountPath(GetProblemAsset(Problem.Subject));
}

std::vector<SProblemFixIt> GetProblemFixIts (const SDiagnostic& Problem, const SProblemFixContext& Context)
{
	std::vector<SProblemFixIt> fixIts;
	if (Context.Mounts == nullptr)
	{
		return fixIts;
	}
	const CMountTable& mounts = *Context.Mounts;
	const SMount* mount = FindMountOf(mounts, Problem.Subject);
	const FStringView asset = GetProblemAsset(Problem.Subject);

	// The lifecycle's own actions, each disabled where this process writes nothing.
	if (Context.Repairs != nullptr)
	{
		for (SLifecycleAction& action : Context.Repairs->GetFixIts(Problem))
		{
			SProblemFixIt& fix = fixIts.emplace_back();
			fix.Kind = EProblemFixKind::Apply;
			DescribeAction(action, Problem, Context.View, fix);
			const SMount* actionMount = FindMountOf(mounts, action.Path);
			if (actionMount == nullptr || Context.Repairs->GetWriterOf(actionMount->Id) == nullptr)
			{
				fix.Disabled = FString::Format("this process does not write /{}: it is read-only here, a guest, or another process owns it",
					actionMount != nullptr ? actionMount->Name.View() : FStringView("?"));
			}
			fix.Action = std::move(action);
		}
	}

	IVcsProvider* provider = Context.Repairs != nullptr && mount != nullptr ? Context.Repairs->GetProvider(mount->Id) : nullptr;
	if (Problem.Code == Meta001 && mount != nullptr && mount->Kind == EMountKind::Engine
		&& (Context.Repairs == nullptr || Context.Repairs->GetWriterOf(mount->Id) == nullptr))
	{
		SProblemFixIt& fix = fixIts.emplace_back();
		fix.Kind = EProblemFixKind::Copy;
		fix.Label = "Fix it from the engine";
		fix.Text = MakeEngineFixCommand(*mount);
	}
	else if (Problem.Code == Meta008 && provider != nullptr && IsLostMeta(fixIts, Context.View))
	{
		if (const std::optional<std::filesystem::path> meta = mounts.FromMountPath(GetMetaPath(asset)); meta.has_value())
		{
			std::filesystem::path file = *meta;
			if (FString command = provider->GetRestoreCommand(FString::FromPath(file.make_preferred())); !command.IsEmpty())
			{
				SProblemFixIt& fix = fixIts.emplace_back();
				fix.Kind = EProblemFixKind::Copy;
				fix.Label = FString::Format("Restore from {}", provider->GetName());
				fix.Text = std::move(command);
			}
		}
	}
	else if (Problem.Code == Vcs003 && provider != nullptr && mount != nullptr && !Problem.File.IsEmpty())
	{
		// File names the half the VCS does not track (Validation.h).
		std::filesystem::path file = (mount->OwnerRoot / Problem.File.ToPath()).lexically_normal();
		if (FString command = provider->GetAddCommand(FString::FromPath(file.make_preferred())); !command.IsEmpty())
		{
			SProblemFixIt& fix = fixIts.emplace_back();
			fix.Kind = EProblemFixKind::Copy;
			fix.Label = FString::Format("Add it to {}", provider->GetName());
			fix.Text = std::move(command);
		}
	}
	else if (Problem.Code == Vcs004)
	{
		SProblemFixIt& fix = fixIts.emplace_back();
		fix.Kind = EProblemFixKind::Note;
		fix.Label = "A Git LFS pointer";
		fix.Text = "the file is a Git LFS pointer, not its content: `git lfs pull` fetches it, and it gets its meta then";
	}
	return fixIts;
}

SPendingFix MakePendingFix (const SDiagnostic& Problem, FStringView Label, uint64 Ticket)
{
	SPendingFix fix;
	fix.Code = Problem.Code;
	fix.Subject = Problem.Subject;
	fix.Label = FString(Label);
	fix.Ticket = Ticket;
	return fix;
}

EFixOutcome ResolvePendingFix (SPendingFix& Fix, EFixResult Result, FStringView ResultMessage, std::span<const SProblemEntry> Entries,
	uint64 ListMs, uint64 NowMs, FString* OutMessage)
{
	const auto say = [&] (FStringView Message)
	{
		if (OutMessage != nullptr)
		{
			*OutMessage = FString(Message);
		}
	};
	switch (Result)
	{
	case EFixResult::Queued:
		return EFixOutcome::Pending;   // however long: it waits for a quiet registry, and nothing has run yet
	case EFixResult::Refused:
	case EFixResult::NotApplied:
		if (!ResultMessage.IsEmpty())
		{
			say(ResultMessage);
		}
		else if (const SProblemEntry* refusal = FindRefusalFor(Entries, GetProblemAsset(Fix.Subject)); refusal != nullptr)
		{
			say(refusal->Problem.Message);
		}
		return Result == EFixResult::Refused ? EFixOutcome::Refused : EFixOutcome::NotApplied;
	case EFixResult::Forgotten:
		say("its outcome is no longer known");
		return EFixOutcome::NoChange;
	case EFixResult::Applied:
		break;
	}
	if (Fix.DoneMs == 0u)
	{
		Fix.DoneMs = std::max<uint64>(NowMs, 1u);
	}
	if (ListMs < Fix.DoneMs)
	{
		return EFixOutcome::Pending;   // a list made before the op ran says nothing about it
	}
	const bool bStill = std::any_of(Entries.begin(), Entries.end(), [&] (const SProblemEntry& Entry)
	{
		return Entry.Problem.Code == Fix.Code && Entry.Problem.Subject == Fix.Subject;
	});
	if (!bStill)
	{
		return EFixOutcome::Fixed;
	}
	if (ListMs >= Fix.DoneMs + FixOutcomeTimeoutMs)
	{
		say("applied, but the problem is still reported");
		return EFixOutcome::NoChange;
	}
	return EFixOutcome::Pending;
}
}
