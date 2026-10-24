#include "Assets/KeyWriter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>


namespace frt::assets
{
namespace
{
constexpr FStringLiteral StaticKeyContext = "FRT DDC static v1";

// Unsigned LEB128, shortest form.
void AppendVarint (std::vector<uint8>& Out, uint64 Value)
{
	while (Value >= 0x80u)
	{
		Out.push_back(static_cast<uint8>(Value | 0x80u));
		Value >>= 7u;
	}
	Out.push_back(static_cast<uint8>(Value));
}

void AppendLittleEndian (std::vector<uint8>& Out, uint64 Value, uint32 ByteCount)
{
	for (uint32 i = 0u; i < ByteCount; ++i)
	{
		Out.push_back(static_cast<uint8>(Value >> (8u * i)));
	}
}

void AppendRaw (std::vector<uint8>& Out, std::span<const uint8> Bytes)
{
	Out.insert(Out.end(), Bytes.begin(), Bytes.end());
}

// Length-prefixed bytes: Str, Enum, Bytes and field names.
void AppendSized (std::vector<uint8>& Out, std::span<const uint8> Bytes)
{
	AppendVarint(Out, Bytes.size());
	AppendRaw(Out, Bytes);
}

// Byte order, a prefix before what extends it: the order the format fixes for field names.
bool IsNameBefore (FStringView A, FStringView B) noexcept
{
	const uint32 common = (std::min)(A.Len(), B.Len());
	const int32 order = common > 0u ? std::memcmp(A.GetData(), B.GetData(), common) : 0;
	return order != 0 ? order < 0 : A.Len() < B.Len();
}
}


CKeyWriter::CKeyWriter ()
{
	Stack.emplace_back();
}

void CKeyWriter::WriteStr (FStringView Name, FStringView Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Str))
	{
		AppendSized(*value, Value.AsBytes());
	}
}

void CKeyWriter::WriteGuid (FStringView Name, const SGuid& Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Guid))
	{
		AppendRaw(*value, std::span<const uint8>(Value.Bytes));
	}
}

void CKeyWriter::WriteHash160 (FStringView Name, const hash::SContentHash& Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Hash160))
	{
		AppendRaw(*value, Value.AsBytes());
	}
}

void CKeyWriter::WriteU64 (FStringView Name, uint64 Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::U64))
	{
		AppendLittleEndian(*value, Value, 8u);
	}
}

void CKeyWriter::WriteI64 (FStringView Name, int64 Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::I64))
	{
		AppendLittleEndian(*value, static_cast<uint64>(Value), 8u);
	}
}

void CKeyWriter::WriteBool (FStringView Name, bool Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Bool))
	{
		value->push_back(static_cast<uint8>(Value ? 1u : 0u));
	}
}

void CKeyWriter::WriteF32 (FStringView Name, float Value)
{
	if (std::isnan(Value))
	{
		SetError(Name, "NaN cannot be part of a key");
		return;
	}
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::F32))
	{
		// -0 == +0: both become +0's bits.
		const float canonical = Value == 0.0f ? 0.0f : Value;
		AppendLittleEndian(*value, std::bit_cast<uint32>(canonical), 4u);
	}
}

void CKeyWriter::WriteF64 (FStringView Name, double Value)
{
	if (std::isnan(Value))
	{
		SetError(Name, "NaN cannot be part of a key");
		return;
	}
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::F64))
	{
		const double canonical = Value == 0.0 ? 0.0 : Value;
		AppendLittleEndian(*value, std::bit_cast<uint64>(canonical), 8u);
	}
}

void CKeyWriter::WriteEnum (FStringView Name, FStringView StableName)
{
	if (StableName.IsEmpty())
	{
		SetError(Name, "an enum needs its stable name");
		return;
	}
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Enum))
	{
		AppendSized(*value, StableName.AsBytes());
	}
}

void CKeyWriter::WriteBytes (FStringView Name, std::span<const uint8> Value)
{
	if (std::vector<uint8>* value = AddField(Name, EKeyFieldType::Bytes))
	{
		AppendSized(*value, Value);
	}
}

void CKeyWriter::BeginRecord (FStringView Name)
{
	if (Name.IsEmpty())
	{
		SetError(Name, "empty field name");
	}
	// Opened even when misnamed, so the matching EndRecord still balances.
	SRecord& record = Stack.emplace_back();
	record.Name = Name;
}

void CKeyWriter::EndRecord ()
{
	if (Stack.size() <= 1u)
	{
		SetError("EndRecord", "no record is open");
		return;
	}

	SRecord record = std::move(Stack.back());
	Stack.pop_back();

	std::vector<uint8> encoded;
	FString error;
	if (!EncodeRecord(record.Fields, GetOpenPath() + record.Name + ".", encoded, error))
	{
		if (Error.IsEmpty())
		{
			Error = std::move(error);
		}
		return;
	}
	if (std::vector<uint8>* value = AddField(record.Name, EKeyFieldType::Record))
	{
		*value = std::move(encoded);
	}
}

std::optional<std::vector<uint8>> CKeyWriter::Finish (FString* OutError) const
{
	FString error = Error;
	if (error.IsEmpty() && Stack.size() > 1u)
	{
		error = GetOpenPath().LeftChop(1u) + ": record not ended";
	}

	std::vector<uint8> blob;
	if (error.IsEmpty())
	{
		EncodeRecord(Stack.front().Fields, FStringView(), blob, error);
	}
	if (!error.IsEmpty())
	{
		if (OutError)
		{
			*OutError = std::move(error);
		}
		return std::nullopt;
	}
	return blob;
}

std::vector<uint8>* CKeyWriter::AddField (FStringView Name, EKeyFieldType Type)
{
	if (Name.IsEmpty())
	{
		SetError(Name, "empty field name");
		return nullptr;
	}
	SField& field = Stack.back().Fields.emplace_back();
	field.Name = Name;
	field.Type = Type;
	return &field.Value;
}

void CKeyWriter::SetError (FStringView Name, FStringView What)
{
	if (Error.IsEmpty())
	{
		Error = GetOpenPath() + (Name.IsEmpty() ? FStringView("<empty name>") : Name) + ": " + What;
	}
}

bool CKeyWriter::EncodeRecord (const std::vector<SField>& Fields, FStringView Path, std::vector<uint8>& Out, FString& OutError)
{
	std::vector<const SField*> sorted;
	sorted.reserve(Fields.size());
	for (const SField& field : Fields)
	{
		sorted.push_back(&field);
	}
	std::stable_sort(sorted.begin(), sorted.end(), [] (const SField* A, const SField* B)
	{
		return IsNameBefore(A->Name, B->Name);
	});

	for (size_t i = 1u; i < sorted.size(); ++i)
	{
		if (sorted[i - 1u]->Name == sorted[i]->Name)
		{
			OutError = FString(Path) + sorted[i]->Name + ": duplicate field name";
			return false;
		}
	}

	AppendVarint(Out, sorted.size());
	for (const SField* field : sorted)
	{
		Out.push_back(static_cast<uint8>(field->Type));
		AppendSized(Out, field->Name.AsBytes());
		AppendRaw(Out, field->Value);
	}
	return true;
}

FString CKeyWriter::GetOpenPath () const
{
	FString path;
	for (size_t i = 1u; i < Stack.size(); ++i)
	{
		path += Stack[i].Name;
		path += '.';
	}
	return path;
}


hash::SContentHash MakeStaticKey (std::span<const uint8> KeyBlob) noexcept
{
	return hash::DeriveKey(StaticKeyContext, KeyBlob);
}
}
