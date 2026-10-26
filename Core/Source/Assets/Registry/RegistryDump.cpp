#include "Assets/Registry/RegistryView.h"

#include <algorithm>
#include <tuple>

#include "Assets/AssetTypes.h"
#include "Text/JsonWriter.h"
#include "Unicode/DisplayText.h"


/**
 * The registry dumps (design §5.3): a golden-file form of a view that depends only on content. Everything is
 * sorted by mount path under the portable fold, then bytes; nothing machine-specific (times, identities, hashes
 * of file bytes, revisions, row numbers, the volume's case rule, absolute paths) is printed.
 *
 * One model per mount (SDumpMount, collected and sorted once) feeds both forms, so the text dump and the JSON
 * lines (`FRT.bat assets dump registry --text|--json`) can never disagree on content or order.
 */
namespace frt::assets
{
using json::CJsonObject;

namespace
{
template <typename E>
void AppendFlags (FString& Out, E Flags)
{
	using U = std::underlying_type_t<E>;
	bool bAny = false;
	for (const auto& entry : enum_::TEnumTraits<E>::Entries)
	{
		if ((static_cast<U>(Flags) & static_cast<U>(entry.first)) != 0u)
		{
			if (bAny)
			{
				Out += '|';
			}
			Out += entry.second;
			bAny = true;
		}
	}
	if (!bAny)
	{
		Out += '-';
	}
}

template <typename E>
std::vector<FString> FlagNames (E Flags)
{
	using U = std::underlying_type_t<E>;
	std::vector<FString> names;
	for (const auto& entry : enum_::TEnumTraits<E>::Entries)
	{
		if ((static_cast<U>(Flags) & static_cast<U>(entry.first)) != 0u)
		{
			names.emplace_back(entry.second);
		}
	}
	return names;
}

void AppendHex64 (FString& Out, uint64 Value)
{
	Out.AppendFormat("{:016x}", Value);
}

FString Hex64 (uint64 Value)
{
	return FString::Format("{:016x}", Value);
}

void AppendTagValue (FString& Out, const STagValue& Value)
{
	switch (Value.Type)
	{
	case ETagType::Int64:
		Out.AppendNumber(Value.AsInt64());
		break;
	case ETagType::Double:
		Out.AppendNumber(Value.AsDouble());
		break;
	case ETagType::Bool:
		Out += Value.AsBool() ? "true" : "false";
		break;
	case ETagType::Name:
		Out += Value.Text;
		break;
	case ETagType::String:
		Out += '"';
		Out += FString::Escape(Value.Text);
		Out += '"';
		break;
	case ETagType::Enum:
		Out.AppendNumber(Value.Bits);
		break;
	case ETagType::AssetRef:
	case ETagType::RowRef:
		AppendHex64(Out, Value.Bits);
		break;
	case ETagType::None:
		Out += '-';
		break;
	}
}

// A path or name as people read it: the on-disk spelling stays the key everywhere, but a WTF-8 surrogate (a Windows
// name's unpaired one) is no UTF-8, so the text dump shows it as U+FFFD (ToDisplayText) and stays valid UTF-8 text.
// The JSON form writes the spelling itself (the JSON writer escapes a surrogate), so it round-trips.
void AppendShown (FString& Out, FStringView Text)
{
	if (Text.IsValidUtf8())
	{
		Out += Text;
	}
	else
	{
		Out += unicode::ToDisplayText(Text);
	}
}

// ---- The model of one mount ----

struct SDumpTag
{
	FString Key;
	FString Value;   // the text form (AppendTagValue)
};

struct SDumpSub
{
	FStringView Label;
	FStringView Type;
	ERecordFlags Flags {};
	std::vector<SDumpTag> Tags;
};

struct SDumpEdge
{
	FString Target;
	uint64 TargetSub = 0u;
	EDepFlags Flags {};
	FString FieldPath;
	FString From;
	FStringLiteral State;
};

struct SDumpLabel
{
	FStringView Label;
	bool bRule = false;
};

struct SDumpRecord
{
	FStringView Type;
	FStringView Importer;
	EImportStatus Status = EImportStatus::NotAnalyzed;
	uint64 Stamp = 0u;
	ECookRule Cook = ECookRule::Auto;
	uint16 Chunk = 0u;
	ERecordFlags Flags {};
	bool bPrimary = false;
	FStringView Primary;
	std::vector<SDumpLabel> Labels;
	std::vector<SDumpTag> Tags;
	std::vector<SDumpSub> Subs;
	std::vector<SDumpEdge> Edges;
	std::vector<FStringView> Companions;
};

struct SDumpFile
{
	FStringView Path;
	EFileClass Class = EFileClass::Unclaimed;
	uint64 Size = 0u;
	bool bHasRecord = false;
	SDumpRecord Record;
};

struct SDumpProblem
{
	const SDiagnostic* Diagnostic = nullptr;
	FStringView Subject;
	EProblemSource Source = EProblemSource::Scan;
};

struct SDumpRuleFile
{
	FString File;    // "Config/Assets.yml", or "<folder mount path>/Assets.yml"
	FStringView Folder;
	uint64 RulesHash = 0u;
	bool bConfig = false;
};

struct SDumpMount
{
	FString Root;
	std::vector<SDumpRuleFile> RuleFiles;
	std::vector<FStringView> Folders;
	std::vector<SDumpFile> Files;
	std::vector<const SDumpProblem*> Problems;
};

// Tags sorted by key name (the values of a MultiValue key keep their order).
std::vector<SDumpTag> CollectTags (const CRegistryView& View, SRecordId Record)
{
	std::vector<SDumpTag> tags;
	View.ForEachTag(Record, [&] (FStringView Key, const STagValue& Value)
	{
		SDumpTag tag;
		tag.Key = FString(Key);
		AppendTagValue(tag.Value, Value);
		tags.push_back(std::move(tag));
	});
	std::stable_sort(tags.begin(), tags.end(), [] (const SDumpTag& A, const SDumpTag& B) { return LessPortableThenBytes(A.Key, B.Key); });
	return tags;
}

// Every problem of the view, sorted by (code, subject, message, then the rest).
std::vector<SDumpProblem> CollectProblems (const CRegistryView& View)
{
	std::vector<SDumpProblem> problems;
	View.ForEachProblem([&] (FStringView Subject, EProblemSource Source, const SDiagnostic& Diagnostic)
	{
		problems.push_back(SDumpProblem { &Diagnostic, Subject, Source });
	});
	std::sort(problems.begin(), problems.end(), [] (const SDumpProblem& A, const SDumpProblem& B)
	{
		const SDiagnostic& a = *A.Diagnostic;
		const SDiagnostic& b = *B.Diagnostic;
		if (a.Code != b.Code)
		{
			return a.Code < b.Code;
		}
		if (A.Subject != B.Subject)
		{
			return LessPortableThenBytes(A.Subject, B.Subject);
		}
		if (a.Message != b.Message)
		{
			return a.Message < b.Message;
		}
		return std::tie(a.File, a.Line, a.Column, a.Severity, A.Source, a.FieldPath)
			< std::tie(b.File, b.Line, b.Column, b.Severity, B.Source, b.FieldPath);
	});
	return problems;
}

bool IsAtOrBelowMountRoot (FStringView Subject, FStringView Root) noexcept
{
	return Subject.StartsWith(Root) && (Subject.Len() == Root.Len() || Subject[Root.Len()] == '/' || Subject[Root.Len()] == '#');
}

bool IsDumped (std::span<const MountId> Mounts, MountId Id) noexcept
{
	return Mounts.empty() || std::find(Mounts.begin(), Mounts.end(), Id) != Mounts.end();
}
}

/**
 * The model of one mount, in dump order. A member (a friend of the tables) so that it reads the view's columns
 * directly; Printed marks the problems it took (the rest are printed as unmounted).
 */
struct CRegistryView::SDumpBuilder
{
	static SDumpMount Build (const CRegistryView& View, const SMountRow& Mount, EDumpFlags Flags, std::span<const SDumpProblem> Problems,
		std::vector<uint8>& Printed)
	{
		const SRegistryTables& tables = View.Tables;
		SDumpMount out;
		out.Root = JoinMountPath(Mount.Name, FStringView());

		// Rule files: the mount's Config/Assets.yml first, then folder files by path.
		if (Mount.ConfigRuleFile != InvalidRow)
		{
			out.RuleFiles.push_back(SDumpRuleFile { FString("Config/Assets.yml"), FStringView(), tables.RuleFiles.RulesHash[Mount.ConfigRuleFile], true });
		}
		std::vector<SDumpRuleFile> folderRules;
		View.ForEachRuleFile([&] (uint32 Row)
		{
			if (tables.RuleFiles.Mount[Row] == Mount.Id && tables.RuleFiles.Folder[Row] != InvalidRow)
			{
				SDumpRuleFile rule;
				rule.Folder = tables.Paths.Get(tables.Folders.Path[tables.RuleFiles.Folder[Row]]);
				rule.RulesHash = tables.RuleFiles.RulesHash[Row];
				folderRules.push_back(std::move(rule));
			}
		});
		std::sort(folderRules.begin(), folderRules.end(), [] (const SDumpRuleFile& A, const SDumpRuleFile& B) { return LessPortableThenBytes(A.Folder, B.Folder); });
		for (SDumpRuleFile& rule : folderRules)
		{
			rule.File = FString(rule.Folder) + "/Assets.yml";
			out.RuleFiles.push_back(std::move(rule));
		}

		if (!HasDumpFlag(Flags, EDumpFlags::NoFolders))
		{
			View.ForEachFolder([&] (uint32 Row)
			{
				if (tables.Folders.Mount[Row] == Mount.Id && tables.Folders.Parent[Row] != InvalidRow)
				{
					out.Folders.push_back(tables.Paths.Get(tables.Folders.Path[Row]));
				}
			});
			std::sort(out.Folders.begin(), out.Folders.end(), LessPortableThenBytes);
		}

		std::vector<uint32> files;
		View.ForEachFile([&] (uint32 Row)
		{
			if (tables.Files.Mount[Row] == Mount.Id)
			{
				files.push_back(Row);
			}
		});
		std::sort(files.begin(), files.end(), [&] (uint32 A, uint32 B)
		{
			return LessPortableThenBytes(tables.Paths.Get(tables.Files.Path[A]), tables.Paths.Get(tables.Files.Path[B]));
		});
		out.Files.reserve(files.size());
		for (const uint32 file : files)
		{
			SDumpFile& entry = out.Files.emplace_back();
			entry.Path = tables.Paths.Get(tables.Files.Path[file]);
			entry.Class = tables.Files.Class[file];
			entry.Size = tables.Files.Size[file];
			const uint32 row = tables.Files.Record[file];
			if (row != InvalidRow)
			{
				entry.bHasRecord = true;
				FillRecord(View, row, entry.Record);
			}
		}

		for (uint32 i = 0u; i < Problems.size(); ++i)
		{
			if (IsAtOrBelowMountRoot(Problems[i].Subject, out.Root))
			{
				out.Problems.push_back(&Problems[i]);
				Printed[i] = 1u;
			}
		}
		return out;
	}

	static void FillRecord (const CRegistryView& View, uint32 Row, SDumpRecord& Out)
	{
		const SRegistryTables& tables = View.Tables;
		const SRecordId record = View.MakeRecordId(Row);
		const SAssetsTable& assets = tables.Assets;
		Out.Type = CAssetTypeRegistry::Get(assets.Type[Row]).Name;
		Out.Importer = View.GetImporterName(record);
		Out.Status = assets.ImportStatus[Row];
		Out.Stamp = assets.AnalyzeStamp[Row];
		Out.Cook = assets.CookRule[Row];
		Out.Chunk = assets.Chunk[Row];
		Out.Flags = assets.Flags[Row];
		if (assets.PrimaryName[Row] != InvalidRow)
		{
			Out.bPrimary = true;
			Out.Primary = tables.Names.Get(assets.PrimaryName[Row]);
		}
		View.ForEachLabel(record, [&] (FStringView Label, bool bRule) { Out.Labels.push_back(SDumpLabel { Label, bRule }); });
		Out.Tags = CollectTags(View, record);

		std::vector<uint32> subs;
		View.ForEachSubEntry(record, [&] (SRecordId Sub) { subs.push_back(Sub.Value & ~SubEntryBit); });
		std::sort(subs.begin(), subs.end(), [&] (uint32 A, uint32 B)
		{
			return LessPortableThenBytes(tables.Names.Get(tables.SubEntries.Key[A]), tables.Names.Get(tables.SubEntries.Key[B]));
		});
		for (const uint32 sub : subs)
		{
			SDumpSub& entry = Out.Subs.emplace_back();
			entry.Label = tables.Names.Get(tables.SubEntries.Key[sub]);
			entry.Type = CAssetTypeRegistry::Get(tables.SubEntries.Type[sub]).Name;
			entry.Flags = tables.SubEntries.Flags[sub];
			entry.Tags = CollectTags(View, View.MakeRecordId(SubEntryBit | sub));
		}

		ForEachLiveEdge(tables, Row, [&] (uint32 EdgeRef)
		{
			const SEdgeRow& edgeRow = GetEdgeRow(tables, EdgeRef);
			const STargetRow& target = tables.Graph.Targets[edgeRow.Target];
			SDumpEdge edge;
			if (target.Kind == ETargetKind::Path)
			{
				edge.Target = FString(tables.Paths.Get(target.Text));
			}
			else
			{
				edge.Target = "{" + target.Guid.ToString() + "}";
				if (target.Text != InvalidRow)
				{
					edge.Target += ' ';
					edge.Target += tables.Paths.Get(target.Text);
				}
			}
			edge.TargetSub = target.TargetSub;
			edge.Flags = edgeRow.Flags;
			edge.FieldPath = edgeRow.FieldPath != InvalidRow ? FString(tables.Names.Get(edgeRow.FieldPath)) : FString();
			if (edgeRow.FromSub != 0u)
			{
				const uint32 fromSub = GetEdgeSourceSub(tables, Row, edgeRow);
				edge.From = fromSub < tables.SubEntries.Owner.Size() && tables.SubEntries.Owner[fromSub] == Row
					? FString(tables.Names.Get(tables.SubEntries.Key[fromSub])) : FString("?");
			}
			edge.State = enum_::ToString(target.State);
			Out.Edges.push_back(std::move(edge));
		});
		std::sort(Out.Edges.begin(), Out.Edges.end(), [] (const SDumpEdge& A, const SDumpEdge& B)
		{
			if (A.Target != B.Target)
			{
				return LessPortableThenBytes(A.Target, B.Target);
			}
			return std::tie(A.TargetSub, A.Flags, A.FieldPath, A.From) < std::tie(B.TargetSub, B.Flags, B.FieldPath, B.From);
		});

		View.ForEachCompanion(record, [&] (FStringView Companion) { Out.Companions.push_back(Companion); });
		std::sort(Out.Companions.begin(), Out.Companions.end(), LessPortableThenBytes);
	}

	// Calls Visit(SDumpMount&&) per dumped mount, by MountId, then Unmounted(problems no mount took) when every
	// mount is dumped.
	template <typename TVisit, typename TUnmounted>
	static void ForEachMount (const CRegistryView& View, EDumpFlags Flags, std::span<const MountId> Mounts, TVisit&& Visit, TUnmounted&& Unmounted)
	{
		std::vector<SDumpProblem> problems;
		if (!HasDumpFlag(Flags, EDumpFlags::NoProblems))
		{
			problems = CollectProblems(View);
		}
		std::vector<uint8> printed(problems.size(), 0u);
		for (const SMountRow& mount : *View.Tables.Mounts)
		{
			if (mount.Name.IsEmpty() || !IsDumped(Mounts, mount.Id))
			{
				continue;
			}
			Visit(Build(View, mount, Flags, problems, printed));
		}
		if (Mounts.empty())
		{
			std::vector<const SDumpProblem*> rest;
			for (uint32 i = 0u; i < problems.size(); ++i)
			{
				if (printed[i] == 0u)
				{
					rest.push_back(&problems[i]);
				}
			}
			if (!rest.empty())
			{
				Unmounted(rest);
			}
		}
	}
};

namespace
{
// ---- The text form ----

void AppendTagLines (FString& Out, const std::vector<SDumpTag>& Tags, FStringView Indent)
{
	for (const SDumpTag& tag : Tags)
	{
		Out += Indent;
		Out += "tag ";
		Out += tag.Key;
		Out += " = ";
		Out += tag.Value;
		Out += '\n';
	}
}

void AppendDiagnostic (FString& Out, const SDumpProblem& Problem)
{
	const SDiagnostic& diagnostic = *Problem.Diagnostic;
	Out += "  problem ";
	if (diagnostic.Code != 0u)
	{
		AppendDiagnosticCode(Out, diagnostic.Code);
	}
	else
	{
		Out += '-';
	}
	Out += ' ';
	Out += GetSeverityName(diagnostic.Severity);
	Out += ' ';
	Out += enum_::ToString(Problem.Source);
	Out += ' ';
	AppendShown(Out, Problem.Subject);
	if (!diagnostic.File.IsEmpty())
	{
		Out += " at ";
		AppendShown(Out, diagnostic.File);
		Out.AppendFormat(":{}:{}", diagnostic.Line, diagnostic.Column);
	}
	if (!diagnostic.FieldPath.IsEmpty())
	{
		Out += " field ";
		Out += diagnostic.FieldPath;
	}
	Out += ": ";
	Out += diagnostic.Code != 0u ? diagnostic.Message.View() : FStringView("(message omitted)");
	Out += '\n';
}

void AppendMountText (FString& Out, const SDumpMount& Mount)
{
	Out += "mount ";
	Out += Mount.Root;
	Out += '\n';
	for (const SDumpRuleFile& rule : Mount.RuleFiles)
	{
		Out += "  rules ";
		AppendShown(Out, rule.File);
		Out += ' ';
		AppendHex64(Out, rule.RulesHash);
		Out += '\n';
	}
	for (const FStringView folder : Mount.Folders)
	{
		Out += "  folder ";
		AppendShown(Out, folder);
		Out += '\n';
	}
	for (const SDumpFile& file : Mount.Files)
	{
		Out += "  file ";
		AppendShown(Out, file.Path);
		Out += ' ';
		Out += enum_::ToString(file.Class);
		Out += " size=";
		Out.AppendNumber(file.Size);
		Out += '\n';
		if (!file.bHasRecord)
		{
			continue;
		}
		const SDumpRecord& record = file.Record;
		Out += "    record type=";
		Out += record.Type;
		Out += " importer=";
		Out += record.Importer.IsEmpty() ? FStringView("-") : record.Importer;
		Out += " status=";
		Out += enum_::ToString(record.Status);
		Out += " stamp=";
		AppendHex64(Out, record.Stamp);
		Out += " cook=";
		Out += enum_::ToString(record.Cook);
		Out += " chunk=";
		Out.AppendNumber(record.Chunk);
		Out += " flags=";
		AppendFlags(Out, record.Flags);
		if (record.bPrimary)
		{
			Out += " primary=";
			Out += record.Primary;
		}
		Out += '\n';

		for (size_t i = 0u; i < record.Labels.size(); ++i)
		{
			Out += i != 0u ? ", " : "    labels ";
			Out += record.Labels[i].bRule ? "rule:" : "authored:";
			Out += record.Labels[i].Label;
		}
		if (!record.Labels.empty())
		{
			Out += '\n';
		}

		AppendTagLines(Out, record.Tags, "    ");
		for (const SDumpSub& sub : record.Subs)
		{
			Out += "    sub ";
			Out += sub.Label;
			Out += " type=";
			Out += sub.Type;
			Out += " flags=";
			AppendFlags(Out, sub.Flags);
			Out += '\n';
			AppendTagLines(Out, sub.Tags, "      ");
		}
		for (const SDumpEdge& edge : record.Edges)
		{
			Out += "    edge ";
			AppendShown(Out, edge.Target);
			Out += " sub=";
			AppendHex64(Out, edge.TargetSub);
			Out += " flags=";
			AppendFlags(Out, edge.Flags);
			if (!edge.FieldPath.IsEmpty())
			{
				Out += " field=";
				Out += edge.FieldPath;
			}
			if (!edge.From.IsEmpty())
			{
				Out += " from=";
				Out += edge.From;
			}
			Out += ' ';
			Out += edge.State;
			Out += '\n';
		}
		for (const FStringView companion : record.Companions)
		{
			Out += "    companion ";
			AppendShown(Out, companion);
			Out += '\n';
		}
	}
	for (const SDumpProblem* problem : Mount.Problems)
	{
		AppendDiagnostic(Out, *problem);
	}
}

// ---- The JSON form ----

std::vector<CJsonObject> TagObjects (const std::vector<SDumpTag>& Tags)
{
	std::vector<CJsonObject> objects;
	objects.reserve(Tags.size());
	for (const SDumpTag& tag : Tags)
	{
		objects.push_back(CJsonObject().AddString("key", tag.Key).AddString("value", tag.Value));
	}
	return objects;
}

CJsonObject ProblemObject (FStringView Mount, const SDumpProblem& Problem)
{
	const SDiagnostic& diagnostic = *Problem.Diagnostic;
	CJsonObject object;
	object.AddString("kind", "problem").AddString("mount", Mount)
		.AddString("code", diagnostic.Code != 0u ? FormatDiagnosticCode(diagnostic.Code) : FString())
		.AddString("severity", GetSeverityName(diagnostic.Severity)).AddString("source", enum_::ToString(Problem.Source))
		.AddString("subject", Problem.Subject).AddString("file", diagnostic.File).AddUInt("line", diagnostic.Line)
		.AddUInt("column", diagnostic.Column).AddString("field", diagnostic.FieldPath)
		.AddString("message", diagnostic.Code != 0u ? diagnostic.Message.View() : FStringView());
	return object;
}

template <typename TEmit>
void EmitMountJson (const SDumpMount& Mount, TEmit&& Emit)
{
	const FStringView mount = Mount.Root;
	Emit(CJsonObject().AddString("kind", "mount").AddString("mount", mount));
	for (const SDumpRuleFile& rule : Mount.RuleFiles)
	{
		Emit(CJsonObject().AddString("kind", "rules").AddString("mount", mount).AddString("file", rule.File).AddString("hash", Hex64(rule.RulesHash)));
	}
	for (const FStringView folder : Mount.Folders)
	{
		Emit(CJsonObject().AddString("kind", "folder").AddString("mount", mount).AddString("path", folder));
	}
	for (const SDumpFile& file : Mount.Files)
	{
		Emit(CJsonObject().AddString("kind", "file").AddString("mount", mount).AddString("path", file.Path)
			.AddString("class", enum_::ToString(file.Class)).AddUInt("size", file.Size));
		if (!file.bHasRecord)
		{
			continue;
		}
		const SDumpRecord& record = file.Record;
		std::vector<FString> labels;
		for (const SDumpLabel& label : record.Labels)
		{
			labels.push_back(FString(label.bRule ? "rule:" : "authored:") + label.Label);
		}
		std::vector<CJsonObject> subs;
		for (const SDumpSub& sub : record.Subs)
		{
			const std::vector<FString> flags = FlagNames(sub.Flags);
			const std::vector<CJsonObject> tags = TagObjects(sub.Tags);
			subs.push_back(CJsonObject().AddString("label", sub.Label).AddString("type", sub.Type).AddStringArray("flags", flags)
				.AddObjectArray("tags", tags));
		}
		std::vector<CJsonObject> edges;
		for (const SDumpEdge& edge : record.Edges)
		{
			const std::vector<FString> flags = FlagNames(edge.Flags);
			edges.push_back(CJsonObject().AddString("target", edge.Target).AddString("sub", Hex64(edge.TargetSub)).AddStringArray("flags", flags)
				.AddString("field", edge.FieldPath).AddString("from", edge.From).AddString("state", edge.State));
		}
		std::vector<FString> companions(record.Companions.begin(), record.Companions.end());
		const std::vector<FString> flags = FlagNames(record.Flags);
		const std::vector<CJsonObject> tags = TagObjects(record.Tags);
		Emit(CJsonObject().AddString("kind", "record").AddString("mount", mount).AddString("path", file.Path).AddString("type", record.Type)
			.AddString("importer", record.Importer).AddString("status", enum_::ToString(record.Status)).AddString("stamp", Hex64(record.Stamp))
			.AddString("cook", enum_::ToString(record.Cook)).AddUInt("chunk", record.Chunk).AddStringArray("flags", flags)
			.AddString("primary", record.Primary).AddStringArray("labels", labels).AddObjectArray("tags", tags)
			.AddObjectArray("subEntries", subs).AddObjectArray("edges", edges).AddStringArray("companions", companions));
	}
	for (const SDumpProblem* problem : Mount.Problems)
	{
		Emit(ProblemObject(mount, *problem));
	}
}
}

void CRegistryView::DumpText (FString& Out, EDumpFlags Flags, std::span<const MountId> Mounts) const
{
	SDumpBuilder::ForEachMount(*this, Flags, Mounts, [&] (const SDumpMount& Mount) { AppendMountText(Out, Mount); },
		[&] (const std::vector<const SDumpProblem*>& Rest)
		{
			Out += "unmounted\n";
			for (const SDumpProblem* problem : Rest)
			{
				AppendDiagnostic(Out, *problem);
			}
		});
}

void CRegistryView::DumpJson (TFunctionRef<void (FStringView Line)> Emit, EDumpFlags Flags, std::span<const MountId> Mounts) const
{
	const auto emit = [&] (const CJsonObject& Object) { Emit(Object.ToString()); };
	SDumpBuilder::ForEachMount(*this, Flags, Mounts, [&] (const SDumpMount& Mount) { EmitMountJson(Mount, emit); },
		[&] (const std::vector<const SDumpProblem*>& Rest)
		{
			for (const SDumpProblem* problem : Rest)
			{
				emit(ProblemObject(FStringView(), *problem));
			}
		});
}
}
