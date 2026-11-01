#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Pipeline/Importers/LegacyTextReader.h"
#include "Assets/TextAssetIO.h"
#include "Reflect/DataTree.h"
#include "Reflect/DataTreeYaml.h"


namespace frt::assets::importers
{
namespace
{
using reflect::HDataNode;

// The texture keys of each format, as the runtime reads them: v2 and v3 through CYamlReadArchive
// (MaterialSerialize.cpp), v1 through CMaterialLegacyTextSerializer.
constexpr FStringLiteral YamlTextureKeys[] = { "albedo.texture", "emissive.texture", "metallicRoughnessTexture", "normal.texture", "occlusion.texture" };
constexpr FStringLiteral LineTextureKeys[] = { "base_color_texture" };

/**
 * One legacy texture reference (§4.4 "Legacy path references"): a path relative to the material's folder
 * (SMaterial::ResolveTexturePath). It becomes a Build | Path edge - resolved at commit through the registry's path
 * index, Unresolved only while no record matches - flags the record LegacyReferences, and adds a REF007 notice
 * until `migrate material-refs` (M7) converts it. A reference the runtime would read as a machine path (a drive,
 * a rooted path, a '..' above the mount) is no edge, only a warning - but it is a legacy path reference all the
 * same, the one most in need of migrating, so it flags the record LegacyReferences too (`is:legacy-ref`).
 */
void AddTextureReference (const SAnalyzeInput& In, const HDataNode& Node, FStringView FieldPath, SAnalyzeResult& Out)
{
	if (!Node || Node.IsNull())
	{
		return;
	}
	const auto report = [&] (ESeverity Severity, FString Message, uint32 Code)
	{
		SDiagnostic diagnostic = MakeFileDiagnostic(In, Severity, std::move(Message), Code);
		diagnostic.Line = Node.GetLine();
		diagnostic.Column = Node.GetColumn();
		diagnostic.FieldPath = FString(FieldPath);
		Out.Diagnostics.push_back(std::move(diagnostic));
	};
	if (!Node.IsScalar())
	{
		report(ESeverity::Warning, FString::Format("{} is not text: the runtime reads no texture from it", FieldPath), 0u);
		return;
	}
	const FStringView reference = Node.GetText();
	if (reference.IsEmpty())
	{
		return;
	}
	Out.Flags |= ERecordFlags::LegacyReferences;
	EReferenceProblem problem = EReferenceProblem::None;
	std::optional<FString> target = ResolveRelativeReference(In.FileMountPath, reference, &problem);
	if (!target)
	{
		report(ESeverity::Warning, FString::Format("the texture '{}' in {} is not recorded: {}", reference, FieldPath, DescribeReferenceProblem(problem)), 0u);
		return;
	}

	report(ESeverity::Info, FString::Format("legacy path reference '{}' in {} (resolves to {}); `FRT.bat assets migrate material-refs` will convert it",
		reference, FieldPath, *target), Ref007);
	SEdgeDecl& edge = Out.Edges.emplace_back();
	edge.TargetPath = std::move(*target);
	edge.Flags = EDepFlags::Build | EDepFlags::Path;
	edge.FieldPath = FString(FieldPath);
}

/**
 * Material.Legacy (§14.1): the legacy `.frtmat.yml` material, dispatched as the runtime dispatches it - by
 * GetTextAssetVersion first: version 2 and 3 are YAML (ParseYaml), version 1 is `key = value` lines (not YAML),
 * read as CMaterialLegacyTextSerializer reads them into a CDataTree. Records the texture references; the
 * schema-free scan runs over the tree too. A YAML file that yaml-cpp reads but FRT's subset refuses (a duplicate
 * key, an alias) keeps its record with a warning: the runtime still loads it.
 */
class CLegacyMaterialImporter final : public IAssetImporter
{
public:
	const SImporterDesc& GetInfo () const noexcept override
	{
		static const SImporterDesc* const desc = FindImporter("Material.Legacy");
		return *desc;
	}

	bool Analyze (const SAnalyzeInput& In, const SResolvedSettings& /*Settings*/, SAnalyzeResult& Out) const override
	{
		std::optional<std::vector<uint8>> bytes = ReadWholeFileHashed(In, Out, MaxSourceTextBytes);
		if (!bytes)
		{
			return false;
		}
		const std::optional<FString> text = DecodeSourceText(std::move(*bytes), In, Out);
		if (!text)
		{
			return false;
		}

		const int32 version = text::GetTextAssetVersionFromText(*text);
		reflect::CDataTree tree;
		std::span<const FStringLiteral> textureKeys;
		if (version == 2 || version == 3)
		{
			SDiagnostic error;
			reflect::EYamlFailure failure = reflect::EYamlFailure::None;
			if (!reflect::ParseYaml(*text, tree, &error, &failure))
			{
				const bool bSubset = failure == reflect::EYamlFailure::Subset;
				SDiagnostic diagnostic = MakeFileDiagnostic(In, bSubset ? ESeverity::Warning : ESeverity::Error, bSubset
					? FString::Format("the material is outside FRT's YAML subset ({}); the runtime still reads it, but its references are not recorded", error.Message)
					: FString::Format("the material is not valid YAML: {}", error.Message));
				diagnostic.Line = error.Line;
				diagnostic.Column = error.Column;
				Out.Diagnostics.push_back(std::move(diagnostic));
				return false;
			}
			textureKeys = YamlTextureKeys;
		}
		else if (version == 1)
		{
			tree = ReadKeyValueTree(*text);
			textureKeys = LineTextureKeys;
		}
		else
		{
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, version == 0
				? FString("the material does not start with a `version:` key, so no reader takes it (versions 1, 2 and 3 have one)")
				: FString::Format("the material's version {} has no reader (versions 1, 2 and 3 have one)", version)));
			return false;
		}

		for (const FStringLiteral key : textureKeys)
		{
			AddTextureReference(In, tree.Find(key), key, Out);
		}
		GatherSchemaFree(tree, In, Out);
		return true;
	}
};
}

const IAssetImporter& GetLegacyMaterialImporter () noexcept
{
	static const CLegacyMaterialImporter importer;
	return importer;
}
}
