#include "Assets/Registry/RegistryView.h"

#include <algorithm>
#include <tuple>

#include "Assets/AssetTypes.h"
#include "Unicode/DisplayText.h"


/**
 * The text dump (design §5.3): a golden-file form of a view that depends only on content. Everything is
 * sorted by mount path under the portable fold, then bytes; nothing machine-specific (times, identities,
 * hashes of file bytes, revisions, row numbers, the volume's case rule, absolute paths) is printed.
 */
namespace frt::assets
{
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

void AppendHex64 (FString& Out, uint64 Value)
{
	Out.AppendFormat("{:016x}", Value);
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

// A tag line per value, sorted by key name (the values of a MultiValue key keep their order).
void AppendTags (FString& Out, const CRegistryView& View, SRecordId Record, FStringView Indent)
{
	std::vector<std::pair<FString, FString>> lines;
	View.ForEachTag(Record, [&] (FStringView Key, const STagValue& Value)
	{
		FString text;
		AppendTagValue(text, Value);
		lines.emplace_back(FString(Key), std::move(text));
	});
	std::stable_sort(lines.begin(), lines.end(), [] (const auto& A, const auto& B) { return LessPortableThenBytes(A.first, B.first); });
	for (const auto& [key, value] : lines)
	{
		Out += Indent;
		Out += "tag ";
		Out += key;
		Out += " = ";
		Out += value;
		Out += '\n';
	}
}

struct SDumpEdge
{
	FString Target;
	uint64 TargetSub = 0u;
	EDepFlags Flags {};
	FString FieldPath;
	FString From;
	FStringLiteral State;
};

struct SDumpProblem
{
	const SDiagnostic* Diagnostic = nullptr;
	FStringView Subject;
	EProblemSource Source = EProblemSource::Scan;
};

// A path or name as people read it: the on-disk spelling stays the key everywhere, but a WTF-8 surrogate (a Windows
// name's unpaired one) is no UTF-8, so the dump shows it as U+FFFD (ToDisplayText) and stays valid UTF-8 text.
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
}

void CRegistryView::DumpText (FString& Out, EDumpFlags Flags) const
{
	const SRegistryTables& tables = Tables;
	std::vector<SDumpProblem> problems;
	if (!HasDumpFlag(Flags, EDumpFlags::NoProblems))
	{
		ForEachProblem([&] (FStringView Subject, EProblemSource Source, const SDiagnostic& Diagnostic)
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
	}
	std::vector<uint8> printed(problems.size(), 0u);

	for (const SMountRow& mount : *tables.Mounts)
	{
		if (mount.Name.IsEmpty())
		{
			continue;
		}
		const FString root = JoinMountPath(mount.Name, FStringView());
		Out += "mount ";
		Out += root;
		Out += '\n';

		// Rule files: the mount's Config/Assets.yml first, then folder files by path.
		std::vector<std::pair<FStringView, uint64>> ruleFiles;
		if (mount.ConfigRuleFile != InvalidRow)
		{
			Out += "  rules Config/Assets.yml ";
			AppendHex64(Out, tables.RuleFiles.RulesHash[mount.ConfigRuleFile]);
			Out += '\n';
		}
		ForEachRuleFile([&] (uint32 Row)
		{
			if (tables.RuleFiles.Mount[Row] == mount.Id && tables.RuleFiles.Folder[Row] != InvalidRow)
			{
				ruleFiles.emplace_back(tables.Paths.Get(tables.Folders.Path[tables.RuleFiles.Folder[Row]]), tables.RuleFiles.RulesHash[Row]);
			}
		});
		std::sort(ruleFiles.begin(), ruleFiles.end(), [] (const auto& A, const auto& B) { return LessPortableThenBytes(A.first, B.first); });
		for (const auto& [folder, rulesHash] : ruleFiles)
		{
			Out += "  rules ";
			AppendShown(Out, folder);
			Out += "/Assets.yml ";
			AppendHex64(Out, rulesHash);
			Out += '\n';
		}

		if (!HasDumpFlag(Flags, EDumpFlags::NoFolders))
		{
			std::vector<FStringView> folders;
			ForEachFolder([&] (uint32 Row)
			{
				if (tables.Folders.Mount[Row] == mount.Id && tables.Folders.Parent[Row] != InvalidRow)
				{
					folders.push_back(tables.Paths.Get(tables.Folders.Path[Row]));
				}
			});
			std::sort(folders.begin(), folders.end(), LessPortableThenBytes);
			for (const FStringView folder : folders)
			{
				Out += "  folder ";
				AppendShown(Out, folder);
				Out += '\n';
			}
		}

		std::vector<uint32> files;
		ForEachFile([&] (uint32 Row)
		{
			if (tables.Files.Mount[Row] == mount.Id)
			{
				files.push_back(Row);
			}
		});
		std::sort(files.begin(), files.end(), [&] (uint32 A, uint32 B)
		{
			return LessPortableThenBytes(tables.Paths.Get(tables.Files.Path[A]), tables.Paths.Get(tables.Files.Path[B]));
		});
		for (const uint32 file : files)
		{
			Out += "  file ";
			AppendShown(Out, tables.Paths.Get(tables.Files.Path[file]));
			Out += ' ';
			Out += enum_::ToString(tables.Files.Class[file]);
			Out += " size=";
			Out.AppendNumber(tables.Files.Size[file]);
			Out += '\n';
			const uint32 row = tables.Files.Record[file];
			if (row == InvalidRow)
			{
				continue;
			}
			const SRecordId record = MakeRecordId(row);
			const SAssetsTable& assets = tables.Assets;
			Out += "    record type=";
			Out += CAssetTypeRegistry::Get(assets.Type[row]).Name;
			Out += " importer=";
			const FStringView importer = GetImporterName(record);
			Out += importer.IsEmpty() ? FStringView("-") : importer;
			Out += " status=";
			Out += enum_::ToString(assets.ImportStatus[row]);
			Out += " stamp=";
			AppendHex64(Out, assets.AnalyzeStamp[row]);
			Out += " cook=";
			Out += enum_::ToString(assets.CookRule[row]);
			Out += " chunk=";
			Out.AppendNumber(assets.Chunk[row]);
			Out += " flags=";
			AppendFlags(Out, assets.Flags[row]);
			if (assets.PrimaryName[row] != InvalidRow)
			{
				Out += " primary=";
				Out += tables.Names.Get(assets.PrimaryName[row]);
			}
			Out += '\n';

			bool bAnyLabel = false;
			ForEachLabel(record, [&] (FStringView Label, bool bRule)
			{
				Out += bAnyLabel ? ", " : "    labels ";
				Out += bRule ? "rule:" : "authored:";
				Out += Label;
				bAnyLabel = true;
			});
			if (bAnyLabel)
			{
				Out += '\n';
			}

			AppendTags(Out, *this, record, "    ");

			std::vector<uint32> subs;
			ForEachSubEntry(record, [&] (SRecordId Sub) { subs.push_back(Sub.Value & ~SubEntryBit); });
			std::sort(subs.begin(), subs.end(), [&] (uint32 A, uint32 B)
			{
				return LessPortableThenBytes(tables.Names.Get(tables.SubEntries.Key[A]), tables.Names.Get(tables.SubEntries.Key[B]));
			});
			for (const uint32 sub : subs)
			{
				Out += "    sub ";
				Out += tables.Names.Get(tables.SubEntries.Key[sub]);
				Out += " type=";
				Out += CAssetTypeRegistry::Get(tables.SubEntries.Type[sub]).Name;
				Out += " flags=";
				AppendFlags(Out, tables.SubEntries.Flags[sub]);
				Out += '\n';
				AppendTags(Out, *this, MakeRecordId(SubEntryBit | sub), "      ");
			}

			std::vector<SDumpEdge> edges;
			ForEachLiveEdge(tables, row, [&] (uint32 EdgeRef)
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
					const uint32 fromSub = GetEdgeSourceSub(tables, row, edgeRow);
					edge.From = fromSub < tables.SubEntries.Owner.Size() && tables.SubEntries.Owner[fromSub] == row
						? FString(tables.Names.Get(tables.SubEntries.Key[fromSub])) : FString("?");
				}
				edge.State = enum_::ToString(target.State);
				edges.push_back(std::move(edge));
			});
			std::sort(edges.begin(), edges.end(), [] (const SDumpEdge& A, const SDumpEdge& B)
			{
				if (A.Target != B.Target)
				{
					return LessPortableThenBytes(A.Target, B.Target);
				}
				return std::tie(A.TargetSub, A.Flags, A.FieldPath, A.From) < std::tie(B.TargetSub, B.Flags, B.FieldPath, B.From);
			});
			for (const SDumpEdge& edge : edges)
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

			std::vector<FStringView> companions;
			ForEachCompanion(record, [&] (FStringView Companion) { companions.push_back(Companion); });
			std::sort(companions.begin(), companions.end(), LessPortableThenBytes);
			for (const FStringView companion : companions)
			{
				Out += "    companion ";
				AppendShown(Out, companion);
				Out += '\n';
			}
		}

		for (uint32 i = 0u; i < problems.size(); ++i)
		{
			const FStringView subject = problems[i].Subject;
			if (subject.StartsWith(root) && (subject.Len() == root.Len() || subject[root.Len()] == '/' || subject[root.Len()] == '#'))
			{
				AppendDiagnostic(Out, problems[i]);
				printed[i] = 1u;
			}
		}
	}

	bool bHeader = false;
	for (uint32 i = 0u; i < problems.size(); ++i)
	{
		if (printed[i] == 0u)
		{
			if (!bHeader)
			{
				Out += "unmounted\n";
				bHeader = true;
			}
			AppendDiagnostic(Out, problems[i]);
		}
	}
}
}
