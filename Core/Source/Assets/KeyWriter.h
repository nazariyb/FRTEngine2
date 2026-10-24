#pragma once

#include <optional>
#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


/**
 * The canonical KeyBlob of a build step (design section 7.4) and its static cache key.
 *
 * A derived-data cache key must change exactly when the output could: a key that misses an input
 * serves stale data, a key that changes for no reason rebuilds forever. So the blob is canonical:
 * fields are sorted by name when written, whatever order the builder wrote them in; every value has
 * one encoding (floats as raw IEEE bits with -0 as +0, NaN refused; enums by their stable name, never
 * their ordinal, which UE writes, so reordering an enum aliases its keys); nothing machine-specific
 * goes in (paths, times, user names: the builder's job, section 7.4 "Never in a key"). Omitting fields
 * equal to their key baseline is the builder's decision too: an omitted field and an absent one are
 * the same blob, which is what lets a new settings field keep every old key.
 *
 * Byte format (frozen: a change needs a new MakeStaticKey context):
 *
 *   KeyBlob = Record
 *   Record  = varint FieldCount, Field * FieldCount     ascending byte order of Name, names unique
 *   Field   = u8 Type, varint NameLength, Name (UTF-8), Value
 *   Value, by Type:
 *     1  Str      varint Length, UTF-8 bytes
 *     2  Guid     16 bytes, SGuid::Bytes
 *     3  Record   Record (nested)
 *     4  Hash160  20 bytes, SContentHash::Bytes
 *     5  U64      8 bytes little-endian
 *     6  I64      8 bytes little-endian two's complement
 *     7  Bool     1 byte, 0 or 1
 *     8  F32      4 bytes little-endian IEEE 754 binary32 bits; -0 is written as +0; NaN is refused
 *     9  F64      8 bytes little-endian IEEE 754 binary64 bits; the same rules
 *     10 Enum     varint Length, the enumerator's stable name (UTF-8)
 *     11 Bytes    varint Length, bytes
 *   varint = unsigned LEB128, shortest form
 *
 * Names are compared as bytes (memcmp, a prefix first), so "Source" < "Source2" < "Sourcea"; they must
 * be non-empty. Type, length and count prefixes make the encoding unambiguous: no two different field
 * sets give the same bytes.
 *
 * Usage, in any field order:
 *
 *   CKeyWriter key;
 *   key.WriteStr("Builder", "Texture");
 *   key.WriteGuid("Version", TextureBuilderVersion);
 *   key.BeginRecord("Source");
 *   key.WriteHash160("Hash", SourceHash);
 *   key.WriteU64("Size", SourceSize);
 *   key.EndRecord();
 *   std::optional<std::vector<uint8>> blob = key.Finish(&error);
 *   hash::SContentHash staticKey = MakeStaticKey(*blob);
 *
 * A mistake (a NaN, a duplicate or empty name, unbalanced records) does not throw or assert: the first
 * one is kept, with its field path, and Finish returns nullopt with it. A value type, std::vector
 * inside: keys are computed on worker threads, never through the engine's memory pool.
 */
namespace frt::assets
{
enum class EKeyFieldType : uint8
{
	Str = 1,
	Guid = 2,
	Record = 3,
	Hash160 = 4,
	U64 = 5,
	I64 = 6,
	Bool = 7,
	F32 = 8,
	F64 = 9,
	Enum = 10,
	Bytes = 11,
};


class FRT_CORE_API CKeyWriter
{
public:
	CKeyWriter ();

	void WriteStr (FStringView Name, FStringView Value);
	void WriteGuid (FStringView Name, const SGuid& Value);
	void WriteHash160 (FStringView Name, const hash::SContentHash& Value);
	void WriteU64 (FStringView Name, uint64 Value);
	void WriteI64 (FStringView Name, int64 Value);
	void WriteBool (FStringView Name, bool Value);
	void WriteF32 (FStringView Name, float Value);
	void WriteF64 (FStringView Name, double Value);
	// The enumerator's stable name, as text files spell it ("Linear"), never its value.
	void WriteEnum (FStringView Name, FStringView StableName);
	void WriteBytes (FStringView Name, std::span<const uint8> Value);

	// A nested record: the fields written until the matching EndRecord.
	void BeginRecord (FStringView Name);
	void EndRecord ();

	// The first mistake, as "<field path>: <what>"; empty while there is none.
	[[nodiscard]] bool HasError () const noexcept { return !Error.IsEmpty(); }
	[[nodiscard]] const FString& GetError () const noexcept { return Error; }

	// The canonical blob of everything written. nullopt after a mistake or with a record still open,
	// with the reason in *OutError when given. The writer is unchanged: more fields may follow.
	[[nodiscard]] std::optional<std::vector<uint8>> Finish (FString* OutError = nullptr) const;

private:
	struct SField
	{
		FString Name;
		EKeyFieldType Type = EKeyFieldType::Str;
		std::vector<uint8> Value;
	};

	struct SRecord
	{
		FString Name;
		std::vector<SField> Fields;
	};

	// Records being written: the root first, the innermost open one last.
	std::vector<SRecord> Stack;
	FString Error;

	// The new field's value bytes to fill, or nullptr (and the error set) for an empty name.
	std::vector<uint8>* AddField (FStringView Name, EKeyFieldType Type);
	// Keeps the first error only: "<path of the open records>.<Name>: <What>".
	void SetError (FStringView Name, FStringView What);
	// Appends Record (the format above) of Fields to Out. False, with OutError naming the field under
	// Path, on a duplicate name.
	static bool EncodeRecord (const std::vector<SField>& Fields, FStringView Path, std::vector<uint8>& Out, FString& OutError);
	// The open records' names joined with '.', each followed by '.'; empty at the root.
	[[nodiscard]] FString GetOpenPath () const;
};


// The DDC static key: BLAKE3-160 derive_key("FRT DDC static v1", KeyBlob).
[[nodiscard]] FRT_CORE_API hash::SContentHash MakeStaticKey (std::span<const uint8> KeyBlob) noexcept;
}
