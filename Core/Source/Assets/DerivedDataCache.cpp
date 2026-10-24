#include "Assets/DerivedDataCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "Assets/ByteStream.h"
#include "Config/Config.h"
#include "Hash/Compression.h"
#include "Log/Log.h"
#include "Platform/BlockingFileReader.h"
#include "Platform/Platform.h"
#include "Platform/Random.h"
#include "Profiler/CpuProfiler.h"
#include "Project/UserRegistry.h"


namespace frt::assets
{
namespace
{
using binary::AppendHash;
using binary::AppendLittleEndian;
using binary::AppendRaw;
using binary::AppendSized;
using binary::AppendText;
using binary::AppendVarint;
using binary::CByteReader;
using binary::CompareBytes;

constexpr uint8 ManifestMagic[4] = { 'F', 'R', 'T', 'M' };
constexpr uint8 RecordMagic[4] = { 'F', 'R', 'T', 'R' };
constexpr uint8 ValueMagic[4] = { 'F', 'R', 'T', 'V' };

// Manifests and records carry the revision FORMAT names, not versions of their own: the store is
// shared by every engine revision on the machine, so a layout change - a field, a limit such as
// DdcMaxManifestSets - is a new FORMAT, which an engine of another revision refuses to open. Two
// revisions therefore never read, judge corrupt and delete each other's files; a manifest or record
// of another revision inside this store is damage like any other.
constexpr uint32 LayoutRevision = 1u;
static_assert(DdcFormat == FStringView("FRT-DDC 1") && LayoutRevision == 1u,
	"A layout change bumps FORMAT (DdcFormat) and LayoutRevision together");
constexpr uint32 ManifestVersion = LayoutRevision;
constexpr uint32 RecordVersion = LayoutRevision;

// Value flags in a record.
constexpr uint8 ValueFlagInline = 1u;
constexpr uint8 ValueFlagZstd = 2u;

// .val codecs.
constexpr uint8 CodecNone = 0u;
constexpr uint8 CodecZstd = 1u;

// "FRTV", codec, three zero bytes, u64 RawSize, Hash RawHash.
constexpr size_t ValueHeaderBytes = 4u + 1u + 3u + 8u + hash::SContentHash::Size;
// Trailing XXH3 of manifests and records.
constexpr size_t ChecksumBytes = 8u;

constexpr uint32 MaxBucketLength = 64u;

// Garbage collection: leftovers younger than this may belong to a write in progress elsewhere.
constexpr int64 LeftoverAgeFileTicks = 3600ll * 10'000'000ll;   // one hour in 100 ns ticks
// Access logs older than this many days are deleted after a collection read them.
constexpr int64 AccessLogMaxAgeDays = 30;

// File times (100 ns ticks since 1601-01-01 UTC) of the Unix epoch.
constexpr int64 UnixEpochFileTicks = 116444736000000000ll;

constexpr FStringLiteral ManifestsDir = "Manifests";
constexpr FStringLiteral RecordsDir = "Records";
constexpr FStringLiteral ValuesDir = "Values";
constexpr FStringLiteral TmpDir = "Tmp";
constexpr FStringLiteral AccessDir = "Access";
constexpr FStringLiteral FormatFile = "FORMAT";
constexpr FStringLiteral ToolVerdictsFile = "ToolVerdicts.txt";

#if FRT_BUILD_RELEASE
constexpr bool DefaultVerifyValues = false;
#else
constexpr bool DefaultVerifyValues = true;
#endif


// ---------------- Small helpers ----------------

// The total order of ComputeKey: Name, then Kind, then Hash, then Size.
bool IsInputBefore (const SDdcInput& A, const SDdcInput& B) noexcept
{
	if (const int32 name = CompareBytes(A.Name, B.Name); name != 0)
	{
		return name < 0;
	}
	if (const int32 kind = CompareBytes(A.Kind, B.Kind); kind != 0)
	{
		return kind < 0;
	}
	if (A.Hash != B.Hash)
	{
		return A.Hash < B.Hash;
	}
	return A.Size < B.Size;
}

FString ToHex (const hash::SContentHash& Hash)
{
	return Hash.ToString();
}

// The first byte of a hash as two hex digits: the fan-out directory.
FString FanOut (const hash::SContentHash& Hash, uint32 Byte)
{
	return FString::Format("{:02x}", Hash.Bytes[Byte]);
}

int64 NowFileTime () noexcept
{
	// MSVC's file clock counts 100 ns ticks since 1601, the unit of SFileStat (FileSystem.h).
	return static_cast<int64>(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
}

int64 NowUnixMicroseconds () noexcept
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64 FileTimeToUnixMicroseconds (int64 FileTime) noexcept
{
	return (FileTime - UnixEpochFileTicks) / 10;
}

// Days since 1970-01-01 (UTC) of a time in Unix microseconds.
int64 ToUnixDay (int64 UnixMicroseconds) noexcept
{
	const auto days = std::chrono::floor<std::chrono::days>(std::chrono::sys_time<std::chrono::microseconds>(
		std::chrono::microseconds(UnixMicroseconds)));
	return days.time_since_epoch().count();
}

// "yyyymmdd" of a day number.
FString FormatDay (int64 UnixDay)
{
	const std::chrono::year_month_day date{ std::chrono::sys_days(std::chrono::days(UnixDay)) };
	return FString::Format("{:04}{:02}{:02}", static_cast<int32>(date.year()), static_cast<uint32>(date.month()),
		static_cast<uint32>(date.day()));
}

// The day number of "yyyymmdd", or nullopt.
std::optional<int64> ParseDay (FStringView Text)
{
	if (Text.Len() != 8u || !std::all_of(Text.begin(), Text.end(), [] (char Ch) { return Ch >= '0' && Ch <= '9'; }))
	{
		return std::nullopt;
	}
	const std::optional<int32> year = Text.Left(4u).TryParse<int32>();
	const std::optional<uint32> month = Text.Mid(4u, 2u).TryParse<uint32>();
	const std::optional<uint32> day = Text.Right(2u).TryParse<uint32>();
	if (!year || !month || !day)
	{
		return std::nullopt;
	}
	const std::chrono::year_month_day date{ std::chrono::year(*year), std::chrono::month(*month), std::chrono::day(*day) };
	if (!date.ok())
	{
		return std::nullopt;
	}
	return std::chrono::sys_days(date).time_since_epoch().count();
}

// "<40 hex digits>.<Extension>" -> the hash; nullopt for any other name.
std::optional<hash::SContentHash> ParseHashFileName (FStringView Name, FStringView Extension)
{
	if (Name.Len() != hash::SContentHash::TextLength + 1u + Extension.Len() || Name[hash::SContentHash::TextLength] != '.'
		|| !Name.EndsWith(Extension))
	{
		return std::nullopt;
	}
	return hash::SContentHash::Parse(Name.Left(hash::SContentHash::TextLength));
}


// ---------------- Whole-file reads ----------------

enum class EReadResult : uint8
{
	Ok,
	Missing,   // no such file: a plain miss
	Failed,    // could not be read now (in use, no access): a miss, but nothing to delete
};

EReadResult ReadWholeFile (const std::filesystem::path& Path, std::vector<uint8>& Out)
{
	platform::CBlockingFileReader reader;
	if (!reader.Open(Path))
	{
		return reader.GetLastFailure() == platform::EFileFailure::NotFound ? EReadResult::Missing : EReadResult::Failed;
	}
	Out.resize(static_cast<size_t>(reader.GetSize()));
	if (!Out.empty() && !reader.ReadExactAt(0u, Out))
	{
		return EReadResult::Failed;
	}
	return EReadResult::Ok;
}

// EnumerateDirectory's result, except that a directory that does not exist (or no longer does) is
// listed completely: it has no entries. False only when one that exists could not be listed in full.
bool ListDirectory (const std::filesystem::path& Directory, TFunctionRef<void (const platform::SDirectoryEntry&)> OnEntry)
{
	return platform::EnumerateDirectory(Directory, [&OnEntry] (const platform::SDirectoryEntry& Entry) { OnEntry(Entry); })
		|| !platform::Stat(Directory).has_value();
}

// Calls OnFile(path, stat) for every file below Directory, depth first; links are not followed.
// False when some directory below could not be listed in full, so some files were not visited.
bool WalkFiles (const std::filesystem::path& Directory, TFunctionRef<void (const std::filesystem::path&, const platform::SFileStat&)> OnFile)
{
	std::vector<std::filesystem::path> subdirectories;
	bool bComplete = ListDirectory(Directory, [&] (const platform::SDirectoryEntry& Entry)
	{
		if (Entry.bReparsePoint)
		{
			return;
		}
		std::filesystem::path path = Directory / Entry.Name.ToPath();
		if (Entry.Stat.bDirectory)
		{
			subdirectories.push_back(std::move(path));
		}
		else
		{
			OnFile(path, Entry.Stat);
		}
	});
	for (const std::filesystem::path& subdirectory : subdirectories)
	{
		bComplete = WalkFiles(subdirectory, OnFile) && bComplete;
	}
	return bComplete;
}


// ---------------- Checksummed files ----------------

void AppendChecksum (std::vector<uint8>& Bytes)
{
	AppendLittleEndian(Bytes, hash::Xxh3_64(Bytes), 8u);
}

// The bytes before the trailing XXH3, if it matches; nullopt otherwise (a torn or zero-filled file:
// all zero bytes fail too, the XXH3 of zero bytes is not zero).
std::optional<std::span<const uint8>> CheckChecksum (std::span<const uint8> Bytes)
{
	if (Bytes.size() < ChecksumBytes)
	{
		return std::nullopt;
	}
	const std::span<const uint8> body = Bytes.first(Bytes.size() - ChecksumBytes);
	const uint64 stored = binary::ReadLittleEndianAt(Bytes, body.size(), 8u);
	if (stored != hash::Xxh3_64(body))
	{
		return std::nullopt;
	}
	return body;
}


// ---------------- Manifests ----------------

void AppendInput (std::vector<uint8>& Out, const SDdcInput& Input)
{
	AppendText(Out, Input.Kind);
	AppendText(Out, Input.Name);
	AppendHash(Out, Input.Hash);
	AppendVarint(Out, Input.Size);
}

bool ReadInput (CByteReader& Reader, SDdcInput& Out)
{
	return Reader.ReadText(Out.Kind) && Reader.ReadText(Out.Name) && Reader.ReadHash(Out.Hash) && Reader.ReadVarint(Out.Size);
}

std::vector<uint8> EncodeManifest (const hash::SContentHash& StaticKey, std::span<const SDdcManifestMatch> Sets)
{
	std::vector<uint8> bytes;
	AppendRaw(bytes, ManifestMagic);
	AppendLittleEndian(bytes, ManifestVersion, 4u);
	AppendHash(bytes, StaticKey);
	AppendVarint(bytes, Sets.size());
	for (const SDdcManifestMatch& set : Sets)
	{
		AppendHash(bytes, set.Key);
		AppendVarint(bytes, set.Inputs.size());
		for (const SDdcInput& input : set.Inputs)
		{
			AppendInput(bytes, input);
		}
	}
	AppendChecksum(bytes);
	return bytes;
}

std::optional<std::vector<SDdcManifestMatch>> ParseManifest (std::span<const uint8> Bytes, const hash::SContentHash& StaticKey)
{
	const std::optional<std::span<const uint8>> body = CheckChecksum(Bytes);
	if (!body)
	{
		return std::nullopt;
	}
	CByteReader reader(*body);
	uint64 version = 0u;
	hash::SContentHash staticKey;
	uint64 setCount = 0u;
	if (!reader.Expect(ManifestMagic) || !reader.ReadLittleEndian(version, 4u) || version != ManifestVersion
		|| !reader.ReadHash(staticKey) || staticKey != StaticKey || !reader.ReadVarint(setCount) || setCount == 0u
		|| setCount > DdcMaxManifestSets)
	{
		return std::nullopt;
	}

	std::vector<SDdcManifestMatch> sets(static_cast<size_t>(setCount));
	for (SDdcManifestMatch& set : sets)
	{
		uint64 inputCount = 0u;
		// Each input takes at least 23 bytes: no count can promise more than the bytes left.
		if (!reader.ReadHash(set.Key) || !reader.ReadVarint(inputCount) || inputCount > reader.GetRemaining() / 23u)
		{
			return std::nullopt;
		}
		set.Inputs.resize(static_cast<size_t>(inputCount));
		for (SDdcInput& input : set.Inputs)
		{
			if (!ReadInput(reader, input))
			{
				return std::nullopt;
			}
		}
	}
	if (!reader.IsAtEnd())
	{
		return std::nullopt;
	}
	return sets;
}


// ---------------- Records ----------------

// A value as it will be stored: its raw hash, codec and stored bytes.
struct SEncodedValue
{
	const SDdcValue* Source = nullptr;
	hash::SContentHash RawHash;
	uint64 RawSize = 0u;
	bool bZstd = false;
	std::vector<uint8> Compressed;   // the zstd frame, when bZstd
	bool bInline = false;

	[[nodiscard]] std::span<const uint8> GetStored () const noexcept
	{
		return bZstd ? std::span<const uint8>(Compressed) : std::span<const uint8>(Source->Bytes);
	}
};

// zstd at level 3, kept only when it saves at least 5% (§7.4): a pure function of the bytes, so every
// machine stores a value the same way.
SEncodedValue EncodeValue (const SDdcValue& Value)
{
	SEncodedValue encoded;
	encoded.Source = &Value;
	encoded.RawHash = hash::HashContent(Value.Bytes);
	encoded.RawSize = Value.Bytes.size();
	if (!Value.Bytes.empty())
	{
		std::vector<uint8> frame = compression::CompressZstd(Value.Bytes, compression::DefaultZstdLevel);
		if (!frame.empty() && static_cast<uint64>(frame.size()) * 20u <= encoded.RawSize * 19u)
		{
			encoded.bZstd = true;
			encoded.Compressed = std::move(frame);
		}
	}
	return encoded;
}

// The record's size without any inline bytes.
uint64 GetRecordBaseSize (const SDdcRecord& Record, std::span<const SEncodedValue> Values)
{
	uint64 size = 4u + 4u + hash::SContentHash::Size;
	size += binary::GetVarintSize(Record.Builder.Len()) + Record.Builder.Len();
	size += SGuid::Size + binary::GetVarintSize(Values.size());
	for (const SEncodedValue& value : Values)
	{
		const uint64 nameLength = value.Source->Name.Len();
		size += binary::GetVarintSize(nameLength) + nameLength + hash::SContentHash::Size + binary::GetVarintSize(value.RawSize) + 1u;
	}
	size += hash::SContentHash::Size + binary::GetVarintSize(Record.BuildMs) + 1u + ChecksumBytes;
	return size;
}

// Inlines every value whose stored form fits DdcMaxInlineValueBytes, then moves the largest inline
// value out (the later one of equal sizes) while the record exceeds DdcMaxRecordBytes.
void ChooseInlineValues (const SDdcRecord& Record, std::vector<SEncodedValue>& Values)
{
	uint64 size = GetRecordBaseSize(Record, Values);
	for (SEncodedValue& value : Values)
	{
		const uint64 stored = value.GetStored().size();
		value.bInline = stored <= DdcMaxInlineValueBytes;
		if (value.bInline)
		{
			size += binary::GetVarintSize(stored) + stored;
		}
	}
	while (size > DdcMaxRecordBytes)
	{
		SEncodedValue* largest = nullptr;
		for (SEncodedValue& value : Values)
		{
			if (value.bInline && (largest == nullptr || value.GetStored().size() >= largest->GetStored().size()))
			{
				largest = &value;
			}
		}
		if (largest == nullptr)
		{
			break;   // only metadata left: a record of thousands of values, written as it is
		}
		const uint64 stored = largest->GetStored().size();
		largest->bInline = false;
		size -= binary::GetVarintSize(stored) + stored;
	}
}

std::vector<uint8> EncodeRecord (const hash::SContentHash& Key, const SDdcRecord& Record, std::span<const SEncodedValue> Values)
{
	std::vector<uint8> bytes;
	bytes.reserve(static_cast<size_t>(GetRecordBaseSize(Record, Values)));
	AppendRaw(bytes, RecordMagic);
	AppendLittleEndian(bytes, RecordVersion, 4u);
	AppendHash(bytes, Key);
	AppendText(bytes, Record.Builder);
	AppendRaw(bytes, std::span<const uint8>(Record.Version.Bytes));
	AppendVarint(bytes, Values.size());
	for (const SEncodedValue& value : Values)
	{
		AppendText(bytes, value.Source->Name);
		AppendHash(bytes, value.RawHash);
		AppendVarint(bytes, value.RawSize);
		const uint8 flags = static_cast<uint8>((value.bInline ? ValueFlagInline : 0u) | (value.bZstd ? ValueFlagZstd : 0u));
		bytes.push_back(flags);
		if (value.bInline)
		{
			AppendSized(bytes, value.GetStored());
		}
	}
	AppendHash(bytes, Record.LogHash);
	AppendVarint(bytes, Record.BuildMs);
	bytes.push_back(static_cast<uint8>(Record.bDeterministic ? 1u : 0u));
	AppendChecksum(bytes);
	return bytes;
}

// A value as a record file describes it; Stored points into the file's bytes (inline values only).
struct SParsedValue
{
	FString Name;
	hash::SContentHash RawHash;
	uint64 RawSize = 0u;
	uint8 Flags = 0u;
	std::span<const uint8> Stored;
};

struct SParsedRecord
{
	SDdcRecord Record;                  // values without bytes
	std::vector<SParsedValue> Values;
};

std::optional<SParsedRecord> ParseRecord (std::span<const uint8> Bytes, const hash::SContentHash& Key)
{
	const std::optional<std::span<const uint8>> body = CheckChecksum(Bytes);
	if (!body)
	{
		return std::nullopt;
	}
	CByteReader reader(*body);
	SParsedRecord parsed;
	uint64 version = 0u;
	hash::SContentHash key;
	std::span<const uint8> guid;
	uint64 valueCount = 0u;
	if (!reader.Expect(RecordMagic) || !reader.ReadLittleEndian(version, 4u) || version != RecordVersion || !reader.ReadHash(key)
		|| key != Key || !reader.ReadText(parsed.Record.Builder) || !reader.ReadSpan(SGuid::Size, guid)
		|| !reader.ReadVarint(valueCount) || valueCount > reader.GetRemaining() / 23u)
	{
		return std::nullopt;
	}
	std::memcpy(parsed.Record.Version.Bytes, guid.data(), SGuid::Size);

	parsed.Values.resize(static_cast<size_t>(valueCount));
	for (SParsedValue& value : parsed.Values)
	{
		if (!reader.ReadText(value.Name) || !reader.ReadHash(value.RawHash) || !reader.ReadVarint(value.RawSize) || !reader.ReadU8(value.Flags)
			|| (value.Flags & ~(ValueFlagInline | ValueFlagZstd)) != 0u)
		{
			return std::nullopt;
		}
		if ((value.Flags & ValueFlagInline) != 0u)
		{
			if (!reader.ReadSized(value.Stored))
			{
				return std::nullopt;
			}
			// Raw inline bytes are exactly the value.
			if ((value.Flags & ValueFlagZstd) == 0u && value.Stored.size() != value.RawSize)
			{
				return std::nullopt;
			}
		}
	}
	uint8 deterministic = 0u;
	if (!reader.ReadHash(parsed.Record.LogHash) || !reader.ReadVarint(parsed.Record.BuildMs) || !reader.ReadU8(deterministic)
		|| deterministic > 1u || !reader.IsAtEnd())
	{
		return std::nullopt;
	}
	parsed.Record.bDeterministic = deterministic == 1u;

	parsed.Record.Values.reserve(parsed.Values.size());
	for (const SParsedValue& value : parsed.Values)
	{
		SDdcValue& out = parsed.Record.Values.emplace_back();
		out.Name = value.Name;
		out.RawHash = value.RawHash;
		out.RawSize = value.RawSize;
		out.bInline = (value.Flags & ValueFlagInline) != 0u;
	}
	return parsed;
}

// The raw bytes of a stored value: decompressed when Zstd, checked against RawHash when bVerify.
std::optional<std::vector<uint8>> DecodeStored (std::span<const uint8> Stored, bool bZstd, uint64 RawSize,
	const hash::SContentHash& RawHash, bool bVerify)
{
	std::optional<std::vector<uint8>> raw;
	if (bZstd)
	{
		raw = compression::DecompressZstd(Stored, static_cast<size_t>(RawSize));
	}
	else if (Stored.size() == RawSize)
	{
		raw.emplace(Stored.begin(), Stored.end());
	}
	if (raw && bVerify && hash::HashContent(*raw) != RawHash)
	{
		raw.reset();
	}
	return raw;
}

std::vector<uint8> EncodeValueFile (const SEncodedValue& Value)
{
	const std::span<const uint8> stored = Value.GetStored();
	std::vector<uint8> bytes;
	bytes.reserve(ValueHeaderBytes + stored.size());
	AppendRaw(bytes, ValueMagic);
	bytes.push_back(Value.bZstd ? CodecZstd : CodecNone);
	bytes.insert(bytes.end(), 3u, uint8(0u));
	AppendLittleEndian(bytes, Value.RawSize, 8u);
	AppendHash(bytes, Value.RawHash);
	AppendRaw(bytes, stored);
	return bytes;
}

// The checks of a .val file that need no decoding: magic, codec, the raw size and hash it claims.
std::optional<std::pair<uint8, std::span<const uint8>>> ParseValueFile (std::span<const uint8> Bytes, const hash::SContentHash& RawHash,
	uint64 RawSize)
{
	CByteReader reader(Bytes);
	uint8 codec = 0u;
	std::span<const uint8> reserved;
	uint64 rawSize = 0u;
	hash::SContentHash rawHash;
	if (!reader.Expect(ValueMagic) || !reader.ReadU8(codec) || codec > CodecZstd || !reader.ReadSpan(3u, reserved)
		|| reserved[0] != 0u || reserved[1] != 0u || reserved[2] != 0u || !reader.ReadLittleEndian(rawSize, 8u) || rawSize != RawSize
		|| !reader.ReadHash(rawHash) || rawHash != RawHash)
	{
		return std::nullopt;
	}
	return std::make_pair(codec, Bytes.subspan(reader.GetPosition()));
}
}


// ---------------- SDdcRecord ----------------

const SDdcValue* SDdcRecord::FindValue (FStringView Name) const noexcept
{
	for (const SDdcValue& value : Values)
	{
		if (value.Name == Name)
		{
			return &value;
		}
	}
	return nullptr;
}


// ---------------- CDerivedDataCache ----------------

// One call in progress, for Open and Close to assert against: they change Root under every other
// call, which the owner must have drained first (DerivedDataCache.h). Counted only where asserts are.
struct CDerivedDataCache::SCallScope
{
	explicit SCallScope (const CDerivedDataCache& InCache) noexcept
		: Cache(InCache)
	{
#if FRT_ASSERTIONS_ENABLED
		Cache.CallsInFlight.fetch_add(1u);
#endif
	}

	~SCallScope ()
	{
#if FRT_ASSERTIONS_ENABLED
		Cache.CallsInFlight.fetch_sub(1u);
#endif
	}

	SCallScope (const SCallScope&) = delete;
	SCallScope& operator= (const SCallScope&) = delete;

	const CDerivedDataCache& Cache;
};

size_t CDerivedDataCache::SKeyHash::operator() (const hash::SContentHash& Key) const noexcept
{
	// A cryptographic hash: its first bytes are as good as any hash of it.
	size_t value = 0u;
	std::memcpy(&value, Key.Bytes, sizeof(value));
	return value;
}

CDerivedDataCache::CDerivedDataCache ()
	: bVerifyValues(DefaultVerifyValues)
{
	uint8 random[8] = {};
	platform::FillRandomBytes(random);
	for (const uint8 byte : random)
	{
		TmpPrefix.AppendFormat("{:02x}", byte);
	}
}

CDerivedDataCache::~CDerivedDataCache ()
{
	Close();
}

std::filesystem::path CDerivedDataCache::GetDefaultRoot (const config::CConfig* Config)
{
	if (std::filesystem::path dir = platform::GetEnvironmentPath("FRT_DDC_DIR"); !dir.empty())
	{
		return dir;
	}
	if (Config != nullptr)
	{
		const FString configured = Config->Get("DerivedDataCache", "Path", FStringView());
		if (!configured.IsEmpty())
		{
			if (const std::optional<std::filesystem::path> path = configured.View().TrimStartAndEnd().TryToPath(); path && !path->empty())
			{
				return path->is_absolute() ? *path : project::GetUserDataDir() / *path;
			}
		}
	}
	return project::GetUserDataDir() / "DerivedDataCache";
}

bool CDerivedDataCache::Open (const std::filesystem::path& InRoot, FString* OutError)
{
	Close();
	if (InRoot.empty())
	{
		if (OutError != nullptr)
		{
			*OutError = "No derived-data cache directory given";
		}
		return false;
	}
	std::error_code ec;
	const std::filesystem::path root = std::filesystem::absolute(InRoot, ec);
	const std::filesystem::path formatPath = (ec ? InRoot : root) / FormatFile.ToPath();

	std::vector<uint8> format;
	const EReadResult formatRead = ReadWholeFile(formatPath, format);
	if (formatRead == EReadResult::Failed)
	{
		// There, but unreadable now (held without read sharing, no access): it may name another
		// engine's format, so it is never written over on a guess.
		if (OutError != nullptr)
		{
			*OutError = FString::Format("Cannot read '{}': the derived-data cache's FORMAT is in use or not accessible",
				FString::FromPath(formatPath));
		}
		return false;
	}
	if (formatRead == EReadResult::Ok)
	{
		const FStringView text = FStringView(reinterpret_cast<const char*>(format.data()), format.size()).TrimEnd();
		if (text != DdcFormat)
		{
			// Another engine's store is left alone; anything else (empty or zero-filled after a power
			// loss) is written again below.
			if (text.StartsWith("FRT-DDC "))
			{
				if (OutError != nullptr)
				{
					*OutError = FString::Format("'{}' holds a derived-data cache of format '{}'; this engine reads '{}'",
						FString::FromPath(InRoot), text, DdcFormat);
				}
				return false;
			}
			format.clear();
		}
	}
	if (format.empty())   // missing, or nothing a writer of any format would leave
	{
		const FString text = FString(DdcFormat) + "\n";
		if (!platform::WriteFileAtomic(formatPath, text.AsBytes(), true, OutError))
		{
			return false;
		}
	}
	Root = ec ? InRoot : root;
	return true;
}

bool CDerivedDataCache::OpenDefault (const config::CConfig* Config, FString* OutError)
{
	return Open(GetDefaultRoot(Config), OutError);
}

void CDerivedDataCache::Close ()
{
	// Every call reads Root: one still running would race with clearing it (and Open's setting it).
	frt_assert(CallsInFlight.load() == 0u);
	{
		const std::lock_guard lock(AccessMutex);
		AccessLog.Close();
		AccessDay.Clear();
		AccessLogged.clear();
	}
	Root.clear();
}


// ---------------- Keys ----------------

std::vector<SDdcInput> CDerivedDataCache::NormalizeInputs (std::span<const SDdcInput> Inputs)
{
	std::vector<SDdcInput> sorted(Inputs.begin(), Inputs.end());
	std::sort(sorted.begin(), sorted.end(), IsInputBefore);
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	return sorted;
}

std::vector<uint8> CDerivedDataCache::EncodeKeyMaterial (const hash::SContentHash& StaticKey, std::span<const SDdcInput> Inputs)
{
	const std::vector<SDdcInput> sorted = NormalizeInputs(Inputs);
	std::vector<uint8> bytes;
	AppendHash(bytes, StaticKey);
	AppendVarint(bytes, sorted.size());
	for (const SDdcInput& input : sorted)
	{
		AppendText(bytes, input.Name);
		AppendText(bytes, input.Kind);
		AppendHash(bytes, input.Hash);
		AppendLittleEndian(bytes, input.Size, 8u);
	}
	return bytes;
}

hash::SContentHash CDerivedDataCache::ComputeKey (const hash::SContentHash& StaticKey, std::span<const SDdcInput> Inputs)
{
	return hash::DeriveKey(DdcKeyContext, EncodeKeyMaterial(StaticKey, Inputs));
}

bool CDerivedDataCache::IsValidBucket (FStringView Bucket) noexcept
{
	if (Bucket.IsEmpty() || Bucket.Len() > MaxBucketLength || Bucket == "." || Bucket == "..")
	{
		return false;
	}
	for (const char ch : Bucket)
	{
		const bool bValid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'
			|| ch == '_';
		if (!bValid)
		{
			return false;
		}
	}
	return true;
}


// ---------------- Paths ----------------

std::filesystem::path CDerivedDataCache::GetManifestPath (const hash::SContentHash& StaticKey) const
{
	return Root / ManifestsDir.ToPath() / FanOut(StaticKey, 0u).ToPath() / (ToHex(StaticKey) + ".man").ToPath();
}

std::filesystem::path CDerivedDataCache::GetRecordPath (FStringView Bucket, const hash::SContentHash& Key) const
{
	return Root / RecordsDir.ToPath() / Bucket.ToPath() / FanOut(Key, 0u).ToPath() / (ToHex(Key) + ".rec").ToPath();
}

std::filesystem::path CDerivedDataCache::GetValuePath (const hash::SContentHash& RawHash) const
{
	return Root / ValuesDir.ToPath() / FanOut(RawHash, 0u).ToPath() / FanOut(RawHash, 1u).ToPath() / (ToHex(RawHash) + ".val").ToPath();
}

std::filesystem::path CDerivedDataCache::GetAccessLogPath (FStringView Day) const
{
	return Root / AccessDir.ToPath() / (FString(Day) + ".log").ToPath();
}

std::filesystem::path CDerivedDataCache::GetTmpDir () const
{
	return Root / TmpDir.ToPath();
}

std::vector<FString> CDerivedDataCache::ReadToolVerdicts () const
{
	const SCallScope call(*this);
	std::vector<FString> verdicts;
	std::vector<uint8> bytes;
	if (Root.empty() || ReadWholeFile(Root / ToolVerdictsFile.ToPath(), bytes) != EReadResult::Ok)
	{
		return verdicts;
	}
	const FStringView text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	for (const FStringView line : text.Lines())
	{
		const FStringView trimmed = line.TrimStartAndEnd();
		if (!trimmed.IsEmpty() && !trimmed.StartsWith('#'))
		{
			verdicts.emplace_back(trimmed);
		}
	}
	return verdicts;
}


// ---------------- Writing ----------------

bool CDerivedDataCache::WriteThroughTmp (const std::filesystem::path& Path, std::span<const uint8> Bytes, FString* OutError)
{
	const uint64 number = TmpCounter.fetch_add(1u, std::memory_order_relaxed);
	const std::filesystem::path temp = GetTmpDir() / FString::Format("{}-{}.tmp", TmpPrefix, number).ToPath();
	return platform::WriteFileAtomicVia(Path, temp, Bytes, true, OutError);
}

void CDerivedDataCache::DeleteCorrupt (const std::filesystem::path& Path)
{
	Counters.CorruptFilesDeleted.fetch_add(1u, std::memory_order_relaxed);
	FString error;
	if (platform::RemoveFile(Path, &error))
	{
		log::Warning(log::Assets, "derived-data cache: deleted the corrupt file '{}'", FString::FromPath(Path));
	}
	else
	{
		log::Warning(log::Assets, "derived-data cache: '{}' is corrupt and could not be deleted: {}", FString::FromPath(Path), error);
	}
}

void CDerivedDataCache::LogAccess (FStringView Bucket, const hash::SContentHash& Key)
{
	const int64 now = NowUnixMicroseconds();
	const FString day = FormatDay(ToUnixDay(now));

	const std::lock_guard lock(AccessMutex);
	if (Root.empty())
	{
		return;
	}
	if (day != AccessDay)
	{
		AccessLog.Close();
		AccessLogged.clear();
		AccessDay = day;
	}
	if (!AccessLogged.insert(Key).second)
	{
		return;
	}
	if (!AccessLog.IsOpen() && !AccessLog.Open(GetAccessLogPath(day)))
	{
		return;   // a missing access line only makes the record look older to the collector
	}
	const FString line = FString::Format("{} {} {}\n", now, Bucket, ToHex(Key));
	AccessLog.Append(line.AsBytes());
}

bool CDerivedDataCache::PutRecord (FStringView Bucket, const hash::SContentHash& Key, const SDdcRecord& Record, FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Ddc.PutRecord");
	const SCallScope call(*this);
	const auto fail = [OutError] (FString Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	if (!IsOpen())
	{
		return fail("The derived-data cache is not open");
	}
	if (!IsValidBucket(Bucket))
	{
		return fail(FString::Format("'{}' is not a valid derived-data cache bucket", Bucket));
	}
	for (size_t i = 0u; i < Record.Values.size(); ++i)
	{
		if (Record.Values[i].Name.IsEmpty())
		{
			return fail(FString::Format("Value {} of the record has no name", i));
		}
		for (size_t j = 0u; j < i; ++j)
		{
			if (Record.Values[j].Name == Record.Values[i].Name)
			{
				return fail(FString::Format("The record has two values named '{}'", Record.Values[i].Name));
			}
		}
	}

	std::vector<SEncodedValue> values;
	values.reserve(Record.Values.size());
	for (const SDdcValue& value : Record.Values)
	{
		values.push_back(EncodeValue(value));
	}
	ChooseInlineValues(Record, values);

	// Large values first: a record never names a value that is not there yet.
	for (const SEncodedValue& value : values)
	{
		if (value.bInline)
		{
			continue;
		}
		const std::filesystem::path path = GetValuePath(value.RawHash);
		const uint64 expectedSize = ValueHeaderBytes + value.GetStored().size();
		if (const std::optional<platform::SFileStat> stat = platform::Stat(path); stat && !stat->bDirectory && stat->Size == expectedSize)
		{
			// Content-addressed: the same bytes are there already (a reader checks them anyway). It may
			// be an old leftover that a collection in another process is about to delete as unreferenced:
			// its write time moves to now first, which that collection checks before it deletes a value
			// (CollectGarbage). If it vanished meanwhile, it is written after all.
			if (platform::TouchFile(path) || platform::Stat(path).has_value())
			{
				Counters.ValueWritesSkipped.fetch_add(1u, std::memory_order_relaxed);
				continue;
			}
		}
		if (!WriteThroughTmp(path, EncodeValueFile(value), OutError))
		{
			return false;
		}
		Counters.ValueWrites.fetch_add(1u, std::memory_order_relaxed);
	}

	if (!WriteThroughTmp(GetRecordPath(Bucket, Key), EncodeRecord(Key, Record, values), OutError))
	{
		return false;
	}
	Counters.RecordWrites.fetch_add(1u, std::memory_order_relaxed);
	LogAccess(Bucket, Key);
	return true;
}

bool CDerivedDataCache::AddToManifest (const hash::SContentHash& StaticKey, const hash::SContentHash& Key, std::span<const SDdcInput> Inputs,
	FString* OutError)
{
	FRT_CPU_SCOPE("Assets.Ddc.AddToManifest");
	const SCallScope call(*this);
	if (!IsOpen())
	{
		if (OutError != nullptr)
		{
			*OutError = "The derived-data cache is not open";
		}
		return false;
	}

	const std::lock_guard lock(ManifestMutexes[StaticKey.Bytes[0] % ManifestLockCount]);
	std::vector<SDdcManifestMatch> sets = ReadManifest(StaticKey);
	std::erase_if(sets, [&Key] (const SDdcManifestMatch& Set) { return Set.Key == Key; });
	SDdcManifestMatch newest;
	newest.Key = Key;
	newest.Inputs = NormalizeInputs(Inputs);
	sets.insert(sets.begin(), std::move(newest));
	if (sets.size() > DdcMaxManifestSets)
	{
		sets.resize(DdcMaxManifestSets);
	}
	if (!WriteThroughTmp(GetManifestPath(StaticKey), EncodeManifest(StaticKey, sets), OutError))
	{
		return false;
	}
	Counters.ManifestWrites.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

std::optional<hash::SContentHash> CDerivedDataCache::Put (FStringView Bucket, const hash::SContentHash& StaticKey,
	std::span<const SDdcInput> Inputs, const SDdcRecord& Record, FString* OutError)
{
	const SCallScope call(*this);
	const hash::SContentHash key = ComputeKey(StaticKey, Inputs);
	if (!PutRecord(Bucket, key, Record, OutError) || !AddToManifest(StaticKey, key, Inputs, OutError))
	{
		return std::nullopt;
	}
	return key;
}


// ---------------- Reading ----------------

std::vector<SDdcManifestMatch> CDerivedDataCache::ReadManifest (const hash::SContentHash& StaticKey)
{
	const SCallScope call(*this);
	if (!IsOpen())
	{
		return {};
	}
	const std::filesystem::path path = GetManifestPath(StaticKey);
	std::vector<uint8> bytes;
	if (ReadWholeFile(path, bytes) != EReadResult::Ok)
	{
		return {};
	}
	std::optional<std::vector<SDdcManifestMatch>> sets = ParseManifest(bytes, StaticKey);
	if (!sets)
	{
		DeleteCorrupt(path);
		return {};
	}
	return std::move(*sets);
}

std::optional<SDdcManifestMatch> CDerivedDataCache::FindKey (const hash::SContentHash& StaticKey,
	TFunctionRef<bool (const SDdcInput& Recorded)> IsCurrent)
{
	FRT_CPU_SCOPE("Assets.Ddc.FindKey");
	const SCallScope call(*this);
	for (SDdcManifestMatch& set : ReadManifest(StaticKey))
	{
		const bool bCurrent = std::all_of(set.Inputs.begin(), set.Inputs.end(), [&IsCurrent] (const SDdcInput& Input)
		{
			return IsCurrent(Input);
		});
		if (bCurrent)
		{
			Counters.ManifestHits.fetch_add(1u, std::memory_order_relaxed);
			return std::move(set);
		}
	}
	Counters.ManifestMisses.fetch_add(1u, std::memory_order_relaxed);
	return std::nullopt;
}

std::optional<SDdcRecord> CDerivedDataCache::Get (FStringView Bucket, const hash::SContentHash& StaticKey,
	TFunctionRef<bool (const SDdcInput& Recorded)> IsCurrent, EDdcRead Read, SDdcManifestMatch* OutMatch)
{
	const SCallScope call(*this);
	std::optional<SDdcManifestMatch> match = FindKey(StaticKey, IsCurrent);
	if (!match)
	{
		return std::nullopt;
	}
	std::optional<SDdcRecord> record = GetRecord(Bucket, match->Key, Read);
	if (record && OutMatch != nullptr)
	{
		*OutMatch = std::move(*match);
	}
	return record;
}

std::optional<std::vector<uint8>> CDerivedDataCache::ReadValueFile (const hash::SContentHash& RawHash, uint64 RawSize, bool bVerify)
{
	const std::filesystem::path path = GetValuePath(RawHash);
	std::vector<uint8> bytes;
	if (ReadWholeFile(path, bytes) != EReadResult::Ok)
	{
		return std::nullopt;
	}
	std::optional<std::vector<uint8>> raw;
	if (const auto parsed = ParseValueFile(bytes, RawHash, RawSize))
	{
		raw = DecodeStored(parsed->second, parsed->first == CodecZstd, RawSize, RawHash, bVerify);
	}
	if (!raw)
	{
		DeleteCorrupt(path);
	}
	return raw;
}

std::optional<std::vector<uint8>> CDerivedDataCache::ReadValue (const hash::SContentHash& RawHash, uint64 RawSize)
{
	const SCallScope call(*this);
	if (!IsOpen())
	{
		return std::nullopt;
	}
	return ReadValueFile(RawHash, RawSize, GetVerifyValues());
}

std::optional<SDdcRecord> CDerivedDataCache::GetRecord (FStringView Bucket, const hash::SContentHash& Key, EDdcRead Read)
{
	FRT_CPU_SCOPE("Assets.Ddc.GetRecord");
	const SCallScope call(*this);
	const auto miss = [this] () -> std::optional<SDdcRecord>
	{
		Counters.RecordMisses.fetch_add(1u, std::memory_order_relaxed);
		return std::nullopt;
	};
	if (!IsOpen() || !IsValidBucket(Bucket))
	{
		return miss();
	}

	const std::filesystem::path path = GetRecordPath(Bucket, Key);
	std::vector<uint8> bytes;
	if (ReadWholeFile(path, bytes) != EReadResult::Ok)
	{
		return miss();
	}
	std::optional<SParsedRecord> parsed = ParseRecord(bytes, Key);
	if (!parsed)
	{
		DeleteCorrupt(path);
		return miss();
	}

	if (Read == EDdcRead::Values)
	{
		const bool bVerify = GetVerifyValues();
		for (size_t i = 0u; i < parsed->Values.size(); ++i)
		{
			const SParsedValue& value = parsed->Values[i];
			std::optional<std::vector<uint8>> raw;
			if ((value.Flags & ValueFlagInline) != 0u)
			{
				raw = DecodeStored(value.Stored, (value.Flags & ValueFlagZstd) != 0u, value.RawSize, value.RawHash, bVerify);
				if (!raw)
				{
					// The checksum held, so the writer stored bytes that do not match their hash.
					DeleteCorrupt(path);
					return miss();
				}
			}
			else
			{
				raw = ReadValueFile(value.RawHash, value.RawSize, bVerify);
				if (!raw)
				{
					return miss();
				}
			}
			parsed->Record.Values[i].Bytes = std::move(*raw);
		}
	}

	Counters.RecordHits.fetch_add(1u, std::memory_order_relaxed);
	LogAccess(Bucket, Key);
	return std::move(parsed->Record);
}


// ---------------- Maintenance ----------------

SDdcStats CDerivedDataCache::GetStats () const
{
	const SCallScope call(*this);
	SDdcStats stats;
	if (!IsOpen())
	{
		return stats;
	}
	const auto add = [] (SDdcFileStats& Kind, const platform::SFileStat& Stat)
	{
		++Kind.Files;
		Kind.Bytes += Stat.Size;
	};
	platform::EnumerateDirectory(Root, [&] (const platform::SDirectoryEntry& Entry)
	{
		if (Entry.bReparsePoint)
		{
			return;
		}
		if (!Entry.Stat.bDirectory)
		{
			add(stats.Other, Entry.Stat);
			return;
		}
		SDdcFileStats* kind = &stats.Other;
		if (Entry.Name == ManifestsDir)
		{
			kind = &stats.Manifests;
		}
		else if (Entry.Name == RecordsDir)
		{
			kind = &stats.Records;
		}
		else if (Entry.Name == ValuesDir)
		{
			kind = &stats.Values;
		}
		else if (Entry.Name == TmpDir)
		{
			kind = &stats.Tmp;
		}
		else if (Entry.Name == AccessDir)
		{
			kind = &stats.Access;
		}
		WalkFiles(Root / Entry.Name.ToPath(), [&] (const std::filesystem::path&, const platform::SFileStat& Stat) { add(*kind, Stat); });
	});
	return stats;
}

SDdcCounters CDerivedDataCache::GetCounters () const noexcept
{
	SDdcCounters counters;
	counters.RecordHits = Counters.RecordHits.load(std::memory_order_relaxed);
	counters.RecordMisses = Counters.RecordMisses.load(std::memory_order_relaxed);
	counters.ManifestHits = Counters.ManifestHits.load(std::memory_order_relaxed);
	counters.ManifestMisses = Counters.ManifestMisses.load(std::memory_order_relaxed);
	counters.RecordWrites = Counters.RecordWrites.load(std::memory_order_relaxed);
	counters.ValueWrites = Counters.ValueWrites.load(std::memory_order_relaxed);
	counters.ValueWritesSkipped = Counters.ValueWritesSkipped.load(std::memory_order_relaxed);
	counters.ManifestWrites = Counters.ManifestWrites.load(std::memory_order_relaxed);
	counters.CorruptFilesDeleted = Counters.CorruptFilesDeleted.load(std::memory_order_relaxed);
	return counters;
}

void CDerivedDataCache::ResetCounters () noexcept
{
	Counters.RecordHits.store(0u, std::memory_order_relaxed);
	Counters.RecordMisses.store(0u, std::memory_order_relaxed);
	Counters.ManifestHits.store(0u, std::memory_order_relaxed);
	Counters.ManifestMisses.store(0u, std::memory_order_relaxed);
	Counters.RecordWrites.store(0u, std::memory_order_relaxed);
	Counters.ValueWrites.store(0u, std::memory_order_relaxed);
	Counters.ValueWritesSkipped.store(0u, std::memory_order_relaxed);
	Counters.ManifestWrites.store(0u, std::memory_order_relaxed);
	Counters.CorruptFilesDeleted.store(0u, std::memory_order_relaxed);
}

namespace
{
// What a collection knows about one file of the store.
struct SStoreFile
{
	std::filesystem::path Path;
	uint64 Size = 0u;
	int64 WriteTime = 0;   // file time
};

struct SRecordFile : SStoreFile
{
	FString Bucket;
	hash::SContentHash Key;
	int64 LastUse = 0;       // Unix microseconds
	uint64 UseOrder = 0u;    // ties of LastUse: the later access line wins
	std::vector<hash::SContentHash> ExternalValues;
	bool bPinned = false;
	bool bEvicted = false;
};

struct SBucketKey
{
	FString Bucket;
	hash::SContentHash Key;

	[[nodiscard]] bool operator< (const SBucketKey& Other) const noexcept
	{
		const int32 bucket = CompareBytes(Bucket, Other.Bucket);
		return bucket != 0 ? bucket < 0 : Key < Other.Key;
	}
};

struct SLastUse
{
	int64 Time = 0;
	uint64 Order = 0u;
};
}

SDdcGcResult CDerivedDataCache::CollectGarbage (uint64 MaxBytes, std::span<const hash::SContentHash> PinnedKeys)
{
	FRT_CPU_SCOPE("Assets.Ddc.CollectGarbage");
	const SCallScope call(*this);
	SDdcGcResult result;
	if (!IsOpen())
	{
		return result;
	}
	const int64 now = NowFileTime();
	const auto isLeftoverAge = [now] (int64 WriteTime) { return now - WriteTime > LeftoverAgeFileTicks; };

	// Tmp/: what crashed writers left.
	WalkFiles(GetTmpDir(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
	{
		if (isLeftoverAge(Stat.LastWriteTime) && platform::RemoveFile(Path))
		{
			++result.TmpFilesDeleted;
		}
	});

	// The access logs: the last use of every record they mention. Lines are processed day by day in
	// file order, so a later line wins a tie of equal times.
	std::map<SBucketKey, SLastUse> lastUses;
	{
		std::map<int64, std::filesystem::path> logs;
		platform::EnumerateDirectory(Root / AccessDir.ToPath(), [&] (const platform::SDirectoryEntry& Entry)
		{
			if (!Entry.Stat.bDirectory && Entry.Name.EndsWith(".log"))
			{
				if (const std::optional<int64> day = ParseDay(Entry.Name.LeftChop(4u)))
				{
					logs.emplace(*day, Root / AccessDir.ToPath() / Entry.Name.ToPath());
				}
			}
		});
		const int64 today = ToUnixDay(NowUnixMicroseconds());
		uint64 order = 0u;
		for (const auto& [day, path] : logs)
		{
			std::vector<uint8> bytes;
			if (ReadWholeFile(path, bytes) == EReadResult::Ok)
			{
				const FStringView text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
				for (const FStringView line : text.Lines())
				{
					FStringView rest = line;
					const std::optional<int64> time = rest.ConsumeToken(' ').TryParse<int64>();
					const FStringView bucket = rest.ConsumeToken(' ');
					const std::optional<hash::SContentHash> key = hash::SContentHash::Parse(rest.TrimEnd());
					if (time && key && IsValidBucket(bucket))
					{
						SLastUse& use = lastUses[SBucketKey{ FString(bucket), *key }];
						if (*time >= use.Time)
						{
							use = SLastUse{ *time, ++order };
						}
					}
				}
			}
			if (today - day > AccessLogMaxAgeDays && platform::RemoveFile(path))
			{
				++result.AccessLogsDeleted;
			}
		}
	}

	// Every record, with the values it references. Which values are referenced is known only if every
	// record was listed and read (see below).
	std::vector<SRecordFile> records;
	bool bBucketsListed = true;
	const bool bRecordsListed = ListDirectory(Root / RecordsDir.ToPath(), [&] (const platform::SDirectoryEntry& BucketEntry)
	{
		if (!BucketEntry.Stat.bDirectory || BucketEntry.bReparsePoint || !IsValidBucket(BucketEntry.Name))
		{
			return;   // nothing this cache writes
		}
		const FString bucket(BucketEntry.Name);
		const std::filesystem::path bucketDir = Root / RecordsDir.ToPath() / bucket.ToPath();
		const bool bBucketListed = WalkFiles(bucketDir, [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
		{
			const std::optional<hash::SContentHash> key = ParseHashFileName(FString::FromPath(Path.filename()), "rec");
			if (!key)
			{
				return;
			}
			SRecordFile& record = records.emplace_back();
			record.Path = Path;
			record.Size = Stat.Size;
			record.WriteTime = Stat.LastWriteTime;
			record.Bucket = bucket;
			record.Key = *key;
		});
		bBucketsListed = bBucketsListed && bBucketListed;
	}) && bBucketsListed;

	std::unordered_map<hash::SContentHash, SStoreFile, SKeyHash> values;
	WalkFiles(Root / ValuesDir.ToPath(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
	{
		if (const std::optional<hash::SContentHash> hash = ParseHashFileName(FString::FromPath(Path.filename()), "val"))
		{
			values[*hash] = SStoreFile{ Path, Stat.Size, Stat.LastWriteTime };
		}
	});

	struct SManifestFile : SStoreFile
	{
		std::vector<hash::SContentHash> Keys;
	};
	std::vector<SManifestFile> manifests;
	WalkFiles(Root / ManifestsDir.ToPath(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
	{
		const std::optional<hash::SContentHash> staticKey = ParseHashFileName(FString::FromPath(Path.filename()), "man");
		if (!staticKey)
		{
			return;
		}
		std::vector<uint8> bytes;
		if (ReadWholeFile(Path, bytes) != EReadResult::Ok)
		{
			return;
		}
		const std::optional<std::vector<SDdcManifestMatch>> sets = ParseManifest(bytes, *staticKey);
		if (!sets)
		{
			DeleteCorrupt(Path);
			return;
		}
		SManifestFile& manifest = manifests.emplace_back();
		static_cast<SStoreFile&>(manifest) = SStoreFile{ Path, Stat.Size, Stat.LastWriteTime };
		for (const SDdcManifestMatch& set : *sets)
		{
			manifest.Keys.push_back(set.Key);
		}
	});

	// Parse the records: their external values, their last use, whether they are pinned. One that is
	// there but cannot be read now (a scanner or a backup holding it without read sharing) stays, with
	// its values unknown.
	std::unordered_set<hash::SContentHash, SKeyHash> pinned(PinnedKeys.begin(), PinnedKeys.end());
	std::unordered_map<hash::SContentHash, uint32, SKeyHash> references;
	std::erase_if(records, [&] (SRecordFile& Record)
	{
		Record.bPinned = pinned.contains(Record.Key);
		std::vector<uint8> bytes;
		const EReadResult read = ReadWholeFile(Record.Path, bytes);
		if (read == EReadResult::Missing)
		{
			return true;   // gone since the listing (another collection, Verify)
		}
		if (read == EReadResult::Failed)
		{
			++result.RecordsUnreadable;   // kept: its bytes count, and it is never evicted unread
			return false;
		}
		const std::optional<SParsedRecord> parsed = ParseRecord(bytes, Record.Key);
		if (!parsed)
		{
			DeleteCorrupt(Record.Path);
			return true;
		}
		for (const SParsedValue& value : parsed->Values)
		{
			if ((value.Flags & ValueFlagInline) == 0u)
			{
				Record.ExternalValues.push_back(value.RawHash);
			}
		}
		std::sort(Record.ExternalValues.begin(), Record.ExternalValues.end());
		Record.ExternalValues.erase(std::unique(Record.ExternalValues.begin(), Record.ExternalValues.end()), Record.ExternalValues.end());
		for (const hash::SContentHash& value : Record.ExternalValues)
		{
			++references[value];
		}

		Record.LastUse = FileTimeToUnixMicroseconds(Record.WriteTime);
		if (const auto use = lastUses.find(SBucketKey{ Record.Bucket, Record.Key }); use != lastUses.end() && use->second.Time >= Record.LastUse)
		{
			Record.LastUse = use->second.Time;
			Record.UseOrder = use->second.Order;
		}
		return false;
	});

	uint64 total = 0u;
	for (const SRecordFile& record : records)
	{
		total += record.Size;
	}
	for (const auto& [rawHash, value] : values)
	{
		total += value.Size;
	}
	for (const SManifestFile& manifest : manifests)
	{
		total += manifest.Size;
	}
	result.ContentBytesBefore = total;

	std::unordered_set<hash::SContentHash, SKeyHash> pinnedValues;
	for (const SRecordFile& record : records)
	{
		if (record.bPinned)
		{
			result.PinnedBytes += record.Size;
			for (const hash::SContentHash& value : record.ExternalValues)
			{
				if (const auto file = values.find(value); file != values.end() && pinnedValues.insert(value).second)
				{
					result.PinnedBytes += file->second.Size;
				}
			}
		}
	}

	// Only a walk that read every record knows which values are referenced. Otherwise a value of a
	// record it missed - a pinned one's too - would look unreferenced, and records evicted without
	// their values would not bring the content down: this collection stops at the leftovers, and the
	// next one (the editor's idle task, the next `assets ddc gc`) does the rest.
	result.bComplete = bRecordsListed && result.RecordsUnreadable == 0u;
	if (!result.bComplete)
	{
		log::Warning(log::Assets, "derived-data cache: garbage collection could not {} ({} unreadable): only leftovers in Tmp/ were deleted",
			bRecordsListed ? FStringView("read every record") : FStringView("list every record"), result.RecordsUnreadable);
		result.ContentBytesAfter = total;
		result.bWithinBudget = total <= MaxBytes;
		return result;
	}

	// A writer reusing a value refreshes its write time before it writes the record naming it
	// (PutRecord): a value whose time changed since the walk is in use again, whatever the walk saw.
	const auto evictValue = [&] (const hash::SContentHash& Hash)
	{
		const auto file = values.find(Hash);
		if (file == values.end())
		{
			return;
		}
		const std::optional<platform::SFileStat> stat = platform::Stat(file->second.Path);
		if (!stat)
		{
			total -= file->second.Size;   // gone already (another collection)
			values.erase(file);
			return;
		}
		if (stat->LastWriteTime != file->second.WriteTime)
		{
			return;
		}
		if (platform::RemoveFile(file->second.Path))
		{
			total -= file->second.Size;
			++result.ValuesEvicted;
			values.erase(file);
		}
	};

	// Values no record references: leftovers of crashed writes or of evicted records, once old
	// enough not to be a write in progress elsewhere (values are written before their record).
	std::vector<hash::SContentHash> orphans;
	for (const auto& [rawHash, value] : values)
	{
		if (!references.contains(rawHash) && isLeftoverAge(value.WriteTime))
		{
			orphans.push_back(rawHash);
		}
	}
	for (const hash::SContentHash& orphan : orphans)
	{
		evictValue(orphan);
	}

	// Least recently used first, until the content fits.
	std::unordered_set<hash::SContentHash, SKeyHash> evictedKeys;
	if (total > MaxBytes)
	{
		std::vector<SRecordFile*> order;
		for (SRecordFile& record : records)
		{
			if (!record.bPinned)
			{
				order.push_back(&record);
			}
		}
		std::sort(order.begin(), order.end(), [] (const SRecordFile* A, const SRecordFile* B)
		{
			if (A->LastUse != B->LastUse)
			{
				return A->LastUse < B->LastUse;
			}
			if (A->UseOrder != B->UseOrder)
			{
				return A->UseOrder < B->UseOrder;
			}
			return SBucketKey{ A->Bucket, A->Key } < SBucketKey{ B->Bucket, B->Key };
		});
		for (SRecordFile* record : order)
		{
			if (total <= MaxBytes)
			{
				break;
			}
			if (!platform::RemoveFile(record->Path))
			{
				continue;
			}
			record->bEvicted = true;
			total -= record->Size;
			++result.RecordsEvicted;
			evictedKeys.insert(record->Key);
			for (const hash::SContentHash& value : record->ExternalValues)
			{
				if (--references[value] == 0u)
				{
					evictValue(value);
				}
			}
		}
	}

	// Manifests whose every key has lost its record. A young one may be another process's fresh
	// write whose record this walk missed: kept unless this collection evicted one of its keys.
	std::unordered_set<hash::SContentHash, SKeyHash> liveKeys;
	for (const SRecordFile& record : records)
	{
		if (!record.bEvicted)
		{
			liveKeys.insert(record.Key);
		}
	}
	for (const SManifestFile& manifest : manifests)
	{
		const bool bAnyLive = std::any_of(manifest.Keys.begin(), manifest.Keys.end(), [&] (const hash::SContentHash& Key)
		{
			return liveKeys.contains(Key);
		});
		const bool bAnyEvicted = std::any_of(manifest.Keys.begin(), manifest.Keys.end(), [&] (const hash::SContentHash& Key)
		{
			return evictedKeys.contains(Key);
		});
		if (!bAnyLive && (bAnyEvicted || isLeftoverAge(manifest.WriteTime)) && platform::RemoveFile(manifest.Path))
		{
			total -= manifest.Size;
			++result.ManifestsEvicted;
		}
	}

	result.ContentBytesAfter = total;
	result.bWithinBudget = total <= MaxBytes;
	return result;
}

SDdcVerifyResult CDerivedDataCache::Verify ()
{
	FRT_CPU_SCOPE("Assets.Ddc.Verify");
	const SCallScope call(*this);
	SDdcVerifyResult result;
	if (!IsOpen())
	{
		return result;
	}
	const auto deleteFile = [&] (const std::filesystem::path& Path, uint64 Size, uint32& Count)
	{
		DeleteCorrupt(Path);
		result.BytesDeleted += Size;
		++Count;
	};

	// Values first, so that records can be checked against what survived.
	std::unordered_map<hash::SContentHash, uint64, SKeyHash> goodValues;   // raw hash -> raw size
	WalkFiles(Root / ValuesDir.ToPath(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
	{
		const std::optional<hash::SContentHash> rawHash = ParseHashFileName(FString::FromPath(Path.filename()), "val");
		if (!rawHash)
		{
			return;
		}
		++result.ValuesChecked;
		std::vector<uint8> bytes;
		if (ReadWholeFile(Path, bytes) != EReadResult::Ok)
		{
			return;
		}
		// The size the file claims, checked by decoding to it and hashing the result.
		const uint64 claimedSize = bytes.size() >= ValueHeaderBytes ? binary::ReadLittleEndianAt(bytes, 8u, 8u) : 0u;
		std::optional<std::vector<uint8>> raw;
		if (const auto parsed = ParseValueFile(bytes, *rawHash, claimedSize))
		{
			raw = DecodeStored(parsed->second, parsed->first == CodecZstd, claimedSize, *rawHash, true);
		}
		if (raw)
		{
			goodValues[*rawHash] = claimedSize;
		}
		else
		{
			deleteFile(Path, Stat.Size, result.CorruptValues);
		}
	});

	platform::EnumerateDirectory(Root / RecordsDir.ToPath(), [&] (const platform::SDirectoryEntry& BucketEntry)
	{
		if (!BucketEntry.Stat.bDirectory || BucketEntry.bReparsePoint)
		{
			return;
		}
		WalkFiles(Root / RecordsDir.ToPath() / BucketEntry.Name.ToPath(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
		{
			const std::optional<hash::SContentHash> key = ParseHashFileName(FString::FromPath(Path.filename()), "rec");
			if (!key)
			{
				return;
			}
			++result.RecordsChecked;
			std::vector<uint8> bytes;
			if (ReadWholeFile(Path, bytes) != EReadResult::Ok)
			{
				return;
			}
			const std::optional<SParsedRecord> parsed = ParseRecord(bytes, *key);
			bool bCorrupt = !parsed;
			bool bDangling = false;
			for (size_t i = 0u; parsed && !bCorrupt && !bDangling && i < parsed->Values.size(); ++i)
			{
				const SParsedValue& value = parsed->Values[i];
				if ((value.Flags & ValueFlagInline) != 0u)
				{
					bCorrupt = !DecodeStored(value.Stored, (value.Flags & ValueFlagZstd) != 0u, value.RawSize, value.RawHash, true);
					continue;
				}
				const auto good = goodValues.find(value.RawHash);
				if (good != goodValues.end() && good->second == value.RawSize)
				{
					continue;
				}
				// Written after the value pass by another process, perhaps: look again before judging.
				if (ReadValueFile(value.RawHash, value.RawSize, true))
				{
					goodValues[value.RawHash] = value.RawSize;
					continue;
				}
				// ReadValueFile deletes a corrupt value: one still there could not be read now (held
				// without read sharing), which says nothing about this record.
				bDangling = !platform::Stat(GetValuePath(value.RawHash)).has_value();
			}
			if (bCorrupt)
			{
				deleteFile(Path, Stat.Size, result.CorruptRecords);
			}
			else if (bDangling)
			{
				deleteFile(Path, Stat.Size, result.DanglingRecords);
			}
		});
	});

	WalkFiles(Root / ManifestsDir.ToPath(), [&] (const std::filesystem::path& Path, const platform::SFileStat& Stat)
	{
		const std::optional<hash::SContentHash> staticKey = ParseHashFileName(FString::FromPath(Path.filename()), "man");
		if (!staticKey)
		{
			return;
		}
		++result.ManifestsChecked;
		std::vector<uint8> bytes;
		if (ReadWholeFile(Path, bytes) == EReadResult::Ok && !ParseManifest(bytes, *staticKey))
		{
			deleteFile(Path, Stat.Size, result.CorruptManifests);
		}
	});
	return result;
}
}
