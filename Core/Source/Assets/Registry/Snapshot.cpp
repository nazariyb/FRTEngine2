#include "Assets/Registry/Snapshot.h"

#include <algorithm>
#include <array>
#include <system_error>

#include "Assets/AssetTypes.h"
#include "Assets/MountPaths.h"
#include "Assets/Registry/Journal.h"
#include "Assets/Registry/RegistryBinary.h"
#include "Assets/Registry/RegistryStore.h"
#include "Assets/Registry/RegistryView.h"
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

// Every section is at layout 1 in M3; a section's layout changes alone (its readers drop it, nothing else).
constexpr uint32 SectionLayoutVersion = 1u;

constexpr std::array<ESnapshotSection, 17u> SectionOrder = {
	ESnapshotSection::Mounts, ESnapshotSection::Strings, ESnapshotSection::Names, ESnapshotSection::Folders, ESnapshotSection::Files,
	ESnapshotSection::RuleFiles, ESnapshotSection::Assets, ESnapshotSection::SubEntry, ESnapshotSection::Labels, ESnapshotSection::Types,
	ESnapshotSection::TypeTables, ESnapshotSection::Deps, ESnapshotSection::Aliases, ESnapshotSection::FormerPaths, ESnapshotSection::Metas,
	ESnapshotSection::Tombstones, ESnapshotSection::Problems };

constexpr size_t SectionSlot (ESnapshotSection Section) noexcept { return static_cast<size_t>(Section) - 1u; }

[[nodiscard]] size_t AlignUp16 (size_t Value) noexcept { return (Value + 15u) & ~size_t(15u); }

bool HasKeys (FStringView TypeName) noexcept
{
	const std::optional<reflect::TypeId> type = FindTypeExact(TypeName);
	return type && !CAssetTypeRegistry::Get(*type).Keys.empty();
}

// Tag values per key type: numbers and references as 8 bytes, bools and enums as varints, Name and String
// as indices into NAMES / STRINGS.
enum class EValueForm : uint8 { Fixed, Varint, Name, String };

EValueForm GetValueForm (ETagType Type) noexcept
{
	switch (Type)
	{
	case ETagType::Bool:
	case ETagType::Enum:
		return EValueForm::Varint;
	case ETagType::Name:
		return EValueForm::Name;
	case ETagType::String:
		return EValueForm::String;
	default:
		return EValueForm::Fixed;
	}
}


// ---------------- Encoding ----------------

void AppendTextTable (std::vector<uint8>& Out, const std::vector<FString>& Texts)
{
	AppendVarint(Out, Texts.size());
	for (const FString& text : Texts)
	{
		AppendText(Out, text);
	}
}

std::vector<uint8> EncodeSection (const SRegistryImage& Image, ESnapshotSection Section)
{
	std::vector<uint8> out;
	switch (Section)
	{
	case ESnapshotSection::Mounts:
		AppendVarint(out, Image.Mounts.size());
		for (const SImageMount& mount : Image.Mounts)
		{
			AppendText(out, mount.Name);
			AppendVarint(out, mount.Id);
			out.push_back(mount.bCaseSensitive ? 1u : 0u);
		}
		break;
	case ESnapshotSection::Strings:
		AppendTextTable(out, Image.Strings);
		break;
	case ESnapshotSection::Names:
		AppendTextTable(out, Image.Names);
		break;
	case ESnapshotSection::Folders:
		AppendVarint(out, Image.Folders.size());
		for (const SImageFolder& folder : Image.Folders)
		{
			AppendVarint(out, folder.Mount);
			AppendOptionalIndex(out, folder.Parent);
			AppendVarint(out, folder.Name);
		}
		break;
	case ESnapshotSection::Files:
		AppendVarint(out, Image.Files.size());
		for (const SImageFile& file : Image.Files)
		{
			AppendVarint(out, file.Folder);
			AppendVarint(out, file.LeafName);
			AppendVarint(out, file.Size);
			AppendSignedVarint(out, file.MTime);
			AppendIdentity(out, file.Identity);
			binary::AppendHash(out, file.ContentHash);
			AppendLittleEndian(out, file.MetaHash, 8u);
			AppendEnum(out, file.Class);
			AppendEnum(out, file.MetaState);
		}
		break;
	case ESnapshotSection::RuleFiles:
		AppendVarint(out, Image.RuleFiles.size());
		for (const SImageRuleFile& ruleFile : Image.RuleFiles)
		{
			AppendVarint(out, ruleFile.Mount);
			AppendOptionalIndex(out, ruleFile.Folder);
			AppendLittleEndian(out, ruleFile.RulesHash, 8u);
		}
		break;
	case ESnapshotSection::Assets:
		// The importer-name table first (only ASSETS refers to it), then the records. A record's labels,
		// sub-entries, edges and companions follow its predecessor's in their sections: counts only.
		AppendTextTable(out, Image.ImporterNames);
		AppendVarint(out, Image.Records.size());
		for (const SImageRecord& record : Image.Records)
		{
			AppendVarint(out, record.File);
			AppendGuid(out, record.Guid);
			AppendVarint(out, record.Type);
			AppendOptionalIndex(out, record.Importer == 0xFFFFu ? ~0u : record.Importer);
			AppendFlags(out, record.Flags);
			AppendEnum(out, record.CookRule);
			AppendVarint(out, record.Chunk);
			AppendOptionalIndex(out, record.PrimaryName);
			AppendEnum(out, record.ImportStatus);
			AppendLittleEndian(out, record.AnalyzeStamp, 8u);
			AppendVarint(out, record.Labels.Count);
			AppendVarint(out, record.SubEntries.Count);
			AppendVarint(out, record.Edges.Count);
			AppendVarint(out, record.Companions.Count);
		}
		break;
	case ESnapshotSection::SubEntry:
		AppendVarint(out, Image.SubEntries.size());
		for (const SImageSubEntry& sub : Image.SubEntries)
		{
			AppendVarint(out, sub.Label);
			AppendLittleEndian(out, sub.SubId, 8u);
			AppendVarint(out, sub.Type);
			AppendFlags(out, sub.Flags);
		}
		break;
	case ESnapshotSection::Labels:
		AppendVarint(out, Image.Labels.size());
		for (const uint32 label : Image.Labels)
		{
			AppendVarint(out, (static_cast<uint64>(label & ~RuleLabelBit) << 1u) | ((label & RuleLabelBit) != 0u ? 1u : 0u));
		}
		break;
	case ESnapshotSection::Types:
		AppendTextTable(out, Image.TypeNames);
		break;
	case ESnapshotSection::Deps:
		AppendVarint(out, Image.Edges.size());
		for (const SImageEdge& edge : Image.Edges)
		{
			AppendLittleEndian(out, edge.FromSub, 8u);
			out.push_back(edge.Target.IsValid() ? 1u : 0u);
			if (edge.Target.IsValid())
			{
				AppendGuid(out, edge.Target.Guid);
				AppendLittleEndian(out, edge.Target.SubId, 8u);
			}
			AppendOptionalIndex(out, edge.TargetPath);
			AppendFlags(out, edge.Flags);
			AppendOptionalIndex(out, edge.FieldPath);
		}
		AppendVarint(out, Image.Companions.size());
		for (const uint32 companion : Image.Companions)
		{
			AppendVarint(out, companion);
		}
		break;
	case ESnapshotSection::Problems:
		AppendVarint(out, Image.Problems.size());
		for (const SImageProblems& problems : Image.Problems)
		{
			AppendVarint(out, problems.Subject);
			AppendEnum(out, problems.Source);
			AppendDiagnostics(out, problems.Diagnostics);
		}
		break;
	case ESnapshotSection::TypeTables:
	{
		/**
		 * One table per known type with keys that some record or sub-entry of the image has (a table with no
		 * rows too, so "no table" always means "lost"): its name, TagSchemaHash, then its payload framed with
		 * its own size and checksum, so one damaged table is dropped alone. Payload: the keys by name, the rows
		 * (record index << 1, or sub-entry index << 1 | 1, for rows with tags), then column by column the
		 * number of values per row and the values.
		 */
		struct SRow { uint32 Owner = 0u; SPoolRange Tags; };
		std::vector<std::vector<SRow>> rowsByType(Image.TypeNames.size());
		std::vector<uint8> used(Image.TypeNames.size(), 0u);
		for (uint32 i = 0u; i < Image.Records.size(); ++i)
		{
			const SImageRecord& record = Image.Records[i];
			if (record.Type < used.size())
			{
				used[record.Type] = 1u;
				if (record.Tags.Count > 0u)
				{
					rowsByType[record.Type].push_back(SRow { i << 1u, record.Tags });
				}
			}
		}
		for (uint32 i = 0u; i < Image.SubEntries.size(); ++i)
		{
			const SImageSubEntry& sub = Image.SubEntries[i];
			if (sub.Type < used.size())
			{
				used[sub.Type] = 1u;
				if (sub.Tags.Count > 0u)
				{
					rowsByType[sub.Type].push_back(SRow { (i << 1u) | 1u, sub.Tags });
				}
			}
		}
		uint32 tableCount = 0u;
		std::vector<uint8> tables;
		std::vector<uint8> payload;
		for (uint32 t = 0u; t < Image.TypeNames.size(); ++t)
		{
			const std::optional<reflect::TypeId> type = FindTypeExact(Image.TypeNames[t]);
			if (used[t] == 0u || !type || CAssetTypeRegistry::Get(*type).Keys.empty())
			{
				continue;
			}
			const SAssetTypeInfo& info = CAssetTypeRegistry::Get(*type);
			const std::vector<SRow>& rows = rowsByType[t];
			payload.clear();
			AppendVarint(payload, info.Keys.size());
			for (const SStaticTagKey& key : info.Keys)
			{
				AppendText(payload, key.Name);
				AppendEnum(payload, key.Type);
				AppendFlags(payload, key.Flags);
			}
			AppendVarint(payload, rows.size());
			for (const SRow& row : rows)
			{
				AppendVarint(payload, row.Owner);
			}
			for (const SStaticTagKey& key : info.Keys)
			{
				const EValueForm form = GetValueForm(key.Type);
				for (const SRow& row : rows)
				{
					uint32 count = 0u;
					for (uint32 i = row.Tags.Begin; i < row.Tags.Begin + row.Tags.Count && i < Image.Tags.size(); ++i)
					{
						const SImageTag& tag = Image.Tags[i];
						count += tag.Key < Image.Names.size() && Image.Names[tag.Key].View() == FStringView(key.Name) && tag.Type == key.Type ? 1u : 0u;
					}
					AppendVarint(payload, count);
					for (uint32 i = row.Tags.Begin; count > 0u && i < row.Tags.Begin + row.Tags.Count && i < Image.Tags.size(); ++i)
					{
						const SImageTag& tag = Image.Tags[i];
						if (tag.Key >= Image.Names.size() || Image.Names[tag.Key].View() != FStringView(key.Name) || tag.Type != key.Type)
						{
							continue;
						}
						switch (form)
						{
						case EValueForm::Fixed: AppendLittleEndian(payload, tag.Bits, 8u); break;
						case EValueForm::Varint: AppendVarint(payload, tag.Bits); break;
						case EValueForm::Name:
						case EValueForm::String: AppendVarint(payload, tag.Text); break;
						}
					}
				}
			}
			AppendText(tables, info.Name);
			AppendLittleEndian(tables, ComputeTagSchemaHash(info), 8u);
			AppendVarint(tables, payload.size());
			AppendLittleEndian(tables, hash::Xxh3_64(payload), 8u);
			binary::AppendRaw(tables, payload);
			++tableCount;
		}
		AppendVarint(out, tableCount);
		binary::AppendRaw(out, tables);
		break;
	}
	case ESnapshotSection::Aliases:
	case ESnapshotSection::FormerPaths:
	case ESnapshotSection::Metas:
	case ESnapshotSection::Tombstones:
		AppendVarint(out, 0u);   // M4 fills them
		break;
	}
	return out;
}


// ---------------- Decoding ----------------

// A section as the table names it: its bytes when its layout and checksum are right.
struct SSectionSpan
{
	bool bPresent = false;       // listed in the table and inside the file
	bool bValid = false;         // ... with this build's layout and a matching checksum
	std::span<const uint8> Bytes;
};

bool ReadTextTable (CByteReader& Reader, std::vector<FString>& Out)
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

// Per-record counts from ASSETS (the ranges are their prefix sums).
struct SRecordCounts
{
	uint32 Labels = 0u;
	uint32 SubEntries = 0u;
	uint32 Edges = 0u;
	uint32 Companions = 0u;
};

/**
 * One snapshot file decoded into an image of its own (indices local to it). Applies §5.3's per-section rules
 * (the header comment of Snapshot.h): drops what a bad section takes with it and flags what must be analyzed
 * or re-evaluated again.
 */
class CSnapshotDecoder
{
public:
	CSnapshotDecoder (std::span<const uint8> InBytes, SSnapshotFileReport& InReport)
		: Bytes(InBytes)
		, Report(InReport)
	{}

	// The ProjectId the header names (after Decode).
	[[nodiscard]] const SGuid& GetProjectId () const noexcept { return ProjectId; }

	// False: the file is rejected (Report says why).
	bool Decode (const SGuid& ExpectedProjectId, bool bAnyProject, SRegistryImage& Out)
	{
		if (!ReadHeader(ExpectedProjectId, bAnyProject))
		{
			return false;
		}
		Image = &Out;
		for (const ESnapshotSection required : { ESnapshotSection::Mounts, ESnapshotSection::Names, ESnapshotSection::Folders, ESnapshotSection::Files })
		{
			if (!DecodeRequired(required))
			{
				Report.State = ESnapshotFileState::Rejected;
				Report.Message = FString::Format("its {} section is damaged or of another layout; the file was not used", enum_::ToString(required));
				Report.DroppedSections.push_back(required);
				return false;
			}
		}
		DecodeOptional();
		return true;
	}

private:
	std::span<const uint8> Bytes;
	SSnapshotFileReport& Report;
	SRegistryImage* Image = nullptr;
	std::array<SSectionSpan, SectionOrder.size()> Sections {};
	std::vector<SRecordCounts> Counts;
	SGuid ProjectId;

	bool Reject (FString Message)
	{
		Report.State = ESnapshotFileState::Rejected;
		Report.Message = std::move(Message);
		return false;
	}

	bool ReadHeader (const SGuid& ExpectedProjectId, bool bAnyProject)
	{
		CByteReader reader(Bytes);
		uint64 version = 0u;
		uint64 sectionCount = 0u;
		SGuid projectId;
		uint64 journalBase = 0u;
		uint64 rulesBasis = 0u;
		if (Bytes.size() < SnapshotHeaderBytes || !reader.Expect(SnapshotMagic))
		{
			return Reject("not a registry snapshot");
		}
		if (!reader.ReadLittleEndian(version, 4u) || !reader.ReadLittleEndian(sectionCount, 4u) || !ReadGuid(reader, projectId)
			|| !ReadU64(reader, journalBase) || !ReadU64(reader, rulesBasis))
		{
			return Reject("its header is cut short");
		}
		if (version != SnapshotFormatVersion)
		{
			return Reject(FString::Format("snapshot format {} (this build reads {})", version, SnapshotFormatVersion));
		}
		if (!bAnyProject && !ExpectedProjectId.IsValid())
		{
			return Reject("this process has no ProjectId for its root, so no snapshot of it is taken");
		}
		if (!bAnyProject && projectId != ExpectedProjectId)
		{
			return Reject(FString::Format("it belongs to project {}, not {}", projectId.ToString(), ExpectedProjectId.ToString()));
		}
		Report.JournalBase = journalBase;
		Report.RulesBasis = rulesBasis;
		ProjectId = projectId;
		if (sectionCount > (Bytes.size() - SnapshotHeaderBytes) / SnapshotSectionEntryBytes)
		{
			return Reject("its section table runs past the end of the file");
		}
		for (uint64 i = 0u; i < sectionCount; ++i)
		{
			const size_t at = SnapshotHeaderBytes + static_cast<size_t>(i) * SnapshotSectionEntryBytes;
			const uint64 id = binary::ReadLittleEndianAt(Bytes, at, 4u);
			const uint64 layout = binary::ReadLittleEndianAt(Bytes, at + 4u, 4u);
			const uint64 offset = binary::ReadLittleEndianAt(Bytes, at + 8u, 8u);
			const uint64 size = binary::ReadLittleEndianAt(Bytes, at + 16u, 8u);
			const uint64 checksum = binary::ReadLittleEndianAt(Bytes, at + 24u, 8u);
			if (id == 0u || id > SectionOrder.size() || Sections[id - 1u].bPresent || offset > Bytes.size() || size > Bytes.size() - offset)
			{
				continue;   // unknown (a later build's) or out of the file: as if absent
			}
			SSectionSpan& section = Sections[id - 1u];
			section.bPresent = true;
			section.Bytes = Bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
			section.bValid = layout == SectionLayoutVersion && hash::Xxh3_64(section.Bytes) == checksum;
		}
		return true;
	}

	void Drop (ESnapshotSection Section)
	{
		if (std::find(Report.DroppedSections.begin(), Report.DroppedSections.end(), Section) == Report.DroppedSections.end())
		{
			Report.DroppedSections.push_back(Section);
		}
	}

	// Runs Parse over a valid section; a parse that fails or leaves bytes over makes it invalid.
	template <typename TParse>
	bool Parse (ESnapshotSection Section, TParse&& ParseBody)
	{
		SSectionSpan& section = Sections[SectionSlot(Section)];
		if (!section.bValid)
		{
			return false;
		}
		CByteReader reader(section.Bytes);
		if (!ParseBody(reader) || !reader.IsAtEnd())
		{
			section.bValid = false;
			return false;
		}
		return true;
	}

	bool DecodeRequired (ESnapshotSection Section)
	{
		SRegistryImage& image = *Image;
		switch (Section)
		{
		case ESnapshotSection::Mounts:
			return Parse(Section, [&] (CByteReader& Reader)
			{
				uint32 count = 0u;
				if (!ReadCount(Reader, 3u, count))
				{
					return false;
				}
				image.Mounts.resize(count);
				for (uint32 i = 0u; i < count; ++i)
				{
					SImageMount& mount = image.Mounts[i];
					uint8 caseSensitive = 0u;
					if (!Reader.ReadText(mount.Name) || !ReadU16Varint(Reader, mount.Id) || !Reader.ReadU8(caseSensitive) || caseSensitive > 1u
						|| mount.Name.IsEmpty())
					{
						return false;
					}
					mount.bCaseSensitive = caseSensitive != 0u;
					for (uint32 j = 0u; j < i; ++j)
					{
						if (image.Mounts[j].Name == mount.Name)
						{
							return false;
						}
					}
				}
				return true;
			});
		case ESnapshotSection::Names:
			return Parse(Section, [&] (CByteReader& Reader) { return ReadTextTable(Reader, image.Names); });
		case ESnapshotSection::Folders:
			return Parse(Section, [&] (CByteReader& Reader)
			{
				uint32 count = 0u;
				if (!ReadCount(Reader, 3u, count))
				{
					return false;
				}
				image.Folders.resize(count);
				for (SImageFolder& folder : image.Folders)
				{
					if (!ReadU16Varint(Reader, folder.Mount) || !ReadOptionalIndex(Reader, folder.Parent) || !ReadU32Varint(Reader, folder.Name)
						|| folder.Mount >= image.Mounts.size() || (folder.Parent != ~0u && folder.Parent >= count) || folder.Name >= image.Names.size())
					{
						return false;
					}
				}
				return true;
			});
		case ESnapshotSection::Files:
			return Parse(Section, [&] (CByteReader& Reader)
			{
				uint32 count = 0u;
				if (!ReadCount(Reader, 58u, count))
				{
					return false;
				}
				image.Files.resize(count);
				for (SImageFile& file : image.Files)
				{
					if (!ReadU32Varint(Reader, file.Folder) || !ReadU32Varint(Reader, file.LeafName) || !Reader.ReadVarint(file.Size)
						|| !ReadSignedVarint(Reader, file.MTime) || !ReadIdentity(Reader, file.Identity) || !Reader.ReadHash(file.ContentHash)
						|| !ReadU64(Reader, file.MetaHash) || !ReadEnum(Reader, EFileClass::Excluded, file.Class)
						|| !ReadEnum(Reader, EMetaState::MissingMeta, file.MetaState) || file.Folder >= image.Folders.size()
						|| file.LeafName >= image.Names.size())
					{
						return false;
					}
				}
				return true;
			});
		default:
			return false;
		}
	}

	void DecodeOptional ()
	{
		SRegistryImage& image = *Image;

		const bool bStrings = Parse(ESnapshotSection::Strings, [&] (CByteReader& Reader) { return ReadTextTable(Reader, image.Strings); });
		if (!bStrings)
		{
			image.Strings.clear();
		}
		const bool bTypes = Parse(ESnapshotSection::Types, [&] (CByteReader& Reader) { return ReadTextTable(Reader, image.TypeNames); });
		if (!bTypes)
		{
			image.TypeNames.clear();
		}

		const bool bRuleFiles = Parse(ESnapshotSection::RuleFiles, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadCount(Reader, 10u, count))
			{
				return false;
			}
			image.RuleFiles.resize(count);
			for (SImageRuleFile& ruleFile : image.RuleFiles)
			{
				if (!ReadU16Varint(Reader, ruleFile.Mount) || !ReadOptionalIndex(Reader, ruleFile.Folder) || !ReadU64(Reader, ruleFile.RulesHash)
					|| ruleFile.Mount >= image.Mounts.size() || (ruleFile.Folder != ~0u && ruleFile.Folder >= image.Folders.size()))
				{
					return false;
				}
			}
			return true;
		});

		// Records. Without TYPES, every type index is meaningless: they all point at an empty name, which no
		// type has (frt.Unknown, Schemaless | StaleTags), and the records are analyzed again.
		const bool bAssets = Parse(ESnapshotSection::Assets, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadTextTable(Reader, image.ImporterNames) || !ReadCount(Reader, 33u, count))
			{
				return false;
			}
			image.Records.resize(count);
			Counts.resize(count);
			for (uint32 i = 0u; i < count; ++i)
			{
				SImageRecord& record = image.Records[i];
				SRecordCounts& counts = Counts[i];
				uint32 importer = ~0u;
				if (!ReadU32Varint(Reader, record.File) || !ReadGuid(Reader, record.Guid) || !ReadU16Varint(Reader, record.Type)
					|| !ReadOptionalIndex(Reader, importer) || !ReadFlags(Reader, record.Flags) || !ReadEnum(Reader, ECookRule::DevelopmentOnly, record.CookRule)
					|| !ReadU16Varint(Reader, record.Chunk) || !ReadOptionalIndex(Reader, record.PrimaryName)
					|| !ReadEnum(Reader, EImportStatus::ImporterUnavailable, record.ImportStatus) || !ReadU64(Reader, record.AnalyzeStamp)
					|| !ReadU32Varint(Reader, counts.Labels) || !ReadU32Varint(Reader, counts.SubEntries) || !ReadU32Varint(Reader, counts.Edges)
					|| !ReadU32Varint(Reader, counts.Companions) || record.File >= image.Files.size()
					|| (bTypes && record.Type >= image.TypeNames.size()) || (importer != ~0u && importer >= image.ImporterNames.size())
					|| (record.PrimaryName != ~0u && record.PrimaryName >= image.Names.size()))
				{
					return false;
				}
				record.Importer = importer == ~0u ? uint16(0xFFFFu) : static_cast<uint16>(importer);
			}
			return true;
		});
		if (!bAssets)
		{
			image.Records.clear();
			image.ImporterNames.clear();
			Counts.clear();
			Drop(ESnapshotSection::Assets);
		}

		const auto sumOf = [&] (uint32 SRecordCounts::* Member)
		{
			uint64 sum = 0u;
			for (const SRecordCounts& counts : Counts)
			{
				sum += counts.*Member;
			}
			return sum;
		};

		const bool bSubs = bAssets && Parse(ESnapshotSection::SubEntry, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadCount(Reader, 11u, count) || count != sumOf(&SRecordCounts::SubEntries))
			{
				return false;
			}
			image.SubEntries.resize(count);
			for (SImageSubEntry& sub : image.SubEntries)
			{
				if (!ReadU32Varint(Reader, sub.Label) || !ReadU64(Reader, sub.SubId) || !ReadU16Varint(Reader, sub.Type) || !ReadFlags(Reader, sub.Flags)
					|| sub.Label >= image.Names.size() || (bTypes && sub.Type >= image.TypeNames.size()))
				{
					return false;
				}
			}
			return true;
		});
		const bool bLabels = bAssets && Parse(ESnapshotSection::Labels, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadCount(Reader, 1u, count) || count != sumOf(&SRecordCounts::Labels))
			{
				return false;
			}
			image.Labels.resize(count);
			for (uint32& label : image.Labels)
			{
				uint64 value = 0u;
				if (!Reader.ReadVarint(value) || (value >> 1u) >= image.Names.size())
				{
					return false;
				}
				label = static_cast<uint32>(value >> 1u) | ((value & 1u) != 0u ? RuleLabelBit : 0u);
			}
			return true;
		});
		const bool bDeps = bAssets && bStrings && Parse(ESnapshotSection::Deps, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadCount(Reader, 12u, count) || count != sumOf(&SRecordCounts::Edges))
			{
				return false;
			}
			image.Edges.resize(count);
			for (SImageEdge& edge : image.Edges)
			{
				uint8 hasTarget = 0u;
				if (!ReadU64(Reader, edge.FromSub) || !Reader.ReadU8(hasTarget) || hasTarget > 1u)
				{
					return false;
				}
				if (hasTarget != 0u && (!ReadGuid(Reader, edge.Target.Guid) || !ReadU64(Reader, edge.Target.SubId)))
				{
					return false;
				}
				if (!ReadOptionalIndex(Reader, edge.TargetPath) || !ReadFlags(Reader, edge.Flags) || !ReadOptionalIndex(Reader, edge.FieldPath)
					|| (edge.TargetPath != ~0u && edge.TargetPath >= image.Strings.size()) || (edge.FieldPath != ~0u && edge.FieldPath >= image.Names.size()))
				{
					return false;
				}
			}
			if (!ReadCount(Reader, 1u, count) || count != sumOf(&SRecordCounts::Companions))
			{
				return false;
			}
			image.Companions.resize(count);
			for (uint32& companion : image.Companions)
			{
				if (!ReadU32Varint(Reader, companion) || companion >= image.Strings.size())
				{
					return false;
				}
			}
			return true;
		});
		const bool bProblems = bStrings && Parse(ESnapshotSection::Problems, [&] (CByteReader& Reader)
		{
			uint32 count = 0u;
			if (!ReadCount(Reader, 3u, count))
			{
				return false;
			}
			image.Problems.resize(count);
			for (SImageProblems& problems : image.Problems)
			{
				if (!ReadU32Varint(Reader, problems.Subject) || !ReadEnum(Reader, EProblemSource::Unclaimed, problems.Source)
					|| !ReadDiagnostics(Reader, problems.Diagnostics) || problems.Subject >= image.Strings.size())
				{
					return false;
				}
			}
			return true;
		});
		if (!bSubs)
		{
			image.SubEntries.clear();
		}
		if (!bLabels)
		{
			image.Labels.clear();
		}
		if (!bDeps)
		{
			image.Edges.clear();
			image.Companions.clear();
		}
		if (!bProblems)
		{
			image.Problems.clear();
		}

		// Ranges: each record's slice follows its predecessor's.
		uint32 labelAt = 0u;
		uint32 subAt = 0u;
		uint32 edgeAt = 0u;
		uint32 companionAt = 0u;
		std::vector<uint8> stale(image.Records.size(), 0u);
		for (uint32 i = 0u; i < image.Records.size(); ++i)
		{
			SImageRecord& record = image.Records[i];
			const SRecordCounts& counts = Counts[i];
			record.Labels = bLabels ? SPoolRange { labelAt, counts.Labels } : SPoolRange {};
			record.SubEntries = bSubs ? SPoolRange { subAt, counts.SubEntries } : SPoolRange {};
			// An edge may name one of the record's sub-entries: without them, its edges go too.
			const bool bEdges = bDeps && (bSubs || counts.SubEntries == 0u);
			record.Edges = bEdges ? SPoolRange { edgeAt, counts.Edges } : SPoolRange {};
			record.Companions = bDeps ? SPoolRange { companionAt, counts.Companions } : SPoolRange {};
			record.Tags = SPoolRange {};
			labelAt += counts.Labels;
			subAt += counts.SubEntries;
			edgeAt += counts.Edges;
			companionAt += counts.Companions;
			if ((!bSubs && counts.SubEntries > 0u) || (!bEdges && counts.Edges > 0u) || (!bDeps && counts.Companions > 0u))
			{
				stale[i] = 1u;
			}
		}

		// Without TYPES every record and sub-entry names the empty type (none has it).
		if (!bTypes)
		{
			image.TypeNames.assign(1u, FString());
			for (SImageRecord& record : image.Records)
			{
				record.Type = 0u;
			}
			for (SImageSubEntry& sub : image.SubEntries)
			{
				sub.Type = 0u;
			}
		}

		const std::vector<uint8> tablesOk = DecodeTypeTables(bTypes, bStrings);

		// A record whose type (or one of whose sub-entries' types) has keys but no table lost its tags.
		const auto lostTags = [&] (uint16 Type) { return Type < tablesOk.size() && tablesOk[Type] == 0u && HasKeys(image.TypeNames[Type]); };
		for (uint32 i = 0u; i < image.Records.size(); ++i)
		{
			const SImageRecord& record = image.Records[i];
			bool bLost = lostTags(record.Type);
			for (uint32 s = record.SubEntries.Begin; !bLost && s < record.SubEntries.Begin + record.SubEntries.Count; ++s)
			{
				bLost = lostTags(image.SubEntries[s].Type);
			}
			if (bLost)
			{
				stale[i] = 1u;
			}
		}

		// Losses (Snapshot.h): Analyze outputs make records stale, rule outputs drop the rule hashes.
		const bool bAllStale = !bTypes || !bProblems;
		const bool bDropRuleFiles = !bRuleFiles || (bAssets && !bLabels) || !bProblems;
		for (uint32 i = 0u; i < image.Records.size(); ++i)
		{
			if (bAllStale || stale[i] != 0u)
			{
				image.Records[i].Flags |= ERecordFlags::StaleTags;
				image.Records[i].AnalyzeStamp = 0u;
				++Report.StaleRecords;
			}
		}
		if (bDropRuleFiles)
		{
			image.RuleFiles.clear();
			Report.bRuleFilesDropped = true;
		}
		Report.Records = static_cast<uint32>(image.Records.size());

		for (const auto& [section, bOk] : { std::pair(ESnapshotSection::Strings, bStrings), std::pair(ESnapshotSection::Types, bTypes),
			std::pair(ESnapshotSection::RuleFiles, !bDropRuleFiles), std::pair(ESnapshotSection::SubEntry, bSubs || !bAssets),
			std::pair(ESnapshotSection::Labels, bLabels || !bAssets), std::pair(ESnapshotSection::Deps, bDeps || !bAssets),
			std::pair(ESnapshotSection::Problems, bProblems) })
		{
			if (!bOk)
			{
				Drop(section);
			}
		}
		if (!bAssets)
		{
			for (const ESnapshotSection section : { ESnapshotSection::SubEntry, ESnapshotSection::Labels, ESnapshotSection::Deps, ESnapshotSection::TypeTables })
			{
				Drop(section);
			}
		}
	}

	/**
	 * TYPETABLES: per table its own checksum, so a section whose checksum fails still gives the tables that are
	 * intact (its framing permitting). Returns, per image type index, whether that type's table was taken.
	 */
	std::vector<uint8> DecodeTypeTables (bool bTypes, bool bStrings)
	{
		SRegistryImage& image = *Image;
		std::vector<uint8> taken(image.TypeNames.size(), 0u);
		const SSectionSpan& section = Sections[SectionSlot(ESnapshotSection::TypeTables)];
		if (image.Records.empty())
		{
			return taken;
		}
		if (!bTypes)
		{
			Drop(ESnapshotSection::TypeTables);   // its rows cannot be matched to types: the records are analyzed again
			return taken;
		}
		if (!section.bPresent)
		{
			Drop(ESnapshotSection::TypeTables);
			return taken;
		}
		// The layout is checked even when the checksum failed: another layout means nothing in it is ours.
		{
			const size_t entry = [&]
			{
				const uint64 count = binary::ReadLittleEndianAt(Bytes, 12u, 4u);
				for (uint64 i = 0u; i < count; ++i)
				{
					const size_t at = SnapshotHeaderBytes + static_cast<size_t>(i) * SnapshotSectionEntryBytes;
					if (binary::ReadLittleEndianAt(Bytes, at, 4u) == static_cast<uint32>(ESnapshotSection::TypeTables))
					{
						return at;
					}
				}
				return size_t(0u);
			}();
			if (entry == 0u || binary::ReadLittleEndianAt(Bytes, entry + 4u, 4u) != SectionLayoutVersion)
			{
				Drop(ESnapshotSection::TypeTables);
				return taken;
			}
		}

		// The image's type index of a name, by linear search (a handful of types).
		const auto findTypeIndex = [&] (FStringView Name) -> int32
		{
			for (uint32 t = 0u; t < image.TypeNames.size(); ++t)
			{
				if (image.TypeNames[t].View() == Name)
				{
					return static_cast<int32>(t);
				}
			}
			return -1;
		};

		CByteReader reader(section.Bytes);
		uint32 tableCount = 0u;
		if (!ReadCount(reader, 18u, tableCount))
		{
			Drop(ESnapshotSection::TypeTables);
			return taken;
		}
		for (uint32 t = 0u; t < tableCount; ++t)
		{
			FString name;
			uint64 schemaHash = 0u;
			std::span<const uint8> payload;
			uint64 checksum = 0u;
			uint64 size = 0u;
			if (!reader.ReadText(name) || !ReadU64(reader, schemaHash) || !reader.ReadVarint(size) || !ReadU64(reader, checksum)
				|| !reader.ReadSpan(size, payload))
			{
				Drop(ESnapshotSection::TypeTables);   // the framing is broken: the tables after this point are lost
				break;
			}
			const int32 typeIndex = findTypeIndex(name);
			const std::optional<reflect::TypeId> type = FindTypeExact(name);
			if (typeIndex < 0 || !type)
			{
				continue;   // a type this build does not know: its records are frt.Unknown anyway
			}
			const SAssetTypeInfo& info = CAssetTypeRegistry::Get(*type);
			if (hash::Xxh3_64(payload) != checksum)
			{
				Report.DroppedTypeTables.push_back(name);
				continue;
			}
			if (schemaHash != ComputeTagSchemaHash(info))
			{
				Report.DroppedTypeTables.push_back(name);   // this build declares other keys: gather the type again
				continue;
			}
			if (!DecodeTypeTable(payload, info, static_cast<uint16>(typeIndex), bStrings))
			{
				Report.DroppedTypeTables.push_back(name);
				continue;
			}
			taken[static_cast<uint32>(typeIndex)] = 1u;
		}
		// A section checksum that failed while every table passed its own: the damage is in bytes no table owns,
		// and nothing was lost.
		return taken;
	}

	bool DecodeTypeTable (std::span<const uint8> Payload, const SAssetTypeInfo& Info, uint16 TypeIndex, bool bStrings)
	{
		SRegistryImage& image = *Image;
		CByteReader reader(Payload);
		uint32 keyCount = 0u;
		if (!ReadCount(reader, 3u, keyCount) || keyCount != Info.Keys.size())
		{
			return false;
		}
		std::vector<uint32> keyNames(keyCount);
		std::vector<EValueForm> forms(keyCount);
		for (uint32 k = 0u; k < keyCount; ++k)
		{
			FString keyName;
			ETagType keyType = ETagType::None;
			ETagFlags keyFlags = ETagFlags::None;
			if (!reader.ReadText(keyName) || !ReadEnum(reader, ETagType::RowRef, keyType) || !ReadFlags(reader, keyFlags)
				|| keyName.View() != FStringView(Info.Keys[k].Name) || keyType != Info.Keys[k].Type)
			{
				return false;
			}
			if (keyType == ETagType::String && !bStrings)
			{
				return false;   // its values index the lost STRINGS
			}
			forms[k] = GetValueForm(keyType);
			keyNames[k] = static_cast<uint32>(image.Names.size());
			image.Names.push_back(std::move(keyName));
		}
		// A row costs at least its owner varint and one count varint per key: that bounds the count, so a forged
		// one cannot make the tables below many times the payload's size.
		uint32 rowCount = 0u;
		if (!ReadCount(reader, 1u + uint64(keyCount), rowCount))
		{
			return false;
		}
		// Rows whose owner is not of this type (a damaged image) keep no tags; their indices are still read.
		std::vector<uint32> owners(rowCount);
		std::vector<uint8> ownerOk(rowCount, 0u);
		for (uint32 r = 0u; r < rowCount; ++r)
		{
			if (!ReadU32Varint(reader, owners[r]))
			{
				return false;
			}
			const uint32 index = owners[r] >> 1u;
			const bool bSub = (owners[r] & 1u) != 0u;
			ownerOk[r] = bSub ? (index < image.SubEntries.size() && image.SubEntries[index].Type == TypeIndex)
				: (index < image.Records.size() && image.Records[index].Type == TypeIndex);
		}
		// Column by column into per-key value lists, then row by row into the image's tag pool (key order).
		// counts: key-major, one per key and row (keyCount * rowCount, bounded above); values are added as read.
		struct SValue { uint64 Bits = 0u; uint32 Text = ~0u; };
		std::vector<uint32> counts(static_cast<size_t>(keyCount) * rowCount);
		std::vector<std::vector<SValue>> values(keyCount);
		for (uint32 k = 0u; k < keyCount; ++k)
		{
			for (uint32 r = 0u; r < rowCount; ++r)
			{
				uint32 count = 0u;
				if (!ReadCount(reader, 1u, count))
				{
					return false;
				}
				counts[static_cast<size_t>(k) * rowCount + r] = count;
				for (uint32 v = 0u; v < count; ++v)
				{
					SValue value;
					switch (forms[k])
					{
					case EValueForm::Fixed:
						if (!ReadU64(reader, value.Bits))
						{
							return false;
						}
						break;
					case EValueForm::Varint:
						if (!reader.ReadVarint(value.Bits))
						{
							return false;
						}
						break;
					case EValueForm::Name:
						if (!ReadU32Varint(reader, value.Text) || value.Text >= image.Names.size())
						{
							return false;
						}
						break;
					case EValueForm::String:
						if (!ReadU32Varint(reader, value.Text) || value.Text >= image.Strings.size())
						{
							return false;
						}
						break;
					}
					values[k].push_back(value);
				}
			}
		}
		if (!reader.IsAtEnd())
		{
			return false;
		}
		std::vector<size_t> cursor(keyCount, 0u);
		for (uint32 r = 0u; r < rowCount; ++r)
		{
			SPoolRange range { static_cast<uint32>(image.Tags.size()), 0u };
			for (uint32 k = 0u; k < keyCount; ++k)
			{
				for (uint32 v = 0u; v < counts[static_cast<size_t>(k) * rowCount + r]; ++v)
				{
					const SValue& value = values[k][cursor[k]++];
					if (ownerOk[r] != 0u)
					{
						SImageTag tag;
						tag.Key = keyNames[k];
						tag.Type = Info.Keys[k].Type;
						tag.Bits = value.Bits;
						tag.Text = value.Text;
						image.Tags.push_back(tag);
					}
				}
			}
			range.Count = static_cast<uint32>(image.Tags.size()) - range.Begin;
			if (ownerOk[r] != 0u)
			{
				const uint32 index = owners[r] >> 1u;
				((owners[r] & 1u) != 0u ? image.SubEntries[index].Tags : image.Records[index].Tags) = range;
			}
		}
		return true;
	}
};


// ---------------- Merging ----------------

// Keeps only the mounts Keep marks (by image mount index), with everything below them; pools stay (unreferenced
// entries are never read).
void FilterImageMounts (SRegistryImage& Image, const std::vector<uint8>& Keep)
{
	std::vector<uint16> mountIndex(Image.Mounts.size(), 0xFFFFu);
	std::vector<SImageMount> mounts;
	std::vector<FString> keptNames;
	for (uint32 m = 0u; m < Image.Mounts.size(); ++m)
	{
		if (Keep[m] != 0u)
		{
			mountIndex[m] = static_cast<uint16>(mounts.size());
			keptNames.push_back(Image.Mounts[m].Name);
			mounts.push_back(std::move(Image.Mounts[m]));
		}
	}
	Image.Mounts = std::move(mounts);

	std::vector<uint32> folderIndex(Image.Folders.size(), ~0u);
	std::vector<SImageFolder> folders;
	for (uint32 f = 0u; f < Image.Folders.size(); ++f)
	{
		SImageFolder folder = Image.Folders[f];
		if (mountIndex[folder.Mount] == 0xFFFFu)
		{
			continue;
		}
		folderIndex[f] = static_cast<uint32>(folders.size());
		folder.Mount = mountIndex[folder.Mount];
		folders.push_back(folder);
	}
	for (SImageFolder& folder : folders)
	{
		folder.Parent = folder.Parent != ~0u ? folderIndex[folder.Parent] : ~0u;
	}
	Image.Folders = std::move(folders);

	std::vector<uint32> fileIndex(Image.Files.size(), ~0u);
	std::vector<SImageFile> files;
	for (uint32 i = 0u; i < Image.Files.size(); ++i)
	{
		SImageFile file = Image.Files[i];
		if (folderIndex[file.Folder] == ~0u)
		{
			continue;
		}
		fileIndex[i] = static_cast<uint32>(files.size());
		file.Folder = folderIndex[file.Folder];
		files.push_back(file);
	}
	Image.Files = std::move(files);

	std::erase_if(Image.RuleFiles, [&] (const SImageRuleFile& RuleFile) { return mountIndex[RuleFile.Mount] == 0xFFFFu; });
	for (SImageRuleFile& ruleFile : Image.RuleFiles)
	{
		ruleFile.Mount = mountIndex[ruleFile.Mount];
		ruleFile.Folder = ruleFile.Folder != ~0u ? folderIndex[ruleFile.Folder] : ~0u;
	}
	std::erase_if(Image.Records, [&] (const SImageRecord& Record) { return fileIndex[Record.File] == ~0u; });
	for (SImageRecord& record : Image.Records)
	{
		record.File = fileIndex[record.File];
	}
	std::erase_if(Image.Problems, [&] (const SImageProblems& Problems)
	{
		const FStringView mount = GetMountNameOf(Image.Strings[Problems.Subject]);
		return std::none_of(keptNames.begin(), keptNames.end(), [&] (const FString& Name) { return Name.View() == mount; });
	});
}

// Appends From to Into with every index shifted past Into's tables.
void AppendImage (SRegistryImage& Into, SRegistryImage&& From)
{
	if (Into.Mounts.empty() && Into.Names.empty() && Into.Strings.empty() && Into.Records.empty())
	{
		Into = std::move(From);
		return;
	}
	const uint16 mountBase = static_cast<uint16>(Into.Mounts.size());
	const uint32 nameBase = static_cast<uint32>(Into.Names.size());
	const uint32 stringBase = static_cast<uint32>(Into.Strings.size());
	const uint16 typeBase = static_cast<uint16>(Into.TypeNames.size());
	const uint16 importerBase = static_cast<uint16>(Into.ImporterNames.size());
	const uint32 folderBase = static_cast<uint32>(Into.Folders.size());
	const uint32 fileBase = static_cast<uint32>(Into.Files.size());
	const uint32 labelBase = static_cast<uint32>(Into.Labels.size());
	const uint32 subBase = static_cast<uint32>(Into.SubEntries.size());
	const uint32 tagBase = static_cast<uint32>(Into.Tags.size());
	const uint32 edgeBase = static_cast<uint32>(Into.Edges.size());
	const uint32 companionBase = static_cast<uint32>(Into.Companions.size());
	const auto shift = [] (uint32 Index, uint32 Base) { return Index != ~0u ? Index + Base : ~0u; };
	const auto moveAll = [] (auto& To, auto& Items)
	{
		To.insert(To.end(), std::make_move_iterator(Items.begin()), std::make_move_iterator(Items.end()));
	};

	for (SImageMount& mount : From.Mounts)
	{
		Into.Mounts.push_back(std::move(mount));
	}
	moveAll(Into.Names, From.Names);
	moveAll(Into.Strings, From.Strings);
	moveAll(Into.TypeNames, From.TypeNames);
	moveAll(Into.ImporterNames, From.ImporterNames);
	for (SImageFolder folder : From.Folders)
	{
		folder.Mount = static_cast<uint16>(folder.Mount + mountBase);
		folder.Parent = shift(folder.Parent, folderBase);
		folder.Name += nameBase;
		Into.Folders.push_back(folder);
	}
	for (SImageFile file : From.Files)
	{
		file.Folder += folderBase;
		file.LeafName += nameBase;
		Into.Files.push_back(file);
	}
	for (SImageRuleFile ruleFile : From.RuleFiles)
	{
		ruleFile.Mount = static_cast<uint16>(ruleFile.Mount + mountBase);
		ruleFile.Folder = shift(ruleFile.Folder, folderBase);
		Into.RuleFiles.push_back(ruleFile);
	}
	for (SImageRecord record : From.Records)
	{
		record.File += fileBase;
		record.Type = static_cast<uint16>(record.Type + typeBase);
		record.Importer = record.Importer != 0xFFFFu ? static_cast<uint16>(record.Importer + importerBase) : uint16(0xFFFFu);
		record.PrimaryName = shift(record.PrimaryName, nameBase);
		record.Labels.Begin += labelBase;
		record.SubEntries.Begin += subBase;
		record.Tags.Begin += tagBase;
		record.Edges.Begin += edgeBase;
		record.Companions.Begin += companionBase;
		Into.Records.push_back(record);
	}
	for (const uint32 label : From.Labels)
	{
		Into.Labels.push_back(((label & ~RuleLabelBit) + nameBase) | (label & RuleLabelBit));
	}
	for (SImageSubEntry sub : From.SubEntries)
	{
		sub.Label += nameBase;
		sub.Type = static_cast<uint16>(sub.Type + typeBase);
		sub.Tags.Begin += tagBase;
		Into.SubEntries.push_back(std::move(sub));
	}
	for (SImageTag tag : From.Tags)
	{
		tag.Key += nameBase;
		if (tag.Type == ETagType::Name)
		{
			tag.Text = shift(tag.Text, nameBase);
		}
		else if (tag.Type == ETagType::String)
		{
			tag.Text = shift(tag.Text, stringBase);
		}
		Into.Tags.push_back(tag);
	}
	for (SImageEdge edge : From.Edges)
	{
		edge.TargetPath = shift(edge.TargetPath, stringBase);
		edge.FieldPath = shift(edge.FieldPath, nameBase);
		Into.Edges.push_back(std::move(edge));
	}
	for (const uint32 companion : From.Companions)
	{
		Into.Companions.push_back(companion + stringBase);
	}
	for (SImageProblems& problems : From.Problems)
	{
		problems.Subject += stringBase;
		Into.Problems.push_back(std::move(problems));
	}
}
}


// ---------------- Project ids and paths ----------------

std::optional<SGuid> ReadProjectId (const std::filesystem::path& OwnerRoot)
{
	const std::optional<FString> text = platform::ReadFileText(GetProjectIdPath(OwnerRoot));
	if (!text)
	{
		return std::nullopt;
	}
	const std::optional<SGuid> id = SGuid::Parse(text->TrimStartAndEnd());
	return id && id->IsValid() ? id : std::nullopt;
}

std::optional<SGuid> ReadOrCreateProjectId (const std::filesystem::path& OwnerRoot, FString* OutError)
{
	if (const std::optional<SGuid> existing = ReadProjectId(OwnerRoot))
	{
		return existing;
	}
	const SGuid id = SGuid::NewRandom();
	FString text = id.ToString();
	text += '\n';
	if (!platform::WriteFileAtomic(GetProjectIdPath(OwnerRoot), text.View(), true, OutError))
	{
		return std::nullopt;
	}
	return id;
}

std::filesystem::path GetJournalPath (const std::filesystem::path& SnapshotPath)
{
	std::filesystem::path path = SnapshotPath;
	path.replace_extension(".journal");
	return path;
}


// ---------------- Writing ----------------

std::vector<uint8> EncodeSnapshot (const SRegistryImage& Image, const SGuid& ProjectId, uint64 JournalBase, uint64 RulesBasis)
{
	FRT_CPU_SCOPE("Assets.Snapshot.Encode");
	std::array<std::vector<uint8>, SectionOrder.size()> sections;
	size_t total = AlignUp16(SnapshotHeaderBytes + SectionOrder.size() * SnapshotSectionEntryBytes);
	for (size_t i = 0u; i < SectionOrder.size(); ++i)
	{
		sections[i] = EncodeSection(Image, SectionOrder[i]);
		total = AlignUp16(total + sections[i].size());
	}

	std::vector<uint8> bytes;
	bytes.reserve(total);
	binary::AppendRaw(bytes, SnapshotMagic);
	AppendLittleEndian(bytes, SnapshotFormatVersion, 4u);
	AppendLittleEndian(bytes, SectionOrder.size(), 4u);
	AppendGuid(bytes, ProjectId);
	AppendLittleEndian(bytes, JournalBase, 8u);
	AppendLittleEndian(bytes, RulesBasis, 8u);
	const size_t table = bytes.size();
	bytes.resize(AlignUp16(table + SectionOrder.size() * SnapshotSectionEntryBytes), 0u);
	for (size_t i = 0u; i < SectionOrder.size(); ++i)
	{
		const size_t offset = bytes.size();
		binary::AppendRaw(bytes, sections[i]);
		const size_t entry = table + i * SnapshotSectionEntryBytes;
		binary::WriteLittleEndianAt(bytes, entry, static_cast<uint32>(SectionOrder[i]), 4u);
		binary::WriteLittleEndianAt(bytes, entry + 4u, SectionLayoutVersion, 4u);
		binary::WriteLittleEndianAt(bytes, entry + 8u, offset, 8u);
		binary::WriteLittleEndianAt(bytes, entry + 16u, sections[i].size(), 8u);
		binary::WriteLittleEndianAt(bytes, entry + 24u, hash::Xxh3_64(sections[i]), 8u);
		bytes.resize(AlignUp16(bytes.size()), 0u);
	}
	return bytes;
}

bool WriteSnapshot (const CRegistryView& View, std::span<const MountId> Mounts, const std::filesystem::path& Path, const SGuid& ProjectId,
	uint64 RulesBasis, FString* OutError, SSnapshotWriteStats* OutStats)
{
	FRT_CPU_SCOPE("Assets.Snapshot.Write");
	SSnapshotWriteStats stats;
	uint64 start = platform::ReadTimestamp();
	const SRegistryImage image = View.ExportImage(Mounts);
	stats.ExportMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	stats.Records = static_cast<uint32>(image.Records.size());
	stats.JournalBase = View.GetRevision();

	start = platform::ReadTimestamp();
	const std::vector<uint8> bytes = EncodeSnapshot(image, ProjectId, stats.JournalBase, RulesBasis);
	stats.EncodeMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	stats.Bytes = bytes.size();

	start = platform::ReadTimestamp();
	const bool bWritten = platform::WriteFileAtomic(Path, bytes, true, OutError);
	stats.WriteMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	if (OutStats != nullptr)
	{
		*OutStats = stats;
	}
	return bWritten;
}


// ---------------- Loading ----------------

SLoadedSnapshots ReadSnapshots (std::span<const SSnapshotSource> Sources, std::span<const SMountUpsert> Mounts)
{
	FRT_CPU_SCOPE("Assets.Snapshot.Read");
	SLoadedSnapshots loaded;
	std::vector<FString> takenMounts;
	std::vector<MountId> takenIds;
	for (const SSnapshotSource& source : Sources)
	{
		SSnapshotFileReport& report = loaded.Files.emplace_back();
		report.Path = source.Path;
		std::error_code ec;
		uint64 start = platform::ReadTimestamp();
		if (!std::filesystem::exists(source.Path, ec))
		{
			report.State = ESnapshotFileState::Missing;
			continue;
		}
		FString error;
		const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(source.Path, &error);
		loaded.ReadMs += platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
		if (!bytes)
		{
			report.State = ESnapshotFileState::Unreadable;
			report.Message = std::move(error);
			continue;
		}
		report.Bytes = bytes->size();

		start = platform::ReadTimestamp();
		SRegistryImage image;
		CSnapshotDecoder decoder(*bytes, report);
		if (!decoder.Decode(source.ProjectId, source.bAnyProject, image))
		{
			loaded.DecodeMs += platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
			continue;
		}

		// Records evaluated against other rules semantics, or another engine fallback file, than this process
		// has: as if RULEFILES were lost, so the file's mounts re-evaluate (Snapshot.h).
		if (source.RulesBasis != 0u && report.RulesBasis != source.RulesBasis)
		{
			image.RuleFiles.clear();
			report.bRulesBasisChanged = true;
			if (!report.bRuleFilesDropped)
			{
				report.bRuleFilesDropped = true;
				report.DroppedSections.push_back(ESnapshotSection::RuleFiles);
			}
		}

		// Mounts by name: this process's ids, or the file's own when none are given.
		std::vector<uint8> keep(image.Mounts.size(), 1u);
		bool bAllKept = true;
		for (uint32 m = 0u; m < image.Mounts.size(); ++m)
		{
			SImageMount& mount = image.Mounts[m];
			FStringView reason;
			if (std::find(takenMounts.begin(), takenMounts.end(), mount.Name) != takenMounts.end())
			{
				reason = "an earlier file gave it";
			}
			else if (!Mounts.empty())
			{
				const auto current = std::find_if(Mounts.begin(), Mounts.end(), [&] (const SMountUpsert& Mount) { return Mount.Name == mount.Name; });
				if (current == Mounts.end())
				{
					reason = "not mounted now";
				}
				else if (current->bCaseSensitive != mount.bCaseSensitive)
				{
					reason = "its volume's case rule changed";
				}
				else
				{
					mount.Id = current->Id;
				}
			}
			else if (std::find(takenIds.begin(), takenIds.end(), mount.Id) != takenIds.end())
			{
				reason = "an earlier file has its MountId";
			}
			if (!reason.IsEmpty())
			{
				keep[m] = 0u;
				bAllKept = false;
				report.SkippedMounts.push_back(FString::Format("{} ({})", mount.Name, reason));
			}
		}
		if (!bAllKept)
		{
			FilterImageMounts(image, keep);
			report.Records = static_cast<uint32>(image.Records.size());
		}
		if (image.Mounts.empty())
		{
			report.State = ESnapshotFileState::Rejected;
			report.Message = "none of its mounts is loaded";
			loaded.DecodeMs += platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
			continue;
		}
		for (const SImageMount& mount : image.Mounts)
		{
			report.Mounts.push_back(mount.Name);
			takenMounts.push_back(mount.Name);
			takenIds.push_back(mount.Id);
		}
		report.State = ESnapshotFileState::Loaded;
		if (!report.DroppedSections.empty() || !report.DroppedTypeTables.empty())
		{
			FString message = "dropped:";
			for (const ESnapshotSection section : report.DroppedSections)
			{
				message.AppendFormat(" {}", enum_::ToString(section));
			}
			for (const FString& table : report.DroppedTypeTables)
			{
				message.AppendFormat(" TYPETABLES[{}]", table);
			}
			report.Message = std::move(message);
		}

		// Its journal, when it continues this snapshot: the deltas for the mounts loaded from this file.
		const SJournalContents journal = ReadJournal(GetJournalPath(source.Path));
		report.bJournalFound = journal.bFound;
		report.JournalBytes = journal.FileBytes;
		if (journal.bFound)
		{
			if (!journal.bHeaderValid)
			{
				report.JournalMessage = journal.Message;
			}
			else if (journal.ProjectId != decoder.GetProjectId() || journal.JournalBase != report.JournalBase)
			{
				report.JournalMessage = FString::Format("it continues revision {} of project {}, not this snapshot (revision {})", journal.JournalBase,
					journal.ProjectId.ToString(), report.JournalBase);
			}
			else
			{
				report.bJournalUsed = true;
				report.bJournalTornTail = journal.bTornTail;
				report.JournalMessage = journal.Message;
				for (const SRegistryDelta& delta : journal.Deltas)
				{
					SRegistryDelta part = FilterDeltaByMount(delta, [&] (FStringView MountName)
					{
						return std::any_of(report.Mounts.begin(), report.Mounts.end(), [&] (const FString& Name) { return Name.View() == MountName; });
					});
					part.Mounts.clear();   // the image made them, with this process's ids
					if (report.bRuleFilesDropped)
					{
						// The snapshot lost its rule outputs, so every record of its mounts is re-evaluated: a
						// journal's RulesHash would mark a folder current whose records' labels are gone.
						part.RuleFileUpserts.clear();
						part.RuleFileRemoves.clear();
					}
					if (!part.IsEmpty())
					{
						loaded.JournalDeltas.push_back(std::move(part));
					}
					++report.JournalCommits;
				}
			}
		}
		loaded.Revision = (std::max)(loaded.Revision, report.JournalBase);
		AppendImage(loaded.Image, std::move(image));
		loaded.DecodeMs += platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	}
	return loaded;
}

SSnapshotLoadResult AdoptSnapshots (CRegistryStore& Store, SLoadedSnapshots&& Loaded)
{
	FRT_CPU_SCOPE("Assets.Snapshot.Adopt");
	SSnapshotLoadResult result;
	result.ReadMs = Loaded.ReadMs;
	result.DecodeMs = Loaded.DecodeMs;
	if (!Loaded.Image.Mounts.empty())
	{
		Store.AdoptImage(std::move(Loaded.Image), Loaded.Revision);
		result.AdoptMs = Store.GetLastCommitStats().Milliseconds;
		result.bAdoptedInBulk = !Store.GetLastCommitStats().bReplayedImage;
	}
	const uint64 start = platform::ReadTimestamp();
	for (SRegistryDelta& delta : Loaded.JournalDeltas)
	{
		Store.Commit(std::move(delta));
	}
	result.ReplayMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	result.Files = std::move(Loaded.Files);
	result.Records = Store.Live().GetRecordCount();
	result.TotalMs = result.ReadMs + result.DecodeMs + result.AdoptMs + result.ReplayMs;
	return result;
}

SSnapshotLoadResult LoadSnapshots (CRegistryStore& Store, std::span<const SSnapshotSource> Sources, std::span<const SMountUpsert> Mounts)
{
	const uint64 start = platform::ReadTimestamp();
	SSnapshotLoadResult result = AdoptSnapshots(Store, ReadSnapshots(Sources, Mounts));
	result.TotalMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return result;
}
}
