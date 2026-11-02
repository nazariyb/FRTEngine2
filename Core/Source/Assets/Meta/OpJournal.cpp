#include "Assets/Meta/OpJournal.h"

#include <cstring>
#include <span>
#include <utility>

#include "Assets/ByteStream.h"
#include "Assets/RecordFrames.h"
#include "Platform/Platform.h"


namespace frt::assets
{
namespace
{
// Frame kinds. The payload of each starts with a format byte, so a later layout is told apart rather than misread.
constexpr uint8 BeginKind = 'B';
constexpr uint8 StepKind = 'S';
constexpr uint8 EndKind = 'E';
constexpr uint8 PayloadFormat = 1u;

void AppendGuid (std::vector<uint8>& Out, const SGuid& Guid)
{
	binary::AppendRaw(Out, std::span<const uint8>(Guid.Bytes, SGuid::Size));
}

bool ReadGuid (binary::CByteReader& Reader, SGuid& Out) noexcept
{
	std::span<const uint8> bytes;
	if (!Reader.ReadSpan(SGuid::Size, bytes))
	{
		return false;
	}
	std::memcpy(Out.Bytes, bytes.data(), SGuid::Size);
	return true;
}

bool ReadFormat (binary::CByteReader& Reader) noexcept
{
	uint8 format = 0u;
	return Reader.ReadU8(format) && format == PayloadFormat;
}

// Begin: format, op id, verb, planned GUIDs, add files, step count.
std::vector<uint8> EncodeBeginPayload (const SOpJournalEntry& Entry)
{
	std::vector<uint8> payload;
	payload.push_back(PayloadFormat);
	AppendGuid(payload, Entry.Op.Id);
	binary::AppendText(payload, Entry.Op.Verb);
	binary::AppendVarint(payload, Entry.Op.PlannedGuids.size());
	for (const SGuid& guid : Entry.Op.PlannedGuids)
	{
		AppendGuid(payload, guid);
	}
	binary::AppendVarint(payload, Entry.AddFiles.size());
	for (const FString& file : Entry.AddFiles)
	{
		binary::AppendText(payload, file);
	}
	binary::AppendVarint(payload, Entry.Op.Steps.size());
	return payload;
}

// Step: format, op id, index, kind, path, to-path, bytes, hash.
std::vector<uint8> EncodeStepPayload (const SGuid& OpId, size_t Index, const SAssetOpStep& Step)
{
	std::vector<uint8> payload;
	payload.push_back(PayloadFormat);
	AppendGuid(payload, OpId);
	binary::AppendVarint(payload, Index);
	payload.push_back(static_cast<uint8>(Step.Kind));
	binary::AppendText(payload, Step.Path);
	binary::AppendText(payload, Step.ToPath);
	binary::AppendSized(payload, Step.Bytes);
	binary::AppendHash(payload, Step.Hash);
	return payload;
}

struct SBeginRecord
{
	SOpJournalEntry Entry;
	uint64 StepCount = 0u;
};

std::optional<SBeginRecord> DecodeBegin (std::span<const uint8> Payload)
{
	binary::CByteReader reader(Payload);
	SBeginRecord record;
	uint64 count = 0u;
	if (!ReadFormat(reader) || !ReadGuid(reader, record.Entry.Op.Id) || !reader.ReadText(record.Entry.Op.Verb) || !reader.ReadVarint(count)
		|| count > reader.GetRemaining() / SGuid::Size)
	{
		return std::nullopt;
	}
	record.Entry.Op.PlannedGuids.resize(static_cast<size_t>(count));
	for (SGuid& guid : record.Entry.Op.PlannedGuids)
	{
		if (!ReadGuid(reader, guid))
		{
			return std::nullopt;
		}
	}
	if (!reader.ReadVarint(count) || count > reader.GetRemaining())
	{
		return std::nullopt;
	}
	record.Entry.AddFiles.resize(static_cast<size_t>(count));
	for (FString& file : record.Entry.AddFiles)
	{
		if (!reader.ReadText(file))
		{
			return std::nullopt;
		}
	}
	if (!reader.ReadVarint(record.StepCount) || !reader.IsAtEnd())
	{
		return std::nullopt;
	}
	return record;
}

std::optional<SAssetOpStep> DecodeStep (std::span<const uint8> Payload, const SGuid& OpId, uint64 Index)
{
	binary::CByteReader reader(Payload);
	SGuid id;
	uint64 index = 0u;
	uint8 kind = 0u;
	SAssetOpStep step;
	std::span<const uint8> bytes;
	if (!ReadFormat(reader) || !ReadGuid(reader, id) || id != OpId || !reader.ReadVarint(index) || index != Index || !reader.ReadU8(kind)
		|| kind > static_cast<uint8>(EAssetOpStep::Trash) || !reader.ReadText(step.Path) || !reader.ReadText(step.ToPath)
		|| !reader.ReadSized(bytes) || !reader.ReadHash(step.Hash) || !reader.IsAtEnd())
	{
		return std::nullopt;
	}
	step.Kind = static_cast<EAssetOpStep>(kind);
	step.Bytes.assign(bytes.begin(), bytes.end());
	return step;
}

std::optional<SGuid> DecodeEnd (std::span<const uint8> Payload)
{
	binary::CByteReader reader(Payload);
	SGuid id;
	if (!ReadFormat(reader) || !ReadGuid(reader, id) || !reader.IsAtEnd())
	{
		return std::nullopt;
	}
	return id;
}

/**
 * The Begin frame and the step frames into Out. False, with Out partial, when a payload is past what a reader takes: such
 * a frame would read back as a torn tail, so the op that Begin reported durable would be dropped.
 */
bool EncodeBeginFrames (const SOpJournalEntry& Entry, std::vector<uint8>& Out, FString* OutError)
{
	const auto append = [&Out, OutError] (uint8 Kind, const std::vector<uint8>& Payload)
	{
		if (Payload.size() > record_frames::MaxPayloadBytes)
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Format("a frame of {} bytes is past the journal's limit of {} bytes", Payload.size(), record_frames::MaxPayloadBytes);
			}
			return false;
		}
		record_frames::AppendFrame(Out, Kind, Payload);
		return true;
	};
	if (!append(BeginKind, EncodeBeginPayload(Entry)))
	{
		return false;
	}
	for (size_t index = 0u; index < Entry.Op.Steps.size(); ++index)
	{
		if (!append(StepKind, EncodeStepPayload(Entry.Op.Id, index, Entry.Op.Steps[index])))
		{
			return false;
		}
	}
	return true;
}
}


bool FitsOpJournalFrame (const SAssetOpStep& Step) noexcept
{
	// EncodeStepPayload's layout, with every varint at its widest.
	constexpr size_t VarintBytes = 10u;
	const size_t size = 1u + SGuid::Size + VarintBytes + 1u + VarintBytes + Step.Path.View().AsBytes().size() + VarintBytes
		+ Step.ToPath.View().AsBytes().size() + VarintBytes + Step.Bytes.size() + hash::SContentHash::Size;
	return size <= record_frames::MaxPayloadBytes;
}

std::vector<uint8> EncodeOpJournalBegin (const SOpJournalEntry& Entry)
{
	std::vector<uint8> bytes;
	(void)EncodeBeginFrames(Entry, bytes, nullptr);
	return bytes;
}

std::vector<uint8> EncodeOpJournalEnd (const SGuid& OpId)
{
	std::vector<uint8> payload;
	payload.push_back(PayloadFormat);
	AppendGuid(payload, OpId);
	std::vector<uint8> bytes;
	record_frames::AppendFrame(bytes, EndKind, payload);
	return bytes;
}

/**
 * Frames in order. An op is a Begin, then exactly its steps by index, then an End with its id. Anything that breaks that
 * shape (a frame that fails its checksum, a step of another op or out of order, an End of another op) ends the reading
 * there: the log is appended by one writer, so whatever follows a bad frame is the torn tail of a crash, and an op whose
 * steps were not all logged had not started touching the disk.
 */
bool ReadOpJournal (const std::filesystem::path& File, SOpJournalContents& Out, FString* OutError)
{
	Out = SOpJournalContents();
	std::error_code ec;
	if (!std::filesystem::exists(File, ec))
	{
		return true;
	}
	std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(File, OutError);
	if (!bytes.has_value())
	{
		return false;
	}
	Out.bEmpty = bytes->empty();

	std::optional<SBeginRecord> open;
	size_t offset = 0u;
	while (offset < bytes->size())
	{
		const std::optional<record_frames::SFrame> frame = record_frames::ReadFrame(*bytes, offset);
		if (!frame.has_value())
		{
			break;
		}
		bool bShaped = false;
		if (frame->Kind == BeginKind)
		{
			open = DecodeBegin(frame->Payload);
			bShaped = open.has_value();
		}
		else if (frame->Kind == StepKind && open.has_value() && open->Entry.Op.Steps.size() < open->StepCount)
		{
			std::optional<SAssetOpStep> step = DecodeStep(frame->Payload, open->Entry.Op.Id, open->Entry.Op.Steps.size());
			if (step.has_value())
			{
				open->Entry.Op.Steps.push_back(std::move(*step));
				bShaped = true;
			}
		}
		else if (frame->Kind == EndKind && open.has_value())
		{
			const std::optional<SGuid> id = DecodeEnd(frame->Payload);
			if (id.has_value() && *id == open->Entry.Op.Id)
			{
				open.reset();
				bShaped = true;
			}
		}
		if (!bShaped)
		{
			break;
		}
		offset += frame->Size;
	}

	Out.bDroppedTail = offset < bytes->size();
	if (open.has_value())
	{
		if (open->Entry.Op.Steps.size() == open->StepCount)
		{
			Out.Unfinished = std::move(open->Entry);
		}
		else
		{
			Out.bDroppedTail = true;
		}
	}
	return true;
}


COpJournal::COpJournal (std::filesystem::path InFile)
	: File(std::move(InFile))
{
}

bool COpJournal::Begin (const SOpJournalEntry& Entry, FString* OutError)
{
	std::vector<uint8> bytes;
	if (!EncodeBeginFrames(Entry, bytes, OutError))
	{
		return false;
	}
	if (!Writer.IsOpen() && !Writer.Open(File, true, OutError))
	{
		return false;
	}
	return Writer.Append(bytes, OutError) && Writer.Flush(OutError);
}

bool COpJournal::End (const SGuid& OpId, FString* OutError)
{
	if (!Writer.IsOpen() && !Writer.Open(File, true, OutError))
	{
		return false;
	}
	const std::vector<uint8> bytes = EncodeOpJournalEnd(OpId);
	if (!Writer.Append(bytes, OutError) || !Writer.Flush(OutError))
	{
		return false;
	}
	return Clear(OutError);
}

bool COpJournal::Clear (FString* OutError)
{
	Writer.Close();
	return platform::RemoveFile(File, OutError);
}
}
