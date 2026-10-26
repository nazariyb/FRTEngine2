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

void AppendRecord (std::vector<uint8>& Out, const SRecordUpsert& Record)
{
	AppendText(Out, Record.FileMountPath);
	AppendGuid(Out, Record.Guid);
	AppendVarint(Out, Record.Type);
	AppendText(Out, Record.Importer);
	AppendFlags(Out, Record.Flags);
	AppendEnum(Out, Record.CookRule);
	AppendVarint(Out, Record.Chunk);
	AppendTexts(Out, Record.RuleLabels);
	AppendText(Out, Record.PrimaryName);
	AppendEnum(Out, Record.ImportStatus);
	AppendLittleEndian(Out, Record.AnalyzeStamp, 8u);
	AppendTags(Out, Record.Tags);
	AppendVarint(Out, Record.SubEntries.size());
	for (const SSubEntryDecl& sub : Record.SubEntries)
	{
		AppendText(Out, sub.Label);
		AppendLittleEndian(Out, sub.SubId, 8u);
		AppendVarint(Out, sub.Type);
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
}

bool ReadRecord (CByteReader& Reader, SRecordUpsert& Out)
{
	uint32 subCount = 0u;
	if (!Reader.ReadText(Out.FileMountPath) || !ReadGuid(Reader, Out.Guid) || !ReadU16Varint(Reader, Out.Type) || !Reader.ReadText(Out.Importer)
		|| !ReadFlags(Reader, Out.Flags) || !ReadEnum(Reader, ECookRule::DevelopmentOnly, Out.CookRule) || !ReadU16Varint(Reader, Out.Chunk)
		|| !ReadTexts(Reader, Out.RuleLabels) || !Reader.ReadText(Out.PrimaryName)
		|| !ReadEnum(Reader, EImportStatus::ImporterUnavailable, Out.ImportStatus) || !ReadU64(Reader, Out.AnalyzeStamp)
		|| !ReadTags(Reader, Out.Tags) || !ReadCount(Reader, 12u, subCount))
	{
		return false;
	}
	Out.SubEntries.resize(subCount);
	for (SSubEntryDecl& sub : Out.SubEntries)
	{
		if (!Reader.ReadText(sub.Label) || !ReadU64(Reader, sub.SubId) || !ReadU16Varint(Reader, sub.Type) || !ReadFlags(Reader, sub.Flags)
			|| !ReadTags(Reader, sub.Tags))
		{
			return false;
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
	return ReadTexts(Reader, Out.Companions);
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
		AppendEnum(Out, file.MetaState);
		AppendLittleEndian(Out, file.MetaHash, 8u);
	}
	AppendVarint(Out, Delta.RuleFileUpserts.size());
	for (const SRuleFileUpsert& ruleFile : Delta.RuleFileUpserts)
	{
		AppendRuleFile(Out, ruleFile);
	}
	AppendVarint(Out, Delta.RecordUpserts.size());
	for (const SRecordUpsert& record : Delta.RecordUpserts)
	{
		AppendRecord(Out, record);
	}
	AppendVarint(Out, Delta.Problems.size());
	for (const SProblemsSet& problems : Delta.Problems)
	{
		AppendText(Out, problems.Subject);
		AppendEnum(Out, problems.Source);
		AppendDiagnostics(Out, problems.Diagnostics);
	}
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
		if (!reader.ReadText(clear.MountPath) || !ReadEnum(reader, EProblemSource::Registry, clear.Source))
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
	if (!ReadCount(reader, 38u, count))
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
		if (!ReadEnum(reader, EFileClass::Excluded, file.Class) || !ReadEnum(reader, EMetaState::MissingMeta, file.MetaState)
			|| !ReadU64(reader, file.MetaHash))
		{
			return std::nullopt;
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
	if (!ReadCount(reader, 37u, count))
	{
		return std::nullopt;
	}
	delta.RecordUpserts.resize(count);
	for (SRecordUpsert& record : delta.RecordUpserts)
	{
		if (!ReadRecord(reader, record))
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
		if (!reader.ReadText(problems.Subject) || !ReadEnum(reader, EProblemSource::Registry, problems.Source)
			|| !ReadDiagnostics(reader, problems.Diagnostics))
		{
			return std::nullopt;
		}
	}
	if (!reader.IsAtEnd())
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

	if (ContinueFrom)
	{
		const SJournalContents contents = ReadJournal(Path, false);
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
			if (!File.Open(Path, true, OutError))
			{
				return false;
			}
			AppendStart = contents.ValidBytes;
			return true;
		}
	}
	return StartNew(ContinueFrom.value_or(0u), {}, OutError);
}

void CRegistryJournal::Close () noexcept
{
	File.Close();
}

bool CRegistryJournal::StartNew (uint64 Base, std::span<const uint8> Frames, FString* OutError)
{
	File.Close();
	std::vector<uint8> bytes = MakeHeader(ProjectId, Base);
	binary::AppendRaw(bytes, Frames);
	if (!platform::WriteFileAtomic(Path, bytes, true, OutError) || !File.Open(Path, true, OutError))
	{
		return false;
	}
	JournalBase = Base;
	AppendStart = JournalHeaderBytes;
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
	if (!File.IsOpen())
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
	if (!File.Append(frames, OutError))
	{
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
	File.Close();
	// This process's commits after the snapshot's view: the worker wrote it while the owner kept committing.
	std::vector<uint8> kept;
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path);
	if (bytes && bytes->size() >= AppendStart)
	{
		const std::span<const uint8> all(*bytes);
		ForEachCommit(all, static_cast<size_t>(AppendStart), false, [&] (SCommitSpan&& Commit)
		{
			if (Commit.Revision > Base)
			{
				binary::AppendRaw(kept, all.subspan(Commit.Begin, Commit.End - Commit.Begin));
			}
		});
	}
	if (!StartNew(Base, kept, OutError))
	{
		return false;
	}
	++Stats.Restarts;
	return true;
}
}
