#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <span>
#include <unordered_map>
#include <vector>

#include <cgltf.h>

#include "Assets/Pipeline/GltfParse.h"
#include "Containers/StringMap.h"
#include "Graphics/AssimpWideIO.h"
#include "Platform/FileSystem.h"


namespace frt::assets::importers
{
namespace
{
struct SPrimitiveCounts
{
	int64 Vertices = 0;
	int64 Triangles = 0;
};

// glTF-native counts of one primitive: its POSITION accessor's count, and the triangles its mode and index (or
// vertex) count make. Points and lines make none. Nothing is read from a buffer: the counts are the accessors'.
SPrimitiveCounts CountPrimitive (const cgltf_primitive& Primitive) noexcept
{
	SPrimitiveCounts counts;
	for (cgltf_size i = 0u; i < Primitive.attributes_count; ++i)
	{
		const cgltf_attribute& attribute = Primitive.attributes[i];
		if (attribute.type == cgltf_attribute_type_position && attribute.index == 0 && attribute.data != nullptr)
		{
			counts.Vertices = static_cast<int64>(attribute.data->count);
			break;
		}
	}
	const int64 elements = Primitive.indices != nullptr ? static_cast<int64>(Primitive.indices->count) : counts.Vertices;
	switch (Primitive.type)
	{
	case cgltf_primitive_type_triangles:      counts.Triangles = elements / 3; break;
	case cgltf_primitive_type_triangle_strip:
	case cgltf_primitive_type_triangle_fan:   counts.Triangles = elements >= 3 ? elements - 2 : 0; break;
	default:                                  break;
	}
	return counts;
}

// cgltf keeps JSON strings as written: unescape a name or URI in place ("é", "\/") before using it.
FStringView DecodeJsonString (char* Text) noexcept
{
	if (Text == nullptr)
	{
		return FStringView();
	}
	const cgltf_size length = cgltf_decode_string(Text);
	return FStringView(Text, static_cast<uint32>(length));
}

/**
 * The labels of one file's imported objects (§3.2, §4.3): "kind:<name>", "kind:#<index>" for an object without a
 * name, and "#2", "#3" appended to the second and third use of one label, in document order. SubIds are
 * MakeSubId of the label; two labels of one file with the same SubId are an import error, and the later object
 * gets no sub-entry.
 */
class CLabelMaker
{
public:
	CLabelMaker (const SAnalyzeInput& InInput, SAnalyzeResult& InResult)
		: In(InInput)
		, Out(InResult)
	{}

	// The new sub-entry, or nullptr when its SubId collides.
	SSubEntryDecl* Add (FStringView Kind, FStringView Name, cgltf_size Index, reflect::TypeId Type)
	{
		FString label(Kind);
		label += ':';
		if (Name.IsEmpty())
		{
			label.AppendFormat("#{}", Index);
		}
		else
		{
			label += Name;
		}
		const uint32 uses = ++Uses[label];
		if (uses > 1u)
		{
			label.AppendFormat("#{}", uses);
		}

		const uint64 subId = MakeSubId(label);
		const auto [existing, bInserted] = Labels.try_emplace(subId, label);
		if (!bInserted)
		{
			SDiagnostic diagnostic = MakeFileDiagnostic(In, ESeverity::Error,
				FString::Format("the {} '{}' (index {}) has the sub-asset id {:016x} of '{}': it gets no sub-entry (rename one of them)",
					Kind, label, Index, subId, existing->second));
			diagnostic.Subject = FString::Format("{}#{}", In.FileMountPath, label);
			Out.Diagnostics.push_back(std::move(diagnostic));
			return nullptr;
		}
		SSubEntryDecl& sub = Out.SubEntries.emplace_back();
		sub.Label = std::move(label);
		sub.SubId = subId;
		sub.Type = Type;
		return &sub;
	}

private:
	const SAnalyzeInput& In;
	SAnalyzeResult& Out;
	TStringMap<uint32> Uses;
	std::unordered_map<uint64, FString> Labels;
};

bool FileExists (const std::filesystem::path& Path)
{
	const std::optional<platform::SFileStat> stat = platform::Stat(Path);
	return stat.has_value() && !stat->bDirectory;
}

/**
 * The mount path a buffer or image URI names, resolved as CAssimpWideIOSystem opens it (Translate reads the same
 * file): the name as written when it names a file beside the model, else its URI escapes decoded when that names
 * one ("Brick%20Wall.png"). When neither exists the decoded form is recorded (a glTF URI is escaped by
 * definition), so the edge waits for the file the exporter meant. `data:` URIs are embedded bytes, no file;
 * anything else that is not a path relative to the model's folder is a warning and no edge.
 */
std::optional<FString> ResolveUri (const SAnalyzeInput& In, FStringView Uri, FStringView FieldPath, SAnalyzeResult& Out)
{
	if (Uri.StartsWith<ESearchCase::IgnoreCaseAscii>("data:"))
	{
		return std::nullopt;
	}
	const auto warn = [&] (FStringView Reference, EReferenceProblem Problem)
	{
		SDiagnostic diagnostic = MakeFileDiagnostic(In, ESeverity::Warning,
			FString::Format("the URI '{}' is not recorded: {}", Reference, DescribeReferenceProblem(Problem)));
		diagnostic.FieldPath = FString(FieldPath);
		Out.Diagnostics.push_back(std::move(diagnostic));
	};

	const std::filesystem::path folder = In.Path.parent_path();
	EReferenceProblem problem = EReferenceProblem::None;
	std::optional<FString> asWritten = ResolveRelativeReference(In.FileMountPath, Uri, &problem);
	if (asWritten && FileExists(folder / Uri.ToPath()))
	{
		return asWritten;
	}
	const std::optional<FString> decoded = graphics::DecodeUriEscapes(Uri);
	if (!decoded)
	{
		if (!asWritten)
		{
			warn(Uri, problem);
		}
		return asWritten;
	}
	std::optional<FString> decodedPath = ResolveRelativeReference(In.FileMountPath, *decoded, &problem);
	if (!decodedPath)
	{
		if (asWritten)
		{
			return asWritten;
		}
		warn(Uri, problem);
		return std::nullopt;
	}
	return decodedPath;
}

void AddPathEdge (SAnalyzeResult& Out, uint64 FromSub, FString TargetPath, FString FieldPath)
{
	SEdgeDecl& edge = Out.Edges.emplace_back();
	edge.FromSub = FromSub;
	edge.TargetPath = std::move(TargetPath);
	edge.Flags = EDepFlags::Build | EDepFlags::Path | EDepFlags::Declared;
	edge.FieldPath = std::move(FieldPath);
}

/**
 * Model.Gltf (§7.11, Analyze in M3): the JSON only, through vendored cgltf in parse-only mode - a .gltf whole
 * (hashed in the same read), a .glb's JSON chunk alone. glTF-native facts, never Assimp's post-processed ones
 * (no extra default material, no split meshes): sub-entries mesh: (frt.Mesh, with VertexCount and TriangleCount),
 * material: (frt.Material) and image: (frt.Texture); on the record MeshCount, MaterialCount, ImageCount and the
 * vertex and triangle counts summed over every mesh's primitives. External buffer and image URIs are
 * Build | Path | Declared edges (an image's from its sub-entry); buffer files are also Companions. The
 * `<stem>_mat<N>.frtmat.yml` files beside a model follow Assimp's material index and are not linked here.
 */
class CGltfImporter final : public IAssetImporter
{
public:
	const SImporterDesc& GetInfo () const noexcept override
	{
		static const SImporterDesc* const desc = FindImporter("Model.Gltf");
		return *desc;
	}

	bool Analyze (const SAnalyzeInput& In, const SResolvedSettings& /*Settings*/, SAnalyzeResult& Out) const override
	{
		std::vector<uint8> json;
		FString error;
		if (GetMountPathLeaf(In.FileMountPath).EndsWith<ESearchCase::IgnoreCase>(".glb"))
		{
			std::optional<std::vector<uint8>> chunk = ReadGlbJsonChunk(In.Path, &error);
			if (!chunk)
			{
				Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cannot read the GLB's JSON chunk: {}", error)));
				return false;
			}
			json = std::move(*chunk);
		}
		else
		{
			std::optional<std::vector<uint8>> bytes = ReadWholeFileHashed(In, Out);
			if (!bytes)
			{
				return false;
			}
			json = std::move(*bytes);
			// A binary glTF under another extension: take its JSON chunk from what was read.
			if (json.size() >= GlbHeaderBytes && json[0] == 'g' && json[1] == 'l' && json[2] == 'T' && json[3] == 'F')
			{
				const std::optional<SGlbJsonChunk> chunk = ParseGlbHeader(json, json.size(), &error);
				if (!chunk || chunk->Offset + chunk->Length > json.size())
				{
					Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("not a valid GLB file: {}", error)));
					return false;
				}
				json = std::vector<uint8>(json.begin() + static_cast<ptrdiff_t>(chunk->Offset),
					json.begin() + static_cast<ptrdiff_t>(chunk->Offset + chunk->Length));
			}
		}

		const TGltfDataPtr document = ParseGltfJson(json, &error);
		if (!document)
		{
			Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cgltf cannot parse the document: {}", error)));
			return false;
		}
		cgltf_data& data = *document;
		CLabelMaker labels(In, Out);

		int64 vertices = 0;
		int64 triangles = 0;
		for (cgltf_size i = 0u; i < data.meshes_count; ++i)
		{
			const cgltf_mesh& mesh = data.meshes[i];
			SPrimitiveCounts meshCounts;
			for (cgltf_size p = 0u; p < mesh.primitives_count; ++p)
			{
				const SPrimitiveCounts counts = CountPrimitive(mesh.primitives[p]);
				meshCounts.Vertices += counts.Vertices;
				meshCounts.Triangles += counts.Triangles;
			}
			vertices += meshCounts.Vertices;
			triangles += meshCounts.Triangles;
			if (SSubEntryDecl* sub = labels.Add("mesh", DecodeJsonString(data.meshes[i].name), i, asset_types::Mesh))
			{
				sub->Tags.push_back({ "VertexCount", STagValue::FromInt64(meshCounts.Vertices) });
				sub->Tags.push_back({ "TriangleCount", STagValue::FromInt64(meshCounts.Triangles) });
			}
		}
		for (cgltf_size i = 0u; i < data.materials_count; ++i)
		{
			(void)labels.Add("material", DecodeJsonString(data.materials[i].name), i, asset_types::Material);
		}
		for (cgltf_size i = 0u; i < data.images_count; ++i)
		{
			cgltf_image& image = data.images[i];
			const SSubEntryDecl* sub = labels.Add("image", DecodeJsonString(image.name), i, asset_types::Texture);
			if (image.uri == nullptr || image.buffer_view != nullptr)
			{
				continue;   // embedded in a buffer
			}
			const FString fieldPath = FString::Format("images.{}.uri", i);
			if (std::optional<FString> target = ResolveUri(In, DecodeJsonString(image.uri), fieldPath, Out))
			{
				AddPathEdge(Out, sub != nullptr ? sub->SubId : 0u, std::move(*target), fieldPath);
			}
		}
		for (cgltf_size i = 0u; i < data.buffers_count; ++i)
		{
			cgltf_buffer& buffer = data.buffers[i];
			if (buffer.uri == nullptr)
			{
				continue;   // a GLB's BIN chunk
			}
			const FString fieldPath = FString::Format("buffers.{}.uri", i);
			if (std::optional<FString> target = ResolveUri(In, DecodeJsonString(buffer.uri), fieldPath, Out))
			{
				bool bKnown = false;
				for (const FString& companion : Out.Companions)
				{
					bKnown = bKnown || companion == *target;
				}
				if (!bKnown)
				{
					Out.Companions.push_back(*target);
				}
				AddPathEdge(Out, 0u, std::move(*target), fieldPath);
			}
		}

		Out.Tags.push_back({ "MeshCount", STagValue::FromInt64(static_cast<int64>(data.meshes_count)) });
		Out.Tags.push_back({ "MaterialCount", STagValue::FromInt64(static_cast<int64>(data.materials_count)) });
		Out.Tags.push_back({ "ImageCount", STagValue::FromInt64(static_cast<int64>(data.images_count)) });
		Out.Tags.push_back({ "VertexCount", STagValue::FromInt64(vertices) });
		Out.Tags.push_back({ "TriangleCount", STagValue::FromInt64(triangles) });
		return true;
	}
};
}

const IAssetImporter& GetGltfImporter () noexcept
{
	static const CGltfImporter importer;
	return importer;
}
}
