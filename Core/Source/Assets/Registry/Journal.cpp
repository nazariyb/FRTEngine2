#include "Assets/Registry/Journal.h"

#include <algorithm>

#include "Assets/RecordFrames.h"
#include "Assets/Registry/RegistryBinary.h"
#include "Platform/Platform.h"
#include "Platform/Timestamp.h"
#include "Profiler/CpuProfiler.h"


namespace frt::assets
{
namespace
{
using binary::AppendLittleEndian;
using binary::AppendText;
using binary::AppendVarint;
using binary::CByteReader;
using namespace registry_binary;

constexpr uint8 KindCommit = 1u;
// A commit frame's payload: Revision (u64 LE), part index and part count (varints), then its share of the
// encoded delta. Parts leave room for that prefix inside a frame.
constexpr size_t CommitPrefixMaxBytes = 8u + 10u + 10u;
constexpr size_t PartBytes = record_frames::MaxPayloadBytes - CommitPrefixMaxBytes;


// ---------------- Delta encoding ----------------

void AppendTagValue (std::vector<uint8>& Out, const STagValue& Value)
{
	AppendEnum(Out, Value.Type);
	AppendVarint(Out, Value.Aux);
	AppendLittleEndian(Out, Value.Bits, 8u);
	AppendText(Out, Value.Text);
}

bool ReadTagValue (CByteReader& Reader, STagValue& Out)
{
	return ReadEnum(Reader, ETagType::RowRef, Out.Type) && ReadU32Varint(Reader, Out.Aux) && ReadU64(Reader, Out.Bits) && Reader.ReadText(Out.Text);
}

void AppendTags (std::vector<uint8>& Out, const std::vector<STagDecl>& Tags)
{
	AppendVarint(Out, Tags.size());
	for (const STagDecl& tag : Tags)
	{
		AppendText(Out, tag.Key);
		AppendTagValue(Out, tag.Value);
	}
}

bool ReadTags (CByteReader& Reader, std::vector<STagDecl>& Out)
{
	uint32 count = 0u;
	if (!ReadCount(Reader, 11u, count))
	{
		return false;
	}
	Out.resize(count);
	for (STagDecl& tag : Out)
	{
		if (!Reader.ReadText(tag.Key) || !ReadTagValue(Reader, tag.Value))
		{
			return false;
		}
	}
	return true;
}

void AppendTexts (std::vector<uint8>& Out, const std::vector<FString>& Texts)
{
	AppendVarint(Out, Texts.size());
	for (const FString& text : Texts)
	{
		AppendText(Out, text);
	}
}

bool ReadTexts (CByteReader& Reader, std::vector<FString>& Out)
{
	uint32 count = 0u;
	if (!ReadCount(Reader, 1u, count))
	{
		return false;
	}
	Out.resize(count);
	for (FString& text : Out)
	{
		if (!Reader.ReadText(text))
		{
			return false;
		}
	}
	return true;
}

void AppendRuleFile (std::vector<uint8>& Out, const SRuleFileUpsert& RuleFile)
{
	AppendText(Out, RuleFile.MountName);
	AppendText(Out, RuleFile.FolderMountPath);
	AppendLittleEndian(Out, RuleFile.RulesHash, 8u);
}

bool ReadRuleFile (CByteReader& Reader, SRuleFileUpsert& Out)
{
	return Reader.ReadText(Out.MountName) && Reader.ReadText(Out.FolderMountPath) && ReadU64(Reader, Out.RulesHash);
}

/**
 * Types are stored by their stable name, never by TypeId (a runtime index that another build numbers
 * differently, AssetTypes.h): each delta carries the names its records and sub-entries use, in first-use order,
 * and they refer to them by index. Decoding maps each name with the snapshot's exact-name lookup.
 */
class CDeltaTypeNames
{
public:
	explicit CDeltaTypeNames (std::span<const SRecordUpsert> Records)
	{
		for (const SRecordUpsert& record : Records)
		{
			Add(record.Type);
			for (const SSubEntryDecl& sub : record.SubEntries)
			{
				Add(sub.Type);
			}
		}
	}

	void Write (std::vector<uint8>& Out) const
	{
		AppendVarint(Out, Types.size());
		for (const reflect::TypeId type : Types)
		{
			AppendText(Out, FStringView(CAssetTypeRegistry::Get(type).Name));
		}
	}

	[[nodiscard]] uint64 IndexOf (reflect::TypeId Type) const noexcept
	{
		return static_cast<uint64>(std::find(Types.begin(), Types.end(), Type) - Types.begin());
	}

private:
	std::vector<reflect::TypeId> Types;

	void Add (reflect::TypeId Type)
	{
		if (std::find(Types.begin(), Types.end(), Type) == Types.end())
		{
			Types.push_back(Type);
		}
	}
};

// A delta's type names as this build knows them: nullopt for a name it does not know.
using TDecodedTypes = std::vector<std::optional<reflect::TypeId>>;

bool ReadTypeNames (CByteReader& Reader, TDecodedTypes& Out)
{
	uint32 count = 0u;
	if (!ReadCount(Reader, 1u, count))
	{
		return false;
	}
	Out.clear();
	for (uint32 i = 0u; i < count; ++i)
	{
		FString name;
		if (!Reader.ReadText(name))
		{
			return false;
		}
		Out.push_back(CAssetTypeRegistry::FindExact(name));
	}
	return true;
}

bool ReadTypeIndex (CByteReader& Reader, const TDecodedTypes& Types, std::optional<reflect::TypeId>& Out)
{
	uint32 index = 0u;
	if (!ReadU32Varint(Reader, index) || index >= Types.size())
	{
		return false;
	}
	Out = Types[index];
	return true;
}

void AppendRecord (std::vector<uint8>& Out, const SRecordUpsert& Record, const CDeltaTypeNames& Types)
{
	AppendText(Out, Record.FileMountPath);
	AppendGuid(Out, Record.Guid);
	AppendVarint(Out, Types.IndexOf(Record.Type));
	AppendText(Out, Record.Importer);
	AppendFlags(Out, Record.Flags);
	AppendEnum(Out, Record.CookRule);
	AppendVarint(Out, Record.Chunk);
	AppendTexts(Out, Record.RuleLabels);
	AppendTexts(Out, Record.AuthoredLabels);
	AppendText(Out, Record.PrimaryName);
	AppendEnum(Out, Record.ImportStatus);
	AppendLittleEndian(Out, Record.AnalyzeStamp, 8u);
	AppendTags(Out, Record.Tags);
	AppendVarint(Out, Record.SubEntries.size());
	for (const SSubEntryDecl& sub : Record.SubEntries)
	{
		AppendText(Out, sub.Label);
		AppendLittleEndian(Out, sub.SubId, 8u);
		AppendVarint(Out, Types.IndexOf(sub.Type));
		AppendFlags(Out, sub.Flags);
		AppendTags(Out, sub.Tags);
	}
	AppendVarint(Out, Record.Edges.size());
	for (const SEdgeDecl& edge : Record.Edges)
	{
		AppendLittleEndian(Out, edge.FromSub, 8u);
		AppendGuid(Out, edge.Target.Guid);
		AppendLittleEndian(Out, edge.Target.SubId, 8u);
		AppendText(Out, edge.TargetPath);
		AppendFlags(Out, edge.Flags);
		AppendText(Out, edge.FieldPath);
	}
	AppendTexts(Out, Record.Companions);
	AppendVarint(Out, Record.Aliases.size());
	for (const SAssetId& alias : Record.Aliases)
	{
		AppendGuid(Out, alias.Guid);
		AppendLittleEndian(Out, alias.SubId, 8u);
	}
	AppendTexts(Out, Record.FormerPaths);
}

/**
 * A record whose type (or a sub-entry whose type) this build does not know becomes frt.Unknown with Schemaless |
 * StaleTags and no tags, AdoptImage's rule for a snapshot's unknown type name: it waits for its type. A
 * sub-entry's unknown type makes its owner StaleTags too.
 */
bool ReadRecord (CByteReader& Reader, const TDecodedTypes& Types, SRecordUpsert& Out)
{
	uint32 subCount = 0u;
	std::optional<reflect::TypeId> type;
	if (!Reader.ReadText(Out.FileMountPath) || !ReadGuid(Reader, Out.Guid) || !ReadTypeIndex(Reader, Types, type) || !Reader.ReadText(Out.Importer)
		|| !ReadFlags(Reader, Out.Flags) || !ReadEnum(Reader, ECookRule::DevelopmentOnly, Out.CookRule) || !ReadU16Varint(Reader, Out.Chunk)
		|| !ReadTexts(Reader, Out.RuleLabels) || !ReadTexts(Reader, Out.AuthoredLabels) || !Reader.ReadText(Out.PrimaryName)
		|| !ReadEnum(Reader, EImportStatus::ImporterUnavailable, Out.ImportStatus) || !ReadU64(Reader, Out.AnalyzeStamp)
		|| !ReadTags(Reader, Out.Tags) || !ReadCount(Reader, 12u, subCount))
	{
		return false;
	}
	Out.Type = type.value_or(asset_types::Unknown);
	if (!type)
	{
		Out.Flags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
		Out.Tags.clear();
	}
	Out.SubEntries.resize(subCount);
	for (SSubEntryDecl& sub : Out.SubEntries)
	{
		std::optional<reflect::TypeId> subType;
		if (!Reader.ReadText(sub.Label) || !ReadU64(Reader, sub.SubId) || !ReadTypeIndex(Reader, Types, subType) || !ReadFlags(Reader, sub.Flags)
			|| !ReadTags(Reader, sub.Tags))
		{
			return false;
		}
		sub.Type = subType.value_or(asset_types::Unknown);
		if (!subType)
		{
			sub.Flags |= ERecordFlags::Schemaless | ERecordFlags::StaleTags;
			sub.Tags.clear();
			Out.Flags |= ERecordFlags::StaleTags;
		}
	}
	uint32 edgeCount = 0u;
	if (!ReadCount(Reader, 34u, edgeCount))
	{
		return false;
	}
	Out.Edges.resize(edgeCount);
	for (SEdgeDecl& edge : Out.Edges)
	{
		if (!ReadU64(Reader, edge.FromSub) || !ReadGuid(Reader, edge.Target.Guid) || !ReadU64(Reader, edge.Target.SubId)
			|| !Reader.ReadText(edge.TargetPath) || !ReadFlags(Reader, edge.Flags) || !Reader.ReadText(edge.FieldPath))
		{
			return false;
		}
	}
	uint32 aliasCount = 0u;
	if (!ReadTexts(Reader, Out.Companions) || !ReadCount(Reader, 24u, aliasCount))
	{
		return false;
	}
	Out.Aliases.resize(aliasCount);
	for (SAssetId& alias : Out.Aliases)
	{
		if (!ReadGuid(Reader, alias.Guid) || !ReadU64(Reader, alias.SubId))
		{
			return false;
		}
	}
	return ReadTexts(Reader, Out.FormerPaths);
}

void AppendGuids (std::vector<uint8>& Out, const std::vector<SGuid>& Guids)
{
	AppendVarint(Out, Guids.size());
	for (const SGuid& guid : Guids)
	{
		AppendGuid(Out, guid);
	}
}

bool ReadGuids (CByteReader& Reader, std::vector<SGuid>& Out)
{
	uint32 count = 0u;
	if (!ReadCount(Reader, 16u, count))
	{
		return false;
	}
	Out.resize(count);
	for (SGuid& guid : Out)
	{
		if (!ReadGuid(Reader, guid))
		{
			return false;
		}
	}
	return true;
}


// ---------------- Frames ----------------

// The frames of one commit: its encoded delta in parts of at most PartBytes.
void AppendCommitFrames (std::vector<uint8>& Out, std::span<const uint8> Encoded, uint64 Revision)
{
	const size_t partCount = Encoded.empty() ? 1u : (Encoded.size() + PartBytes - 1u) / PartBytes;
	std::vector<uint8> payload;
	for (size_t part = 0u; part < partCount; ++part)
	{
		const size_t begin = part * PartBytes;
		const size_t size = (std::min)(PartBytes, Encoded.size() - begin);
		payload.clear();
		AppendLittleEndian(payload, Revision, 8u);
		AppendVarint(payload, part);
		AppendVarint(payload, partCount);
		binary::AppendRaw(payload, Encoded.subspan(begin, size));
		record_frames::AppendFrame(Out, KindCommit, payload);
	}
}

// One commit as read from its frames.
struct SCommitSpan
{
	uint64 Revision = 0u;
	size_t Begin = 0u;           // file offset of its first frame
	size_t End = 0u;             // past its last frame
	std::vector<uint8> Encoded;
};

/**
 * Walks the whole commits from Offset: calls Visit(SCommitSpan&&) for each, stops at the first frame that
 * fails its check, a frame of another kind, a part out of sequence or a commit cut short. Returns the offset
 * past the last whole commit.
 */
template <typename TVisit>
size_t ForEachCommit (std::span<const uint8> Bytes, size_t Offset, bool bKeepPayload, TVisit&& Visit)
{
	size_t validEnd = Offset;
	SCommitSpan commit;
	uint64 expectedParts = 0u;
	uint64 nextPart = 0u;
	while (Offset < Bytes.size())
	{
		const std::optional<record_frames::SFrame> frame = record_frames::ReadFrame(Bytes, Offset);
		if (!frame || frame->Kind != KindCommit)
		{
			break;
		}
		CByteReader reader(frame->Payload);
		uint64 revision = 0u;
		uint64 part = 0u;
		uint64 parts = 0u;
		if (!ReadU64(reader, revision) || !reader.ReadVarint(part) || !reader.ReadVarint(parts) || parts == 0u || part != nextPart
			|| (part > 0u && (parts != expectedParts || revision != commit.Revision)))
		{
			break;
		}
		if (part == 0u)
		{
			commit = SCommitSpan {};
			commit.Revision = revision;
			commit.Begin = Offset;
			expectedParts = parts;
		}
		if (bKeepPayload)
		{
			const std::span<const uint8> share = frame->Payload.subspan(reader.GetPosition());
			commit.Encoded.insert(commit.Encoded.end(), share.begin(), share.end());
		}
		Offset += frame->Size;
		nextPart = part + 1u;
		if (nextPart == expectedParts)
		{
			commit.End = Offset;
			validEnd = Offset;
			nextPart = 0u;
			Visit(std::move(commit));
		}
	}
	return validEnd;
}

std::vector<uint8> MakeHeader (const SGuid& ProjectId, uint64 Base)
{
	std::vector<uint8> header;
	header.reserve(JournalHeaderBytes);
	binary::AppendRaw(header, JournalMagic);
	AppendLittleEndian(header, JournalFormatVersion, 4u);
	AppendLittleEndian(header, 0u, 4u);
	AppendGuid(header, ProjectId);
	AppendLittleEndian(header, Base, 8u);
	return header;
}
}


void EncodeRegistryDelta (const SRegistryDelta& Delta, std::vector<uint8>& Out)
{
	AppendVarint(Out, Delta.Sequence);
	AppendVarint(Out, Delta.Mounts.size());
	for (const SMountUpsert& mount : Delta.Mounts)
	{
		AppendText(Out, mount.Name);
		Out.push_back(mount.bCaseSensitive ? 1u : 0u);
		AppendVarint(Out, mount.Id);
	}
	AppendTexts(Out, Delta.FolderRemoves);
	AppendTexts(Out, Delta.FileRemoves);
	AppendTexts(Out, Delta.RecordRemoves);
	AppendVarint(Out, Delta.RuleFileRemoves.size());
	for (const SRuleFileUpsert& ruleFile : Delta.RuleFileRemoves)
	{
		AppendRuleFile(Out, ruleFile);
	}
	AppendVarint(Out, Delta.ProblemClears.size());
	for (const SProblemsClear& clear : Delta.ProblemClears)
	{
		AppendText(Out, clear.MountPath);
		AppendEnum(Out, clear.Source);
	}
	AppendVarint(Out, Delta.FolderUpserts.size());
	for (const SFolderUpsert& folder : Delta.FolderUpserts)
	{
		AppendText(Out, folder.MountPath);
	}
	AppendVarint(Out, Delta.FileUpserts.size());
	for (const SFileUpsert& file : Delta.FileUpserts)
	{
		AppendText(Out, file.MountPath);
		AppendVarint(Out, file.Size);
		AppendSignedVarint(Out, file.MTime);
		AppendIdentity(Out, file.Identity);
		Out.push_back(file.ContentHash ? 1u : 0u);
		if (file.ContentHash)
		{
			binary::AppendHash(Out, *file.ContentHash);
		}
		AppendEnum(Out, file.Class);
		Out.push_back(file.Meta ? 1u : 0u);
		if (file.Meta)
		{
			AppendEnum(Out, file.Meta->State);
			AppendLittleEndian(Out, file.Meta->Hash, 8u);
		}
	}
	AppendVarint(Out, Delta.RuleFileUpserts.size());
	for (const SRuleFileUpsert& ruleFile : Delta.RuleFileUpserts)
	{
		AppendRuleFile(Out, ruleFile);
	}
	const CDeltaTypeNames types(Delta.RecordUpserts);
	types.Write(Out);
	AppendVarint(Out, Delta.RecordUpserts.size());
	for (const SRecordUpsert& record : Delta.RecordUpserts)
	{
		AppendRecord(Out, record, types);
	}
	AppendVarint(Out, Delta.Problems.size());
	for (const SProblemsSet& problems : Delta.Problems)
	{
		AppendText(Out, problems.Subject);
		AppendEnum(Out, problems.Source);
		AppendDiagnostics(Out, problems.Diagnostics);
	}
	AppendVarint(Out, Delta.MetaCopies.size());
	for (const SMetaCopy& copy : Delta.MetaCopies)
	{
		AppendGuid(Out, copy.Guid);
		AppendLittleEndian(Out, copy.Hash, 8u);
		binary::AppendSized(Out, copy.Bytes);
		AppendText(Out, copy.SourcePath);
	}
	AppendGuids(Out, Delta.MetaCopyRemoves);
	AppendVarint(Out, Delta.Tombstones.size());
	for (const STombstone& tombstone : Delta.Tombstones)
	{
		AppendGuid(Out, tombstone.Guid);
		AppendText(Out, tombstone.LastPath);
		binary::AppendHash(Out, tombstone.Hash);
		AppendIdentity(Out, tombstone.Identity);
		AppendSignedVarint(Out, tombstone.RemovedFileTime);
	}
	AppendGuids(Out, Delta.TombstoneRemoves);
}

std::optional<SRegistryDelta> DecodeRegistryDelta (std::span<const uint8> Bytes)
{
	CByteReader reader(Bytes);
	SRegistryDelta delta;
	uint32 count = 0u;
	if (!reader.ReadVarint(delta.Sequence) || !ReadCount(reader, 3u, count))
	{
		return std::nullopt;
	}
	delta.Mounts.resize(count);
	for (SMountUpsert& mount : delta.Mounts)
	{
		uint8 caseSensitive = 0u;
		if (!reader.ReadText(mount.Name) || !reader.ReadU8(caseSensitive) || caseSensitive > 1u || !ReadU16Varint(reader, mount.Id))
		{
			return std::nullopt;
		}
		mount.bCaseSensitive = caseSensitive != 0u;
	}
	if (!ReadTexts(reader, delta.FolderRemoves) || !ReadTexts(reader, delta.FileRemoves) || !ReadTexts(reader, delta.RecordRemoves)
		|| !ReadCount(reader, 10u, count))
	{
		return std::nullopt;
	}
	delta.RuleFileRemoves.resize(count);
	for (SRuleFileUpsert& ruleFile : delta.RuleFileRemoves)
	{
		if (!ReadRuleFile(reader, ruleFile))
		{
			return std::nullopt;
		}
	}
	if (!ReadCount(reader, 2u, count))
	{
		return std::nullopt;
	}
	delta.ProblemClears.resize(count);
	for (SProblemsClear& clear : delta.ProblemClears)
	{
		if (!reader.ReadText(clear.MountPath) || !ReadEnum(reader, EProblemSource::Lifecycle, clear.Source))
		{
			return std::nullopt;
		}
	}
	if (!ReadCount(reader, 1u, count))
	{
		return std::nullopt;
	}
	delta.FolderUpserts.resize(count);
	for (SFolderUpsert& folder : delta.FolderUpserts)
	{
		if (!reader.ReadText(folder.MountPath))
		{
			return std::nullopt;
		}
	}
	if (!ReadCount(reader, 30u, count))
	{
		return std::nullopt;
	}
	delta.FileUpserts.resize(count);
	for (SFileUpsert& file : delta.FileUpserts)
	{
		uint8 hasHash = 0u;
		if (!reader.ReadText(file.MountPath) || !reader.ReadVarint(file.Size) || !ReadSignedVarint(reader, file.MTime)
			|| !ReadIdentity(reader, file.Identity) || !reader.ReadU8(hasHash) || hasHash > 1u)
		{
			return std::nullopt;
		}
		if (hasHash != 0u)
		{
			hash::SContentHash contentHash;
			if (!reader.ReadHash(contentHash))
			{
				return std::nullopt;
			}
			file.ContentHash = contentHash;
		}
		uint8 hasMeta = 0u;
		if (!ReadEnum(reader, EFileClass::Meta, file.Class) || !reader.ReadU8(hasMeta) || hasMeta > 1u)
		{
			return std::nullopt;
		}
		if (hasMeta != 0u)
		{
			SFileMetaState meta;
			if (!ReadEnum(reader, EMetaState::MissingMeta, meta.State) || !ReadU64(reader, meta.Hash))
			{
				return std::nullopt;
			}
			file.Meta = meta;
		}
	}
	if (!ReadCount(reader, 10u, count))
	{
		return std::nullopt;
	}
	delta.RuleFileUpserts.resize(count);
	for (SRuleFileUpsert& ruleFile : delta.RuleFileUpserts)
	{
		if (!ReadRuleFile(reader, ruleFile))
		{
			return std::nullopt;
		}
	}
	TDecodedTypes types;
	if (!ReadTypeNames(reader, types) || !ReadCount(reader, 37u, count))
	{
		return std::nullopt;
	}
	delta.RecordUpserts.resize(count);
	for (SRecordUpsert& record : delta.RecordUpserts)
	{
		if (!ReadRecord(reader, types, record))
		{
			return std::nullopt;
		}
	}
	if (!ReadCount(reader, 3u, count))
	{
		return std::nullopt;
	}
	delta.Problems.resize(count);
	for (SProblemsSet& problems : delta.Problems)
	{
		if (!reader.ReadText(problems.Subject) || !ReadEnum(reader, EProblemSource::Lifecycle, problems.Source)
			|| !ReadDiagnostics(reader, problems.Diagnostics))
		{
			return std::nullopt;
		}
	}
	if (!ReadCount(reader, 26u, count))
	{
		return std::nullopt;
	}
	delta.MetaCopies.resize(count);
	for (SMetaCopy& copy : delta.MetaCopies)
	{
		std::span<const uint8> bytes;
		if (!ReadGuid(reader, copy.Guid) || !ReadU64(reader, copy.Hash) || !reader.ReadSized(bytes) || !reader.ReadText(copy.SourcePath))
		{
			return std::nullopt;
		}
		copy.Bytes.assign(bytes.begin(), bytes.end());
	}
	if (!ReadGuids(reader, delta.MetaCopyRemoves) || !ReadCount(reader, 62u, count))
	{
		return std::nullopt;
	}
	delta.Tombstones.resize(count);
	for (STombstone& tombstone : delta.Tombstones)
	{
		if (!ReadGuid(reader, tombstone.Guid) || !reader.ReadText(tombstone.LastPath) || !reader.ReadHash(tombstone.Hash)
			|| !ReadIdentity(reader, tombstone.Identity) || !ReadSignedVarint(reader, tombstone.RemovedFileTime))
		{
			return std::nullopt;
		}
	}
	if (!ReadGuids(reader, delta.TombstoneRemoves) || !reader.IsAtEnd())
	{
		return std::nullopt;
	}
	return delta;
}

SRegistryDelta FilterDeltaByMount (const SRegistryDelta& Delta, TFunctionRef<bool (FStringView MountName)> Keep)
{
	SRegistryDelta out;
	out.Sequence = Delta.Sequence;
	const auto keepPath = [&] (FStringView Path) { return Keep(GetMountNameOf(Path)); };
	for (const SMountUpsert& mount : Delta.Mounts)
	{
		if (Keep(mount.Name))
		{
			out.Mounts.push_back(mount);
		}
	}
	const auto copyPaths = [&] (const std::vector<FString>& From, std::vector<FString>& To)
	{
		for (const FString& path : From)
		{
			if (keepPath(path))
			{
				To.push_back(path);
			}
		}
	};
	copyPaths(Delta.FolderRemoves, out.FolderRemoves);
	copyPaths(Delta.FileRemoves, out.FileRemoves);
	copyPaths(Delta.RecordRemoves, out.RecordRemoves);
	for (const SRuleFileUpsert& ruleFile : Delta.RuleFileRemoves)
	{
		if (Keep(ruleFile.MountName))
		{
			out.RuleFileRemoves.push_back(ruleFile);
		}
	}
	for (const SProblemsClear& clear : Delta.ProblemClears)
	{
		if (keepPath(clear.MountPath))
		{
			out.ProblemClears.push_back(clear);
		}
	}
	for (const SFolderUpsert& folder : Delta.FolderUpserts)
	{
		if (keepPath(folder.MountPath))
		{
			out.FolderUpserts.push_back(folder);
		}
	}
	for (const SFileUpsert& file : Delta.FileUpserts)
	{
		if (keepPath(file.MountPath))
		{
			out.FileUpserts.push_back(file);
		}
	}
	for (const SRuleFileUpsert& ruleFile : Delta.RuleFileUpserts)
	{
		if (Keep(ruleFile.MountName))
		{
			out.RuleFileUpserts.push_back(ruleFile);
		}
	}
	for (const SRecordUpsert& record : Delta.RecordUpserts)
	{
		if (keepPath(record.FileMountPath))
		{
			out.RecordUpserts.push_back(record);
		}
	}
	for (const SProblemsSet& problems : Delta.Problems)
	{
		if (keepPath(problems.Subject))
		{
			out.Problems.push_back(problems);
		}
	}
	for (const SMetaCopy& copy : Delta.MetaCopies)
	{
		if (keepPath(copy.SourcePath))
		{
			out.MetaCopies.push_back(copy);
		}
	}
	for (const STombstone& tombstone : Delta.Tombstones)
	{
		if (keepPath(tombstone.LastPath))
		{
			out.Tombstones.push_back(tombstone);
		}
	}
	out.MetaCopyRemoves = Delta.MetaCopyRemoves;
	out.TombstoneRemoves = Delta.TombstoneRemoves;
	return out;
}

SJournalContents ReadJournal (const std::filesystem::path& Path, bool bDecode)
{
	FRT_CPU_SCOPE("Assets.Journal.Read");
	SJournalContents contents;
	std::error_code ec;
	if (!std::filesystem::exists(Path, ec))
	{
		return contents;
	}
	contents.bFound = true;
	FString error;
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path, &error);
	if (!bytes)
	{
		contents.Message = std::move(error);
		return contents;
	}
	contents.FileBytes = bytes->size();
	CByteReader header(*bytes);
	uint64 version = 0u;
	uint64 reserved = 0u;
	if (!header.Expect(JournalMagic) || !header.ReadLittleEndian(version, 4u) || !header.ReadLittleEndian(reserved, 4u)
		|| !ReadGuid(header, contents.ProjectId) || !ReadU64(header, contents.JournalBase))
	{
		contents.Message = "not a registry journal";
		return contents;
	}
	if (version != JournalFormatVersion)
	{
		contents.Message = FString::Format("journal format {} (this build reads {})", version, JournalFormatVersion);
		return contents;
	}
	contents.bHeaderValid = true;
	bool bDecodeFailed = false;
	size_t decodedEnd = JournalHeaderBytes;
	const size_t validEnd = ForEachCommit(*bytes, JournalHeaderBytes, bDecode, [&] (SCommitSpan&& Commit)
	{
		if (bDecodeFailed)
		{
			return;
		}
		if (bDecode)
		{
			std::optional<SRegistryDelta> delta = DecodeRegistryDelta(Commit.Encoded);
			if (!delta)
			{
				bDecodeFailed = true;   // deltas build on each other: nothing after this one is replayed
				return;
			}
			contents.Deltas.push_back(std::move(*delta));
		}
		contents.Revisions.push_back(Commit.Revision);
		decodedEnd = Commit.End;
	});
	contents.ValidBytes = bDecodeFailed ? decodedEnd : validEnd;
	contents.bTornTail = contents.ValidBytes < contents.FileBytes;
	if (bDecodeFailed)
	{
		contents.Message = "a commit's delta does not decode; it and everything after it were dropped";
	}
	else if (contents.bTornTail)
	{
		contents.Message = FString::Format("{} bytes after the last whole commit were dropped (a torn tail)", contents.FileBytes - contents.ValidBytes);
	}
	return contents;
}


// ---------------- CRegistryJournal ----------------

bool CRegistryJournal::Open (const std::filesystem::path& InPath, const SGuid& InProjectId, std::optional<uint64> ContinueFrom,
	std::vector<FString> InMountNames, FString* OutError)
{
	Close();
	Path = InPath;
	ProjectId = InProjectId;
	MountNames = std::move(InMountNames);
	Stats = SJournalStats {};
	++Generation;

	if (ContinueFrom)
	{
		// Decoded, so its valid end is where the load stops too: a commit whose frames pass but whose delta does
		// not decode is cut off with everything after it, or this process's appends would never be replayed.
		const SJournalContents contents = ReadJournal(Path, true);
		if (contents.bHeaderValid && contents.ProjectId == ProjectId && contents.JournalBase == *ContinueFrom)
		{
			JournalBase = *ContinueFrom;
			if (contents.bTornTail)
			{
				// Cut the torn tail off first: a commit appended after garbage would never be read.
				const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path, OutError);
				if (!bytes || bytes->size() < contents.ValidBytes
					|| !platform::WriteFileAtomic(Path, std::span<const uint8>(*bytes).first(static_cast<size_t>(contents.ValidBytes)), true, OutError))
				{
					return false;
				}
			}
			return File.Open(Path, true, OutError);
		}
	}
	return StartNew(ContinueFrom.value_or(0u), {}, OutError);
}

void CRegistryJournal::Close () noexcept
{
	File.Close();
	std::vector<uint8>().swap(OwnFrames);
	bBroken = false;
	++Generation;
}

bool CRegistryJournal::StartNew (uint64 Base, std::span<const uint8> Frames, FString* OutError)
{
	File.Close();
	std::vector<uint8> bytes = MakeHeader(ProjectId, Base);
	binary::AppendRaw(bytes, Frames);
	// In place and unflushed (WriteFileBytes, not WriteFileAtomic): the journal never survives a power loss
	// anyway (its appends are not flushed either), and a write cut short leaves an empty file, a foreign header
	// or a torn tail - a journal the load ignores or shortens, which costs a rescan, never a wrong load. It keeps
	// Restart, which runs on the owner thread after every full write, free of a flush.
	if (!platform::WriteFileBytes(Path, bytes, true, OutError) || !File.Open(Path, true, OutError))
	{
		File.Close();
		return false;
	}
	JournalBase = Base;
	return true;
}

bool CRegistryJournal::Keeps (FStringView MountName) const noexcept
{
	return MountNames.empty() || std::any_of(MountNames.begin(), MountNames.end(), [&] (const FString& Name) { return Name.View() == MountName; });
}

bool CRegistryJournal::Append (const SRegistryDelta& Delta, uint64 Revision, FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Journal.Append");
	const uint64 start = platform::ReadTimestamp();
	if (!File.IsOpen() && !bBroken)
	{
		if (OutError != nullptr)
		{
			*OutError = "the registry journal is not open";
		}
		return false;
	}
	std::vector<uint8> encoded;
	if (MountNames.empty())
	{
		if (Delta.IsEmpty())
		{
			return true;
		}
		EncodeRegistryDelta(Delta, encoded);
	}
	else
	{
		const SRegistryDelta part = FilterDeltaByMount(Delta, [this] (FStringView MountName) { return Keeps(MountName); });
		if (part.IsEmpty())
		{
			return true;
		}
		EncodeRegistryDelta(part, encoded);
	}
	std::vector<uint8> frames;
	frames.reserve(encoded.size() + 64u);
	AppendCommitFrames(frames, encoded, Revision);
	// Kept in memory whether or not it reaches the file: Restart writes the next journal from these.
	binary::AppendRaw(OwnFrames, frames);
	if (bBroken)
	{
		if (OutError != nullptr)
		{
			*OutError = "the registry journal is broken by an earlier failed append; the commit is kept in memory until the next full write";
		}
		return false;
	}
	bool bWritten = false;
	if (FailAppendAfterBytes < frames.size())
	{
		(void)File.Append(std::span<const uint8>(frames).first(FailAppendAfterBytes), nullptr);
		FailAppendAfterBytes = ~size_t(0);
		if (OutError != nullptr)
		{
			*OutError = "the write was cut short (injected)";
		}
	}
	else
	{
		bWritten = File.Append(frames, OutError);
	}
	if (!bWritten)
	{
		// Part of the frames may be in the file: whatever is appended after them would never be read (the reader
		// stops at the first bad frame), so nothing more is written until a full write starts a new journal.
		File.Close();
		bBroken = true;
		++Stats.FailedAppends;
		return false;
	}
	++Stats.Commits;
	Stats.Bytes += frames.size();
	Stats.LastAppendMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return true;
}

bool CRegistryJournal::Restart (uint64 Base, FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Journal.Restart");
	const uint64 start = platform::ReadTimestamp();
	// This process's commits after the snapshot's view (the worker wrote it while the owner kept committing),
	// from memory: nothing is read back, and a commit that never reached the file (a broken journal) is kept.
	std::vector<uint8> kept;
	ForEachCommit(OwnFrames, 0u, false, [&] (SCommitSpan&& Commit)
	{
		if (Commit.Revision > Base)
		{
			binary::AppendRaw(kept, std::span<const uint8>(OwnFrames).subspan(Commit.Begin, Commit.End - Commit.Begin));
		}
	});
	const uint64 written = platform::ReadTimestamp();
	++Generation;
	if (!StartNew(Base, kept, OutError))
	{
		// The commits stay in memory; the next full write tries again.
		bBroken = true;
		return false;
	}
	OwnFrames = std::move(kept);
	bBroken = false;
	++Stats.Restarts;
	const uint64 end = platform::ReadTimestamp();
	Stats.LastRestartMs = platform::TimestampToMilliseconds(end - start);
	Stats.LastRestartFileMs = platform::TimestampToMilliseconds(end - written);
	Stats.LastRestartWorkerMs = 0.0;
	return true;
}

CJournalRestart CRegistryJournal::BeginRestart (uint64 Base)
{
	const uint64 start = platform::ReadTimestamp();
	CJournalRestart restart;
	restart.Path = Path;
	restart.ProjectId = ProjectId;
	restart.Base = Base;
	restart.Generation = Generation;
	restart.CoveredBytes = OwnFrames.size();
	ForEachCommit(OwnFrames, 0u, false, [&] (SCommitSpan&& Commit)
	{
		if (Commit.Revision > Base)
		{
			binary::AppendRaw(restart.Frames, std::span<const uint8>(OwnFrames).subspan(Commit.Begin, Commit.End - Commit.Begin));
		}
	});
	BeginRestartMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return restart;
}

bool CJournalRestart::Prepare (FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Journal.PrepareRestart");
	const uint64 start = platform::ReadTimestamp();
	bPrepared = false;
	std::filesystem::path side = Path;
	side += ".next";
	std::vector<uint8> bytes = MakeHeader(ProjectId, Base);
	binary::AppendRaw(bytes, Frames);
	// Written unflushed beside the journal (StartNew's reasoning), then renamed over it: the owner keeps
	// appending to the journal it has open until FinishRestart, and those appends land in the old file, which
	// the rename (POSIX semantics: the old handle shares delete) leaves to that handle.
	if (!platform::WriteFileBytes(side, bytes, true, OutError) || !platform::ReplaceFileAtomic(side, Path, OutError) || !File.Open(Path, true, OutError))
	{
		File.Close();
		(void)platform::RemoveFile(side);
		PrepareMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		return false;
	}
	bPrepared = true;
	PrepareMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return true;
}

bool CRegistryJournal::FinishRestart (CJournalRestart&& Prepared, FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Journal.FinishRestart");
	const uint64 start = platform::ReadTimestamp();
	if (Prepared.Generation != Generation || Prepared.Path != Path)
	{
		if (OutError != nullptr)
		{
			*OutError = "the journal was reopened or restarted since this restart began";
		}
		return false;
	}
	if (!Prepared.bPrepared)
	{
		const double workerMs = Prepared.PrepareMs;
		const bool bRestarted = Restart(Prepared.Base, OutError);
		Stats.LastRestartWorkerMs = workerMs;
		return bRestarted;
	}
	// The commits after the view, from memory; those made since BeginRestart went into the replaced file.
	std::vector<uint8> kept;
	ForEachCommit(OwnFrames, 0u, false, [&] (SCommitSpan&& Commit)
	{
		if (Commit.Revision > Prepared.Base)
		{
			binary::AppendRaw(kept, std::span<const uint8>(OwnFrames).subspan(Commit.Begin, Commit.End - Commit.Begin));
		}
	});
	const std::span<const uint8> later = std::span<const uint8>(OwnFrames).subspan((std::min)(Prepared.CoveredBytes, OwnFrames.size()));
	const uint64 fileStart = platform::ReadTimestamp();
	++Generation;
	File = std::move(Prepared.File);
	JournalBase = Prepared.Base;
	if (!later.empty() && !File.Append(later, OutError))
	{
		// Part of them may be in the file: as a failed Append, nothing more is written until the next restart.
		File.Close();
		OwnFrames = std::move(kept);
		bBroken = true;
		++Stats.FailedAppends;
		return false;
	}
	const uint64 end = platform::ReadTimestamp();
	OwnFrames = std::move(kept);
	bBroken = false;
	++Stats.Restarts;
	Stats.LastRestartMs = BeginRestartMs + platform::TimestampToMilliseconds(end - start);
	Stats.LastRestartFileMs = platform::TimestampToMilliseconds(end - fileStart);
	Stats.LastRestartWorkerMs = Prepared.PrepareMs;
	return true;
}
}
