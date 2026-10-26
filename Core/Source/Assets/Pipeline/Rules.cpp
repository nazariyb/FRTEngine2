#include "Assets/Pipeline/Rules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

#include "Asserts.h"
#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetTypes.h"
#include "Containers/PathGlob.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Reflect/DataTree.h"
#include "Reflect/DataTreeJson.h"
#include "Reflect/DataTreeYaml.h"
#include "Text/JsonWriter.h"
#include "Unicode/Unicode.h"


namespace frt::assets
{
// ---------------- The compiled form of one rule file ----------------
//
// Everything a scope evaluates, resolved at parse time: globs compiled (with the mount an absolute pattern is
// restricted to), importers and settings tables looked up, setting values validated and normalised to rule-file
// text. Pointers into the static importer and settings tables only; nothing points into another file.

namespace
{
/**
 * A glob of a Scan list, a `Match:` or a `Directories:` entry. OnlyMount: the mount an absolute pattern
 * ("/Engine/Fonts/*.ttf" in a mount file) is restricted to; empty: the evaluated file's own mount.
 *
 * RequiredRuns: a quick reject before the glob runs. For a pattern without sets or escapes, every literal run
 * between its wildcards and separators ("_n." in "*_n.*", "art" and "reference" in "Art/Reference") must match path
 * characters one for one under the portable fold, so each run, folded, occurs in the folded path whenever the glob
 * matches - whichever segment it matches in. A pattern with one brace group (not nested) is its alternatives, each
 * with its runs, and a path must hold every run of at least one of them. A path that cannot match skips CPathGlob's
 * backtracking, which is most of what re-evaluating a subtree costs. Empty: no reject, the glob decides alone.
 */
struct SGlobEntry
{
	CPathGlob Glob;
	FString Text;        // as written, mount prefix included
	FString OnlyMount;
	FString OnlyMountFolded;
	std::vector<std::vector<FString>> RequiredRuns;   // per alternative, folded
	uint32 Line = 0u;
};

// The folded literal runs of a pattern without braces, sets or escapes.
std::vector<FString> GetLiteralRuns (FStringView Pattern)
{
	std::vector<FString> runs;
	uint32 start = 0u;
	for (uint32 i = 0u; i <= Pattern.Len(); ++i)
	{
		const bool bBreak = i == Pattern.Len() || Pattern[i] == '*' || Pattern[i] == '?' || Pattern[i] == '/';
		if (!bBreak)
		{
			continue;
		}
		if (i > start)
		{
			runs.push_back(FString(Pattern.Mid(start, i - start)).FoldCase());
		}
		start = i + 1u;
	}
	return runs;
}

// SGlobEntry::RequiredRuns of a pattern CPathGlob compiled: none for sets, escapes, nested or several brace groups,
// or when an alternative has no literal run at all (it could match any path).
std::vector<std::vector<FString>> GetRequiredRuns (FStringView Pattern)
{
	std::vector<std::vector<FString>> alternatives;
	if (Pattern.Contains('[') || Pattern.Contains('\\'))
	{
		return alternatives;
	}
	const uint32 open = Pattern.Find('{');
	if (open == FStringView::NotFound)
	{
		alternatives.push_back(GetLiteralRuns(Pattern));
	}
	else
	{
		const uint32 close = Pattern.Find('}', open);
		if (close == FStringView::NotFound || Pattern.Find('{', open + 1u) != FStringView::NotFound
			|| Pattern.Find('}', close + 1u) != FStringView::NotFound)
		{
			return alternatives;
		}
		const FStringView prefix = Pattern.Left(open);
		const FStringView suffix = Pattern.RightChop(close + 1u);
		FStringView choices = Pattern.Mid(open + 1u, close - open - 1u);
		for (;;)
		{
			const SStringSplit split = choices.SplitFirst(',');
			alternatives.push_back(GetLiteralRuns(FString(prefix) + split.Left + suffix));
			if (!split.bFound)
			{
				break;
			}
			choices = split.Right;
		}
	}
	if (std::any_of(alternatives.begin(), alternatives.end(), [] (const std::vector<FString>& Runs) { return Runs.empty(); }))
	{
		alternatives.clear();
	}
	return alternatives;
}

// One setting value: Field indexes its table's Fields; Text is the normalised rule-file text.
struct SSettingDef
{
	uint16 Field = 0u;
	FString Text;
	uint32 Line = 0u;
};

// A `Targets:` block of a rule: a platform ("Win64") or a full target ("Win64-Headless").
struct STargetBlock
{
	FString Name;        // canonical
	bool bPlatform = false;
	uint32 Line = 0u;
	std::optional<ECookRule> Cook;
	uint32 CookLine = 0u;
	std::optional<uint16> Chunk;
	uint32 ChunkLine = 0u;
	std::vector<SSettingDef> Settings;
};

// A type named in Types lists and `Type:` selectors: resolved for the engine's types, kept as text otherwise.
struct STypeRef
{
	std::optional<reflect::TypeId> Type;
	FString Name;        // canonical for a resolved type, as written otherwise
	uint32 Line = 0u;
};

struct SRuleDef
{
	uint32 Line = 0u;
	int32 Index = 0;
	std::vector<SGlobEntry> Match;          // empty: every file
	std::optional<STypeRef> Type;
	std::optional<FString> Label;
	FString Preset;                         // canonical "<Importer>/<Name>"; empty: none
	uint32 PresetLine = 0u;
	const SSettingsTable* SettingsTable = nullptr;   // the table of For: (or Importer:), for Settings and Targets
	std::vector<SSettingDef> Settings;
	const SImporterDesc* Importer = nullptr;
	uint32 ImporterLine = 0u;
	std::vector<std::pair<FString, uint32>> Labels;
	std::optional<ECookRule> Cook;
	uint32 CookLine = 0u;
	std::optional<uint16> Chunk;
	uint32 ChunkLine = 0u;
	std::vector<STargetBlock> Targets;
	bool bFinal = false;

	// A Type or Label selector: known only after Analyze, so it may not touch Analyze's inputs.
	[[nodiscard]] bool IsSelectorRule () const noexcept { return Type.has_value() || Label.has_value(); }
};

struct SPresetDef
{
	FString Name;        // canonical "<Importer>/<Name>"
	const SImporterDesc* Importer = nullptr;
	std::vector<SSettingDef> Values;
	uint32 Line = 0u;
};

struct SImporterRef
{
	const SImporterDesc* Importer = nullptr;
	uint32 Line = 0u;
};

struct SValidationDef
{
	uint32 Code = 0u;
	ESeverity Severity = ESeverity::Error;
	uint32 Line = 0u;
};
}


struct CRuleFile::SData
{
	FString BaseRelative;   // the folder the globs are relative to, inside its mount ("" for a mount file)
	std::vector<SGlobEntry> Include;
	std::vector<SGlobEntry> Exclude;
	std::vector<SGlobEntry> Quiet;
	std::vector<SGlobEntry> Raw;
	std::vector<SImporterRef> ImporterExcludes;
	std::vector<STypeRef> TypeIncludes;
	std::vector<STypeRef> TypeExcludes;
	std::vector<SPresetDef> Presets;
	std::vector<SRuleDef> Rules;
	std::vector<SValidationDef> Validation;
};


namespace
{
// ---------------- Names the rule files use ----------------

// The targets of §7.13 and their platforms; `Targets:` keys name either.
struct STargetName
{
	FStringLiteral Name;
	FStringLiteral Platform;
};

constexpr STargetName KnownTargets[] = {
	{ "Win64-D3D12", "Win64" },
	{ "Win64-Headless", "Win64" },
};

constexpr FStringLiteral KnownPlatforms[] = { "Win64" };

constexpr FStringLiteral SafetyKeys[] = {
	"AutoRepairMetas", "DerivedAutoGuids", "AutoResolveDuplicates", "OrphanPairing", "BuilderSourceGuard", "IdLedger", "GuestProcesses",
};

const SImporterDesc* GetRawImporter () noexcept
{
	static const SImporterDesc* const raw = FindImporter("Raw");
	return raw;
}

template <typename TRange, typename TProjection>
FString JoinNames (const TRange& Range, TProjection&& Projection)
{
	FString text;
	for (const auto& item : Range)
	{
		if (!text.IsEmpty())
		{
			text += ", ";
		}
		text += Projection(item);
	}
	return text;
}

FString ListImporterNames ()
{
	return JoinNames(GetImporters(), [] (const SImporterDesc& Importer) { return FStringView(Importer.Name); });
}

FString ListTargetNames ()
{
	FString text = JoinNames(KnownPlatforms, [] (FStringLiteral Name) { return FStringView(Name); });
	for (const STargetName& target : KnownTargets)
	{
		text += ", ";
		text += target.Name;
	}
	return text;
}


// ---------------- Canonical text ----------------

// A YAML double-quoted string: '"' and '\' escaped, control characters as \xHH.
void AppendQuoted (FString& Out, FStringView Text)
{
	constexpr FStringLiteral Hex = "0123456789ABCDEF";
	Out += '"';
	for (const char ch : Text)
	{
		const uint32 byte = static_cast<unsigned char>(ch);
		if (ch == '"' || ch == '\\')
		{
			Out += '\\';
			Out += ch;
		}
		else if (byte < 0x20u || byte == 0x7Fu)
		{
			Out += "\\x";
			Out += Hex[byte >> 4u];
			Out += Hex[byte & 0xFu];
		}
		else
		{
			Out += ch;
		}
	}
	Out += '"';
}

FString Quoted (FStringView Text)
{
	FString out;
	AppendQuoted(out, Text);
	return out;
}

// A setting's value in canonical text: enums, numbers and bools plain, names and strings quoted.
void AppendSettingValue (FString& Out, const SSettingsField& Field, FStringView Text)
{
	if (Field.Kind == ETagType::Name || Field.Kind == ETagType::String)
	{
		AppendQuoted(Out, Text);
	}
	else
	{
		Out += Text;
	}
}

// "{ ColorSpace: Srgb, MaxSize: 1024 }" in table field order.
void AppendSettingsFlow (FString& Out, const SSettingsTable& Table, std::span<const SSettingDef> Values)
{
	std::vector<const SSettingDef*> sorted;
	sorted.reserve(Values.size());
	for (const SSettingDef& value : Values)
	{
		sorted.push_back(&value);
	}
	std::sort(sorted.begin(), sorted.end(), [] (const SSettingDef* A, const SSettingDef* B) { return A->Field < B->Field; });
	Out += "{ ";
	for (uint32 i = 0u; i < sorted.size(); ++i)
	{
		const SSettingsField& field = Table.Fields[sorted[i]->Field];
		if (i > 0u)
		{
			Out += ", ";
		}
		Out += field.Name;
		Out += ": ";
		AppendSettingValue(Out, field, sorted[i]->Text);
	}
	Out += " }";
}

void AppendQuotedList (FString& Out, std::span<const SGlobEntry> Globs)
{
	Out += "[ ";
	for (uint32 i = 0u; i < Globs.size(); ++i)
	{
		if (i > 0u)
		{
			Out += ", ";
		}
		AppendQuoted(Out, Globs[i].Text);
	}
	Out += " ]";
}

void AppendTypeName (FString& Out, const STypeRef& Type)
{
	if (Type.Type.has_value())
	{
		Out += Type.Name;
	}
	else
	{
		AppendQuoted(Out, Type.Name);
	}
}


// ---------------- Paths ----------------

// The part of a mount path below its mount root ("" for the root itself); false when Path is not in MountName.
bool GetRelativeInMount (FStringView Path, FStringView MountName, FStringView& OutRelative) noexcept
{
	FStringView mount;
	if (!SplitMountPath(Path, mount, OutRelative) || mount != MountName)
	{
		return false;
	}
	return true;
}

bool IsMountPrefixName (FStringView Name) noexcept
{
	return Name.Equals<ESearchCase::IgnoreCase>("Engine") || Name.Equals<ESearchCase::IgnoreCase>("Project")
		|| (Name.StartsWith<ESearchCase::IgnoreCase>("Plugin.") && Name.Len() > 7u);
}


// ---------------- The parser ----------------

/**
 * One rule file's text -> CRuleFile::SData plus its listing entries. Every problem is reported (not only the
 * first), each with the position of the key or value at fault; any problem rejects the file.
 */
class CRuleFileParser
{
public:
	CRuleFileParser (const SRuleFileOrigin& InOrigin, std::vector<SDiagnostic>& InOut)
		: Origin(InOrigin)
		, Diagnostics(InOut)
		, Data(std::make_unique<CRuleFile::SData>())
	{
		bMountLevel = Origin.Level == ERuleFileLevel::Mount;
		bEngineFile = bMountLevel && Origin.MountName.Equals<ESearchCase::IgnoreCase>("Engine");
		FStringView relative;
		FStringView mount;
		if (!bMountLevel && SplitMountPath(Origin.FolderMountPath, mount, relative))
		{
			Data->BaseRelative = FString(relative);
		}
	}

	std::shared_ptr<const CRuleFile> Run (FStringView Text)
	{
		reflect::CDataTree tree;
		SDiagnostic parseError;
		if (!reflect::ParseYaml(Text, tree, &parseError))
		{
			Error(parseError.Line, parseError.Column, FString::Format("not valid YAML: {}", parseError.Message));
			return nullptr;
		}
		const reflect::HDataNode root = tree.GetRoot();
		if (tree.IsEmpty() || root.IsNull())
		{
			return Finish();   // comments only: a valid file without rules
		}
		if (!root.IsMap())
		{
			ErrorAt(root, "an Assets.yml holds a map of sections (FormatVersion, Scan, Rules, ...)");
			return nullptr;
		}

		constexpr FStringLiteral Sections[] = {
			"FormatVersion", "Scan", "Importers", "Types", "Presets", "Rules", "Validation", "Cook", "PrimaryAssetTypes", "Safety",
		};
		for (const reflect::SDataChild& child : root.GetChildren())
		{
			const bool bKnown = std::any_of(std::begin(Sections), std::end(Sections), [&child] (FStringLiteral Name) { return child.Key == Name; });
			if (!bKnown)
			{
				ErrorAtKey(child, FString::Format("unknown section '{}' (known: {})", child.Key,
					JoinNames(Sections, [] (FStringLiteral Name) { return FStringView(Name); })));
			}
			else if (!bMountLevel && (child.Key == "Cook" || child.Key == "PrimaryAssetTypes" || child.Key == "Safety"))
			{
				ErrorAtKey(child, FString::Format("a folder Assets.yml cannot set the mount-level section '{}': only a mount's "
					"Config/Assets.yml may", child.Key));
			}
		}

		const reflect::HDataNode version = root.FindChild("FormatVersion");
		if (!version)
		{
			Error(root.GetLine(), root.GetColumn(), "FormatVersion is missing: an Assets.yml starts with 'FormatVersion: 1'");
		}
		else
		{
			SDiagnostic error;
			const std::optional<int64> number = version.AsInt64(&error);
			if (!number.has_value())
			{
				ErrorAt(version, FString::Format("FormatVersion: {}", error.Message));
			}
			else if (*number != 1)
			{
				ErrorAt(version, FString::Format("unsupported FormatVersion {} (this engine reads 1)", *number));
			}
			else
			{
				AddEntry("FormatVersion", "1", version.GetLine());
			}
		}

		ParseScan(root.FindChild("Scan"));
		ParseImporters(root.FindChild("Importers"));
		ParseTypes(root.FindChild("Types"));
		ParsePresets(root.FindChild("Presets"));
		ParseRules(root.FindChild("Rules"));
		ParseValidation(root.FindChild("Validation"));
		if (bMountLevel)
		{
			ParseCookSection(root.FindChild("Cook"));
			ParsePrimaryAssetTypes(root.FindChild("PrimaryAssetTypes"));
			ParseSafety(root.FindChild("Safety"));
		}
		return Finish();
	}

private:
	using HNode = reflect::HDataNode;

	std::shared_ptr<const CRuleFile> Finish ()
	{
		if (ErrorCount > 0u)
		{
			return nullptr;
		}
		FString normalised;
		for (const SRuleListEntry& entry : Entries)
		{
			normalised += entry.Kind;
			normalised += '\x1F';
			normalised += entry.Text;
			normalised += '\x1E';
		}
		uint64 rulesHash = hash::Xxh3_64(normalised.View());
		if (rulesHash == 0u)
		{
			rulesHash = 1u;   // 0 means "no rules"
		}
		return std::make_shared<const CRuleFile>(Origin, std::move(Data), std::move(Entries), rulesHash);
	}

	void Error (uint32 Line, uint32 Column, FString Message)
	{
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Error;
		diagnostic.Code = Rule001;
		diagnostic.Subject = Origin.FolderMountPath;
		diagnostic.File = Origin.File;
		diagnostic.Line = Line;
		diagnostic.Column = Column;
		diagnostic.Message = std::move(Message);
		Diagnostics.push_back(std::move(diagnostic));
		++ErrorCount;
	}

	void ErrorAt (HNode Node, FString Message) { Error(Node.GetLine(), Node.GetColumn(), std::move(Message)); }
	void ErrorAtKey (const reflect::SDataChild& Child, FString Message) { Error(Child.KeyLine, Child.KeyColumn, std::move(Message)); }

	void AddEntry (FStringView Kind, FString Text, uint32 Line, int32 RuleIndex = -1)
	{
		SRuleListEntry entry;
		entry.Kind = FString(Kind);
		entry.Text = std::move(Text);
		entry.Line = Line;
		entry.RuleIndex = RuleIndex;
		Entries.push_back(std::move(entry));
	}

	// The children of a map, each key checked against Known; false (with a RULE001) when Node is not a map.
	bool CheckMap (HNode Node, FStringView What, std::span<const FStringLiteral> Known)
	{
		if (!Node.IsMap())
		{
			ErrorAt(Node, FString::Format("{} must be a map", What));
			return false;
		}
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			const bool bKnown = std::any_of(Known.begin(), Known.end(), [&child] (FStringLiteral Name) { return child.Key == Name; });
			if (!bKnown)
			{
				ErrorAtKey(child, FString::Format("unknown key '{}' in {} (known: {})", child.Key, What,
					JoinNames(Known, [] (FStringLiteral Name) { return FStringView(Name); })));
			}
		}
		return true;
	}

	std::optional<FStringView> ReadText (HNode Node, FStringView What)
	{
		if (!Node.IsScalar())
		{
			ErrorAt(Node, FString::Format("{} must be a text", What));
			return std::nullopt;
		}
		const FStringView text = Node.GetText();
		if (text.IsEmpty())
		{
			ErrorAt(Node, FString::Format("{} must not be empty", What));
			return std::nullopt;
		}
		return text;
	}

	// A text or a sequence of texts.
	bool ReadTextList (HNode Node, FStringView What, std::vector<std::pair<FStringView, HNode>>& OutItems)
	{
		if (Node.IsScalar())
		{
			if (const std::optional<FStringView> text = ReadText(Node, What))
			{
				OutItems.emplace_back(*text, Node);
				return true;
			}
			return false;
		}
		if (!Node.IsSequence())
		{
			ErrorAt(Node, FString::Format("{} must be a text or a list of texts", What));
			return false;
		}
		bool bOk = true;
		for (const reflect::SDataChild& item : Node.GetChildren())
		{
			if (const std::optional<FStringView> text = ReadText(item.Value, What))
			{
				OutItems.emplace_back(*text, item.Value);
			}
			else
			{
				bOk = false;
			}
		}
		return bOk;
	}

	std::optional<bool> ReadBool (HNode Node, FStringView What)
	{
		SDiagnostic error;
		const std::optional<bool> value = Node.AsBool(&error);
		if (!value.has_value())
		{
			ErrorAt(Node, FString::Format("{} must be true or false", What));
		}
		return value;
	}

	std::optional<int64> ReadInt (HNode Node, FStringView What, int64 Min, int64 Max)
	{
		SDiagnostic error;
		const std::optional<int64> value = Node.AsInt64(&error);
		if (!value.has_value())
		{
			ErrorAt(Node, FString::Format("{} must be a whole number: {}", What, error.Message));
			return std::nullopt;
		}
		if (*value < Min || *value > Max)
		{
			ErrorAt(Node, FString::Format("{} must be between {} and {}", What, Min, Max));
			return std::nullopt;
		}
		return value;
	}

	std::optional<ECookRule> ReadCook (HNode Node, FStringView What)
	{
		const std::optional<FStringView> text = ReadText(Node, What);
		if (!text.has_value())
		{
			return std::nullopt;
		}
		ECookRule rule = ECookRule::Auto;
		if (enum_::TryParse(*text, &rule))
		{
			return rule;
		}
		ErrorAt(Node, FString::Format("'{}' is not a cook rule (Auto, Always, Never, EditorOnly, DevelopmentOnly)", *text));
		return std::nullopt;
	}

	const SImporterDesc* ReadImporter (HNode Node, FStringView What)
	{
		const std::optional<FStringView> text = ReadText(Node, What);
		if (!text.has_value())
		{
			return nullptr;
		}
		const SImporterDesc* importer = FindImporter(*text);
		if (importer == nullptr)
		{
			ErrorAt(Node, FString::Format("unknown importer '{}' (known: {})", *text, ListImporterNames()));
		}
		return importer;
	}

	std::optional<STypeRef> ReadType (HNode Node, FStringView What)
	{
		const std::optional<FStringView> text = ReadText(Node, What);
		if (!text.has_value())
		{
			return std::nullopt;
		}
		STypeRef type;
		type.Line = Node.GetLine();
		if (const std::optional<reflect::TypeId> id = CAssetTypeRegistry::Find(*text))
		{
			type.Type = *id;
			type.Name = FString(CAssetTypeRegistry::Get(*id).Name);
			return type;
		}
		if (text->StartsWith<ESearchCase::IgnoreCase>("frt."))
		{
			ErrorAt(Node, FString::Format("unknown engine type '{}' (known: {})", *text,
				JoinNames(CAssetTypeRegistry::GetTypes(), [] (const SAssetTypeInfo& Info) { return FStringView(Info.Name); })));
			return std::nullopt;
		}
		type.Name = FString(*text);   // a reflected game type (M5): kept, matches nothing yet
		return type;
	}

	/**
	 * A glob, its mount prefix resolved (§3.5.2, §3.5.4): in a mount file "/Engine/", "/Project/" or
	 * "/Plugin.<Name>/" makes it absolute - any mount in the engine's file, only its own elsewhere; a folder file
	 * takes none.
	 */
	std::optional<SGlobEntry> ReadGlob (FStringView Text, HNode Node)
	{
		SGlobEntry entry;
		entry.Text = FString(Text);
		entry.Line = Node.GetLine();
		FString pattern(Text);
		if (Text.StartsWith('/'))
		{
			const SStringSplit split = Text.RightChop(1u).SplitFirst('/');
			if (split.bFound && IsMountPrefixName(split.Left))
			{
				if (!bMountLevel)
				{
					ErrorAt(Node, FString::Format("'{}': a folder Assets.yml cannot use the mount prefix '/{}/' - its globs are relative "
						"to its folder", Text, split.Left));
					return std::nullopt;
				}
				if (!bEngineFile && !split.Left.Equals<ESearchCase::IgnoreCase>(Origin.MountName))
				{
					ErrorAt(Node, FString::Format("'{}': the rules of /{} apply to its own files only, never to /{}", Text, Origin.MountName,
						split.Left));
					return std::nullopt;
				}
				entry.OnlyMount = FString(split.Left);
				entry.OnlyMountFolded = entry.OnlyMount.FoldCase();
				pattern = FString("/") + split.Right;
			}
		}
		FString error;
		std::optional<CPathGlob> glob = CPathGlob::Compile(pattern, &error);
		if (!glob.has_value())
		{
			ErrorAt(Node, FString::Format("bad glob '{}': {}", Text, error));
			return std::nullopt;
		}
		entry.Glob = std::move(*glob);
		entry.RequiredRuns = GetRequiredRuns(pattern);
		return entry;
	}

	bool ReadGlobList (HNode Node, FStringView What, std::vector<SGlobEntry>& Out)
	{
		std::vector<std::pair<FStringView, HNode>> items;
		bool bOk = ReadTextList(Node, What, items);
		for (const auto& [text, node] : items)
		{
			if (std::optional<SGlobEntry> glob = ReadGlob(text, node))
			{
				Out.push_back(std::move(*glob));
			}
			else
			{
				bOk = false;
			}
		}
		return bOk;
	}

	// "<Importer>/<Name>" with an importer that has settings; the canonical name, or nullopt with a RULE001.
	std::optional<std::pair<FString, const SImporterDesc*>> ReadPresetName (FStringView Text, uint32 Line, uint32 Column)
	{
		const SStringSplit split = Text.SplitFirst('/');
		if (!split.bFound || split.Left.IsEmpty() || split.Right.IsEmpty())
		{
			Error(Line, Column, FString::Format("preset '{}' must be named '<Importer>/<Name>' (Texture/Color)", Text));
			return std::nullopt;
		}
		const SImporterDesc* importer = FindImporter(split.Left);
		if (importer == nullptr)
		{
			Error(Line, Column, FString::Format("preset '{}': unknown importer '{}' (known: {})", Text, split.Left, ListImporterNames()));
			return std::nullopt;
		}
		if (importer->Settings == nullptr)
		{
			Error(Line, Column, FString::Format("preset '{}': the importer {} has no settings", Text, importer->Name));
			return std::nullopt;
		}
		return std::pair<FString, const SImporterDesc*>(FString(importer->Name) + "/" + split.Right, importer);
	}

	// A map of settings fields of Table, each validated and normalised.
	bool ReadSettings (HNode Node, const SSettingsTable& Table, FStringView What, std::vector<SSettingDef>& Out)
	{
		if (!Node.IsMap())
		{
			ErrorAt(Node, FString::Format("{} must be a map of {} settings", What, Table.Name));
			return false;
		}
		bool bOk = true;
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			const int32 field = FindSettingsField(Table, child.Key);
			if (field < 0)
			{
				ErrorAtKey(child, FString::Format("unknown {} setting '{}' (fields: {})", Table.Name, child.Key,
					JoinNames(Table.Fields, [] (const SSettingsField& Field) { return FStringView(Field.Name); })));
				bOk = false;
				continue;
			}
			if (std::any_of(Out.begin(), Out.end(), [field] (const SSettingDef& Def) { return Def.Field == static_cast<uint16>(field); }))
			{
				ErrorAtKey(child, FString::Format("the setting '{}' is set twice", Table.Fields[static_cast<uint32>(field)].Name));
				bOk = false;
				continue;
			}
			const SSettingsField& desc = Table.Fields[static_cast<uint32>(field)];
			std::optional<FString> value = ReadSettingValue(child.Value, desc);
			if (!value.has_value())
			{
				bOk = false;
				continue;
			}
			Out.push_back({ static_cast<uint16>(field), std::move(*value), child.KeyLine });
		}
		return bOk;
	}

	std::optional<FString> ReadSettingValue (HNode Node, const SSettingsField& Field)
	{
		SDiagnostic error;
		switch (Field.Kind)
		{
		case ETagType::Enum:
		{
			const std::optional<FStringView> text = ReadText(Node, Field.Name);
			if (!text.has_value())
			{
				return std::nullopt;
			}
			for (const FStringLiteral value : Field.EnumValues)
			{
				if (text->Equals<ESearchCase::IgnoreCase>(value))
				{
					return FString(value);
				}
			}
			ErrorAt(Node, FString::Format("'{}' is not a {} value ({})", *text, Field.Name,
				JoinNames(Field.EnumValues, [] (FStringLiteral Value) { return FStringView(Value); })));
			return std::nullopt;
		}
		case ETagType::Int64:
		{
			const std::optional<int64> number = Node.AsInt64(&error);
			if (!number.has_value())
			{
				ErrorAt(Node, FString::Format("{} must be a whole number: {}", Field.Name, error.Message));
				return std::nullopt;
			}
			return FString::FromNumber(*number);
		}
		case ETagType::Double:
		{
			const std::optional<double> number = Node.AsDouble(&error);
			if (!number.has_value())
			{
				ErrorAt(Node, FString::Format("{} must be a number: {}", Field.Name, error.Message));
				return std::nullopt;
			}
			if (!std::isfinite(*number))
			{
				// YAML's .nan and .inf parse as numbers, but no setting means anything by them, and a NaN would reach
				// the import and its derived-data keys.
				ErrorAt(Node, FString::Format("{} must be a finite number", Field.Name));
				return std::nullopt;
			}
			return FString::FromNumber(*number);
		}
		case ETagType::Bool:
		{
			const std::optional<bool> value = Node.AsBool(&error);
			if (!value.has_value())
			{
				ErrorAt(Node, FString::Format("{} must be true or false", Field.Name));
				return std::nullopt;
			}
			return FString(*value ? "true" : "false");
		}
		default:
		{
			const std::optional<FStringView> text = ReadText(Node, Field.Name);
			return text.has_value() ? std::optional<FString>(FString(*text)) : std::nullopt;
		}
		}
	}

	// ---- Sections ----

	void ParseScan (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		constexpr FStringLiteral Keys[] = { "Include", "Exclude", "Quiet", "Raw" };
		if (!CheckMap(Node, "Scan", Keys))
		{
			return;
		}
		const std::pair<FStringView, std::vector<SGlobEntry>*> lists[] = {
			{ "Include", &Data->Include }, { "Exclude", &Data->Exclude }, { "Quiet", &Data->Quiet }, { "Raw", &Data->Raw },
		};
		for (const auto& [key, list] : lists)
		{
			const HNode value = Node.FindChild(key);
			if (!value)
			{
				continue;
			}
			const FString what = FString("Scan.") + key;
			ReadGlobList(value, what, *list);
			for (const SGlobEntry& glob : *list)
			{
				AddEntry(what, Quoted(glob.Text), glob.Line);
			}
		}
	}

	void ParseImporters (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		constexpr FStringLiteral Keys[] = { "Exclude" };
		if (!CheckMap(Node, "Importers", Keys))
		{
			return;
		}
		const HNode exclude = Node.FindChild("Exclude");
		if (!exclude)
		{
			return;
		}
		std::vector<std::pair<FStringView, HNode>> items;
		ReadTextList(exclude, "Importers.Exclude", items);
		for (const auto& [text, node] : items)
		{
			const SImporterDesc* importer = FindImporter(text);
			if (importer == nullptr)
			{
				ErrorAt(node, FString::Format("unknown importer '{}' (known: {})", text, ListImporterNames()));
				continue;
			}
			Data->ImporterExcludes.push_back({ importer, node.GetLine() });
			AddEntry("Importers.Exclude", FString(importer->Name), node.GetLine());
		}
	}

	void ParseTypes (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		constexpr FStringLiteral Keys[] = { "Include", "Exclude" };
		if (!CheckMap(Node, "Types", Keys))
		{
			return;
		}
		const std::pair<FStringView, std::vector<STypeRef>*> lists[] = { { "Include", &Data->TypeIncludes }, { "Exclude", &Data->TypeExcludes } };
		for (const auto& [key, list] : lists)
		{
			const HNode value = Node.FindChild(key);
			if (!value)
			{
				continue;
			}
			const FString what = FString("Types.") + key;
			std::vector<std::pair<FStringView, HNode>> items;
			ReadTextList(value, what, items);
			for (const auto& [text, node] : items)
			{
				if (std::optional<STypeRef> type = ReadType(node, what))
				{
					FString entry;
					AppendTypeName(entry, *type);
					AddEntry(what, std::move(entry), type->Line);
					list->push_back(std::move(*type));
				}
			}
		}
	}

	void ParsePresets (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		if (!Node.IsMap())
		{
			ErrorAt(Node, "Presets must be a map of '<Importer>/<Name>: { settings }'");
			return;
		}
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			std::optional<std::pair<FString, const SImporterDesc*>> name = ReadPresetName(child.Key, child.KeyLine, child.KeyColumn);
			if (!name.has_value())
			{
				continue;
			}
			const bool bDuplicate = std::any_of(Data->Presets.begin(), Data->Presets.end(),
				[&name] (const SPresetDef& Preset) { return Preset.Name.Equals<ESearchCase::IgnoreCase>(name->first); });
			if (bDuplicate)
			{
				ErrorAtKey(child, FString::Format("the preset '{}' is defined twice in this file", child.Key));
				continue;
			}
			SPresetDef preset;
			preset.Name = std::move(name->first);
			preset.Importer = name->second;
			preset.Line = child.KeyLine;
			if (!ReadSettings(child.Value, *preset.Importer->Settings, FString::Format("the preset '{}'", child.Key), preset.Values))
			{
				continue;
			}
			FString text;
			AppendQuoted(text, preset.Name);
			text += ": ";
			AppendSettingsFlow(text, *preset.Importer->Settings, preset.Values);
			AddEntry("Preset", std::move(text), preset.Line);
			Data->Presets.push_back(std::move(preset));
		}
	}

	void ParseRules (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		if (!Node.IsSequence())
		{
			ErrorAt(Node, "Rules must be a list of rules ('- Match: ...')");
			return;
		}
		int32 index = 0;
		for (const reflect::SDataChild& item : Node.GetChildren())
		{
			ParseRule(item.Value, index);
			++index;
		}
	}

	bool ReadTargetBlock (const reflect::SDataChild& Child, const SRuleDef& Rule, STargetBlock& Out)
	{
		const FStringView key = Child.Key;
		bool bFound = false;
		for (const FStringLiteral platform : KnownPlatforms)
		{
			if (key.Equals<ESearchCase::IgnoreCase>(platform))
			{
				Out.Name = FString(platform);
				Out.bPlatform = true;
				bFound = true;
			}
		}
		for (const STargetName& target : KnownTargets)
		{
			if (key.Equals<ESearchCase::IgnoreCase>(target.Name))
			{
				Out.Name = FString(target.Name);
				Out.bPlatform = false;
				bFound = true;
			}
		}
		if (!bFound)
		{
			ErrorAtKey(Child, FString::Format("unknown platform or target '{}' in Targets (known: {})", key, ListTargetNames()));
			return false;
		}
		Out.Line = Child.KeyLine;
		constexpr FStringLiteral Keys[] = { "Cook", "Chunk", "Settings" };
		if (!CheckMap(Child.Value, FString::Format("Targets.{}", key), Keys))
		{
			return false;
		}
		bool bOk = true;
		if (const HNode cook = Child.Value.FindChild("Cook"))
		{
			Out.Cook = ReadCook(cook, "Cook");
			Out.CookLine = cook.GetLine();
			bOk = bOk && Out.Cook.has_value();
		}
		if (const HNode chunk = Child.Value.FindChild("Chunk"))
		{
			const std::optional<int64> value = ReadInt(chunk, "Chunk", 0, 65535);
			if (value.has_value())
			{
				Out.Chunk = static_cast<uint16>(*value);
				Out.ChunkLine = chunk.GetLine();
			}
			bOk = bOk && value.has_value();
		}
		if (const HNode settings = Child.Value.FindChild("Settings"))
		{
			if (Rule.SettingsTable == nullptr)
			{
				ErrorAt(settings, "Settings in a Targets block need 'For: <Importer>' (or 'Importer:') on the rule");
				bOk = false;
			}
			else
			{
				bOk = ReadSettings(settings, *Rule.SettingsTable, "Settings", Out.Settings) && bOk;
			}
		}
		return bOk;
	}

	void ParseRule (HNode Node, int32 Index)
	{
		constexpr FStringLiteral Keys[] = {
			"Match", "Type", "Label", "Preset", "For", "Settings", "Importer", "Labels", "Cook", "Chunk", "Targets", "Final",
		};
		const FString what = FString::Format("rule {}", Index + 1);
		if (!CheckMap(Node, what, Keys))
		{
			return;
		}
		SRuleDef rule;
		rule.Line = Node.GetLine();
		rule.Index = Index;
		const uint32 errorsBefore = ErrorCount;

		if (const HNode match = Node.FindChild("Match"))
		{
			ReadGlobList(match, "Match", rule.Match);
			if (match.IsSequence() && match.GetCount() == 0u)
			{
				ErrorAt(match, "Match lists no pattern (leave Match out to match every file)");
			}
		}
		if (const HNode type = Node.FindChild("Type"))
		{
			rule.Type = ReadType(type, "Type");
		}
		if (const HNode label = Node.FindChild("Label"))
		{
			if (const std::optional<FStringView> text = ReadText(label, "Label"))
			{
				rule.Label = FString(*text);
			}
		}
		if (const HNode preset = Node.FindChild("Preset"))
		{
			if (const std::optional<FStringView> text = ReadText(preset, "Preset"))
			{
				if (std::optional<std::pair<FString, const SImporterDesc*>> name = ReadPresetName(*text, preset.GetLine(), preset.GetColumn()))
				{
					rule.Preset = std::move(name->first);
					rule.PresetLine = preset.GetLine();
				}
			}
		}
		const SImporterDesc* forImporter = nullptr;
		if (const HNode forNode = Node.FindChild("For"))
		{
			forImporter = ReadImporter(forNode, "For");
			if (forImporter != nullptr && forImporter->Settings == nullptr)
			{
				ErrorAt(forNode, FString::Format("For: the importer {} has no settings", forImporter->Name));
				forImporter = nullptr;
			}
		}
		if (const HNode importer = Node.FindChild("Importer"))
		{
			rule.Importer = ReadImporter(importer, "Importer");
			rule.ImporterLine = importer.GetLine();
			if (Node.FindChild("Type") || Node.FindChild("Label"))
			{
				ErrorAt(importer, "a rule with a Type or Label selector cannot set Importer: type and labels are known only after "
					"Analyze, which the importer decides");
			}
		}
		rule.SettingsTable = forImporter != nullptr ? forImporter->Settings : (rule.Importer != nullptr ? rule.Importer->Settings : nullptr);
		if (forImporter != nullptr && rule.Importer != nullptr && rule.Importer->Settings != forImporter->Settings)
		{
			ErrorAt(Node.FindChild("For"), FString::Format("For: {} does not share the settings of the rule's Importer: {}", forImporter->Name,
				rule.Importer->Name));
		}
		if (const HNode settings = Node.FindChild("Settings"))
		{
			if (rule.SettingsTable == nullptr)
			{
				ErrorAt(settings, "Settings need 'For: <Importer>' (or 'Importer:' with an importer that has settings)");
			}
			else
			{
				ReadSettings(settings, *rule.SettingsTable, "Settings", rule.Settings);
			}
		}
		if (const HNode labels = Node.FindChild("Labels"))
		{
			std::vector<std::pair<FStringView, HNode>> items;
			ReadTextList(labels, "Labels", items);
			for (const auto& [text, node] : items)
			{
				rule.Labels.emplace_back(FString(text), node.GetLine());
			}
		}
		if (const HNode cook = Node.FindChild("Cook"))
		{
			rule.Cook = ReadCook(cook, "Cook");
			rule.CookLine = cook.GetLine();
		}
		if (const HNode chunk = Node.FindChild("Chunk"))
		{
			if (const std::optional<int64> value = ReadInt(chunk, "Chunk", 0, 65535))
			{
				rule.Chunk = static_cast<uint16>(*value);
				rule.ChunkLine = chunk.GetLine();
			}
		}
		if (const HNode targets = Node.FindChild("Targets"))
		{
			if (!targets.IsMap())
			{
				ErrorAt(targets, "Targets must be a map of '<platform or target>: { Cook, Chunk, Settings }'");
			}
			else
			{
				for (const reflect::SDataChild& child : targets.GetChildren())
				{
					STargetBlock block;
					if (ReadTargetBlock(child, rule, block))
					{
						rule.Targets.push_back(std::move(block));
					}
				}
			}
		}
		if (const HNode final = Node.FindChild("Final"))
		{
			rule.bFinal = ReadBool(final, "Final").value_or(false);
		}

		// Type and Label rules are evaluated after Analyze, so nothing they set may change Analyze.
		if (rule.IsSelectorRule())
		{
			const auto checkAnalyzeRelevant = [this] (const SSettingsTable* Table, std::span<const SSettingDef> Values)
			{
				for (const SSettingDef& value : Values)
				{
					if (Table != nullptr && Table->Fields[value.Field].bAnalyzeRelevant)
					{
						Error(value.Line, 0u, FString::Format("a rule with a Type or Label selector cannot set '{}': it changes what Analyze "
							"reads, and type and labels are known only after Analyze", Table->Fields[value.Field].Name));
					}
				}
			};
			checkAnalyzeRelevant(rule.SettingsTable, rule.Settings);
			for (const STargetBlock& block : rule.Targets)
			{
				checkAnalyzeRelevant(rule.SettingsTable, block.Settings);
			}
		}
		if (ErrorCount != errorsBefore)
		{
			return;
		}
		AddEntry("Rule", FormatRule(rule), rule.Line, Index);
		Data->Rules.push_back(std::move(rule));
	}

	static FString FormatRule (const SRuleDef& Rule)
	{
		FString text = "{ ";
		bool bFirst = true;
		const auto key = [&text, &bFirst] (FStringView Name)
		{
			if (!bFirst)
			{
				text += ", ";
			}
			bFirst = false;
			text += Name;
			text += ": ";
		};
		if (!Rule.Match.empty())
		{
			key("Match");
			AppendQuotedList(text, Rule.Match);
		}
		if (Rule.Type.has_value())
		{
			key("Type");
			AppendTypeName(text, *Rule.Type);
		}
		if (Rule.Label.has_value())
		{
			key("Label");
			AppendQuoted(text, *Rule.Label);
		}
		if (!Rule.Preset.IsEmpty())
		{
			key("Preset");
			AppendQuoted(text, Rule.Preset);
		}
		if (Rule.SettingsTable != nullptr && (Rule.Importer == nullptr || Rule.Importer->Settings != Rule.SettingsTable))
		{
			key("For");
			text += Rule.SettingsTable->Name;
		}
		if (Rule.Importer != nullptr)
		{
			key("Importer");
			text += Rule.Importer->Name;
		}
		if (!Rule.Settings.empty())
		{
			key("Settings");
			AppendSettingsFlow(text, *Rule.SettingsTable, Rule.Settings);
		}
		if (!Rule.Labels.empty())
		{
			key("Labels");
			text += "[ ";
			for (uint32 i = 0u; i < Rule.Labels.size(); ++i)
			{
				if (i > 0u)
				{
					text += ", ";
				}
				AppendQuoted(text, Rule.Labels[i].first);
			}
			text += " ]";
		}
		if (Rule.Cook.has_value())
		{
			key("Cook");
			text += enum_::ToString(*Rule.Cook);
		}
		if (Rule.Chunk.has_value())
		{
			key("Chunk");
			text.AppendNumber(*Rule.Chunk);
		}
		if (!Rule.Targets.empty())
		{
			key("Targets");
			text += "{ ";
			for (uint32 i = 0u; i < Rule.Targets.size(); ++i)
			{
				const STargetBlock& block = Rule.Targets[i];
				if (i > 0u)
				{
					text += ", ";
				}
				text += block.Name;
				text += ": { ";
				bool bFirstField = true;
				const auto field = [&text, &bFirstField] (FStringView Name)
				{
					if (!bFirstField)
					{
						text += ", ";
					}
					bFirstField = false;
					text += Name;
					text += ": ";
				};
				if (block.Cook.has_value())
				{
					field("Cook");
					text += enum_::ToString(*block.Cook);
				}
				if (block.Chunk.has_value())
				{
					field("Chunk");
					text.AppendNumber(*block.Chunk);
				}
				if (!block.Settings.empty())
				{
					field("Settings");
					AppendSettingsFlow(text, *Rule.SettingsTable, block.Settings);
				}
				text += " }";
			}
			text += " }";
		}
		if (Rule.bFinal)
		{
			key("Final");
			text += "true";
		}
		text += " }";
		if (bFirst)
		{
			text = "{ }";
		}
		return text;
	}

	void ParseValidation (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		if (!Node.IsMap())
		{
			ErrorAt(Node, "Validation must be a map of '<CODE>: Error|Warning|Info' (and Naming)");
			return;
		}
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			if (child.Key == "Naming")
			{
				ParseNaming(child.Value);
				continue;
			}
			const std::optional<uint32> code = EncodeDiagnosticCode(child.Key);
			if (!code.has_value())
			{
				ErrorAtKey(child, FString::Format("'{}' is not a diagnostic code (one to five capital letters and three digits: REF010)", child.Key));
				continue;
			}
			const std::optional<FStringView> text = ReadText(child.Value, child.Key);
			if (!text.has_value())
			{
				continue;
			}
			std::optional<ESeverity> severity;
			for (const ESeverity candidate : { ESeverity::Error, ESeverity::Warning, ESeverity::Info })
			{
				if (text->Equals<ESearchCase::IgnoreCase>(candidate == ESeverity::Error ? "Error" : candidate == ESeverity::Warning ? "Warning" : "Info"))
				{
					severity = candidate;
				}
			}
			if (!severity.has_value())
			{
				ErrorAt(child.Value, FString::Format("'{}' is not a severity (Error, Warning, Info)", *text));
				continue;
			}
			Data->Validation.push_back({ *code, *severity, child.KeyLine });
			AddEntry("Validation", FString::Format("{}: {}", child.Key, *severity == ESeverity::Error ? "Error" : *severity == ESeverity::Warning ? "Warning" : "Info"),
				child.KeyLine);
		}
	}

	void ParseNaming (HNode Node)
	{
		if (!Node.IsSequence())
		{
			ErrorAt(Node, "Validation.Naming must be a list of '{ Type: <type>, Pattern: <glob> }'");
			return;
		}
		constexpr FStringLiteral Keys[] = { "Type", "Pattern" };
		for (const reflect::SDataChild& item : Node.GetChildren())
		{
			if (!CheckMap(item.Value, "a Validation.Naming entry", Keys))
			{
				continue;
			}
			const HNode typeNode = item.Value.FindChild("Type");
			const HNode patternNode = item.Value.FindChild("Pattern");
			if (!typeNode || !patternNode)
			{
				ErrorAt(item.Value, "a Validation.Naming entry needs Type and Pattern");
				continue;
			}
			const std::optional<STypeRef> type = ReadType(typeNode, "Type");
			const std::optional<FStringView> pattern = ReadText(patternNode, "Pattern");
			if (!type.has_value() || !pattern.has_value())
			{
				continue;
			}
			FString error;
			if (!CPathGlob::Compile(*pattern, &error).has_value())
			{
				ErrorAt(patternNode, FString::Format("bad name pattern '{}': {}", *pattern, error));
				continue;
			}
			FString text = "{ Type: ";
			AppendTypeName(text, *type);
			text += ", Pattern: ";
			AppendQuoted(text, *pattern);
			text += " }";
			AddEntry("Validation.Naming", std::move(text), item.Value.GetLine());
		}
	}

	void ParseCookSection (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		constexpr FStringLiteral Keys[] = { "Mode", "Roots", "PathLookup", "CookedTags", "Targets", "ContentId", "ContentVersion" };
		if (!CheckMap(Node, "Cook", Keys))
		{
			return;
		}
		if (const HNode mode = Node.FindChild("Mode"))
		{
			if (const std::optional<FStringView> text = ReadText(mode, "Cook.Mode"))
			{
				if (text->Equals<ESearchCase::IgnoreCase>("AllIncluded") || text->Equals<ESearchCase::IgnoreCase>("Reachable"))
				{
					AddEntry("Cook.Mode", FString(text->Equals<ESearchCase::IgnoreCase>("AllIncluded") ? "AllIncluded" : "Reachable"), mode.GetLine());
				}
				else
				{
					ErrorAt(mode, FString::Format("'{}' is not a cook mode (AllIncluded, Reachable)", *text));
				}
			}
		}
		if (const HNode roots = Node.FindChild("Roots"))
		{
			std::vector<std::pair<FStringView, HNode>> items;
			if (roots.IsSequence() && roots.GetCount() == 0u)
			{
				AddEntry("Cook.Roots", "[ ]", roots.GetLine());
			}
			else if (ReadTextList(roots, "Cook.Roots", items))
			{
				FString text = "[ ";
				for (uint32 i = 0u; i < items.size(); ++i)
				{
					if (i > 0u)
					{
						text += ", ";
					}
					AppendQuoted(text, items[i].first);
				}
				text += " ]";
				AddEntry("Cook.Roots", std::move(text), roots.GetLine());
			}
		}
		if (const HNode pathLookup = Node.FindChild("PathLookup"))
		{
			if (const std::optional<bool> value = ReadBool(pathLookup, "Cook.PathLookup"))
			{
				AddEntry("Cook.PathLookup", FString(*value ? "true" : "false"), pathLookup.GetLine());
			}
		}
		if (const HNode cookedTags = Node.FindChild("CookedTags"))
		{
			ParseCookedTags(cookedTags);
		}
		if (const HNode targets = Node.FindChild("Targets"))
		{
			std::vector<std::pair<FStringView, HNode>> items;
			if (!(targets.IsSequence() && targets.GetCount() == 0u))
			{
				ReadTextList(targets, "Cook.Targets", items);
			}
			FString text = "[ ";
			bool bOk = true;
			for (uint32 i = 0u; i < items.size(); ++i)
			{
				const STargetName* found = nullptr;
				for (const STargetName& target : KnownTargets)
				{
					if (items[i].first.Equals<ESearchCase::IgnoreCase>(target.Name))
					{
						found = &target;
					}
				}
				if (found == nullptr)
				{
					ErrorAt(items[i].second, FString::Format("unknown target '{}' in Cook.Targets (known: {})", items[i].first,
						JoinNames(KnownTargets, [] (const STargetName& Target) { return FStringView(Target.Name); })));
					bOk = false;
					continue;
				}
				if (i > 0u)
				{
					text += ", ";
				}
				text += found->Name;
			}
			text += " ]";
			if (bOk)
			{
				AddEntry("Cook.Targets", std::move(text), targets.GetLine());
			}
		}
		if (const HNode contentId = Node.FindChild("ContentId"))
		{
			if (const std::optional<FStringView> text = ReadText(contentId, "Cook.ContentId"))
			{
				const bool bHex = text->Len() == SGuid::TextLength && std::all_of(text->begin(), text->end(),
					[] (char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F'); });
				if (!bHex)
				{
					ErrorAt(contentId, FString::Format("Cook.ContentId '{}' must be a GUID of 32 hex digits", *text));
				}
				else
				{
					AddEntry("Cook.ContentId", FString(*text).ToLower(), contentId.GetLine());
				}
			}
		}
		if (const HNode contentVersion = Node.FindChild("ContentVersion"))
		{
			if (const std::optional<int64> value = ReadInt(contentVersion, "Cook.ContentVersion", 0, 0x7FFFFFFF))
			{
				AddEntry("Cook.ContentVersion", FString::FromNumber(*value), contentVersion.GetLine());
			}
		}
	}

	// `Cook.CookedTags: { <type>: [ <tag>, ... ] }` for types this mount does not own (§10.6): the engine owns the
	// frt. types, so the engine's own file listing one is RULE001; a known type's tags must exist.
	void ParseCookedTags (HNode Node)
	{
		if (!Node.IsMap())
		{
			ErrorAt(Node, "Cook.CookedTags must be a map of '<type>: [ <tag>, ... ]'");
			return;
		}
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			STypeRef type;
			type.Line = child.KeyLine;
			if (const std::optional<reflect::TypeId> id = CAssetTypeRegistry::Find(child.Key))
			{
				type.Type = *id;
				type.Name = FString(CAssetTypeRegistry::Get(*id).Name);
				if (bEngineFile)
				{
					ErrorAtKey(child, FString::Format("Cook.CookedTags names {}, one of /Engine's own types: own types choose their cooked "
						"tags in code", type.Name));
					continue;
				}
			}
			else if (child.Key.StartsWith<ESearchCase::IgnoreCase>("frt."))
			{
				ErrorAtKey(child, FString::Format("unknown engine type '{}' in Cook.CookedTags", child.Key));
				continue;
			}
			else
			{
				type.Name = FString(child.Key);
			}
			std::vector<std::pair<FStringView, HNode>> items;
			if (!ReadTextList(child.Value, "Cook.CookedTags", items))
			{
				continue;
			}
			FString text;
			AppendTypeName(text, type);
			text += ": [ ";
			bool bOk = true;
			for (uint32 i = 0u; i < items.size(); ++i)
			{
				FString tag(items[i].first);
				if (type.Type.has_value())
				{
					const std::optional<uint32> key = CAssetTypeRegistry::FindKey(*type.Type, items[i].first);
					if (!key.has_value())
					{
						ErrorAt(items[i].second, FString::Format("{} has no tag '{}'", type.Name, items[i].first));
						bOk = false;
						continue;
					}
					tag = FString(CAssetTypeRegistry::Get(*type.Type).Keys[*key].Name);
				}
				if (i > 0u)
				{
					text += ", ";
				}
				text += tag;
			}
			text += " ]";
			if (bOk)
			{
				AddEntry("Cook.CookedTags", std::move(text), child.KeyLine);
			}
		}
	}

	void ParsePrimaryAssetTypes (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		if (!Node.IsSequence())
		{
			ErrorAt(Node, "PrimaryAssetTypes must be a list of '{ Type, Directories, Cook }'");
			return;
		}
		constexpr FStringLiteral Keys[] = { "Type", "Directories", "Cook" };
		for (const reflect::SDataChild& item : Node.GetChildren())
		{
			if (!CheckMap(item.Value, "a PrimaryAssetTypes entry", Keys))
			{
				continue;
			}
			const HNode typeNode = item.Value.FindChild("Type");
			if (!typeNode)
			{
				ErrorAt(item.Value, "a PrimaryAssetTypes entry needs Type");
				continue;
			}
			const std::optional<STypeRef> type = ReadType(typeNode, "Type");
			std::vector<SGlobEntry> directories;
			bool bOk = type.has_value();
			if (const HNode dirs = item.Value.FindChild("Directories"))
			{
				bOk = ReadGlobList(dirs, "Directories", directories) && bOk;
			}
			std::optional<ECookRule> cook;
			if (const HNode cookNode = item.Value.FindChild("Cook"))
			{
				cook = ReadCook(cookNode, "Cook");
				bOk = bOk && cook.has_value();
			}
			if (!bOk)
			{
				continue;
			}
			FString text = "{ Type: ";
			AppendTypeName(text, *type);
			if (!directories.empty())
			{
				text += ", Directories: ";
				AppendQuotedList(text, directories);
			}
			if (cook.has_value())
			{
				text += ", Cook: ";
				text += enum_::ToString(*cook);
			}
			text += " }";
			AddEntry("PrimaryAssetTypes", std::move(text), item.Value.GetLine());
		}
	}

	void ParseSafety (HNode Node)
	{
		if (!Node)
		{
			return;
		}
		if (!CheckMap(Node, "Safety", SafetyKeys))
		{
			return;
		}
		for (const reflect::SDataChild& child : Node.GetChildren())
		{
			if (std::none_of(std::begin(SafetyKeys), std::end(SafetyKeys), [&child] (FStringLiteral Name) { return child.Key == Name; }))
			{
				continue;   // reported by CheckMap
			}
			if (const std::optional<bool> value = ReadBool(child.Value, FString::Format("Safety.{}", child.Key)))
			{
				AddEntry("Safety", FString::Format("{}: {}", child.Key, *value ? "true" : "false"), child.KeyLine);
			}
		}
	}

	const SRuleFileOrigin& Origin;
	std::vector<SDiagnostic>& Diagnostics;
	std::unique_ptr<CRuleFile::SData> Data;
	std::vector<SRuleListEntry> Entries;
	uint32 ErrorCount = 0u;
	bool bMountLevel = false;
	bool bEngineFile = false;
};
}


// ---------------- CRuleFile ----------------

CRuleFile::CRuleFile (SRuleFileOrigin InOrigin, std::unique_ptr<const SData> InData, std::vector<SRuleListEntry> InEntries, uint64 InRulesHash) noexcept
	: Origin(std::move(InOrigin))
	, Data(std::move(InData))
	, Entries(std::move(InEntries))
	, RulesHash(InRulesHash)
{
}

CRuleFile::~CRuleFile () = default;

std::shared_ptr<const CRuleFile> CRuleFile::Parse (FStringView Text, const SRuleFileOrigin& Origin, std::vector<SDiagnostic>& OutDiagnostics)
{
	CRuleFileParser parser(Origin, OutDiagnostics);
	return parser.Run(Text);
}


// ---------------- Scopes ----------------

namespace
{
// A preset name bound to its innermost definition along a chain, and the definition it shadows (for explain).
struct SPresetBinding
{
	const SPresetDef* Def = nullptr;
	const CRuleFile* File = nullptr;
	const SPresetDef* ShadowedDef = nullptr;
	const CRuleFile* ShadowedFile = nullptr;
};

using FPresetTable = TStringMapIgnoreCase<SPresetBinding>;
}


/**
 * The scope of a folder: its chain of rule files, deepest first (its own file, its ancestors' up to the content
 * root, the mount's Config/Assets.yml, the engine's for other mounts), and the preset table along it. Immutable;
 * the shared files keep every pointer into them valid.
 */
struct CRulesEngine::SScope final : SScanScope
{
	uint32 MountIndex = 0u;
	FString MountName;
	FString MountNameFolded;   // for absolute patterns' OnlyMount
	std::vector<std::shared_ptr<const CRuleFile>> Chain;
	std::shared_ptr<const FPresetTable> Presets;
	bool bExcluded = false;   // inside an excluded folder (a subtree root below one): nothing is a member
};


namespace
{
using SScope = CRulesEngine::SScope;

const SScope& AsScope (const SScanScope& Scope) noexcept
{
	frt_assert(dynamic_cast<const SScope*>(&Scope) != nullptr);
	return static_cast<const SScope&>(Scope);
}

FString FormatProvenance (const SRuleFileOrigin& Origin, uint32 Line)
{
	return FString::Format("{}:{}", Origin.File, Line);
}

// Parent's scope with File as its new deepest link. Every preset a rule of File names must resolve along the
// new chain (RULE001 into OutDiagnostics otherwise; the rule's preset then contributes nothing).
std::shared_ptr<const SScope> MakeScope (const SScope& Parent, const std::shared_ptr<const CRuleFile>& File, std::vector<SDiagnostic>* OutDiagnostics)
{
	auto scope = std::make_shared<SScope>();
	scope->MountIndex = Parent.MountIndex;
	scope->MountName = Parent.MountName;
	scope->MountNameFolded = Parent.MountNameFolded;
	scope->bExcluded = Parent.bExcluded;
	scope->Chain.reserve(Parent.Chain.size() + 1u);
	scope->Chain.push_back(File);
	scope->Chain.insert(scope->Chain.end(), Parent.Chain.begin(), Parent.Chain.end());

	const CRuleFile::SData& data = File->GetData();
	if (data.Presets.empty())
	{
		scope->Presets = Parent.Presets;
	}
	else
	{
		auto presets = Parent.Presets != nullptr ? std::make_shared<FPresetTable>(*Parent.Presets) : std::make_shared<FPresetTable>();
		for (const SPresetDef& preset : data.Presets)
		{
			SPresetBinding& binding = (*presets)[preset.Name];
			SPresetBinding updated;
			updated.Def = &preset;
			updated.File = File.get();
			if (binding.Def != nullptr)
			{
				updated.ShadowedDef = binding.Def;
				updated.ShadowedFile = binding.File;
			}
			binding = updated;
		}
		scope->Presets = std::move(presets);
	}

	if (OutDiagnostics != nullptr)
	{
		const auto report = [&File, OutDiagnostics] (uint32 Line, FString Message)
		{
			SDiagnostic diagnostic;
			diagnostic.Severity = ESeverity::Error;
			diagnostic.Code = Rule001;
			diagnostic.Subject = File->GetOrigin().FolderMountPath;
			diagnostic.File = File->GetOrigin().File;
			diagnostic.Line = Line;
			diagnostic.Message = std::move(Message);
			OutDiagnostics->push_back(std::move(diagnostic));
		};
		for (const SRuleDef& rule : data.Rules)
		{
			if (rule.Preset.IsEmpty())
			{
				continue;
			}
			const SPresetBinding* binding = nullptr;
			if (scope->Presets != nullptr)
			{
				if (const auto found = scope->Presets->find(rule.Preset); found != scope->Presets->end())
				{
					binding = &found->second;
				}
			}
			if (binding == nullptr)
			{
				report(rule.PresetLine, FString::Format("rule {}: the preset '{}' is defined neither in this file nor in an outer rule file of "
					"its chain", rule.Index + 1, rule.Preset));
				continue;
			}
			// A Type or Label rule may not change Analyze through its preset either (its inline Settings were checked at
			// parse time; the preset is known only along a chain). Resolving skips such a value anyway: this says so.
			const SPresetDef& preset = *binding->Def;
			if (rule.IsSelectorRule() && preset.Importer->Settings != nullptr)
			{
				for (const SSettingDef& value : preset.Values)
				{
					const SSettingsField& field = preset.Importer->Settings->Fields[value.Field];
					if (field.bAnalyzeRelevant)
					{
						report(rule.PresetLine, FString::Format("rule {}: a rule with a Type or Label selector cannot set '{}' through the preset "
							"'{}' ({}): it changes what Analyze reads, and type and labels are known only after Analyze", rule.Index + 1, field.Name,
							rule.Preset, FormatProvenance(binding->File->GetOrigin(), preset.Line)));
					}
				}
			}
		}
	}
	return scope;
}

bool Applies (const SGlobEntry& Entry, const SScope& Scope) noexcept
{
	return Entry.OnlyMount.IsEmpty() || Entry.OnlyMountFolded == Scope.MountNameFolded;
}

/**
 * One file as the links of a chain see it: its path relative to each link's base, worked out once, and the result of
 * each rule's Match globs, evaluated at most once per file however many passes ask (membership, the importer, the
 * fields). Glob matching is what re-evaluating a subtree costs, so nothing is matched twice. Lives on the stack of
 * one evaluation; inline storage covers the usual chain and rule counts without a heap allocation.
 */
class CFileMatcher
{
public:
	CFileMatcher (const SScope& InScope, FStringView InRelative) noexcept
		: Scope(InScope)
		, Relative(InRelative)
	{
		const uint32 links = static_cast<uint32>(Scope.Chain.size());
		uint32 rules = 0u;
		for (const std::shared_ptr<const CRuleFile>& file : Scope.Chain)
		{
			rules += static_cast<uint32>(file->GetData().Rules.size());
		}
		if (links > InlineLinks)
		{
			HeapLinks.resize(links);
		}
		if (rules > InlineRules)
		{
			HeapRules.resize(rules);
		}
		SLink* linkData = GetLinks();
		int8* ruleData = GetRules();
		uint32 ruleOffset = 0u;
		for (uint32 i = 0u; i < links; ++i)
		{
			const CRuleFile::SData& data = Scope.Chain[i]->GetData();
			SLink& link = linkData[i];
			link.FirstRule = ruleOffset;
			ruleOffset += static_cast<uint32>(data.Rules.size());
			const FStringView base = data.BaseRelative;
			if (base.IsEmpty())
			{
				link.Path = Relative;
			}
			else if (Relative.Len() > base.Len() && Relative[base.Len()] == '/' && Relative.StartsWith(base))
			{
				link.Path = Relative.RightChop(base.Len() + 1u);   // the usual case: the spelling the folder was entered with
			}
			else
			{
				link.bMatchBelow = true;   // another spelling: CPathGlob compares the base under the fold
			}
		}
		std::fill(ruleData, ruleData + rules, int8(-1));
	}

	CFileMatcher (const CFileMatcher&) = delete;
	CFileMatcher& operator= (const CFileMatcher&) = delete;

	[[nodiscard]] FStringView GetRelative () const noexcept { return Relative; }

	// The first entry of List (link Link's) that applies to the scope's mount and matches the file, or null.
	[[nodiscard]] const SGlobEntry* FindMatch (std::span<const SGlobEntry> List, uint32 Link) noexcept
	{
		const SLink& link = GetLinks()[Link];
		for (const SGlobEntry& entry : List)
		{
			if (!Applies(entry, Scope) || !HasRequiredRuns(entry))
			{
				continue;
			}
			const bool bMatch = link.bMatchBelow ? entry.Glob.MatchesBelow(Scope.Chain[Link]->GetData().BaseRelative, Relative)
				: entry.Glob.Matches(link.Path);
			if (bMatch)
			{
				return &entry;
			}
		}
		return nullptr;
	}

	// The rule's selectors: Match (cached per rule), Type and Label.
	[[nodiscard]] bool MatchesRule (uint32 Link, uint32 RuleIndex, const SRuleDef& Rule, const SFileFacts& Facts) noexcept
	{
		if (!Rule.Match.empty())
		{
			int8& cached = GetRules()[GetLinks()[Link].FirstRule + RuleIndex];
			if (cached < 0)
			{
				cached = FindMatch(Rule.Match, Link) != nullptr ? int8(1) : int8(0);
			}
			if (cached == 0)
			{
				return false;
			}
		}
		if (Rule.Type.has_value() && (!Rule.Type->Type.has_value() || *Rule.Type->Type != Facts.Type))
		{
			return false;
		}
		if (Rule.Label.has_value())
		{
			const bool bHasLabel = std::any_of(Facts.AuthoredLabels.begin(), Facts.AuthoredLabels.end(),
				[&Rule] (const FString& Label) { return Label.Equals<ESearchCase::IgnoreCase>(*Rule.Label); });
			if (!bHasLabel)
			{
				return false;
			}
		}
		return true;
	}

private:
	static constexpr uint32 InlineLinks = 16u;
	static constexpr uint32 InlineRules = 128u;

	struct SLink
	{
		FStringView Path;          // the file below the link's base
		uint32 FirstRule = 0u;     // the link's first rule in the match cache
		bool bMatchBelow = false;
	};

	// The quick reject of SGlobEntry: every required run occurs in the folded path (folded once, on first use).
	[[nodiscard]] bool HasRequiredRuns (const SGlobEntry& Entry) noexcept
	{
		if (Entry.RequiredRuns.empty())
		{
			return true;
		}
		if (!bFolded)
		{
			FoldedRelative = FString(Relative).FoldCase();
			bFolded = true;
		}
		return std::any_of(Entry.RequiredRuns.begin(), Entry.RequiredRuns.end(), [this] (const std::vector<FString>& Runs)
		{
			return std::all_of(Runs.begin(), Runs.end(), [this] (const FString& Run) { return FoldedRelative.Contains(Run); });
		});
	}

	[[nodiscard]] SLink* GetLinks () noexcept { return HeapLinks.empty() ? InlineLinkData.data() : HeapLinks.data(); }
	[[nodiscard]] const SLink* GetLinks () const noexcept { return HeapLinks.empty() ? InlineLinkData.data() : HeapLinks.data(); }
	[[nodiscard]] int8* GetRules () noexcept { return HeapRules.empty() ? InlineRuleData.data() : HeapRules.data(); }

	const SScope& Scope;
	FStringView Relative;
	FString FoldedRelative;
	bool bFolded = false;
	std::array<SLink, InlineLinks> InlineLinkData {};
	std::array<int8, InlineRules> InlineRuleData {};
	std::vector<SLink> HeapLinks;
	std::vector<int8> HeapRules;
};

// What membership and the importer pass found for one file.
struct SClassification
{
	bool bExcluded = false;
	FString ExcludedBy;                       // filled when explaining
	const SImporterDesc* Importer = nullptr;  // the effective importer: null when Unclaimed
	SProvenance ImporterSource;
	FString ImporterNote;                     // filled when explaining: a disabled or unavailable importer
	EFileClass Class = EFileClass::Unclaimed;
	bool bQuiet = false;
};

// Where a value set by a rule came from.
SProvenance MakeSource (const CRuleFile& File, uint32 Line, int32 RuleIndex)
{
	return SProvenance { File.GetOrigin().File, Line, RuleIndex };
}

const SSettingDef* FindSetting (std::span<const SSettingDef> Values, uint32 Field) noexcept
{
	for (const SSettingDef& value : Values)
	{
		if (value.Field == Field)
		{
			return &value;
		}
	}
	return nullptr;
}
}


// ---------------- CRulesEngine ----------------

namespace
{
/**
 * The version of this engine's own semantics (CRulesEngine::GetSemanticsHash). Bump it when a rule text would
 * classify or resolve differently with no change to ImporterTable: precedence, Final, the importer order (Raw,
 * Importer rules, extension claims), Quiet, the meaning of a Cook: value, or IsBuiltinExcludedName's list.
 */
constexpr uint32 RulesEngineVersion = 2u;   // 2: Importers.Exclude no longer makes a file quiet

uint64 ComputeSemanticsHash (FStringView Target)
{
	FString bytes;
	const auto add = [&bytes] (FStringView Text)
	{
		bytes += Text;
		bytes += '\x1F';
	};
	add(FString::FromNumber(RulesEngineVersion));
	for (const SImporterDesc& importer : GetImporters())
	{
		bytes += '\x1E';
		add(importer.Name);
		for (const FStringLiteral extension : importer.Extensions)
		{
			add(extension);
		}
		add(importer.bAvailable ? FStringView("available") : FStringView("unavailable"));
		add(FString::FromNumber(importer.MainType));
		if (importer.Settings == nullptr)
		{
			add("no settings");
			continue;
		}
		add(importer.Settings->Name);
		for (const SSettingsField& field : importer.Settings->Fields)
		{
			bytes += '\x1D';
			add(field.Name);
			add(enum_::ToString(field.Kind));
			add(field.Default);
			for (const FStringLiteral value : field.EnumValues)
			{
				add(value);
			}
			add(field.bAnalyzeRelevant ? FStringView("analyze") : FStringView("-"));
		}
	}
	bytes += '\x1E';
	for (const STargetName& target : KnownTargets)
	{
		add(target.Name);
		add(target.Platform);
	}
	bytes += '\x1E';
	add(Target);
	const uint64 hash = hash::Xxh3_64(bytes.View());
	return hash != 0u ? hash : 1u;
}
}

CRulesEngine::CRulesEngine (const CMountTable& InMounts, SRulesOptions InOptions)
	: Mounts(InMounts)
	, Options(std::move(InOptions))
{
	if (!Options.Target.IsEmpty())
	{
		const STargetName* found = nullptr;
		for (const STargetName& target : KnownTargets)
		{
			if (Options.Target.Equals<ESearchCase::IgnoreCase>(target.Name))
			{
				found = &target;
			}
		}
		frt_assert(found != nullptr);
		Options.Target = found != nullptr ? FString(found->Name) : FString();
		TargetPlatform = found != nullptr ? FString(found->Platform) : FString();
	}

	const std::span<const SMount> mounts = Mounts.GetMounts();
	MountFiles.resize(mounts.size());
	MountScopes.resize(mounts.size());
	MountDiagnostics.resize(mounts.size());
	SemanticsHash = ComputeSemanticsHash(Options.Target);
}

CRulesEngine::~CRulesEngine () = default;

int32 CRulesEngine::FindMountIndex (FStringView MountName) const noexcept
{
	const SMount* mount = Mounts.Find(MountName);
	return mount != nullptr ? static_cast<int32>(mount - Mounts.GetMounts().data()) : -1;
}

SRuleFileOrigin CRulesEngine::MakeFolderOrigin (uint32 MountIndex, FStringView FolderMountPath, FStringView FileName) const
{
	const SMount& mount = Mounts.GetMounts()[MountIndex];
	FStringView relative;
	GetRelativeInMount(FolderMountPath, mount.Name, relative);
	SRuleFileOrigin origin;
	origin.MountName = mount.Name;
	origin.FolderMountPath = FString(FolderMountPath);
	origin.Level = ERuleFileLevel::Folder;
	origin.File = mount.GetSourceFile(relative.IsEmpty() ? FString(FileName) : FString(relative) + "/" + FileName);
	return origin;
}

void CRulesEngine::UpdateFileState (SFileState& State, const SRuleFileOrigin& Origin, std::optional<FStringView> Text, FStringView ReadError) const
{
	if (!Text.has_value())
	{
		State.Origin = Origin;
		State.bRead = false;
		State.bMalformed = true;
		State.TextHash = 0u;
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Error;
		diagnostic.Code = Rule001;
		diagnostic.Subject = Origin.FolderMountPath;
		diagnostic.File = Origin.File;
		diagnostic.Message = FString::Format("cannot read the rule file: {}", ReadError);
		State.Diagnostics.assign(1u, std::move(diagnostic));
		return;
	}
	const uint64 textHash = hash::Xxh3_64(*Text);
	if (State.bRead && State.TextHash == textHash && State.Origin == Origin)
	{
		return;   // the same text as last time: the same parse, the same diagnostics
	}
	std::vector<SDiagnostic> diagnostics;
	std::shared_ptr<const CRuleFile> file = CRuleFile::Parse(*Text, Origin, diagnostics);
	State.Origin = Origin;
	State.TextHash = textHash;
	State.bRead = true;
	State.Diagnostics = std::move(diagnostics);
	State.bMalformed = file == nullptr;
	if (file != nullptr)
	{
		State.LastGood = std::move(file);   // else the last good parse stays in force
	}
}

void CRulesEngine::LoadMountRules (std::vector<SDiagnostic>* OutDiagnostics, std::vector<FString>* OutChangedMounts)
{
	std::scoped_lock loadLock(LoadMutex);
	LoadMountRulesLocked(OutDiagnostics, OutChangedMounts);
}

void CRulesEngine::LoadMountRulesLocked (std::vector<SDiagnostic>* OutDiagnostics, std::vector<FString>* OutChangedMounts)
{
	const std::span<const SMount> mounts = Mounts.GetMounts();

	// Read outside Mutex (scans keep looking scopes up meanwhile), under LoadMutex (no other load interleaves, so
	// the state stored below is the one these reads were compared against); parse against the previous state so an
	// unchanged text is not parsed again.
	std::vector<SFileState> states;
	{
		std::scoped_lock lock(Mutex);
		states = MountFiles;
	}
	std::vector<uint64> oldHashes(mounts.size(), 0u);
	for (uint32 i = 0u; i < mounts.size(); ++i)
	{
		const SMount& mount = mounts[i];
		oldHashes[i] = states[i].LastGood != nullptr ? states[i].LastGood->GetRulesHash() : 0u;
		SRuleFileOrigin origin;
		origin.MountName = mount.Name;
		origin.FolderMountPath = mount.GetRootMountPath();
		origin.Level = ERuleFileLevel::Mount;
		std::optional<FString> relative = platform::GetRelativePath(mount.RulesFile, mount.OwnerRoot);
		origin.File = relative.has_value() ? std::move(*relative) : FString::FromPath(mount.RulesFile.filename(), EPathStyle::Generic);

		FString statError;
		const std::optional<platform::SFileStat> stat = platform::Stat(mount.RulesFile, &statError);
		if (!stat.has_value() || stat->bDirectory)
		{
			states[i] = SFileState {};   // no file: no rules, no problem
			states[i].Origin = std::move(origin);
			continue;
		}
		const std::optional<FString> text = platform::ReadFileText(mount.RulesFile);
		if (!text.has_value())
		{
			UpdateFileState(states[i], origin, std::nullopt, FString::Format("{} could not be read", FString::FromPath(mount.RulesFile)));
		}
		else
		{
			UpdateFileState(states[i], origin, text->View(), FStringView());
		}
	}

	// The mount scopes: the engine's file as the trailing fallback, then the mount's own.
	int32 engineIndex = -1;
	for (uint32 i = 0u; i < mounts.size(); ++i)
	{
		if (mounts[i].Kind == EMountKind::Engine)
		{
			engineIndex = static_cast<int32>(i);
		}
	}
	const std::shared_ptr<const CRuleFile> engineFile = engineIndex >= 0 ? states[static_cast<uint32>(engineIndex)].LastGood : nullptr;
	std::vector<std::shared_ptr<const SScope>> scopes(mounts.size());
	std::vector<std::vector<SDiagnostic>> diagnostics(mounts.size());
	for (uint32 i = 0u; i < mounts.size(); ++i)
	{
		auto base = std::make_shared<SScope>();
		base->MountIndex = i;
		base->MountName = mounts[i].Name;
		base->MountNameFolded = mounts[i].Name.FoldCase();
		std::shared_ptr<const SScope> scope = base;
		if (static_cast<int32>(i) != engineIndex && engineFile != nullptr)
		{
			scope = MakeScope(*scope, engineFile, nullptr);
		}
		diagnostics[i] = states[i].Diagnostics;
		if (states[i].LastGood != nullptr)
		{
			scope = MakeScope(*scope, states[i].LastGood, &diagnostics[i]);
		}
		scopes[i] = std::move(scope);
	}

	const uint64 oldEngineHash = engineIndex >= 0 ? oldHashes[static_cast<uint32>(engineIndex)] : 0u;
	const uint64 newEngineHash = engineFile != nullptr ? engineFile->GetRulesHash() : 0u;
	bool bFirstLoad = false;
	{
		std::scoped_lock lock(Mutex);
		bFirstLoad = !bLoaded;
		MountFiles = states;
		MountScopes = scopes;
		MountDiagnostics = diagnostics;
		bLoaded = true;
	}

	for (uint32 i = 0u; i < mounts.size(); ++i)
	{
		if (OutDiagnostics != nullptr)
		{
			// The mount root's complete Rules list: the mount file's, then the content root's Assets.yml as last read,
			// bound against the new chain (a preset it names may have come or gone with the mount file).
			OutDiagnostics->insert(OutDiagnostics->end(), diagnostics[i].begin(), diagnostics[i].end());
			SFileState root;
			bool bRootRead = false;
			{
				std::scoped_lock lock(Mutex);
				if (const auto found = FolderFiles.find(mounts[i].GetRootMountPath()); found != FolderFiles.end())
				{
					root = found->second;
					bRootRead = true;
				}
			}
			if (bRootRead)
			{
				OutDiagnostics->insert(OutDiagnostics->end(), root.Diagnostics.begin(), root.Diagnostics.end());
				if (root.LastGood != nullptr)
				{
					(void)MakeScope(*scopes[i], root.LastGood, OutDiagnostics);
				}
			}
		}
		const uint64 newHash = states[i].LastGood != nullptr ? states[i].LastGood->GetRulesHash() : 0u;
		const bool bChanged = bFirstLoad || newHash != oldHashes[i] || (static_cast<int32>(i) != engineIndex && newEngineHash != oldEngineHash);
		if (OutChangedMounts != nullptr && bChanged)
		{
			OutChangedMounts->push_back(mounts[i].Name);
		}
	}
}

void CRulesEngine::EnsureLoaded () const
{
	{
		std::scoped_lock lock(Mutex);
		if (bLoaded)
		{
			return;
		}
	}
	// One thread loads; the others wait for it and then find the rules loaded.
	std::scoped_lock loadLock(LoadMutex);
	{
		std::scoped_lock lock(Mutex);
		if (bLoaded)
		{
			return;
		}
	}
	const_cast<CRulesEngine*>(this)->LoadMountRulesLocked(nullptr, nullptr);
}

uint64 CRulesEngine::GetMountRulesHash (FStringView MountName) const
{
	EnsureLoaded();
	const int32 index = FindMountIndex(MountName);
	if (index < 0)
	{
		return 0u;
	}
	std::scoped_lock lock(Mutex);
	const SFileState& state = MountFiles[static_cast<uint32>(index)];
	return state.LastGood != nullptr ? state.LastGood->GetRulesHash() : 0u;
}

uint64 CRulesEngine::GetRulesBasis (bool bWithEngineFallback) const
{
	if (!bWithEngineFallback)
	{
		return SemanticsHash;
	}
	uint8 bytes[16];
	const uint64 engineHash = GetMountRulesHash("Engine");
	for (uint32 i = 0u; i < 8u; ++i)
	{
		bytes[i] = static_cast<uint8>(SemanticsHash >> (8u * i));
		bytes[8u + i] = static_cast<uint8>(engineHash >> (8u * i));
	}
	const uint64 basis = hash::Xxh3_64(std::span<const uint8>(bytes));
	return basis != 0u ? basis : 1u;
}

std::shared_ptr<const SScanScope> CRulesEngine::OpenScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const
{
	return OpenScopeImpl(FolderMountPath, false, OutDiagnostics);
}

std::shared_ptr<const SScanScope> CRulesEngine::OpenFolderScope (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const
{
	return OpenScopeImpl(FolderMountPath, true, OutDiagnostics);
}

std::shared_ptr<const SScanScope> CRulesEngine::OpenScopeImpl (FStringView FolderMountPath, bool bIncludeSelf, std::vector<SDiagnostic>& OutDiagnostics) const
{
	EnsureLoaded();
	FStringView mountName;
	FStringView relative;
	const int32 index = SplitMountPath(FolderMountPath, mountName, relative) ? FindMountIndex(mountName) : -1;
	if (index < 0)
	{
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Error;
		diagnostic.Subject = FString(FolderMountPath);
		diagnostic.Message = FString::Format("'{}' is in no mount: no rules apply, nothing is a member", ToDisplayText(FolderMountPath));
		OutDiagnostics.push_back(std::move(diagnostic));
		auto excluded = std::make_shared<SScope>();
		excluded->MountName = FString(mountName);
		excluded->MountNameFolded = excluded->MountName.FoldCase();
		excluded->bExcluded = true;
		return excluded;
	}

	std::shared_ptr<const SScope> mountScope;
	{
		std::scoped_lock lock(Mutex);
		mountScope = MountScopes[static_cast<uint32>(index)];
	}
	if (relative.IsEmpty() && !bIncludeSelf)
	{
		return mountScope;
	}

	// The folders from the mount root down, each folder file read and bound, never inside an excluded folder. Only
	// the requested folder's own file reports its problems: the others belong to their own folders' scans.
	const SMount& mount = Mounts.GetMounts()[static_cast<uint32>(index)];
	std::shared_ptr<const SScanScope> scope = mountScope;
	FString folder = mount.GetRootMountPath();
	FStringView rest = relative;
	bool bSelf = relative.IsEmpty();
	for (;;)
	{
		if (bSelf && !bIncludeSelf)
		{
			break;   // the root itself is EnterFolder's
		}
		if (folder.Len() > mount.Name.Len() + 1u && !ShouldDescend(*scope, folder))
		{
			auto excluded = std::make_shared<SScope>(AsScope(*scope));
			excluded->bExcluded = true;
			return excluded;
		}
		std::vector<SDiagnostic>* diagnostics = bSelf ? &OutDiagnostics : nullptr;
		const std::optional<std::filesystem::path> directory = Mounts.FromMountPath(folder);
		uint64 rulesHash = 0u;
		if (directory.has_value())
		{
			// The folder's rule file found the way the scanner finds it (MountScanner.cpp): a listing, any spelling
			// under the portable fold, the reserved spelling winning on a case-sensitive folder that holds two. A
			// probe of the reserved spelling alone would miss "assets.yml" on a case-sensitive folder, and on NTFS
			// record a spelling the scan does not, so the two paths would disagree on the folder's rules.
			FString fileName;
			FString listError;
			const bool bListed = platform::EnumerateDirectory(*directory, [&fileName] (const platform::SDirectoryEntry& Entry)
			{
				if (!Entry.Stat.bDirectory && !Entry.bNameSurrogate && IsRuleFileName(Entry.Name) && IsPreferredRuleFileName(Entry.Name, fileName))
				{
					fileName = FString(Entry.Name);
				}
				return fileName != RuleFileName;   // the reserved spelling ends the search
			}, &listError);
			if (!bListed)
			{
				// Unknown, not gone: the folder's last read stays in force (the scanner treats an unlistable folder
				// as unknown too).
				if (diagnostics != nullptr)
				{
					SDiagnostic diagnostic;
					diagnostic.Severity = ESeverity::Warning;
					diagnostic.Subject = folder;
					diagnostic.Message = FString::Format("cannot list the folder to find its {}: {}; its last read rules stay in force",
						RuleFileName, listError);
					diagnostics->push_back(std::move(diagnostic));
				}
				std::vector<SDiagnostic> ignored;
				scope = EnterFolderCached(scope, folder, rulesHash, diagnostics != nullptr ? *diagnostics : ignored);
			}
			else if (!fileName.IsEmpty())
			{
				const std::filesystem::path ruleFile = *directory / fileName.ToPath();
				const std::optional<FString> text = platform::ReadFileText(ruleFile);
				scope = BindFolder(scope, folder, text.has_value() ? std::optional<FStringView>(text->View()) : std::nullopt, !text.has_value(),
					FString::Format("{} could not be read", FString::FromPath(ruleFile)), fileName, rulesHash, diagnostics);
			}
			else
			{
				scope = BindFolder(scope, folder, std::nullopt, false, FStringView(), RuleFileName, rulesHash, diagnostics);   // listed: none
			}
		}
		if (bSelf)
		{
			break;
		}
		const SStringSplit split = rest.SplitFirst('/');
		folder += '/';
		folder += split.Left;
		rest = split.Right;
		bSelf = !split.bFound;
	}
	return scope;
}

std::shared_ptr<const SScanScope> CRulesEngine::BindFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
	std::optional<FStringView> Text, bool bReadFailed, FStringView ReadError, FStringView FileName, uint64& OutRulesHash,
	std::vector<SDiagnostic>* OutDiagnostics) const
{
	frt_assert(Parent != nullptr);
	const SScope& parent = AsScope(*Parent);
	if (OutDiagnostics != nullptr)
	{
		AppendMountRootDiagnostics(FolderMountPath, *OutDiagnostics);
	}
	if (!Text.has_value() && !bReadFailed)
	{
		std::scoped_lock lock(Mutex);
		if (const auto found = FolderFiles.find(FolderMountPath); found != FolderFiles.end())
		{
			FolderFiles.erase(found);
		}
		OutRulesHash = 0u;
		return Parent;
	}

	const SRuleFileOrigin origin = MakeFolderOrigin(parent.MountIndex, FolderMountPath, FileName);
	SFileState state;
	{
		std::scoped_lock lock(Mutex);
		const auto found = FolderFiles.find(FolderMountPath);
		if (found != FolderFiles.end())
		{
			state = found->second;
		}
	}
	UpdateFileState(state, origin, Text, ReadError);
	{
		std::scoped_lock lock(Mutex);
		FolderFiles.insert_or_assign(FString(FolderMountPath), state);
	}
	if (OutDiagnostics != nullptr)
	{
		OutDiagnostics->insert(OutDiagnostics->end(), state.Diagnostics.begin(), state.Diagnostics.end());
	}
	if (state.LastGood == nullptr)
	{
		OutRulesHash = 0u;
		return Parent;
	}
	OutRulesHash = state.LastGood->GetRulesHash();
	return MakeScope(parent, state.LastGood, OutDiagnostics);
}

std::shared_ptr<const SScanScope> CRulesEngine::EnterFolder (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
	const std::filesystem::path* RuleFile, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const
{
	if (RuleFile == nullptr)
	{
		return BindFolder(Parent, FolderMountPath, std::nullopt, false, FStringView(), RuleFileName, OutRulesHash, &OutDiagnostics);
	}
	const FString fileName = FString::FromPath(RuleFile->filename(), EPathStyle::Generic);
	const std::optional<FString> text = platform::ReadFileText(*RuleFile);
	if (!text.has_value())
	{
		return BindFolder(Parent, FolderMountPath, std::nullopt, true, FString::Format("{} could not be read", FString::FromPath(*RuleFile)),
			fileName, OutRulesHash, &OutDiagnostics);
	}
	return BindFolder(Parent, FolderMountPath, text->View(), false, FStringView(), fileName, OutRulesHash, &OutDiagnostics);
}

std::shared_ptr<const SScanScope> CRulesEngine::EnterFolderFromText (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
	std::optional<FStringView> Text, uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics, FStringView FileName) const
{
	return BindFolder(Parent, FolderMountPath, Text, false, FStringView(), FileName, OutRulesHash, &OutDiagnostics);
}

std::shared_ptr<const SScanScope> CRulesEngine::EnterFolderCached (const std::shared_ptr<const SScanScope>& Parent, FStringView FolderMountPath,
	uint64& OutRulesHash, std::vector<SDiagnostic>& OutDiagnostics) const
{
	frt_assert(Parent != nullptr);
	AppendMountRootDiagnostics(FolderMountPath, OutDiagnostics);
	SFileState state;
	{
		std::scoped_lock lock(Mutex);
		const auto found = FolderFiles.find(FolderMountPath);
		if (found == FolderFiles.end())
		{
			OutRulesHash = 0u;
			return Parent;
		}
		state = found->second;
	}
	OutDiagnostics.insert(OutDiagnostics.end(), state.Diagnostics.begin(), state.Diagnostics.end());
	if (state.LastGood == nullptr)
	{
		OutRulesHash = 0u;
		return Parent;
	}
	OutRulesHash = state.LastGood->GetRulesHash();
	return MakeScope(AsScope(*Parent), state.LastGood, &OutDiagnostics);
}

void CRulesEngine::AppendMountRootDiagnostics (FStringView FolderMountPath, std::vector<SDiagnostic>& OutDiagnostics) const
{
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(FolderMountPath, mountName, relative) || !relative.IsEmpty())
	{
		return;
	}
	EnsureLoaded();
	const int32 index = FindMountIndex(mountName);
	if (index < 0)
	{
		return;
	}
	std::scoped_lock lock(Mutex);
	const std::vector<SDiagnostic>& diagnostics = MountDiagnostics[static_cast<uint32>(index)];
	OutDiagnostics.insert(OutDiagnostics.end(), diagnostics.begin(), diagnostics.end());
}

void CRulesEngine::ForgetFolders (FStringView SubtreeMountPath)
{
	std::scoped_lock lock(Mutex);
	for (auto it = FolderFiles.begin(); it != FolderFiles.end();)
	{
		const FStringView path = it->first;
		const bool bBelow = path == SubtreeMountPath || (path.StartsWith(SubtreeMountPath) && path.Len() > SubtreeMountPath.Len()
			&& path[SubtreeMountPath.Len()] == '/');
		it = bBelow ? FolderFiles.erase(it) : std::next(it);
	}
}

bool CRulesEngine::ShouldDescend (const SScanScope& Scope, FStringView FolderMountPath) const
{
	const SScope& scope = AsScope(Scope);
	FStringView relative;
	if (!GetRelativeInMount(FolderMountPath, scope.MountName, relative))
	{
		return false;
	}
	if (relative.IsEmpty())
	{
		return true;   // a mount root
	}
	if (scope.bExcluded || IsBuiltinExcludedName(GetMountPathLeaf(FolderMountPath), true))
	{
		return false;
	}
	for (const std::shared_ptr<const CRuleFile>& file : scope.Chain)
	{
		const CRuleFile::SData& data = file->GetData();
		for (const SGlobEntry& entry : data.Exclude)
		{
			if (Applies(entry, scope) && entry.Glob.CoversFolderBelow(data.BaseRelative, relative))
			{
				return false;
			}
		}
	}
	return true;
}

namespace
{
// The parts of a rule the fields pass can still use: false when everything the rule could set is decided already
// (its Match globs need not be evaluated then). bStopAll: only Analyze-relevant settings are still open.
bool CanContribute (const SRuleDef& Rule, const SScope& Scope, const SSettingsTable* Table, std::span<const bool> FieldSet, bool bCookSet,
	bool bChunkSet, bool bStopAll) noexcept
{
	if (Rule.bFinal)
	{
		return true;
	}
	if (!bStopAll)
	{
		if (!Rule.Labels.empty() || (!bCookSet && (Rule.Cook.has_value() || !Rule.Targets.empty()))
			|| (!bChunkSet && (Rule.Chunk.has_value() || !Rule.Targets.empty())))
		{
			return true;
		}
	}
	if (Table == nullptr)
	{
		return false;
	}
	const bool bOwnTable = Rule.SettingsTable == Table;
	const SPresetDef* preset = nullptr;
	if (!Rule.Preset.IsEmpty() && Scope.Presets != nullptr)
	{
		const auto found = Scope.Presets->find(Rule.Preset);
		if (found != Scope.Presets->end() && found->second.Def->Importer->Settings == Table)
		{
			preset = found->second.Def;
		}
	}
	if (!bOwnTable && preset == nullptr)
	{
		return false;
	}
	const auto opens = [&] (std::span<const SSettingDef> Values)
	{
		return std::any_of(Values.begin(), Values.end(), [&] (const SSettingDef& Value)
		{
			return !FieldSet[Value.Field] && (!bStopAll || Table->Fields[Value.Field].bAnalyzeRelevant);
		});
	};
	if (bOwnTable)
	{
		if (opens(Rule.Settings))
		{
			return true;
		}
		for (const STargetBlock& block : Rule.Targets)
		{
			if (opens(block.Settings))
			{
				return true;
			}
		}
	}
	return preset != nullptr && opens(preset->Values);
}

/**
 * Membership and the importer (§3.5.2 "Membership", see Rules.h): built-in excludes, Scan.Include, Scan.Exclude,
 * then Raw, the Match-only rules that set Importer or Final, the extension claim; Importers.Exclude and
 * availability; Scan.Quiet for a file that stays Unclaimed.
 */
void ClassifyFile (const SScope& Scope, FStringView FileMountPath, CFileMatcher& Matcher, bool bExplain, SClassification& Out)
{
	const FStringView relative = Matcher.GetRelative();
	if (relative.IsEmpty())
	{
		Out.bExcluded = true;
		if (bExplain)
		{
			Out.ExcludedBy = FString::Format("not a file of /{}", Scope.MountName);
		}
		return;
	}
	if (IsBuiltinExcludedName(GetMountPathLeaf(FileMountPath), false))
	{
		Out.bExcluded = true;
		if (bExplain)
		{
			Out.ExcludedBy = "a built-in exclude";
		}
		return;
	}
	if (Scope.bExcluded)
	{
		Out.bExcluded = true;
		if (bExplain)
		{
			Out.ExcludedBy = "inside an excluded folder";
		}
		return;
	}

	const uint32 links = static_cast<uint32>(Scope.Chain.size());
	for (uint32 link = 0u; link < links; ++link)
	{
		const CRuleFile& file = *Scope.Chain[link];
		const CRuleFile::SData& data = file.GetData();
		const bool bListsAny = std::any_of(data.Include.begin(), data.Include.end(), [&Scope] (const SGlobEntry& Entry) { return Applies(Entry, Scope); });
		if (bListsAny && Matcher.FindMatch(data.Include, link) == nullptr)
		{
			Out.bExcluded = true;
			if (bExplain)
			{
				Out.ExcludedBy = FString::Format("not in Scan.Include ({})", FormatProvenance(file.GetOrigin(), data.Include.front().Line));
			}
			return;
		}
	}
	for (uint32 link = 0u; link < links; ++link)
	{
		const CRuleFile& file = *Scope.Chain[link];
		if (const SGlobEntry* entry = Matcher.FindMatch(file.GetData().Exclude, link))
		{
			Out.bExcluded = true;
			if (bExplain)
			{
				Out.ExcludedBy = FString::Format("Scan.Exclude {} ({})", Quoted(entry->Text), FormatProvenance(file.GetOrigin(), entry->Line));
			}
			return;
		}
	}

	const SImporterDesc* importer = nullptr;
	for (uint32 link = 0u; link < links && importer == nullptr; ++link)
	{
		const CRuleFile& file = *Scope.Chain[link];
		if (const SGlobEntry* entry = Matcher.FindMatch(file.GetData().Raw, link))
		{
			importer = GetRawImporter();
			Out.ImporterSource = MakeSource(file, entry->Line, -1);
		}
	}
	if (importer == nullptr)
	{
		// Only a rule that sets Importer or Final can decide it; the others are not matched here.
		const SFileFacts noFacts;
		bool bStopped = false;
		for (uint32 link = 0u; link < links && !bStopped; ++link)
		{
			const CRuleFile& file = *Scope.Chain[link];
			const std::vector<SRuleDef>& rules = file.GetData().Rules;
			for (uint32 index = 0u; index < rules.size(); ++index)
			{
				const SRuleDef& rule = rules[index];
				if (rule.IsSelectorRule() || (rule.Importer == nullptr && !rule.bFinal) || !Matcher.MatchesRule(link, index, rule, noFacts))
				{
					continue;
				}
				if (rule.Importer != nullptr)
				{
					importer = rule.Importer;
					Out.ImporterSource = MakeSource(file, rule.ImporterLine, rule.Index);
				}
				bStopped = true;
				break;
			}
		}
	}
	if (importer == nullptr)
	{
		importer = FindImporterForFile(GetMountPathLeaf(FileMountPath));
	}

	const auto markQuiet = [&] ()
	{
		for (uint32 link = 0u; link < links && !Out.bQuiet; ++link)
		{
			Out.bQuiet = Matcher.FindMatch(Scope.Chain[link]->GetData().Quiet, link) != nullptr;
		}
	};
	if (importer == nullptr)
	{
		Out.Class = EFileClass::Unclaimed;
		markQuiet();
		return;
	}
	for (uint32 link = 0u; link < links; ++link)
	{
		const CRuleFile& file = *Scope.Chain[link];
		for (const SImporterRef& excluded : file.GetData().ImporterExcludes)
		{
			if (excluded.Importer == importer)
			{
				// Unclaimed and reported like any unclaimed file (RULE002) unless Scan.Quiet lists it (§3.5.2).
				Out.Class = EFileClass::Unclaimed;
				markQuiet();
				if (bExplain)
				{
					Out.ImporterNote = FString::Format("the importer {} is disabled by Importers.Exclude ({})", importer->Name,
						FormatProvenance(file.GetOrigin(), excluded.Line));
				}
				return;
			}
		}
	}
	if (!importer->bAvailable)
	{
		Out.Class = EFileClass::Unclaimed;
		markQuiet();
		if (bExplain)
		{
			Out.ImporterNote = FString::Format("the importer {} is not available in this build", importer->Name);
		}
		return;
	}
	Out.Importer = importer;
	Out.Class = importer == GetRawImporter() ? EFileClass::Raw : EFileClass::Asset;
}

// The Final rule that stopped evaluation and the presets the fields came from, for explain.
struct SResolveTrace
{
	const CRuleFile* FinalFile = nullptr;
	const SRuleDef* FinalRule = nullptr;
	std::vector<std::pair<const SPresetBinding*, const SRuleDef*>> PresetsUsed;
};

/**
 * The fields pass (§3.5.2 "Rules are ordered and field-wise"). Classification has the importer; this fills cook,
 * chunk, labels and settings, each from the first matching rule that sets it, then the importer's defaults. A rule
 * that can no longer set anything is skipped without matching its globs.
 */
void ResolveRules (const SScope& Scope, CFileMatcher& Matcher, const SFileFacts& Facts, bool bProvenance, FStringView Target, FStringView Platform,
	const SClassification& Classification, SEffectiveFileSettings& Out, SResolveTrace* Trace)
{
	const SSettingsTable* table = Out.Importer != nullptr ? Out.Importer->Settings : nullptr;
	const uint32 fieldCount = table != nullptr ? static_cast<uint32>(table->Fields.size()) : 0u;
	Out.Settings.Table = table;
	Out.Settings.Values.resize(fieldCount);
	constexpr uint32 MaxInlineFields = 32u;
	std::array<bool, MaxInlineFields> inlineFieldSet {};
	bool* fieldSetData = inlineFieldSet.data();
	std::unique_ptr<bool[]> heapFields;
	if (fieldCount > MaxInlineFields)
	{
		heapFields = std::make_unique<bool[]>(fieldCount);
		fieldSetData = heapFields.get();
	}
	const std::span<bool> fieldSet(fieldSetData, fieldCount);
	std::vector<SProvenance> fieldSources(bProvenance ? fieldCount : 0u);
	bool bCookSet = false;
	bool bChunkSet = false;
	SProvenance cookSource;
	SProvenance chunkSource;
	std::vector<std::pair<FString, SProvenance>> labelSources;

	const auto findBlock = [&] (const SRuleDef& Rule, bool bPlatform) -> const STargetBlock*
	{
		const FStringView name = bPlatform ? Platform : Target;
		if (name.IsEmpty())
		{
			return nullptr;
		}
		for (const STargetBlock& block : Rule.Targets)
		{
			if (block.bPlatform == bPlatform && block.Name == name)
			{
				return &block;
			}
		}
		return nullptr;
	};

	bool bStopAll = false;
	bool bStopAnalyze = false;
	const uint32 links = static_cast<uint32>(Scope.Chain.size());
	for (uint32 link = 0u; link < links && !bStopAnalyze; ++link)
	{
		const CRuleFile& file = *Scope.Chain[link];
		const std::vector<SRuleDef>& rules = file.GetData().Rules;
		for (uint32 index = 0u; index < rules.size(); ++index)
		{
			const SRuleDef& rule = rules[index];
			const bool bSelector = rule.IsSelectorRule();
			if (bStopAll && bSelector)
			{
				continue;
			}
			if (!CanContribute(rule, Scope, table, fieldSet, bCookSet, bChunkSet, bStopAll) || !Matcher.MatchesRule(link, index, rule, Facts))
			{
				continue;
			}
			const STargetBlock* targetBlock = rule.Targets.empty() ? nullptr : findBlock(rule, false);
			const STargetBlock* platformBlock = rule.Targets.empty() ? nullptr : findBlock(rule, true);
			if (!bStopAll)
			{
				if (!bCookSet)
				{
					const STargetBlock* block = targetBlock != nullptr && targetBlock->Cook.has_value() ? targetBlock
						: (platformBlock != nullptr && platformBlock->Cook.has_value() ? platformBlock : nullptr);
					if (block != nullptr || rule.Cook.has_value())
					{
						Out.CookRule = block != nullptr ? *block->Cook : *rule.Cook;
						bCookSet = true;
						if (bProvenance)
						{
							cookSource = MakeSource(file, block != nullptr ? block->CookLine : rule.CookLine, rule.Index);
						}
					}
				}
				if (!bChunkSet)
				{
					const STargetBlock* block = targetBlock != nullptr && targetBlock->Chunk.has_value() ? targetBlock
						: (platformBlock != nullptr && platformBlock->Chunk.has_value() ? platformBlock : nullptr);
					if (block != nullptr || rule.Chunk.has_value())
					{
						Out.Chunk = block != nullptr ? *block->Chunk : *rule.Chunk;
						bChunkSet = true;
						if (bProvenance)
						{
							chunkSource = MakeSource(file, block != nullptr ? block->ChunkLine : rule.ChunkLine, rule.Index);
						}
					}
				}
				for (const auto& [label, line] : rule.Labels)
				{
					const bool bKnown = std::any_of(Out.RuleLabels.begin(), Out.RuleLabels.end(),
						[&label] (const FString& Existing) { return Existing.Equals<ESearchCase::IgnoreCase>(label); });
					if (!bKnown)
					{
						Out.RuleLabels.push_back(label);
						if (bProvenance)
						{
							labelSources.emplace_back(FString("Labels.") + label, MakeSource(file, line, rule.Index));
						}
					}
				}
			}

			if (table != nullptr)
			{
				const SPresetBinding* preset = nullptr;
				if (!rule.Preset.IsEmpty() && Scope.Presets != nullptr)
				{
					const auto found = Scope.Presets->find(rule.Preset);
					if (found != Scope.Presets->end() && found->second.Def->Importer->Settings == table)
					{
						preset = &found->second;
					}
				}
				const bool bOwnTable = rule.SettingsTable == table;
				bool bUsedPreset = false;
				for (uint32 field = 0u; field < fieldCount; ++field)
				{
					if (fieldSet[field])
					{
						continue;
					}
					const bool bRelevant = table->Fields[field].bAnalyzeRelevant;
					if ((bStopAll && !bRelevant) || (bSelector && bRelevant))
					{
						continue;
					}
					const SSettingDef* value = nullptr;
					if (bOwnTable)
					{
						if (targetBlock != nullptr)
						{
							value = FindSetting(targetBlock->Settings, field);
						}
						if (value == nullptr && platformBlock != nullptr)
						{
							value = FindSetting(platformBlock->Settings, field);
						}
						if (value == nullptr)
						{
							value = FindSetting(rule.Settings, field);
						}
					}
					uint32 line = value != nullptr ? value->Line : 0u;
					if (value == nullptr && preset != nullptr)
					{
						value = FindSetting(preset->Def->Values, field);
						line = rule.PresetLine;
						bUsedPreset = bUsedPreset || value != nullptr;
					}
					if (value == nullptr)
					{
						continue;
					}
					fieldSet[field] = true;
					Out.Settings.Values[field].Field = static_cast<uint16>(field);
					Out.Settings.Values[field].Text = value->Text;
					if (bProvenance)
					{
						fieldSources[field] = MakeSource(file, line, rule.Index);
					}
				}
				if (Trace != nullptr && bUsedPreset)
				{
					Trace->PresetsUsed.emplace_back(preset, &rule);
				}
			}

			if (rule.bFinal)
			{
				if (Trace != nullptr && !bStopAll)
				{
					Trace->FinalFile = &file;
					Trace->FinalRule = &rule;
				}
				bStopAll = true;
				bStopAnalyze = bStopAnalyze || !bSelector;
				if (bStopAnalyze)
				{
					break;
				}
			}
		}
	}

	bool bAnyRelevant = false;
	FString analyzeBytes;
	for (uint32 field = 0u; field < fieldCount; ++field)
	{
		SSettingValue& value = Out.Settings.Values[field];
		if (!fieldSet[field])
		{
			value.Field = static_cast<uint16>(field);
			value.Text = FString(table->Fields[field].Default);
		}
		if (bProvenance)
		{
			value.Source = fieldSources[field];
		}
		if (table->Fields[field].bAnalyzeRelevant)
		{
			bAnyRelevant = true;
			analyzeBytes += table->Fields[field].Name;
			analyzeBytes += '\0';
			analyzeBytes += value.Text;
			analyzeBytes += '\0';
		}
	}
	Out.Settings.AnalyzeHash = bAnyRelevant ? hash::Xxh3_64(analyzeBytes.View()) : 0u;

	if (bProvenance)
	{
		Out.Provenance.reserve(3u + fieldCount + labelSources.size());
		Out.Provenance.emplace_back(FString("Importer"), Classification.ImporterSource);
		Out.Provenance.emplace_back(FString("Cook"), cookSource);
		Out.Provenance.emplace_back(FString("Chunk"), chunkSource);
		for (uint32 field = 0u; field < fieldCount; ++field)
		{
			Out.Provenance.emplace_back(FString(table->Fields[field].Name), fieldSources[field]);
		}
		for (auto& source : labelSources)
		{
			Out.Provenance.push_back(std::move(source));
		}
	}
}

// The file's path below Scope's mount root; empty when it is not a file of that mount.
FStringView GetFileRelative (const SScope& Scope, FStringView FileMountPath) noexcept
{
	FStringView relative;
	return GetRelativeInMount(FileMountPath, Scope.MountName, relative) ? relative : FStringView();
}
}

std::optional<SFileVerdict> CRulesEngine::Classify (const SScanScope& Scope, FStringView FileMountPath) const
{
	const SScope& scope = AsScope(Scope);
	CFileMatcher matcher(scope, GetFileRelative(scope, FileMountPath));
	SClassification classification;
	ClassifyFile(scope, FileMountPath, matcher, false, classification);
	if (classification.bExcluded)
	{
		return std::nullopt;
	}
	return SFileVerdict { classification.Class, classification.bQuiet };
}

SEffectiveFileSettings CRulesEngine::ResolveFile (const SScanScope& Scope, FStringView FileMountPath, const SFileFacts& Facts, bool bProvenance) const
{
	const SScope& scope = AsScope(Scope);
	CFileMatcher matcher(scope, GetFileRelative(scope, FileMountPath));
	SEffectiveFileSettings settings;
	SClassification classification;
	ClassifyFile(scope, FileMountPath, matcher, false, classification);
	if (classification.bExcluded)
	{
		settings.Class = EFileClass::Excluded;
		return settings;
	}
	settings.Importer = classification.Importer;
	settings.Class = classification.Class;
	settings.bQuiet = classification.bQuiet;
	ResolveRules(scope, matcher, Facts, bProvenance, Options.Target, TargetPlatform, classification, settings, nullptr);
	return settings;
}

bool CRulesEngine::IsTypeIncluded (const SScanScope& Scope, reflect::TypeId Type) const
{
	const SScope& scope = AsScope(Scope);
	for (const std::shared_ptr<const CRuleFile>& file : scope.Chain)
	{
		for (const STypeRef& excluded : file->GetData().TypeExcludes)
		{
			if (excluded.Type.has_value() && *excluded.Type == Type)
			{
				return false;
			}
		}
	}
	for (const std::shared_ptr<const CRuleFile>& file : scope.Chain)
	{
		const std::vector<STypeRef>& includes = file->GetData().TypeIncludes;
		if (!includes.empty() && std::none_of(includes.begin(), includes.end(),
			[Type] (const STypeRef& Included) { return Included.Type.has_value() && *Included.Type == Type; }))
		{
			return false;
		}
	}
	return true;
}

void CRulesEngine::ApplyValidation (const SScanScope& Scope, SDiagnostic& Diagnostic) const
{
	for (const std::shared_ptr<const CRuleFile>& file : AsScope(Scope).Chain)
	{
		for (const SValidationDef& validation : file->GetData().Validation)
		{
			if (validation.Code == Diagnostic.Code)
			{
				Diagnostic.Severity = validation.Severity;
				return;
			}
		}
	}
}

SRuleExplanation CRulesEngine::Explain (const SScanScope& Scope, FStringView FileMountPath, const SFileFacts& Facts) const
{
	const SScope& scope = AsScope(Scope);
	SRuleExplanation explanation;
	for (const std::shared_ptr<const CRuleFile>& file : scope.Chain)
	{
		explanation.Chain.push_back({ file->GetOrigin(), file->GetRulesHash() });
	}

	CFileMatcher matcher(scope, GetFileRelative(scope, FileMountPath));
	SClassification classification;
	ClassifyFile(scope, FileMountPath, matcher, true, classification);
	if (classification.bExcluded)
	{
		explanation.ExcludedBy = classification.ExcludedBy;
		explanation.Settings.Class = EFileClass::Excluded;
		return explanation;
	}
	explanation.Verdict = SFileVerdict { classification.Class, classification.bQuiet };
	if (!classification.ImporterNote.IsEmpty())
	{
		explanation.Notes.push_back(classification.ImporterNote);
	}
	explanation.Settings.Importer = classification.Importer;
	explanation.Settings.Class = classification.Class;
	explanation.Settings.bQuiet = classification.bQuiet;
	SResolveTrace trace;
	ResolveRules(scope, matcher, Facts, true, Options.Target, TargetPlatform, classification, explanation.Settings, &trace);

	for (const auto& [binding, rule] : trace.PresetsUsed)
	{
		FString note = FString::Format("rule {} uses the preset {} defined at {}", rule->Index + 1, binding->Def->Name,
			FormatProvenance(binding->File->GetOrigin(), binding->Def->Line));
		if (binding->ShadowedDef != nullptr)
		{
			note += FString::Format(", which shadows the one at {}", FormatProvenance(binding->ShadowedFile->GetOrigin(), binding->ShadowedDef->Line));
		}
		explanation.Notes.push_back(std::move(note));
	}
	if (trace.FinalRule != nullptr)
	{
		explanation.Notes.push_back(FString::Format("evaluation stopped at the Final rule {} ({})", trace.FinalRule->Index + 1,
			FormatProvenance(trace.FinalFile->GetOrigin(), trace.FinalRule->Line)));
	}
	if (Facts.Type != reflect::InvalidTypeId)
	{
		explanation.bTypeIncluded = IsTypeIncluded(Scope, Facts.Type);
		if (!explanation.bTypeIncluded)
		{
			explanation.Notes.push_back(FString::Format("the type {} is filtered out by Types: registered as Excluded",
				CAssetTypeRegistry::Get(Facts.Type).Name));
		}
	}
	return explanation;
}

SRulesListing CRulesEngine::GetListing () const
{
	EnsureLoaded();
	const auto toListing = [] (const SFileState& State)
	{
		SRuleFileListing listing;
		listing.Origin = State.Origin;
		listing.bMalformed = State.bMalformed;
		listing.Diagnostics = State.Diagnostics;
		if (State.LastGood != nullptr)
		{
			listing.RulesHash = State.LastGood->GetRulesHash();
			const std::span<const SRuleListEntry> entries = State.LastGood->GetEntries();
			listing.Entries.assign(entries.begin(), entries.end());
		}
		return listing;
	};

	SRulesListing listing;
	std::scoped_lock lock(Mutex);
	std::vector<std::pair<int32, const SFileState*>> folders;
	folders.reserve(FolderFiles.size());
	for (const auto& [path, state] : FolderFiles)
	{
		folders.emplace_back(FindMountIndex(state.Origin.MountName), &state);
	}
	std::sort(folders.begin(), folders.end(), [] (const auto& A, const auto& B)
	{
		if (A.first != B.first)
		{
			return A.first < B.first;
		}
		const FStringView a = A.second->Origin.FolderMountPath;
		const FStringView b = B.second->Origin.FolderMountPath;
		const int32 folded = a.Compare<ESearchCase::IgnoreCase>(b);
		return folded != 0 ? folded < 0 : a < b;
	});

	uint32 next = 0u;
	for (uint32 i = 0u; i < MountFiles.size(); ++i)
	{
		const SFileState& state = MountFiles[i];
		if (state.LastGood != nullptr || state.bMalformed)
		{
			listing.Files.push_back(toListing(state));
		}
		while (next < folders.size() && folders[next].first <= static_cast<int32>(i))
		{
			listing.Files.push_back(toListing(*folders[next].second));
			++next;
		}
	}
	for (; next < folders.size(); ++next)
	{
		listing.Files.push_back(toListing(*folders[next].second));
	}
	return listing;
}


// ---------------- RULE002 ----------------

std::vector<SDiagnostic> ReportUnclaimedFiles (std::span<const SUnclaimedFile> Files)
{
	struct SGroup
	{
		FStringView Folder;
		FString FoldedExtension;
		std::vector<FStringView> Names;
	};
	std::vector<SGroup> groups;
	std::map<std::pair<FString, FString>, uint32> index;   // (folder, folded extension) -> group
	for (const SUnclaimedFile& file : Files)
	{
		if (file.bQuiet)
		{
			continue;
		}
		const FStringView folder = GetMountPathParent(file.MountPath);
		const FStringView name = GetMountPathLeaf(file.MountPath);
		const SStringSplit split = name.SplitLast('.');
		const FString extension = split.bFound && !split.Left.IsEmpty() ? FString(name.RightChop(split.Left.Len())).FoldCase() : FString();
		const auto [it, bInserted] = index.try_emplace(std::pair<FString, FString>(FString(folder), extension), static_cast<uint32>(groups.size()));
		if (bInserted)
		{
			groups.push_back({ folder, extension, {} });
		}
		groups[it->second].Names.push_back(name);
	}

	const SStringLessIgnoreCase less;
	std::sort(groups.begin(), groups.end(), [&less] (const SGroup& A, const SGroup& B)
	{
		if (A.Folder != B.Folder)
		{
			if (less(A.Folder, B.Folder) != less(B.Folder, A.Folder))
			{
				return less(A.Folder, B.Folder);
			}
			return A.Folder < B.Folder;
		}
		return A.FoldedExtension < B.FoldedExtension;
	});

	std::vector<SDiagnostic> diagnostics;
	diagnostics.reserve(groups.size());
	for (SGroup& group : groups)
	{
		std::sort(group.Names.begin(), group.Names.end(), [&less] (FStringView A, FStringView B)
		{
			return less(A, B) != less(B, A) ? less(A, B) : A < B;
		});
		const FStringView example = group.Names.front();
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Warning;
		diagnostic.Code = Rule002;
		diagnostic.Subject = FString(group.Folder);
		const FString count = group.Names.size() == 1u ? FString::Format("{}", ToDisplayText(example))
			: FString::Format("{} files, e.g. {}", group.Names.size(), ToDisplayText(example));
		if (group.FoldedExtension.IsEmpty())
		{
			diagnostic.Message = FString::Format("no importer for files without an extension under {} ({}); list them in Scan.Quiet if they "
				"are not assets", ToDisplayText(group.Folder), count);
		}
		else
		{
			const uint32 dot = example.SplitLast('.').Left.Len();
			diagnostic.Message = FString::Format("no importer for {} under {} ({}); list them in Scan.Quiet if they are not assets",
				ToDisplayText(example.RightChop(dot)), ToDisplayText(group.Folder), count);
		}
		diagnostics.push_back(std::move(diagnostic));
	}
	return diagnostics;
}


// ---------------- Rules.cache ----------------

namespace
{
constexpr FStringLiteral RulesCacheFormat = "frt.rules.cache";
constexpr int64 RulesCacheVersion = 1;

std::optional<uint64> ParseHex64 (FStringView Text) noexcept
{
	if (Text.Len() != 16u)
	{
		return std::nullopt;
	}
	uint64 value = 0u;
	for (const char ch : Text)
	{
		uint32 digit = 0u;
		if (ch >= '0' && ch <= '9')
		{
			digit = static_cast<uint32>(ch - '0');
		}
		else if (ch >= 'a' && ch <= 'f')
		{
			digit = static_cast<uint32>(ch - 'a' + 10);
		}
		else
		{
			return std::nullopt;
		}
		value = value << 4u | digit;
	}
	return value;
}
}

FString FormatRulesCache (const SRulesListing& Listing)
{
	FString text = json::CJsonObject().AddString("format", RulesCacheFormat).AddInt("version", RulesCacheVersion).ToString();
	text += '\n';
	for (const SRuleFileListing& file : Listing.Files)
	{
		text += json::CJsonObject().AddString("kind", "file").AddString("mount", file.Origin.MountName)
			.AddString("folder", file.Origin.FolderMountPath).AddString("file", file.Origin.File)
			.AddString("level", enum_::ToString(file.Origin.Level)).AddString("hash", FString::Format("{:016x}", file.RulesHash))
			.AddBool("malformed", file.bMalformed).ToString();
		text += '\n';
		for (const SRuleListEntry& entry : file.Entries)
		{
			text += json::CJsonObject().AddString("kind", entry.Kind).AddString("mount", file.Origin.MountName)
				.AddString("file", file.Origin.File).AddInt("line", entry.Line).AddInt("rule", entry.RuleIndex)
				.AddString("text", entry.Text).ToString();
			text += '\n';
		}
	}
	return text;
}

bool WriteRulesCache (const std::filesystem::path& Path, const SRulesListing& Listing, FString* OutError)
{
	const FString text = FormatRulesCache(Listing);
	if (const std::optional<FString> existing = platform::ReadFileText(Path, ETextFileEncoding::Raw); existing.has_value() && *existing == text)
	{
		return true;
	}
	return platform::WriteFileAtomic(Path, text.View(), true, OutError);
}

std::optional<SRulesListing> ReadRulesCache (const std::filesystem::path& Path, FString* OutError)
{
	const auto fail = [OutError] (FString Message) -> std::optional<SRulesListing>
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return std::nullopt;
	};
	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text.has_value())
	{
		return fail(FString::Format("{} could not be read", FString::FromPath(Path)));
	}

	SRulesListing listing;
	bool bHeader = false;
	uint32 lineNumber = 0u;
	FStringView rest = text->View();
	while (!rest.IsEmpty())
	{
		const SStringSplit split = rest.SplitFirst('\n');
		const FStringView line = split.Left.TrimEnd();
		rest = split.bFound ? split.Right : FStringView();
		++lineNumber;
		if (line.IsEmpty())
		{
			continue;
		}
		reflect::CDataTree tree;
		SDiagnostic error;
		if (!reflect::ParseJson(line, tree, &error) || !tree.GetRoot().IsMap())
		{
			return fail(FString::Format("line {}: not a JSON object: {}", lineNumber, error.Message));
		}
		const reflect::HDataNode root = tree.GetRoot();
		const auto getText = [&root] (FStringView Key) { return root.FindChild(Key).AsText(); };
		if (!bHeader)
		{
			const std::optional<FStringView> format = getText("format");
			const std::optional<int64> version = root.FindChild("version").AsInt64();
			if (!format.has_value() || *format != RulesCacheFormat || version.value_or(0) != RulesCacheVersion)
			{
				return fail(FString::Format("not a {} file of version {}", RulesCacheFormat, RulesCacheVersion));
			}
			bHeader = true;
			continue;
		}
		const std::optional<FStringView> kind = getText("kind");
		if (!kind.has_value())
		{
			return fail(FString::Format("line {}: no kind", lineNumber));
		}
		if (*kind == "file")
		{
			SRuleFileListing file;
			const std::optional<FStringView> mount = getText("mount");
			const std::optional<FStringView> folder = getText("folder");
			const std::optional<FStringView> fileName = getText("file");
			const std::optional<FStringView> level = getText("level");
			const std::optional<FStringView> hashText = getText("hash");
			const std::optional<bool> malformed = root.FindChild("malformed").AsBool();
			const std::optional<uint64> hashValue = hashText.has_value() ? ParseHex64(*hashText) : std::nullopt;
			if (!mount || !folder || !fileName || !level || !hashValue || !malformed || !enum_::TryParse(*level, &file.Origin.Level, false))
			{
				return fail(FString::Format("line {}: a malformed file line", lineNumber));
			}
			file.Origin.MountName = FString(*mount);
			file.Origin.FolderMountPath = FString(*folder);
			file.Origin.File = FString(*fileName);
			file.RulesHash = *hashValue;
			file.bMalformed = *malformed;
			listing.Files.push_back(std::move(file));
			continue;
		}
		const std::optional<FStringView> entryText = getText("text");
		const std::optional<int64> entryLine = root.FindChild("line").AsInt64();
		const std::optional<int64> ruleIndex = root.FindChild("rule").AsInt64();
		if (listing.Files.empty() || !entryText || !entryLine || !ruleIndex || *entryLine < 0 || *entryLine > 0xFFFFFFFFll
			|| *ruleIndex < -1 || *ruleIndex > 0x7FFFFFFFll)
		{
			return fail(FString::Format("line {}: a malformed entry line", lineNumber));
		}
		SRuleListEntry entry;
		entry.Kind = FString(*kind);
		entry.Text = FString(*entryText);
		entry.Line = static_cast<uint32>(*entryLine);
		entry.RuleIndex = static_cast<int32>(*ruleIndex);
		listing.Files.back().Entries.push_back(std::move(entry));
	}
	if (!bHeader)
	{
		return fail("empty");
	}
	return listing;
}
}
