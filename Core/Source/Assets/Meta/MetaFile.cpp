#include "Assets/Meta/MetaFile.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Reflect/CanonicalEmitter.h"
#include "Reflect/DataTreeYaml.h"


namespace frt::assets
{
namespace
{
using reflect::CDataTree;
using reflect::CDataTreeBuilder;
using reflect::ECollectionStyle;
using reflect::EDataNodeKind;
using reflect::EScalarStyle;
using reflect::HDataNode;
using reflect::SDataChild;

constexpr uint32 SupportedFormatVersion = 1u;
constexpr FStringLiteral Bom = "\xEF\xBB\xBF";

// The one rule of a meta's document: its top-level order. Unknown keys are not in it, so they follow in the tree's order.
constexpr reflect::SEmitRule TopLevelRule = { .Path = "", .KeyOrder = MetaTopLevelKeys };


// ---------------- The line scan: what a broken file still says, without a parser ----------------

struct SLineScan
{
	std::vector<SGuid> Guids;      // what the top-level `Guid:` lines say, distinct and in file order
	uint32 GuidLineCount = 0u;     // those lines, readable or not
	uint32 SecondGuidLine = 0u;    // from 1
	uint32 ConflictLine = 0u;      // from 1, the first merge conflict marker; 0 for none
};

// Git's markers: at the start of a line, seven of a character, then a space or the end of the line.
[[nodiscard]] bool IsConflictMarker (FStringView Line) noexcept
{
	static constexpr FStringLiteral Markers[] = { "<<<<<<<", "|||||||", "=======", ">>>>>>>" };
	for (const FStringLiteral marker : Markers)
	{
		if (Line.StartsWith(marker) && (Line.Len() == marker.Len() || Line[marker.Len()] == ' '))
		{
			return true;
		}
	}
	return false;
}

// The value of a line that is the top-level key `Guid`, or nullopt when it is not. Its text is unparsed: a comment or
// quotes may still surround the GUID.
[[nodiscard]] std::optional<FStringView> GetGuidLineValue (FStringView Line) noexcept
{
	if (!Line.StartsWith("Guid"))
	{
		return std::nullopt;
	}
	FStringView rest = Line.RightChop(4u);
	while (rest.StartsWith(' ') || rest.StartsWith('\t'))
	{
		rest = rest.RightChop(1u);
	}
	if (!rest.StartsWith(':'))
	{
		return std::nullopt;
	}
	rest = rest.RightChop(1u);
	if (!rest.IsEmpty() && rest[0] != ' ' && rest[0] != '\t')
	{
		return std::nullopt;   // `Guid:x` is the plain text of no key
	}
	return rest;
}

[[nodiscard]] std::optional<SGuid> ParseGuidLineValue (FStringView Value) noexcept
{
	FStringView text = Value;
	while (text.StartsWith(' ') || text.StartsWith('\t'))
	{
		text = text.RightChop(1u);
	}
	if (text.StartsWith('"') || text.StartsWith('\''))
	{
		const char quote = text[0];
		text = text.RightChop(1u);
		const uint32 end = text.Find(quote);
		if (end == FStringView::NotFound)
		{
			return std::nullopt;
		}
		text = text.Left(end);
	}
	else
	{
		const uint32 comment = text.Find(FStringView(" #"));
		if (comment != FStringView::NotFound)
		{
			text = text.Left(comment);
		}
		while (text.EndsWith(' ') || text.EndsWith('\t'))
		{
			text = text.LeftChop(1u);
		}
	}
	const std::optional<SGuid> guid = SGuid::Parse(text);
	return guid && guid->IsValid() ? guid : std::nullopt;
}

[[nodiscard]] SLineScan ScanLines (FStringView Bytes) noexcept
{
	SLineScan scan;
	FStringView rest = Bytes.StartsWith(Bom) ? Bytes.RightChop(Bom.Len()) : Bytes;
	uint32 lineNumber = 0u;
	while (!rest.IsEmpty())
	{
		const SStringSplit split = rest.SplitFirst('\n');
		rest = split.Right;
		++lineNumber;
		const FStringView line = split.Left.EndsWith('\r') ? split.Left.LeftChop(1u) : split.Left;
		if (scan.ConflictLine == 0u && IsConflictMarker(line))
		{
			scan.ConflictLine = lineNumber;
		}
		const std::optional<FStringView> value = GetGuidLineValue(line);
		if (!value)
		{
			continue;
		}
		if (++scan.GuidLineCount == 2u)
		{
			scan.SecondGuidLine = lineNumber;
		}
		const std::optional<SGuid> guid = ParseGuidLineValue(*value);
		if (guid && std::find(scan.Guids.begin(), scan.Guids.end(), *guid) == scan.Guids.end())
		{
			scan.Guids.push_back(*guid);
		}
	}
	return scan;
}


// ---------------- The tree -> SAssetMeta ----------------

// Reads the parsed document, stopping at the first value that is not what §3.2 says.
class CMetaReader
{
public:
	CMetaReader (SAssetMeta& InMeta, std::vector<SDiagnostic>& InDiagnostics) noexcept
		: Meta(InMeta)
		, Diagnostics(InDiagnostics)
	{}

	[[nodiscard]] bool Read (const HDataNode& Root)
	{
		bool bHasFormatVersion = false;
		bool bHasGuid = false;
		for (const SDataChild child : Root.GetChildren())
		{
			Key = child.Key;
			if (IsKnown(Key))
			{
				// A key that holds nothing is absent, and a required one stays missing.
				if (child.Value.IsNull())
				{
					continue;
				}
				bHasFormatVersion = bHasFormatVersion || Key == "FormatVersion";
				bHasGuid = bHasGuid || Key == "Guid";
				if (!ReadKnown(child.Value))
				{
					return false;
				}
			}
			else
			{
				ReadUnknown(child);
			}
		}
		if (!bHasFormatVersion || !bHasGuid)
		{
			Key = FStringView();
			return Fail(Root, FString::Format("the required key {} is missing", bHasFormatVersion ? "Guid" : "FormatVersion").View());
		}
		return true;
	}

	SDiagnostic Failure;

private:
	SAssetMeta& Meta;
	std::vector<SDiagnostic>& Diagnostics;
	FStringView Key;

	[[nodiscard]] static bool IsKnown (FStringView Name) noexcept
	{
		return std::find(std::begin(MetaTopLevelKeys), std::end(MetaTopLevelKeys), Name) != std::end(MetaTopLevelKeys);
	}

	bool Fail (const HDataNode& Node, FStringView Message)
	{
		Failure.Severity = ESeverity::Error;
		Failure.Code = Meta004;
		Failure.Line = Node.GetLine();
		Failure.Column = Node.GetColumn();
		Failure.FieldPath = FString(Key);
		Failure.Message = Key.IsEmpty() ? FString(Message) : FString::Format("{}: {}", Key, Message);
		return false;
	}

	[[nodiscard]] bool ReadKnown (const HDataNode& Value)
	{
		if (Key == "FormatVersion")
		{
			return ReadFormatVersion(Value);
		}
		if (Key == "Guid")
		{
			return ReadGuid(Value, Meta.Guid);
		}
		if (Key == "Importer")
		{
			return ReadText(Value, Meta.Importer);
		}
		if (Key == "SettingsVersion")
		{
			uint32 version = 0u;
			if (!ReadInteger(Value, version))
			{
				return false;
			}
			Meta.SettingsVersion = version;
			return true;
		}
		if (Key == "Settings")
		{
			return ReadMap(Value, Meta.Settings);
		}
		if (Key == "Targets")
		{
			return ReadMap(Value, Meta.Targets);
		}
		if (Key == "SubAssets")
		{
			return ReadMap(Value, Meta.SubAssets);
		}
		if (Key == "Labels")
		{
			return ReadTextList(Value, Meta.Labels);
		}
		if (Key == "FormerNames")
		{
			return ReadTextList(Value, Meta.FormerNames);
		}
		if (Key == "Aliases")
		{
			return ReadGuidList(Value, Meta.Aliases);
		}
		if (Key == "FormerPaths")
		{
			return ReadTextList(Value, Meta.FormerPaths);
		}
		if (Key == "Cook")
		{
			return ReadCook(Value);
		}
		if (Key == "Chunk")
		{
			uint16 chunk = 0u;
			if (!ReadInteger(Value, chunk))
			{
				return false;
			}
			Meta.Chunk = chunk;
			return true;
		}
		// RebuildNonce, the last of MetaTopLevelKeys.
		SGuid nonce;
		if (!ReadGuid(Value, nonce))
		{
			return false;
		}
		Meta.RebuildNonce = nonce;
		return true;
	}

	void ReadUnknown (const SDataChild& Child)
	{
		SMetaUnknownKey unknown;
		unknown.Key = FString(Child.Key);
		CopySubtree(Child.Value, unknown.Value);
		Meta.UnknownKeys.push_back(std::move(unknown));

		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Warning;
		diagnostic.Line = Child.KeyLine;
		diagnostic.Column = Child.KeyColumn;
		diagnostic.FieldPath = FString(Child.Key);
		diagnostic.Message = FString::Format("the key {} is unknown to this build: it is kept and written back", Child.Key);
		Diagnostics.push_back(std::move(diagnostic));
	}

	static void CopySubtree (const HDataNode& Node, CDataTree& Out)
	{
		CDataTreeBuilder builder;
		builder.Subtree(Node);
		builder.Finish(Out);
	}

	bool ReadFormatVersion (const HDataNode& Value)
	{
		if (!ReadInteger(Value, Meta.FormatVersion))
		{
			return false;
		}
		if (Meta.FormatVersion == 0u)
		{
			return Fail(Value, "0 is no format version");
		}
		if (Meta.FormatVersion > SupportedFormatVersion)
		{
			SDiagnostic diagnostic;
			diagnostic.Severity = ESeverity::Warning;
			diagnostic.Line = Value.GetLine();
			diagnostic.Column = Value.GetColumn();
			diagnostic.FieldPath = FString(Key);
			diagnostic.Message = FString::Format("format version {} is newer than this build's {}: an update may be due",
				Meta.FormatVersion, SupportedFormatVersion);
			Diagnostics.push_back(std::move(diagnostic));
		}
		return true;
	}

	/**
	 * A GUID in any spelling §3.2 accepts. Unquoted braces are YAML too: `{5d1e7a3b-...}` is a flow map with that one key
	 * and no value, so that form is read back from the key.
	 */
	bool ReadGuid (const HDataNode& Value, SGuid& Out)
	{
		FStringView text;
		if (Value.IsScalar())
		{
			text = Value.GetText();
		}
		else if (Value.IsMap() && Value.GetCollectionStyle() == ECollectionStyle::Flow && Value.GetCount() == 1u && Value.GetValue(0u).IsNull())
		{
			text = Value.GetKey(0u);
		}
		else
		{
			return Fail(Value, "expected a GUID");
		}
		const std::optional<SGuid> guid = SGuid::Parse(text);
		if (!guid)
		{
			return Fail(Value, FString::Format("'{}' is not a GUID", text.Truncate(40u)).View());
		}
		if (!guid->IsValid())
		{
			return Fail(Value, "the all-zero GUID names nothing");
		}
		Out = *guid;
		return true;
	}

	bool ReadText (const HDataNode& Value, FString& Out)
	{
		SDiagnostic error;
		const std::optional<FStringView> text = Value.AsText(&error);
		if (!text)
		{
			return Fail(Value, error.Message.View());
		}
		Out = FString(*text);
		return true;
	}

	template <typename T>
	bool ReadInteger (const HDataNode& Value, T& Out)
	{
		SDiagnostic error;
		const std::optional<int64> number = Value.AsInt64(&error);
		if (!number)
		{
			return Fail(Value, error.Message.View());
		}
		if (*number < 0 || static_cast<uint64>(*number) > std::numeric_limits<T>::max())
		{
			return Fail(Value, FString::Format("{} is out of range", *number).View());
		}
		Out = static_cast<T>(*number);
		return true;
	}

	bool ReadMap (const HDataNode& Value, CDataTree& Out)
	{
		if (!Value.IsMap())
		{
			return Fail(Value, "expected a map");
		}
		CopySubtree(Value, Out);
		return true;
	}

	bool ReadTextList (const HDataNode& Value, std::vector<FString>& Out)
	{
		if (!Value.IsSequence())
		{
			return Fail(Value, "expected a list");
		}
		for (const SDataChild item : Value.GetChildren())
		{
			FString text;
			if (!ReadText(item.Value, text))
			{
				return false;
			}
			Out.push_back(std::move(text));
		}
		return true;
	}

	bool ReadGuidList (const HDataNode& Value, std::vector<SGuid>& Out)
	{
		if (!Value.IsSequence())
		{
			return Fail(Value, "expected a list of GUIDs");
		}
		for (const SDataChild item : Value.GetChildren())
		{
			SGuid guid;
			if (!ReadGuid(item.Value, guid))
			{
				return false;
			}
			Out.push_back(guid);
		}
		return true;
	}

	bool ReadCook (const HDataNode& Value)
	{
		FString text;
		if (!ReadText(Value, text))
		{
			return false;
		}
		ECookRule rule = ECookRule::Auto;
		if (!enum_::TryParse(text.View(), &rule))
		{
			return Fail(Value, FString::Format("'{}' is not a cook rule (Auto, Always, Never, EditorOnly, DevelopmentOnly)", text.View().Truncate(40u)).View());
		}
		Meta.Cook = rule;
		return true;
	}
};


// ---------------- SAssetMeta -> the tree ----------------

// One key at a time, in §3.2's order. Text goes in as quoted, which the emitter drops when nothing needs it; that way a
// GUID or a label that spells a number stays text. Numbers go in plain.
class CMetaTreeBuilder
{
public:
	CMetaTreeBuilder ()
	{
		Builder.BeginMap(0u, 0u);
	}

	void Number (FStringLiteral Key, uint64 Value)
	{
		FString text;
		text.AppendNumber(Value);
		Builder.Key(Key, 0u, 0u);
		Builder.Scalar(text.View(), EScalarStyle::Plain, 0u, 0u);
	}

	void Text (FStringLiteral Key, FStringView Value)
	{
		Builder.Key(Key, 0u, 0u);
		Builder.Scalar(Value, EScalarStyle::DoubleQuoted, 0u, 0u);
	}

	void Tree (FStringView Key, const CDataTree& Value)
	{
		if (!Value.IsEmpty())
		{
			Builder.Key(Key, 0u, 0u);
			Builder.Subtree(Value.GetRoot());
		}
	}

	void TextList (FStringLiteral Key, std::span<const FString> Values)
	{
		if (!Values.empty())
		{
			Builder.Key(Key, 0u, 0u);
			Builder.BeginSequence(0u, 0u);
			for (const FString& value : Values)
			{
				Builder.Scalar(value.View(), EScalarStyle::DoubleQuoted, 0u, 0u);
			}
			Builder.End();
		}
	}

	[[nodiscard]] bool Finish (CDataTree& Out, SDiagnostic* OutError)
	{
		Builder.End();
		return Builder.Finish(Out, OutError);
	}

private:
	CDataTreeBuilder Builder;
};

[[nodiscard]] bool RefuseMeta (SDiagnostic* OutError, FStringView Message)
{
	if (OutError != nullptr)
	{
		OutError->Severity = ESeverity::Error;
		OutError->Line = 0u;
		OutError->Column = 0u;
		OutError->Message = FString(Message);
	}
	return false;
}

[[nodiscard]] bool BuildMetaTree (const SAssetMeta& Meta, CDataTree& Out, SDiagnostic* OutError)
{
	if (Meta.FormatVersion == 0u)
	{
		return RefuseMeta(OutError, "0 is no format version");
	}
	if (!Meta.Guid.IsValid())
	{
		return RefuseMeta(OutError, "a meta needs a valid Guid");
	}
	if (Meta.RebuildNonce && !Meta.RebuildNonce->IsValid())
	{
		return RefuseMeta(OutError, "RebuildNonce is not a valid GUID");
	}
	for (const SGuid& alias : Meta.Aliases)
	{
		if (!alias.IsValid())
		{
			return RefuseMeta(OutError, "Aliases holds an invalid GUID");
		}
	}
	const auto isMapOrAbsent = [] (const CDataTree& Tree) { return Tree.IsEmpty() || Tree.GetRoot().IsMap(); };
	if (!isMapOrAbsent(Meta.Settings) || !isMapOrAbsent(Meta.Targets) || !isMapOrAbsent(Meta.SubAssets))
	{
		return RefuseMeta(OutError, "Settings, Targets and SubAssets must be maps");
	}

	std::vector<FString> labels = Meta.Labels;
	std::sort(labels.begin(), labels.end());
	labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
	std::vector<FString> aliases;
	aliases.reserve(Meta.Aliases.size());
	for (const SGuid& alias : Meta.Aliases)
	{
		aliases.push_back(alias.ToString());
	}

	CMetaTreeBuilder builder;
	builder.Number("FormatVersion", Meta.FormatVersion);
	builder.Text("Guid", Meta.Guid.ToString().View());
	if (!Meta.Importer.IsEmpty())
	{
		builder.Text("Importer", Meta.Importer.View());
	}
	if (Meta.SettingsVersion && !Meta.Settings.IsEmpty())
	{
		builder.Number("SettingsVersion", *Meta.SettingsVersion);
	}
	builder.Tree("Settings", Meta.Settings);
	builder.Tree("Targets", Meta.Targets);
	builder.Tree("SubAssets", Meta.SubAssets);
	builder.TextList("Labels", labels);
	builder.TextList("FormerNames", Meta.FormerNames);
	builder.TextList("Aliases", aliases);
	builder.TextList("FormerPaths", Meta.FormerPaths);
	if (Meta.Cook)
	{
		builder.Text("Cook", enum_::ToString(*Meta.Cook));
	}
	if (Meta.Chunk)
	{
		builder.Number("Chunk", *Meta.Chunk);
	}
	if (Meta.RebuildNonce)
	{
		builder.Text("RebuildNonce", Meta.RebuildNonce->ToString().View());
	}
	for (const SMetaUnknownKey& unknown : Meta.UnknownKeys)
	{
		builder.Tree(unknown.Key.View(), unknown.Value);
	}
	return builder.Finish(Out, OutError);
}

[[nodiscard]] bool EmitMetaWith (const SAssetMeta& Meta, bool bVerify, FString& Out, SDiagnostic* OutError)
{
	Out.Clear();
	CDataTree tree;
	if (!BuildMetaTree(Meta, tree, OutError))
	{
		return false;
	}
	const reflect::SEmitPolicy policy = { .Rules = std::span(&TopLevelRule, 1u), .bVerifyRoundTrip = bVerify };
	return reflect::EmitCanonical(tree, policy, Out, OutError);
}

void MarkBroken (SMetaParseResult& Out, uint32 Line, uint32 Column, FStringView Message, const SLineScan& Scan)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = ESeverity::Error;
	diagnostic.Code = Meta004;
	diagnostic.Line = Line;
	diagnostic.Column = Column;
	diagnostic.Message = FString(Message);
	Out.Diagnostics.push_back(std::move(diagnostic));
	Out.Meta = SAssetMeta();
	Out.State = EMetaParseState::Broken;
	Out.SalvagedGuids = Scan.Guids;
}
}


void ParseMeta (FStringView Bytes, SMetaParseResult& Out)
{
	Out = SMetaParseResult();
	Out.bHasBom = Bytes.StartsWith(Bom);
	Out.bHasCrLf = Bytes.Contains(FStringView("\r\n"));
	Out.bInvalidUtf8 = !Bytes.IsValidUtf8();
	if (Out.bHasCrLf)
	{
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Error;
		diagnostic.Code = Vcs001;
		diagnostic.Message = FString("CRLF line endings in a hashed FRT text file");
		Out.Diagnostics.push_back(std::move(diagnostic));
	}

	const SLineScan scan = ScanLines(Bytes);
	if (Out.bInvalidUtf8)
	{
		MarkBroken(Out, 0u, 0u, "the file is not valid UTF-8", scan);
		return;
	}
	if (scan.ConflictLine != 0u)
	{
		MarkBroken(Out, scan.ConflictLine, 1u, "the file holds merge conflict markers", scan);
		return;
	}
	if (scan.GuidLineCount >= 2u)
	{
		MarkBroken(Out, scan.SecondGuidLine, 1u, "the file has two Guid lines", scan);
		return;
	}

	CDataTree tree;
	SDiagnostic parseError;
	if (!reflect::ParseYaml(Bytes, tree, &parseError))
	{
		MarkBroken(Out, parseError.Line, parseError.Column, parseError.Message.View(), scan);
		return;
	}
	if (!tree.GetRoot().IsMap())
	{
		MarkBroken(Out, 1u, 1u, "the file holds no map of keys", scan);
		return;
	}
	CMetaReader reader(Out.Meta, Out.Diagnostics);
	if (!reader.Read(tree.GetRoot()))
	{
		const SDiagnostic failure = reader.Failure;
		MarkBroken(Out, failure.Line, failure.Column, failure.Message.View(), scan);
		Out.Diagnostics.back().FieldPath = failure.FieldPath;
		return;
	}
	Out.State = EMetaParseState::Ok;

	// Bytes that parsed into this meta: equal output needs no second parse to prove anything.
	FString canonical;
	Out.bCanonical = EmitMetaWith(Out.Meta, false, canonical, nullptr) && canonical.View() == Bytes;
	if (!Out.bCanonical)
	{
		SDiagnostic diagnostic;
		diagnostic.Severity = ESeverity::Warning;
		diagnostic.Code = Meta005;
		diagnostic.Message = FString("the meta is not canonical: the next write rewrites it (`FRT.bat assets resave --metas` does it in its own commit)");
		Out.Diagnostics.push_back(std::move(diagnostic));
	}
}

bool EmitMeta (const SAssetMeta& Meta, FString& Out, SDiagnostic* OutError)
{
	return EmitMetaWith(Meta, true, Out, OutError);
}
}
