#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <vector>

#include "Assets/AssetTypes.h"
#include "Assets/Pipeline/Importers/LegacyTextReader.h"
#include "Assets/TextAssetIO.h"
#include "Containers/StringMap.h"
#include "Reflect/DataTree.h"


namespace frt::assets::importers
{
namespace
{
using reflect::HDataNode;

constexpr FStringLiteral InputFolder = "Input";
constexpr FStringLiteral ActionExtension = ".frtinputaction";

/**
 * The action file an input map's action name loads, as CInputActionLibrary::MakeActionPath builds it: the
 * mount's Input folder (ResolveInputContentRoot: the content root the map's runtime loads from, here the map's
 * own mount) joined with the name, plus ".frtinputaction" when the name's last segment has no extension
 * (std::filesystem's has_extension: a dot that is not the first character). nullopt for a name that leaves the
 * Input folder's mount or is not a relative path.
 */
std::optional<FString> MakeActionMountPath (FStringView MapMountPath, FStringView Name, EReferenceProblem& OutProblem)
{
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(MapMountPath, mountName, relative))
	{
		OutProblem = EReferenceProblem::NotRelative;
		return std::nullopt;
	}
	std::optional<FString> path = ResolveFolderReference(JoinMountPath(mountName, InputFolder), Name, &OutProblem);
	if (!path)
	{
		return std::nullopt;
	}
	const FStringView leaf = GetMountPathLeaf(*path);
	const uint32 dot = leaf.FindLast('.');
	if (dot == FStringView::NotFound || dot == 0u)
	{
		*path += ActionExtension;
	}
	return path;
}

/**
 * Input.Legacy (§14.4): `.frtinputmap` and `.frtinputaction`, line formats read as the runtime reads them
 * (LegacyTextReader.h). A map is an frt.InputActionMap: the Actions tag (each action name once, in file order), a
 * Path edge per action to the action file the runtime loads for it, and the Primary flag with PrimaryName = the
 * file's stem (§10: input maps are primary assets, type alias InputMap). An action is an frt.InputAction with the
 * ValueType tag (Button or Axis; the runtime's default Button when no `type:` parses). Both trees go through the
 * schema-free scan.
 */
class CLegacyInputImporter final : public IAssetImporter
{
public:
	const SImporterDesc& GetInfo () const noexcept override
	{
		static const SImporterDesc* const desc = FindImporter("Input.Legacy");
		return *desc;
	}

	bool Analyze (const SAnalyzeInput& In, const SResolvedSettings& /*Settings*/, SAnalyzeResult& Out) const override
	{
		std::optional<std::vector<uint8>> bytes = ReadWholeFileHashed(In, Out);
		if (!bytes)
		{
			return false;
		}
		const std::optional<FString> text = DecodeSourceText(std::move(*bytes), In, Out);
		if (!text)
		{
			return false;
		}

		if (Out.Type == asset_types::InputAction)
		{
			AnalyzeAction(In, *text, Out);
		}
		else
		{
			AnalyzeMap(In, *text, Out);
		}
		return true;
	}

private:
	static void AnalyzeAction (const SAnalyzeInput& In, FStringView Text, SAnalyzeResult& Out)
	{
		const reflect::CDataTree tree = ReadKeyValueTree(Text);
		// Every `type:` line in turn, as LoadInputActionDefinition reads them: one the runtime cannot parse keeps
		// the kind an earlier one set. The tree holds only the last line of a repeated key.
		FStringView kind = "Button";
		uint32 lineNumber = 0u;
		for (const FStringView line : Text.Lines())
		{
			++lineNumber;
			FStringView key;
			FStringView value;
			if (!text::TryParseKeyValue(line, &key, &value) || key != "type")
			{
				continue;
			}
			const FStringView parsed = ParseInputActionKind(value);
			if (parsed.IsEmpty())
			{
				SDiagnostic diagnostic = MakeFileDiagnostic(In, ESeverity::Warning,
					FString::Format("the action type '{}' is neither Button nor Axis: the runtime ignores it", value));
				diagnostic.Line = lineNumber;
				diagnostic.FieldPath = "type";
				Out.Diagnostics.push_back(std::move(diagnostic));
				continue;
			}
			kind = parsed;
		}
		Out.Tags.push_back({ "ValueType", STagValue::FromName(kind) });
		GatherSchemaFree(tree, In, Out);
	}

	static void AnalyzeMap (const SAnalyzeInput& In, FStringView Text, SAnalyzeResult& Out)
	{
		const reflect::CDataTree tree = ReadInputMapTree(Text);
		const HDataNode actions = tree.Find("actions");
		FStringSet names;
		FStringSet targets;
		for (uint32 i = 0u; i < actions.GetCount(); ++i)
		{
			const HDataNode name = actions.GetValue(i).FindChild("action");
			const FString fieldPath = FString::Format("actions.{}.action", i);
			const FStringView text = name.GetText();
			const auto report = [&] (FString Message)
			{
				SDiagnostic diagnostic = MakeFileDiagnostic(In, ESeverity::Warning, std::move(Message));
				diagnostic.Line = name.GetLine();
				diagnostic.Column = name.GetColumn();
				diagnostic.FieldPath = fieldPath;
				Out.Diagnostics.push_back(std::move(diagnostic));
			};
			if (text.IsEmpty())
			{
				report("an action without a name: the runtime loads no action file for it");
				continue;
			}
			if (names.insert(FString(text)).second)
			{
				Out.Tags.push_back({ "Actions", STagValue::FromName(text) });
			}
			EReferenceProblem problem = EReferenceProblem::None;
			std::optional<FString> target = MakeActionMountPath(In.FileMountPath, text, problem);
			if (!target)
			{
				report(FString::Format("the action '{}' names no file in the Input folder: {}", text, DescribeReferenceProblem(problem)));
				continue;
			}
			if (!targets.insert(*target).second)
			{
				continue;
			}
			SEdgeDecl& edge = Out.Edges.emplace_back();
			edge.TargetPath = std::move(*target);
			edge.Flags = EDepFlags::Path;
			edge.FieldPath = fieldPath;
		}
		Out.Flags |= ERecordFlags::Primary;
		Out.PrimaryName = FString(GetAssetStem(GetMountPathLeaf(In.FileMountPath)));
		GatherSchemaFree(tree, In, Out);
	}
};
}

const IAssetImporter& GetLegacyInputImporter () noexcept
{
	static const CLegacyInputImporter importer;
	return importer;
}
}
