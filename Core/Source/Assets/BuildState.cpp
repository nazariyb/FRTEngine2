#include "Assets/BuildState.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "Assets/ByteStream.h"
#include "Assets/KeyWriter.h"
#include "Assets/RecordFrames.h"
#include "Log/Log.h"
#include "Platform/Timestamp.h"
#include "Profiler/CpuProfiler.h"


namespace frt::assets
{
namespace
{
using binary::AppendHash;
using binary::AppendLittleEndian;
using binary::AppendRaw;
using binary::AppendText;
using binary::AppendVarint;
using binary::CByteReader;

using record_frames::AppendFrame;
using record_frames::FindNextMagic;
using record_frames::MaxPayloadBytes;
using record_frames::ReadFrame;
using record_frames::SFrame;

constexpr uint8 LogHeader[8] = { 'F', 'R', 'T', 'B', 'S', 'L', 'G', '1' };

constexpr uint8 KindStep = 1u;
constexpr uint8 KindKeyBlob = 2u;


// ---------------- Records ----------------

// Records are framed by Assets/RecordFrames.h (AppendFrame, ReadFrame).

// The step's four fields as a step record starts: what the index hashes.
void AppendStepKey (std::vector<uint8>& Out, const SBuildStepId& Step)
{
	AppendText(Out, Step.Asset);
	AppendText(Out, Step.Builder);
	AppendText(Out, Step.Label);
	AppendText(Out, Step.Target);
}

std::vector<uint8> EncodeStepKey (const SBuildStepId& Step)
{
	std::vector<uint8> bytes;
	AppendStepKey(bytes, Step);
	return bytes;
}

// Byte order of the four fields, field by field: the order ForEachStep and Compact use.
bool IsStepBefore (const SBuildStepId& A, const SBuildStepId& B) noexcept
{
	for (const auto& [x, y] : { std::pair<FStringView, FStringView>(A.Asset, B.Asset), std::pair<FStringView, FStringView>(A.Builder, B.Builder),
		std::pair<FStringView, FStringView>(A.Label, B.Label), std::pair<FStringView, FStringView>(A.Target, B.Target) })
	{
		if (const int32 order = binary::CompareBytes(x, y); order != 0)
		{
			return order < 0;
		}
	}
	return false;
}

// Entry as a step payload, with Diagnostics in place of Entry.Diagnostics (FitDiagnostics' choice).
std::vector<uint8> EncodeStepPayload (const SBuildStateEntry& Entry, const hash::SContentHash& StaticKey,
	std::span<const FString> Diagnostics)
{
	std::vector<uint8> payload;
	AppendStepKey(payload, Entry.Step);
	AppendHash(payload, StaticKey);
	AppendHash(payload, Entry.Key);
	payload.push_back(static_cast<uint8>(Entry.Status));
	AppendVarint(payload, Entry.Inputs.size());
	for (const SDdcInput& input : Entry.Inputs)
	{
		AppendText(payload, input.Kind);
		AppendText(payload, input.Name);
		AppendHash(payload, input.Hash);
		AppendVarint(payload, input.Size);
	}
	AppendVarint(payload, Diagnostics.size());
	for (const FString& line : Diagnostics)
	{
		AppendText(payload, line);
	}
	return payload;
}

/**
 * A step record is a record like any other: a reader takes a payload past MaxPayloadBytes for garbage,
 * so one that large would be written, indexed and then lost as a miss. Diagnostics are the only field
 * that grows without bound (a builder's log, a validator's findings), so they give way: the first lines
 * that fit are kept whole, then one line says how many were left out. BaseBytes is the payload without
 * diagnostics and without their count.
 */
std::vector<FString> FitDiagnostics (std::span<const FString> Lines, size_t BaseBytes)
{
	// Room for the count (at most 10 bytes) and the closing line (well under 128).
	constexpr size_t Reserve = 10u + 128u;
	std::vector<FString> kept;
	if (BaseBytes + Reserve > MaxPayloadBytes)
	{
		return kept;
	}
	const size_t budget = MaxPayloadBytes - BaseBytes - Reserve;
	size_t used = 0u;
	size_t count = 0u;
	for (; count < Lines.size(); ++count)
	{
		const size_t cost = static_cast<size_t>(binary::GetVarintSize(Lines[count].Len())) + Lines[count].Len();
		if (cost > budget - used)
		{
			break;
		}
		used += cost;
	}
	kept.assign(Lines.begin(), Lines.begin() + static_cast<ptrdiff_t>(count));
	kept.push_back(FString::Format("... {} more diagnostic lines left out: a build state record holds at most {} MiB", Lines.size() - count,
		MaxPayloadBytes / (1024u * 1024u)));
	return kept;
}

// The step fields at the start of a step payload, as the index hashes them; nullopt if malformed.
std::optional<std::span<const uint8>> ReadStepKey (std::span<const uint8> Payload)
{
	CByteReader reader(Payload);
	std::span<const uint8> field;
	for (uint32 i = 0u; i < 4u; ++i)
	{
		if (!reader.ReadSized(field))
		{
			return std::nullopt;
		}
	}
	return Payload.first(reader.GetPosition());
}

std::optional<SBuildStateEntry> DecodeStepPayload (std::span<const uint8> Payload)
{
	CByteReader reader(Payload);
	SBuildStateEntry entry;
	uint8 status = 0u;
	uint64 inputCount = 0u;
	if (!reader.ReadText(entry.Step.Asset) || !reader.ReadText(entry.Step.Builder) || !reader.ReadText(entry.Step.Label)
		|| !reader.ReadText(entry.Step.Target) || !reader.ReadHash(entry.StaticKey) || !reader.ReadHash(entry.Key) || !reader.ReadU8(status)
		|| status > static_cast<uint8>(EBuildStatus::Crashed) || !reader.ReadVarint(inputCount) || inputCount > reader.GetRemaining() / 23u)
	{
		return std::nullopt;
	}
	entry.Status = static_cast<EBuildStatus>(status);
	entry.Inputs.resize(static_cast<size_t>(inputCount));
	for (SDdcInput& input : entry.Inputs)
	{
		if (!reader.ReadText(input.Kind) || !reader.ReadText(input.Name) || !reader.ReadHash(input.Hash) || !reader.ReadVarint(input.Size))
		{
			return std::nullopt;
		}
	}
	uint64 diagnosticCount = 0u;
	if (!reader.ReadVarint(diagnosticCount) || diagnosticCount > reader.GetRemaining())
	{
		return std::nullopt;
	}
	entry.Diagnostics.resize(static_cast<size_t>(diagnosticCount));
	for (FString& line : entry.Diagnostics)
	{
		if (!reader.ReadText(line))
		{
			return std::nullopt;
		}
	}
	if (!reader.IsAtEnd())
	{
		return std::nullopt;
	}
	return entry;
}

// A KeyBlob record's blob, if its StaticKey is the blob's.
std::optional<std::vector<uint8>> DecodeKeyBlobPayload (std::span<const uint8> Payload, hash::SContentHash* OutStaticKey = nullptr)
{
	CByteReader reader(Payload);
	hash::SContentHash staticKey;
	if (!reader.ReadHash(staticKey))
	{
		return std::nullopt;
	}
	const std::span<const uint8> blob = Payload.subspan(reader.GetPosition());
	if (MakeStaticKey(blob) != staticKey)
	{
		return std::nullopt;
	}
	if (OutStaticKey != nullptr)
	{
		*OutStaticKey = staticKey;
	}
	return std::vector<uint8>(blob.begin(), blob.end());
}

bool IsHeaderValid (std::span<const uint8> Bytes) noexcept
{
	return Bytes.size() >= sizeof(LogHeader) && std::memcmp(Bytes.data(), LogHeader, sizeof(LogHeader)) == 0;
}


// ---------------- The step's text form ----------------

bool IsEscaped (char Ch) noexcept
{
	const uint8 byte = static_cast<uint8>(Ch);
	return Ch == '%' || Ch == '|' || byte <= 0x20u || byte == 0x7Fu;
}

void AppendEscaped (FString& Out, FStringView Field)
{
	for (const char ch : Field)
	{
		if (IsEscaped(ch))
		{
			Out.AppendFormat("%{:02X}", static_cast<uint8>(ch));
		}
		else
		{
			Out += ch;
		}
	}
}

std::optional<FString> Unescape (FStringView Field)
{
	FString out;
	for (uint32 i = 0u; i < Field.Len(); ++i)
	{
		const char ch = Field[i];
		if (ch != '%')
		{
			if (IsEscaped(ch))
			{
				return std::nullopt;
			}
			out += ch;
			continue;
		}
		uint8 byte = 0u;
		if (i + 2u >= Field.Len() || !Field.Mid(i + 1u, 2u).DecodeHex(std::span<uint8>(&byte, 1u)))
		{
			return std::nullopt;
		}
		out += static_cast<char>(byte);
		i += 2u;
	}
	return out;
}

// UTC now as 2026-10-24T12:34:56Z.
FString FormatUtcNow ()
{
	const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
	const auto day = std::chrono::floor<std::chrono::days>(now);
	const std::chrono::year_month_day date{ day };
	const std::chrono::hh_mm_ss<std::chrono::seconds> time{ now - day };
	return FString::Format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int32>(date.year()), static_cast<uint32>(date.month()),
		static_cast<uint32>(date.day()), time.hours().count(), time.minutes().count(), time.seconds().count());
}
}


std::filesystem::path GetBuildStatePath (const std::filesystem::path& MountRoot)
{
	return GetMountAssetsLocalDir(MountRoot) / BuildStateFileName.ToPath();
}

std::filesystem::path GetBuildJournalPath (const std::filesystem::path& MountRoot)
{
	return GetMountAssetsLocalDir(MountRoot) / BuildJournalFileName.ToPath();
}

FStringLiteral GetBuildStatusName (EBuildStatus Status) noexcept
{
	switch (Status)
	{
	case EBuildStatus::Ok: return "Ok";
	case EBuildStatus::Failed: return "Failed";
	case EBuildStatus::Crashed: return "Crashed";
	}
	return "";
}

std::optional<EBuildStatus> ParseBuildStatus (FStringView Name) noexcept
{
	for (const EBuildStatus status : { EBuildStatus::Ok, EBuildStatus::Failed, EBuildStatus::Crashed })
	{
		if (Name == GetBuildStatusName(status))
		{
			return status;
		}
	}
	return std::nullopt;
}


// ---------------- SBuildStepId ----------------

FString SBuildStepId::ToString () const
{
	FString text;
	AppendEscaped(text, Asset);
	text += '|';
	AppendEscaped(text, Builder);
	text += '|';
	AppendEscaped(text, Label);
	text += '|';
	AppendEscaped(text, Target);
	return text;
}

std::optional<SBuildStepId> SBuildStepId::Parse (FStringView Text)
{
	FStringView fields[4];
	FStringView rest = Text;
	for (uint32 i = 0u; i < 3u; ++i)
	{
		const uint32 bar = rest.Find('|');
		if (bar == FStringView::NotFound)
		{
			return std::nullopt;
		}
		fields[i] = rest.Left(bar);
		rest = rest.RightChop(bar + 1u);
	}
	if (rest.Contains('|'))
	{
		return std::nullopt;
	}
	fields[3] = rest;

	std::optional<FString> asset = Unescape(fields[0]);
	std::optional<FString> builder = Unescape(fields[1]);
	std::optional<FString> label = Unescape(fields[2]);
	std::optional<FString> target = Unescape(fields[3]);
	if (!asset || !builder || !label || !target)
	{
		return std::nullopt;
	}
	return SBuildStepId{ std::move(*asset), std::move(*builder), std::move(*label), std::move(*target) };
}


// ---------------- CBuildStateLog ----------------

CBuildStateLog::SStepHash CBuildStateLog::HashStep (std::span<const uint8> StepKey) noexcept
{
	// Two independent XXH3 seeds make 128 bits ("FRTSTEP1", "FRTSTEP2" read as le64).
	return SStepHash{ hash::Xxh3_64(StepKey, 0x3150455453545246ull), hash::Xxh3_64(StepKey, 0x3250455453545246ull) };
}

CBuildStateLog::~CBuildStateLog ()
{
	Close();
}

bool CBuildStateLog::Open (const std::filesystem::path& File, FString* OutError)
{
	const std::lock_guard lock(Mutex);
	Writer.Close();
	Reader.Close();
	Steps.clear();
	KeyBlobs.clear();
	bIndexLoaded = false;
	Stats = SBuildStateLogStats();
	Path = File;

	// A missing, empty or foreign file starts anew with the header: the log is rebuildable.
	std::vector<uint8> header(sizeof(LogHeader));
	platform::CBlockingFileReader probe;
	const bool bValid = probe.Open(File) && probe.GetSize() >= sizeof(LogHeader) && probe.ReadExactAt(0u, header) && IsHeaderValid(header);
	probe.Close();
	if (!bValid)
	{
		if (std::optional<platform::SFileStat> stat = platform::Stat(File); stat && stat->Size > 0u)
		{
			log::Warning(log::Assets, "'{}' is not a build state log of this format: starting it anew", FString::FromPath(File));
		}
		if (!platform::WriteFileAtomic(File, LogHeader, true, OutError))
		{
			Path.clear();
			return false;
		}
	}
	if (!OpenHandlesLocked(OutError))
	{
		Path.clear();
		return false;
	}
	return true;
}

bool CBuildStateLog::OpenHandlesLocked (FString* OutError)
{
	if (!Writer.Open(Path, true, OutError) || !Reader.Open(Path, OutError))
	{
		Writer.Close();
		Reader.Close();
		return false;
	}
	return true;
}

void CBuildStateLog::Close ()
{
	const std::lock_guard lock(Mutex);
	Writer.Close();
	Reader.Close();
	Steps.clear();
	KeyBlobs.clear();
	bIndexLoaded = false;
	Path.clear();
}

bool CBuildStateLog::IsOpen () const
{
	const std::lock_guard lock(Mutex);
	return Writer.IsOpen();
}

std::filesystem::path CBuildStateLog::GetPath () const
{
	const std::lock_guard lock(Mutex);
	return Path;
}

bool CBuildStateLog::LoadIndex (FString* OutError)
{
	const std::lock_guard lock(Mutex);
	return LoadIndexLocked(OutError);
}

bool CBuildStateLog::LoadIndexLocked (FString* OutError)
{
	if (bIndexLoaded)
	{
		return true;
	}
	if (!Writer.IsOpen())
	{
		if (OutError != nullptr)
		{
			*OutError = "The build state log is not open";
		}
		return false;
	}
	FRT_CPU_SCOPE("Assets.BuildState.LoadIndex");
	const uint64 start = platform::ReadTimestamp();
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(Path, OutError);
	if (!bytes)
	{
		return false;
	}
	IndexBytes(*bytes);
	bIndexLoaded = true;
	Stats.IndexLoadMs = platform::TimestampToMilliseconds(platform::ReadTimestamp() - start);
	return true;
}

void CBuildStateLog::IndexBytes (std::span<const uint8> Bytes)
{
	Steps.clear();
	KeyBlobs.clear();
	const double indexLoadMs = Stats.IndexLoadMs;
	Stats = SBuildStateLogStats();
	Stats.IndexLoadMs = indexLoadMs;
	FileSize = Bytes.size();

	// Records run to some 150 bytes: room for all of them up front, so the map never rehashes.
	Steps.reserve(Bytes.size() / 128u);
	size_t offset = IsHeaderValid(Bytes) ? sizeof(LogHeader) : 0u;
	while (offset < Bytes.size())
	{
		const std::optional<SFrame> frame = ReadFrame(Bytes, offset);
		if (!frame)
		{
			// Torn or corrupt: skip to the next magic number (a later record may follow a torn one).
			const size_t next = FindNextMagic(Bytes, offset);
			++Stats.BadRanges;
			offset = next;
			continue;
		}
		const SLocation location{ offset, static_cast<uint32>(frame->Size) };
		if (frame->Kind == KindStep)
		{
			if (const std::optional<std::span<const uint8>> stepKey = ReadStepKey(frame->Payload))
			{
				Steps.insert_or_assign(HashStep(*stepKey), location);
				++Stats.Records;
			}
		}
		else if (frame->Kind == KindKeyBlob)
		{
			CByteReader reader(frame->Payload);
			hash::SContentHash staticKey;
			if (reader.ReadHash(staticKey))
			{
				KeyBlobs.try_emplace(staticKey, location);
				++Stats.Records;
			}
		}
		offset += frame->Size;
	}
}

std::optional<std::vector<uint8>> CBuildStateLog::ReadRecordLocked (const SLocation& Location, uint8 ExpectedKind)
{
	std::vector<uint8> bytes(Location.Size);
	if (!Reader.IsOpen() || !Reader.ReadExactAt(Location.Offset, bytes))
	{
		return std::nullopt;
	}
	const std::optional<SFrame> frame = ReadFrame(bytes, 0u);
	if (!frame || frame->Kind != ExpectedKind || frame->Size != bytes.size())
	{
		return std::nullopt;
	}
	return std::vector<uint8>(frame->Payload.begin(), frame->Payload.end());
}

bool CBuildStateLog::Append (const SBuildStateEntry& Entry, FString* OutError)
{
	const std::lock_guard lock(Mutex);
	if (!LoadIndexLocked(OutError))
	{
		return false;
	}

	const auto fail = [OutError] (FString Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};

	// Every record within MaxPayloadBytes, which is what readers accept (and what its u32 size holds).
	if (hash::SContentHash::Size + Entry.KeyBlob.size() > MaxPayloadBytes)
	{
		return fail(FString::Format("The KeyBlob of build step {} is {} bytes; a build state record holds at most {}", Entry.Step.ToString(),
			Entry.KeyBlob.size(), MaxPayloadBytes));
	}
	const hash::SContentHash staticKey = Entry.KeyBlob.empty() ? Entry.StaticKey : MakeStaticKey(Entry.KeyBlob);
	std::vector<uint8> stepPayload = EncodeStepPayload(Entry, staticKey, Entry.Diagnostics);
	if (stepPayload.size() > MaxPayloadBytes)
	{
		const size_t baseBytes = EncodeStepPayload(Entry, staticKey, {}).size() - 1u;   // less the count of zero lines
		const std::vector<FString> diagnostics = FitDiagnostics(Entry.Diagnostics, baseBytes);
		stepPayload = EncodeStepPayload(Entry, staticKey, diagnostics);
		if (stepPayload.size() > MaxPayloadBytes)
		{
			return fail(FString::Format("Build step {} has {} discovered inputs, more than a build state record of at most {} bytes holds",
				Entry.Step.ToString(), Entry.Inputs.size(), MaxPayloadBytes));
		}
		const size_t keptLines = diagnostics.empty() ? 0u : diagnostics.size() - 1u;   // less the closing line
		log::Warning(log::Assets, "build step {}: {} of its {} diagnostic lines did not fit its build state record", Entry.Step.ToString(),
			Entry.Diagnostics.size() - keptLines, Entry.Diagnostics.size());
	}

	std::vector<uint8> bytes;
	std::optional<SLocation> blobLocation;
	if (!Entry.KeyBlob.empty() && !KeyBlobs.contains(staticKey))
	{
		std::vector<uint8> payload;
		AppendHash(payload, staticKey);
		AppendRaw(payload, Entry.KeyBlob);
		AppendFrame(bytes, KindKeyBlob, payload);
		blobLocation = SLocation{ FileSize, static_cast<uint32>(bytes.size()) };
	}
	const size_t stepStart = bytes.size();
	AppendFrame(bytes, KindStep, stepPayload);

	// One write: a crash tears at most the end of it, which the reader skips.
	if (!Writer.Append(bytes, OutError))
	{
		// Whatever part landed is garbage the reader skips; carry on after it.
		FileSize = Writer.GetSize().value_or(FileSize);
		return false;
	}
	if (blobLocation)
	{
		KeyBlobs.insert_or_assign(staticKey, *blobLocation);
		++Stats.Records;
	}
	Steps.insert_or_assign(HashStep(EncodeStepKey(Entry.Step)), SLocation{ FileSize + stepStart, static_cast<uint32>(bytes.size() - stepStart) });
	++Stats.Records;
	FileSize += bytes.size();
	return true;
}

std::optional<SBuildStateEntry> CBuildStateLog::FindLocked (const SBuildStepId& Step, EBuildStateRead Read)
{
	const auto found = Steps.find(HashStep(EncodeStepKey(Step)));
	if (found == Steps.end())
	{
		return std::nullopt;
	}
	const std::optional<std::vector<uint8>> payload = ReadRecordLocked(found->second, KindStep);
	std::optional<SBuildStateEntry> entry = payload ? DecodeStepPayload(*payload) : std::nullopt;
	if (!entry)
	{
		// Changed underneath since the index was built (it never should): a miss from now on.
		Steps.erase(found);
		return std::nullopt;
	}
	if (entry->Step != Step)
	{
		return std::nullopt;   // another step with the same 128-bit hash: a miss, never its record
	}
	if (Read == EBuildStateRead::WithKeyBlob)
	{
		if (const auto blob = KeyBlobs.find(entry->StaticKey); blob != KeyBlobs.end())
		{
			if (const std::optional<std::vector<uint8>> blobPayload = ReadRecordLocked(blob->second, KindKeyBlob))
			{
				entry->KeyBlob = DecodeKeyBlobPayload(*blobPayload).value_or(std::vector<uint8>());
			}
		}
	}
	return entry;
}

std::optional<SBuildStateEntry> CBuildStateLog::Find (const SBuildStepId& Step, EBuildStateRead Read)
{
	const std::lock_guard lock(Mutex);
	if (!LoadIndexLocked(nullptr))
	{
		return std::nullopt;
	}
	return FindLocked(Step, Read);
}

std::optional<std::vector<uint8>> CBuildStateLog::FindKeyBlob (const hash::SContentHash& StaticKey)
{
	const std::lock_guard lock(Mutex);
	if (!LoadIndexLocked(nullptr))
	{
		return std::nullopt;
	}
	const auto found = KeyBlobs.find(StaticKey);
	if (found == KeyBlobs.end())
	{
		return std::nullopt;
	}
	const std::optional<std::vector<uint8>> payload = ReadRecordLocked(found->second, KindKeyBlob);
	return payload ? DecodeKeyBlobPayload(*payload) : std::nullopt;
}

std::vector<std::pair<SBuildStateEntry, CBuildStateLog::SLocation>> CBuildStateLog::CollectStepsLocked ()
{
	std::vector<std::pair<SBuildStateEntry, SLocation>> steps;
	steps.reserve(Steps.size());
	for (const auto& [stepHash, location] : Steps)
	{
		const std::optional<std::vector<uint8>> payload = ReadRecordLocked(location, KindStep);
		if (std::optional<SBuildStateEntry> entry = payload ? DecodeStepPayload(*payload) : std::nullopt)
		{
			steps.emplace_back(std::move(*entry), location);
		}
	}
	std::sort(steps.begin(), steps.end(), [] (const auto& A, const auto& B) { return IsStepBefore(A.first.Step, B.first.Step); });
	return steps;
}

void CBuildStateLog::ForEachStep (TFunctionRef<void (const SBuildStateEntry&)> Visit)
{
	std::vector<std::pair<SBuildStateEntry, SLocation>> steps;
	{
		const std::lock_guard lock(Mutex);
		if (!LoadIndexLocked(nullptr))
		{
			return;
		}
		steps = CollectStepsLocked();
	}
	// Outside the lock: the visitor may call back into the log.
	for (const auto& [entry, location] : steps)
	{
		Visit(entry);
	}
}

std::vector<hash::SContentHash> CBuildStateLog::CollectKeys ()
{
	std::vector<hash::SContentHash> keys;
	ForEachStep([&keys] (const SBuildStateEntry& Entry) { keys.push_back(Entry.Key); });
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return keys;
}

std::optional<std::vector<hash::SContentHash>> CBuildStateLog::ReadKeys (const std::filesystem::path& File,
	FString* OutError)
{
	const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(File, OutError);
	if (!bytes)
	{
		return std::nullopt;
	}
	if (!IsHeaderValid(*bytes))
	{
		if (OutError != nullptr)
		{
			*OutError = "not a build state log of this format";
		}
		return std::nullopt;
	}
	// An index of the bytes in memory, as LoadIndex builds one: the latest record of each step. The log
	// object opens no file.
	CBuildStateLog index;
	index.IndexBytes(*bytes);
	std::vector<hash::SContentHash> keys;
	keys.reserve(index.Steps.size());
	for (const auto& [stepHash, location] : index.Steps)
	{
		const std::optional<SFrame> frame = ReadFrame(*bytes, location.Offset);
		if (frame && frame->Kind == KindStep)
		{
			if (const std::optional<SBuildStateEntry> entry = DecodeStepPayload(frame->Payload))
			{
				keys.push_back(entry->Key);
			}
		}
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return keys;
}

bool CBuildStateLog::Compact (FString* OutError)
{
	FRT_CPU_SCOPE("Assets.BuildState.Compact");
	const std::lock_guard lock(Mutex);
	if (!LoadIndexLocked(OutError))
	{
		return false;
	}
	const std::optional<std::vector<uint8>> old = platform::ReadFileBytes(Path, OutError);
	if (!old)
	{
		return false;
	}

	// Steps in byte order of their fields (decoded from the copy in memory: one read, not one per
	// step); each KeyBlob just before the first step that names it.
	std::vector<std::pair<SBuildStateEntry, SLocation>> steps;
	steps.reserve(Steps.size());
	for (const auto& [stepHash, location] : Steps)
	{
		const std::optional<SFrame> frame = location.Offset + location.Size <= old->size() ? ReadFrame(*old, location.Offset) : std::nullopt;
		if (frame && frame->Kind == KindStep)
		{
			if (std::optional<SBuildStateEntry> entry = DecodeStepPayload(frame->Payload))
			{
				steps.emplace_back(std::move(*entry), location);
			}
		}
	}
	std::sort(steps.begin(), steps.end(), [] (const auto& A, const auto& B) { return IsStepBefore(A.first.Step, B.first.Step); });

	std::vector<uint8> bytes(LogHeader, LogHeader + sizeof(LogHeader));
	std::unordered_set<hash::SContentHash, hash::SContentHashHasher> written;
	for (const auto& [entry, location] : steps)
	{
		if (const auto blob = KeyBlobs.find(entry.StaticKey); blob != KeyBlobs.end() && written.insert(entry.StaticKey).second)
		{
			const SLocation& blobLocation = blob->second;
			if (blobLocation.Offset + blobLocation.Size <= old->size() && ReadFrame(*old, blobLocation.Offset))
			{
				AppendRaw(bytes, std::span<const uint8>(*old).subspan(static_cast<size_t>(blobLocation.Offset), blobLocation.Size));
			}
		}
		AppendRaw(bytes, std::span<const uint8>(*old).subspan(static_cast<size_t>(location.Offset), location.Size));
	}

	Writer.Close();
	Reader.Close();
	const bool bWritten = platform::WriteFileAtomic(Path, bytes, true, OutError);
	// Reopen either way: after a failure the old log is still there, intact.
	if (!OpenHandlesLocked(bWritten ? OutError : nullptr))
	{
		bIndexLoaded = false;
		return false;
	}
	if (!bWritten)
	{
		return false;
	}
	IndexBytes(bytes);
	return true;
}

SBuildStateLogStats CBuildStateLog::GetStats ()
{
	const std::lock_guard lock(Mutex);
	LoadIndexLocked(nullptr);
	SBuildStateLogStats stats = Stats;
	stats.FileBytes = FileSize;
	stats.Steps = static_cast<uint32>(Steps.size());
	stats.KeyBlobs = static_cast<uint32>(KeyBlobs.size());

	// Live: the header, every step's latest record and the blobs those name (read to know which).
	uint64 live = sizeof(LogHeader);
	std::unordered_set<hash::SContentHash, hash::SContentHashHasher> named;
	for (const auto& [key, location] : Steps)
	{
		live += location.Size;
		if (const std::optional<std::vector<uint8>> payload = ReadRecordLocked(location, KindStep))
		{
			CByteReader reader(*payload);
			std::span<const uint8> field;
			hash::SContentHash staticKey;
			if (reader.ReadSized(field) && reader.ReadSized(field) && reader.ReadSized(field) && reader.ReadSized(field)
				&& reader.ReadHash(staticKey) && KeyBlobs.contains(staticKey) && named.insert(staticKey).second)
			{
				live += KeyBlobs.at(staticKey).Size;
			}
		}
	}
	stats.DeadBytes = stats.FileBytes > live ? stats.FileBytes - live : 0u;
	return stats;
}


// ---------------- CBuildJournal ----------------

CBuildJournal::~CBuildJournal ()
{
	Close();
}

bool CBuildJournal::Open (const std::filesystem::path& File, FString* OutError)
{
	const std::lock_guard lock(Mutex);
	Writer.Close();
	Crashed.clear();
	Path = File;

	// B lines without a later E, by step; a last line without its line break is torn: ignored.
	std::vector<std::pair<FString, FString>> open;   // step text -> its B line
	if (const std::optional<std::vector<uint8>> bytes = platform::ReadFileBytes(File, nullptr))
	{
		FStringView text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		while (!text.IsEmpty())
		{
			const uint32 end = text.Find('\n');
			if (end == FStringView::NotFound)
			{
				break;
			}
			const FStringView line = text.Left(end).TrimEnd();
			text = text.RightChop(end + 1u);

			FStringView rest = line;
			const FStringView kind = rest.ConsumeToken(' ');
			const FStringView step = rest.ConsumeToken(' ');
			if (step.IsEmpty() || !SBuildStepId::Parse(step))
			{
				continue;
			}
			const auto found = std::find_if(open.begin(), open.end(), [&step] (const auto& Entry) { return Entry.first == step; });
			if (kind == "B")
			{
				if (found == open.end())
				{
					open.emplace_back(FString(step), FString(line));
				}
				else
				{
					found->second = line;
				}
			}
			else if (kind == "E" && found != open.end())
			{
				open.erase(found);
			}
		}
	}

	std::sort(open.begin(), open.end(), [] (const auto& A, const auto& B) { return A.first < B.first; });
	FString kept;
	for (const auto& [step, line] : open)
	{
		Crashed.push_back(step);
		kept += line;
		kept += '\n';
	}
	if (!platform::WriteFileAtomic(File, kept.AsBytes(), true, OutError) || !Writer.Open(File, true, OutError))
	{
		Writer.Close();
		Path.clear();
		return false;
	}
	for (const FString& step : Crashed)
	{
		log::Warning(log::Assets, "build step {} crashed its process last time: skipped until it is retried", step);
	}
	return true;
}

void CBuildJournal::Close ()
{
	const std::lock_guard lock(Mutex);
	Writer.Close();
	Crashed.clear();
	Path.clear();
}

bool CBuildJournal::IsOpen () const
{
	const std::lock_guard lock(Mutex);
	return Writer.IsOpen();
}

std::vector<SBuildStepId> CBuildJournal::GetCrashedSteps () const
{
	const std::lock_guard lock(Mutex);
	std::vector<SBuildStepId> steps;
	for (const FString& text : Crashed)
	{
		if (std::optional<SBuildStepId> step = SBuildStepId::Parse(text))
		{
			steps.push_back(std::move(*step));
		}
	}
	return steps;
}

bool CBuildJournal::IsCrashed (const SBuildStepId& Step) const
{
	const FString text = Step.ToString();
	const std::lock_guard lock(Mutex);
	return std::binary_search(Crashed.begin(), Crashed.end(), text);
}

bool CBuildJournal::AppendLineLocked (FStringView Line, FString* OutError)
{
	if (!Writer.IsOpen())
	{
		if (OutError != nullptr)
		{
			*OutError = "The build journal is not open";
		}
		return false;
	}
	return Writer.Append(Line.AsBytes(), OutError);
}

void CBuildJournal::ClearCrashedLocked (const FString& StepText)
{
	const auto found = std::lower_bound(Crashed.begin(), Crashed.end(), StepText);
	if (found != Crashed.end() && *found == StepText)
	{
		Crashed.erase(found);
	}
}

bool CBuildJournal::BeginStep (const SBuildStepId& Step, FString* OutError)
{
	const FString line = FString::Format("B {} {}\n", Step.ToString(), FormatUtcNow());
	const std::lock_guard lock(Mutex);
	return AppendLineLocked(line, OutError);
}

bool CBuildJournal::EndStep (const SBuildStepId& Step, EBuildStatus Result, FString* OutError)
{
	const FString text = Step.ToString();
	const FString line = FString::Format("E {} {}\n", text, GetBuildStatusName(Result));
	const std::lock_guard lock(Mutex);
	ClearCrashedLocked(text);
	return AppendLineLocked(line, OutError);
}

bool CBuildJournal::Retry (const SBuildStepId& Step, FString* OutError)
{
	const FString text = Step.ToString();
	const FString line = FString::Format("E {} Retry\n", text);
	const std::lock_guard lock(Mutex);
	ClearCrashedLocked(text);
	return AppendLineLocked(line, OutError);
}
}
