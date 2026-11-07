#include "Assets/Meta/AssetRepairs.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Registry/RegistryView.h"
#include "Platform/FileSystem.h"
#include "Unicode/DisplayText.h"


/**
 * PlanRepairOp (AssetRepairs.h): the lifecycle's actions as ops of the asset writer. Each plan reads what it needs from the
 * disk and the view and refuses with reasons rather than half-planning, as M4's commands do (AssetCommands.h); the writer
 * then checks out, journals and writes. What a repair writes is always canonical (EmitMeta), except a restore, which puts
 * back the verified bytes as they were (§4.6 "byte for byte").
 */
namespace frt::assets
{
namespace
{
[[nodiscard]] SDiagnostic MakeProblem (const CAssetWriter& Writer, FStringView Subject, FString Message)
{
	SDiagnostic problem;
	problem.Severity = ESeverity::Error;
	problem.Subject = FString(Subject);
	FStringView mountName;
	FStringView relative;
	if (Writer.GetMounts() != nullptr && SplitMountPath(Subject, mountName, relative))
	{
		if (const SMount* mount = Writer.GetMounts()->Find(mountName); mount != nullptr)
		{
			problem.File = mount->GetSourceFile(relative);
		}
	}
	problem.Message = std::move(Message);
	return problem;
}

// The asset a meta path is about (problems name the asset).
[[nodiscard]] FStringView GetAssetOf (FStringView Path) noexcept
{
	return GetMetaSource(Path).value_or(Path);
}

[[nodiscard]] SAssetOp MakeOp (FStringView Verb)
{
	SAssetOp op;
	op.Id = SGuid::NewRandom();
	op.Verb = FString(Verb);
	return op;
}

[[nodiscard]] SAssetOpStep MakeStep (EAssetOpStep Kind, FStringView Path, FStringView ToPath = FStringView())
{
	SAssetOpStep step;
	step.Kind = Kind;
	step.Path = FString(Path);
	step.ToPath = FString(ToPath);
	return step;
}

[[nodiscard]] SAssetOpStep MakeWrite (FStringView Path, FStringView Text)
{
	SAssetOpStep step = MakeStep(EAssetOpStep::WriteFile, Path);
	step.Bytes.assign(Text.AsBytes().begin(), Text.AsBytes().end());
	return step;
}

// The meta at MountPath, read race-free and parsed; nullopt when it cannot be read (gone, or changing while read).
[[nodiscard]] std::optional<SMetaParseResult> ReadMeta (const CAssetWriter& Writer, FStringView MountPath)
{
	const std::optional<std::filesystem::path> path = Writer.GetMounts() != nullptr ? Writer.GetMounts()->FromMountPath(MountPath) : std::nullopt;
	if (!path.has_value())
	{
		return std::nullopt;
	}
	FString bytes;
	const platform::EStableRead read = platform::ReadFileChunksStable(*path, [&bytes] (std::span<const uint8> Chunk)
	{
		bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), static_cast<uint32>(Chunk.size()));
	});
	if (read != platform::EStableRead::Stable)
	{
		return std::nullopt;
	}
	SMetaParseResult parsed;
	ParseMeta(bytes, parsed);
	return parsed;
}

[[nodiscard]] bool IsInWriter (const CAssetWriter& Writer, FStringView MountPath)
{
	return Writer.FindProvider(MountPath) != nullptr;
}

/**
 * One `$asset` rewriter for the metas (edit [73]): the tree copied node by node, styles and positions kept, with the value
 * of every `$asset` key that names an old GUID of Map replaced by its new GUID. A GUID is matched in any of the spellings
 * ParseMeta accepts and written canonically.
 */
class CRefRewriter
{
public:
	explicit CRefRewriter (const std::map<SGuid, SGuid>& InMap)
		: Map(InMap)
	{}

	// False (Out untouched) when nothing changed or the copy failed.
	bool Rewrite (const reflect::CDataTree& In, reflect::CDataTree& Out)
	{
		if (In.IsEmpty())
		{
			return false;
		}
		reflect::CDataTreeBuilder builder;
		bChanged = false;
		Copy(builder, In.GetRoot());
		reflect::CDataTree tree;
		if (!bChanged || !builder.Finish(tree))
		{
			return false;
		}
		Out = std::move(tree);
		return true;
	}

private:
	void Copy (reflect::CDataTreeBuilder& Builder, reflect::HDataNode Node)
	{
		if (Node.IsMap())
		{
			Builder.BeginMap(Node.GetLine(), Node.GetColumn(), Node.GetCollectionStyle());
			for (const reflect::SDataChild& child : Node.GetChildren())
			{
				Builder.Key(child.Key, child.KeyLine, child.KeyColumn);
				if (child.Key == FStringView("$asset") && child.Value.IsScalar())
				{
					const std::optional<SGuid> guid = SGuid::Parse(child.Value.GetText());
					const auto found = guid.has_value() ? Map.find(*guid) : Map.end();
					if (found != Map.end())
					{
						bChanged = true;
						Builder.Scalar(found->second.ToString(), reflect::EScalarStyle::Plain, child.Value.GetLine(), child.Value.GetColumn());
						continue;
					}
				}
				Copy(Builder, child.Value);
			}
			Builder.End();
			return;
		}
		if (Node.IsSequence())
		{
			Builder.BeginSequence(Node.GetLine(), Node.GetColumn(), Node.GetCollectionStyle());
			for (const reflect::SDataChild& child : Node.GetChildren())
			{
				Copy(Builder, child.Value);
			}
			Builder.End();
			return;
		}
		Builder.Subtree(Node);
	}

	const std::map<SGuid, SGuid>& Map;
	bool bChanged = false;
};

// Every ReGuid of one result, as one op (LifecycleTypes.h): each loser's meta rewritten with its new GUID and the map.
bool PlanReGuids (const CAssetWriter& Writer, std::span<const SLifecycleAction> Actions, SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp("dedupe");
	// DerivedAutoGuids off: each action's Guid is invalid and its Remap's new GUIDs are stand-ins (the derived values),
	// found through each action's own first pair. Mint one random GUID per action and replace every stand-in with it.
	std::map<SGuid, SGuid> standIns;
	std::vector<SGuid> newGuids;
	for (const SLifecycleAction& action : Actions)
	{
		SGuid guid = action.Guid;
		if (!guid.IsValid())
		{
			guid = SGuid::NewRandom();
			if (!action.Remap.empty())
			{
				standIns[action.Remap.front().second] = guid;
			}
		}
		newGuids.push_back(guid);
	}
	const size_t problemsBefore = OutProblems.size();
	for (size_t i = 0u; i < Actions.size(); ++i)
	{
		const SLifecycleAction& action = Actions[i];
		if (action.Kind != ELifecycleAction::ReGuid || action.Remap.empty() || !IsInWriter(Writer, action.Path))
		{
			OutProblems.push_back(MakeProblem(Writer, GetAssetOf(action.Path), FString::Format("cannot give '{}' a new GUID: not a duplicate "
				"this process writes", ToDisplayText(action.Path))));
			continue;
		}
		std::optional<SMetaParseResult> parsed = ReadMeta(Writer, action.Path);
		const SGuid oldGuid = action.Remap.front().first;
		if (!parsed.has_value() || parsed->State != EMetaParseState::Ok || parsed->Meta.Guid != oldGuid)
		{
			OutProblems.push_back(MakeProblem(Writer, GetAssetOf(action.Path), FString::Format("'{}' no longer holds the duplicate GUID {}: "
				"left as it is", ToDisplayText(action.Path), oldGuid.ToString())));
			continue;
		}
		std::vector<std::pair<SGuid, SGuid>> map;
		for (const auto& [from, to] : action.Remap)
		{
			const auto standIn = standIns.find(to);
			map.emplace_back(from, standIn != standIns.end() ? standIn->second : to);
		}
		SAssetMeta meta = std::move(parsed->Meta);
		meta.Guid = newGuids[i];
		(void)ApplyGuidRemap(meta, std::span<const std::pair<SGuid, SGuid>>(map).subspan(1u));
		FString text;
		SDiagnostic error;
		if (!EmitMeta(meta, text, &error))
		{
			OutProblems.push_back(MakeProblem(Writer, GetAssetOf(action.Path), FString::Format("cannot rewrite '{}': {}", ToDisplayText(action.Path),
				error.Message)));
			continue;
		}
		OutOp.Steps.push_back(MakeWrite(action.Path, text));
		OutOp.PlannedGuids.push_back(newGuids[i]);
	}
	return OutProblems.size() == problemsBefore || !OutOp.Steps.empty();
}

bool PlanRestore (const CAssetWriter& Writer, const CRegistryView& View, const SLifecycleAction& Action, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems)
{
	const std::optional<SMetaCopyView> copy = View.FindMetaCopy(Action.Guid);
	if (!copy.has_value() || HashMetaBytes(FStringView(reinterpret_cast<const char*>(copy->Bytes.data()), static_cast<uint32>(copy->Bytes.size())))
		!= copy->Hash)
	{
		OutProblems.push_back(MakeProblem(Writer, Action.Path, FString::Format("no verified copy of the meta of '{}' is left to restore",
			ToDisplayText(Action.Path))));
		return false;
	}
	// The create plan checks what a restore must too: the source is a file, and no meta exists in any spelling.
	SCreateMetaRequest request;
	request.Source = Action.Path;
	request.Meta.Guid = Action.Guid;
	if (!PlanCreateMeta(Writer, request, OutOp, OutProblems))
	{
		return false;
	}
	OutOp.Verb = "restore-meta";
	OutOp.Steps.front().Bytes.assign(copy->Bytes.begin(), copy->Bytes.end());
	OutOp.Steps.front().Hash = hash::SContentHash {};
	return true;
}

bool PlanKeepAlias (const CAssetWriter& Writer, const CRegistryView& View, const SLifecycleAction& Action, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp("keep-alias");
	if (!Action.Guid.IsValid() || !IsInWriter(Writer, Action.Path))
	{
		OutProblems.push_back(MakeProblem(Writer, Action.Path, FString::Format("cannot rewrite '{}' here", ToDisplayText(Action.Path))));
		return false;
	}
	// Only the broken meta this fix-it was offered for is rewritten: one resolved by hand since (or broken another way)
	// keeps what the person made of it.
	const std::optional<SMetaParseResult> current = ReadMeta(Writer, Action.Path);
	const auto salvaged = [&current] (const SGuid& Guid)
	{
		return std::find(current->SalvagedGuids.begin(), current->SalvagedGuids.end(), Guid) != current->SalvagedGuids.end();
	};
	if (!current.has_value() || current->State != EMetaParseState::Broken || !salvaged(Action.Guid)
		|| !std::all_of(Action.Aliases.begin(), Action.Aliases.end(), salvaged))
	{
		OutProblems.push_back(MakeProblem(Writer, Action.Path, FString::Format("'{}' is no longer the broken meta the fix was offered for: "
			"left as it is", ToDisplayText(Action.Path))));
		return false;
	}
	SAssetMeta meta;
	if (const std::optional<SMetaCopyView> copy = View.FindMetaCopy(Action.Guid); copy.has_value())
	{
		SMetaParseResult parsed;
		ParseMeta(FStringView(reinterpret_cast<const char*>(copy->Bytes.data()), static_cast<uint32>(copy->Bytes.size())), parsed);
		if (parsed.State == EMetaParseState::Ok && parsed.Meta.Guid == Action.Guid)
		{
			meta = std::move(parsed.Meta);
		}
	}
	meta.Guid = Action.Guid;
	for (const SGuid& alias : Action.Aliases)
	{
		if (alias != Action.Guid && std::find(meta.Aliases.begin(), meta.Aliases.end(), alias) == meta.Aliases.end())
		{
			meta.Aliases.push_back(alias);
		}
	}
	FString text;
	SDiagnostic error;
	if (!EmitMeta(meta, text, &error))
	{
		OutProblems.push_back(MakeProblem(Writer, Action.Path, FString::Format("cannot rewrite '{}': {}", ToDisplayText(Action.Path), error.Message)));
		return false;
	}
	OutOp.Steps.push_back(MakeWrite(Action.Path, text));
	OutOp.PlannedGuids.push_back(Action.Guid);
	return true;
}

bool PlanMove (const CAssetWriter& Writer, const SLifecycleAction& Action, FStringView Verb, SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp(Verb);
	if (Action.Path.IsEmpty() || Action.ToPath.IsEmpty() || !IsInWriter(Writer, Action.Path) || !IsInWriter(Writer, Action.ToPath))
	{
		OutProblems.push_back(MakeProblem(Writer, GetAssetOf(Action.ToPath.IsEmpty() ? Action.Path.View() : Action.ToPath.View()),
			FString::Format("cannot move '{}' to '{}' here", ToDisplayText(Action.Path), ToDisplayText(Action.ToPath))));
		return false;
	}
	OutOp.Steps.push_back(MakeStep(EAssetOpStep::Move, Action.Path, Action.ToPath));
	if (Action.Guid.IsValid())
	{
		OutOp.PlannedGuids.push_back(Action.Guid);
	}
	return true;
}
}


bool ApplyGuidRemap (SAssetMeta& Meta, std::span<const std::pair<SGuid, SGuid>> Map)
{
	if (Map.empty())
	{
		return false;
	}
	std::map<SGuid, SGuid> map;
	for (const auto& [from, to] : Map)
	{
		map.emplace(from, to);
	}
	bool bChanged = false;
	for (SGuid& alias : Meta.Aliases)
	{
		if (const auto found = map.find(alias); found != map.end() && found->second != alias)
		{
			alias = found->second;
			bChanged = true;
		}
	}
	CRefRewriter rewriter(map);
	reflect::CDataTree subAssets;
	if (rewriter.Rewrite(Meta.SubAssets, subAssets))
	{
		Meta.SubAssets = std::move(subAssets);
		bChanged = true;
	}
	return bChanged;
}

bool PlanRepairOp (const CAssetWriter& Writer, const CRegistryView& View, std::span<const SLifecycleAction> Actions, SAssetOp& OutOp,
	std::vector<SDiagnostic>& OutProblems)
{
	OutOp = SAssetOp {};
	if (Actions.empty())
	{
		return false;
	}
	if (Actions.size() > 1u || Actions.front().Kind == ELifecycleAction::ReGuid)
	{
		return PlanReGuids(Writer, Actions, OutOp, OutProblems) && !OutOp.Steps.empty();
	}
	const SLifecycleAction& action = Actions.front();
	switch (action.Kind)
	{
	case ELifecycleAction::CreateMeta:
	{
		SCreateMetaRequest request;
		request.Source = action.Path;
		request.Meta.Guid = action.Guid;
		return PlanCreateMeta(Writer, request, OutOp, OutProblems);
	}
	case ELifecycleAction::RestoreMeta:
		return PlanRestore(Writer, View, action, OutOp, OutProblems);
	case ELifecycleAction::MoveMeta:
		return PlanMove(Writer, action, "relink-meta", OutOp, OutProblems);
	case ELifecycleAction::FixMetaCase:
		return PlanMove(Writer, action, "fix-meta-case", OutOp, OutProblems);
	case ELifecycleAction::KeepAliasB:
		return PlanKeepAlias(Writer, View, action, OutOp, OutProblems);
	case ELifecycleAction::ReGuid:
		break;
	}
	return false;
}
}
