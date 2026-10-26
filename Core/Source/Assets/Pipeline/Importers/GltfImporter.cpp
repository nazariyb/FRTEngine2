#include "Assets/Pipeline/Importers/ImporterUtils.h"

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cgltf.h>

#include "Assets/Pipeline/GltfParse.h"
#include "Containers/StringMap.h"
#include "Graphics/AssimpWideIO.h"
#include "Platform/FileSystem.h"
#include "Unicode/Unicode.h"


namespace frt::assets::importers
{
namespace
{
struct SPrimitiveCounts
{
	const cgltf_accessor* Positions = nullptr;
	int64 Vertices = 0;
	int64 Triangles = 0;
};

// glTF-native counts of one primitive: its POSITION accessor (and that accessor's count), and the triangles its
// mode and index (or vertex) count make. Points and lines make none. Nothing is read from a buffer: the counts
// are the accessors'.
SPrimitiveCounts CountPrimitive (const cgltf_primitive& Primitive) noexcept
{
	SPrimitiveCounts counts;
	for (cgltf_size i = 0u; i < Primitive.attributes_count; ++i)
	{
		const cgltf_attribute& attribute = Primitive.attributes[i];
		if (attribute.type == cgltf_attribute_type_position && attribute.index == 0 && attribute.data != nullptr)
		{
			counts.Positions = attribute.data;
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

// The value of 4 hex digits at Text, or -1 when one of them is not a hex digit.
int32 ParseHex4 (const char* Text) noexcept
{
	int32 value = 0;
	for (uint32 i = 0u; i < 4u; ++i)
	{
		const char c = Text[i];
		const int32 digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (digit < 0)
		{
			return -1;
		}
		value = value * 16 + digit;
	}
	return value;
}

/**
 * cgltf keeps JSON strings as written: unescape a name or URI in place ("\u00e9", "\/") before using it. Not
 * cgltf_decode_string, which turns every \uXXXX into UTF-8 on its own: an escaped surrogate pair (how Python's
 * json.dumps writes U+1F600) would become CESU-8 - invalid UTF-8 whose label and SubId differ from the same name
 * written unescaped, and a URI that no longer names the file Assimp's JSON reader opens. Here a high surrogate
 * followed by an escaped low one is one 4-byte sequence; a lone surrogate (or a malformed escape) becomes U+FFFD
 * and sets bOutReplaced. The text only shrinks, so the decoding is in place.
 */
FStringView DecodeJsonString (char* Text, bool& bOutReplaced) noexcept
{
	bOutReplaced = false;
	if (Text == nullptr)
	{
		return FStringView();
	}
	const char* read = Text;
	char* write = Text;
	while (*read != 0)
	{
		if (*read != '\\')
		{
			*write++ = *read++;
			continue;
		}
		const char escape = read[1];
		if (escape == 0)
		{
			break;
		}
		read += 2;
		switch (escape)
		{
		case 'b': *write++ = '\b'; break;
		case 'f': *write++ = '\f'; break;
		case 'r': *write++ = '\r'; break;
		case 'n': *write++ = '\n'; break;
		case 't': *write++ = '\t'; break;
		case 'u':
		{
			const int32 unit = ParseHex4(read);
			char32_t codePoint = unicode::ReplacementCharacter;
			if (unit < 0)
			{
				bOutReplaced = true;   // jsmn checks escapes; keep what follows as text
			}
			else if (unit >= 0xD800 && unit <= 0xDBFF)
			{
				read += 4;
				const int32 low = read[0] == '\\' && read[1] == 'u' ? ParseHex4(read + 2) : -1;
				if (low >= 0xDC00 && low <= 0xDFFF)
				{
					codePoint = static_cast<char32_t>(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
					read += 6;
				}
				else
				{
					bOutReplaced = true;
				}
			}
			else
			{
				read += 4;
				if (unit >= 0xDC00 && unit <= 0xDFFF)
				{
					bOutReplaced = true;
				}
				else
				{
					codePoint = static_cast<char32_t>(unit);
				}
			}
			write += unicode::EncodeUtf8(codePoint, write);
			break;
		}
		default: *write++ = escape; break;   // '"', '\\', '/'
		}
	}
	*write = 0;
	return FStringView(Text, static_cast<uint32>(write - Text));
}

// DecodeJsonString with a warning on the file when it read an unpaired surrogate as U+FFFD.
FStringView DecodeJsonString (char* Text, const SAnalyzeInput& In, FStringView FieldPath, SAnalyzeResult& Out)
{
	bool bReplaced = false;
	const FStringView decoded = DecodeJsonString(Text, bReplaced);
	if (bReplaced)
	{
		SDiagnostic diagnostic = MakeFileDiagnostic(In, ESeverity::Warning,
			FString::Format("the string '{}' holds an unpaired UTF-16 surrogate escape: read as U+FFFD", decoded));
		diagnostic.FieldPath = FString(FieldPath);
		Out.Diagnostics.push_back(std::move(diagnostic));
	}
	return decoded;
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
 * vertex and triangle counts - vertices summed over distinct POSITION accessors, triangles over primitives.
 * An external image URI whose file an importer claims is a Build | Path | Declared edge from the image's sub-entry
 * to that file's Texture record. A file with no record of its own - every buffer, and an image no available
 * importer claims (.ktx2, .webp, .dds) - is a Companion only, never an edge: the store gives a companion FILES row
 * no record (§5.2), so a path edge to it would name nothing, and the record names it through Companions. The
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
			// A .gltf is its JSON: capped as a .glb's JSON chunk is (cgltf's token array grows with it).
			std::optional<std::vector<uint8>> bytes = ReadWholeFileHashed(In, Out, MaxGlbJsonChunkBytes);
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

		// Vertices are counted once per distinct POSITION accessor (a vertex buffer drawn by several primitives,
		// or shared by two meshes, is one set of vertices); triangles per primitive, as they are drawn.
		int64 vertices = 0;
		int64 triangles = 0;
		std::unordered_set<const cgltf_accessor*> fileAccessors;
		std::unordered_set<const cgltf_accessor*> meshAccessors;
		for (cgltf_size i = 0u; i < data.meshes_count; ++i)
		{
			const cgltf_mesh& mesh = data.meshes[i];
			SPrimitiveCounts meshCounts;
			meshAccessors.clear();
			for (cgltf_size p = 0u; p < mesh.primitives_count; ++p)
			{
				const SPrimitiveCounts counts = CountPrimitive(mesh.primitives[p]);
				if (counts.Positions != nullptr && meshAccessors.insert(counts.Positions).second)
				{
					meshCounts.Vertices += counts.Vertices;
				}
				if (counts.Positions != nullptr && fileAccessors.insert(counts.Positions).second)
				{
					vertices += counts.Vertices;
				}
				meshCounts.Triangles += counts.Triangles;
			}
			triangles += meshCounts.Triangles;
			const FStringView name = DecodeJsonString(data.meshes[i].name, In, FString::Format("meshes.{}.name", i), Out);
			if (SSubEntryDecl* sub = labels.Add("mesh", name, i, asset_types::Mesh))
			{
				sub->Tags.push_back({ "VertexCount", STagValue::FromInt64(meshCounts.Vertices) });
				sub->Tags.push_back({ "TriangleCount", STagValue::FromInt64(meshCounts.Triangles) });
			}
		}
		for (cgltf_size i = 0u; i < data.materials_count; ++i)
		{
			const FStringView name = DecodeJsonString(data.materials[i].name, In, FString::Format("materials.{}.name", i), Out);
			(void)labels.Add("material", name, i, asset_types::Material);
		}
		const auto addCompanion = [&] (const FString& Target)
		{
			if (std::find(Out.Companions.begin(), Out.Companions.end(), Target) == Out.Companions.end())
			{
				Out.Companions.push_back(Target);
			}
		};
		for (cgltf_size i = 0u; i < data.images_count; ++i)
		{
			cgltf_image& image = data.images[i];
			const FStringView name = DecodeJsonString(image.name, In, FString::Format("images.{}.name", i), Out);
			const SSubEntryDecl* sub = labels.Add("image", name, i, asset_types::Texture);
			if (image.uri == nullptr || image.buffer_view != nullptr)
			{
				continue;   // embedded in a buffer
			}
			const FString fieldPath = FString::Format("images.{}.uri", i);
			if (std::optional<FString> target = ResolveUri(In, DecodeJsonString(image.uri, In, fieldPath, Out), fieldPath, Out))
			{
				// An image no available importer claims (.ktx2, .webp, .dds through extensions) has no record of
				// its own: it is the model's companion, like a buffer, rather than an Unclaimed file.
				if (FindImporterForFile(GetMountPathLeaf(*target)) == nullptr)
				{
					addCompanion(*target);
				}
				else
				{
					AddPathEdge(Out, sub != nullptr ? sub->SubId : 0u, std::move(*target), fieldPath);
				}
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
			if (std::optional<FString> target = ResolveUri(In, DecodeJsonString(buffer.uri, In, fieldPath, Out), fieldPath, Out))
			{
				addCompanion(*target);
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
