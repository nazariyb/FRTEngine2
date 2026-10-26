#include "Assets/Pipeline/Importers/LegacyTextReader.h"

#include <optional>
#include <vector>

#include "Assets/TextAssetIO.h"


namespace frt::assets::importers
{
namespace
{
using reflect::CDataTreeBuilder;
using reflect::EScalarStyle;

// One `key: value` line: views into the text, its 1-based line, and byte columns (0 when a part is empty).
struct SLineValue
{
	FStringView Key;
	FStringView Value;
	uint32 Line = 0u;
	uint32 KeyColumn = 0u;
	uint32 ValueColumn = 0u;
	EScalarStyle Style = EScalarStyle::Plain;
};

uint32 ColumnOf (FStringView Line, FStringView Part) noexcept
{
	const char* begin = Line.GetData();
	const char* part = Part.GetData();
	if (Part.IsEmpty() || part < begin || part > begin + Line.Len())
	{
		return 0u;
	}
	return static_cast<uint32>(part - begin) + 1u;
}

// TryParseKeyValue over one line, with positions; false for what the runtime skips (and empty keys).
bool ReadLine (FStringView Line, uint32 LineNumber, SLineValue& Out)
{
	FStringView key;
	FStringView value;
	if (!text::TryParseKeyValue(Line, &key, &value) || key.IsEmpty())
	{
		return false;
	}
	Out.Key = key;
	Out.Value = value;
	Out.Line = LineNumber;
	Out.KeyColumn = ColumnOf(Line, key);
	Out.ValueColumn = ColumnOf(Line, value);
	Out.Style = EScalarStyle::Plain;
	// TryParseKeyValue removed one pair of quotes when the byte before the value is one.
	if (Out.ValueColumn > 1u)
	{
		const char before = Line[Out.ValueColumn - 2u];
		Out.Style = before == '"' ? EScalarStyle::DoubleQuoted : before == '\'' ? EScalarStyle::SingleQuoted : EScalarStyle::Plain;
	}
	return true;
}

// Calls Visit(SLineValue) for every line the runtime reads, in order.
template <typename F>
void ForEachLine (FStringView Text, F&& Visit)
{
	uint32 lineNumber = 0u;
	for (const FStringView line : Text.Lines())
	{
		++lineNumber;
		SLineValue value;
		if (ReadLine(line, lineNumber, value))
		{
			Visit(value);
		}
	}
}

void AddEntry (CDataTreeBuilder& Builder, FStringView Key, const SLineValue& Value)
{
	Builder.Key(Key, Value.Line, Value.KeyColumn);
	Builder.Scalar(Value.Value, Value.Style, Value.Line, Value.ValueColumn != 0u ? Value.ValueColumn : Value.KeyColumn);
}

reflect::CDataTree FinishTree (CDataTreeBuilder& Builder)
{
	reflect::CDataTree tree;
	if (!Builder.Finish(tree))
	{
		return reflect::CDataTree();
	}
	return tree;
}
}


reflect::CDataTree ReadKeyValueTree (FStringView Text)
{
	std::vector<SLineValue> entries;
	ForEachLine(Text, [&] (const SLineValue& Value)
	{
		for (auto it = entries.begin(); it != entries.end(); ++it)
		{
			if (it->Key == Value.Key)
			{
				entries.erase(it);
				break;
			}
		}
		entries.push_back(Value);
	});

	CDataTreeBuilder builder;
	builder.BeginMap(entries.empty() ? 1u : entries.front().Line, 1u);
	for (const SLineValue& entry : entries)
	{
		AddEntry(builder, entry.Key, entry);
	}
	builder.End();
	return FinishTree(builder);
}

reflect::CDataTree ReadInputMapTree (FStringView Text)
{
	struct SAction
	{
		SLineValue Name;
		std::optional<SLineValue> Type;
		std::vector<SLineValue> Binds;
	};
	std::optional<SLineValue> version;
	std::vector<SAction> actions;
	ForEachLine(Text, [&] (const SLineValue& Value)
	{
		if (Value.Key == "version")
		{
			version = Value;
		}
		else if (Value.Key == "action")
		{
			actions.push_back(SAction { Value, std::nullopt, {} });
		}
		else if (actions.empty())
		{
			return;
		}
		else if (Value.Key == "type")
		{
			actions.back().Type = Value;
		}
		else if (Value.Key == "bind")
		{
			actions.back().Binds.push_back(Value);
		}
	});

	CDataTreeBuilder builder;
	builder.BeginMap(1u, 1u);
	if (version)
	{
		AddEntry(builder, "version", *version);
	}
	if (!actions.empty())
	{
		const SLineValue& first = actions.front().Name;
		builder.Key("actions", first.Line, first.KeyColumn);
		builder.BeginSequence(first.Line, first.KeyColumn);
		for (const SAction& action : actions)
		{
			builder.BeginMap(action.Name.Line, action.Name.KeyColumn);
			AddEntry(builder, "action", action.Name);
			if (action.Type)
			{
				AddEntry(builder, "type", *action.Type);
			}
			if (!action.Binds.empty())
			{
				builder.Key("bind", action.Binds.front().Line, action.Binds.front().KeyColumn);
				builder.BeginSequence(action.Binds.front().Line, action.Binds.front().KeyColumn);
				for (const SLineValue& bind : action.Binds)
				{
					builder.Scalar(bind.Value, bind.Style, bind.Line, bind.ValueColumn != 0u ? bind.ValueColumn : bind.KeyColumn);
				}
				builder.End();
			}
			builder.End();
		}
		builder.End();
	}
	builder.End();
	return FinishTree(builder);
}

FStringView ParseInputActionKind (FStringView Value) noexcept
{
	// The runtime's NormalizeEnumToken, then a case-insensitive match of EInputActionKind's names.
	char normalized[16] = {};
	uint32 length = 0u;
	for (uint32 i = 0u; i < Value.Len(); ++i)
	{
		const char ch = Value[i];
		if (ch == '_' || ch == '-' || ch == ' ')
		{
			continue;
		}
		if (length == sizeof(normalized))
		{
			return FStringView();
		}
		normalized[length++] = ch;
	}
	const FStringView token(normalized, length);
	if (token.Equals<ESearchCase::IgnoreCaseAscii>("Button"))
	{
		return "Button";
	}
	if (token.Equals<ESearchCase::IgnoreCaseAscii>("Axis"))
	{
		return "Axis";
	}
	return FStringView();
}
}
