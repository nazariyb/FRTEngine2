#pragma once

#include <cstring>
#include <cwchar>
#include <string_view>

#include "Containers/String.h"
#include "Core.h"
#include "CoreTypes.h"


namespace frt
{
/**
 * UTF-16 temporary for OS / D3D12 / DXC calls: the only place wide text exists in the engine. Never in a
 * header API or a long-lived member (one exception: DXC argument vectors local to a compile call).
 *
 * Idiom: Object->SetName(ToWide(Name).CStr()); - the temporary lives to the end of the full expression.
 * Never `const wchar_t* p = ToWide(x).CStr();`.
 *
 * 127 units inline (256 bytes with the terminator) cover nearly every name and path without allocating;
 * longer text takes a string_detail block. Data points at Inline while inline, so moves re-point it.
 */
class FWideString
{
public:
	static constexpr uint32 InlineCapacity = 127u;           // + terminator: 256 bytes inline
	static constexpr uint32 MaxLength = 0x7FFFFFFFu;         // UTF-16 units; longer results are fatal

	FWideString () noexcept
		: Data(Inline)
		, Size(0u)
		, Capacity(InlineCapacity)
	{
		Inline[0] = 0;
	}

	// WTF-8 decoding: ill-formed -> U+FFFD, WTF-8 surrogates -> lone surrogates (paths round-trip).
	FRT_CORE_API explicit FWideString (FStringView Utf8) noexcept;
	// A copy: FWideString(L"...") or a std::wstring_view.
	FRT_CORE_API explicit FWideString (std::wstring_view Utf16) noexcept;

	FWideString (const FWideString& Other) noexcept
		: FWideString()
	{
		std::wmemcpy(Prepare(Other.Size), Other.Data, Other.Size);
	}

	// Re-points Data when Other is inline.
	FWideString (FWideString&& Other) noexcept
		: FWideString()
	{
		Take(Other);
	}

	FWideString& operator= (const FWideString& Other) noexcept
	{
		if (this != &Other)
		{
			std::wmemcpy(Prepare(Other.Size), Other.Data, Other.Size);
		}
		return *this;
	}

	FWideString& operator= (FWideString&& Other) noexcept
	{
		if (this != &Other)
		{
			ReleaseBlock();
			Take(Other);
		}
		return *this;
	}

	~FWideString ()
	{
		ReleaseBlock();
	}

	// Never null, NUL-terminated.
	[[nodiscard]] const wchar_t* CStr () const noexcept FRT_LIFETIMEBOUND { return Data; }
	// Writable (CreateProcessW's command line).
	[[nodiscard]] wchar_t* GetData () noexcept FRT_LIFETIMEBOUND { return Data; }
	// UTF-16 code units.
	[[nodiscard]] uint32 Len () const noexcept { return Size; }
	[[nodiscard]] bool IsEmpty () const noexcept { return Size == 0u; }
	[[nodiscard]] std::wstring_view View () const noexcept FRT_LIFETIMEBOUND { return std::wstring_view(Data, Size); }
	// Canonical WTF-8.
	[[nodiscard]] FString ToUtf8 () const noexcept { return FString::FromWide(Data, Size); }

private:
	// Room for Units units (fatal past MaxLength): this block when it is large enough, else an exact one
	// (the old content is dropped). Size becomes Units, the terminator is written; returns Data.
	FRT_CORE_API wchar_t* Prepare (uint64 Units) noexcept;

	void ReleaseBlock () noexcept
	{
		if (Data != Inline)
		{
			string_detail::Free(Data);
			Data = Inline;
			Capacity = InlineCapacity;
		}
	}

	// Other's text into this (inline and empty); Other becomes empty and inline.
	void Take (FWideString& Other) noexcept
	{
		Size = Other.Size;
		if (Other.Data == Other.Inline)
		{
			std::wmemcpy(Inline, Other.Inline, static_cast<size_t>(Size) + 1u);
		}
		else
		{
			Data = Other.Data;
			Capacity = Other.Capacity;
			Other.Data = Other.Inline;
			Other.Capacity = InlineCapacity;
		}
		Other.Size = 0u;
		Other.Inline[0] = 0;
	}

	wchar_t* Data;            // Inline or a string_detail block
	uint32 Size;
	uint32 Capacity;          // terminator excluded
	wchar_t Inline[InlineCapacity + 1];
};

// == FWideString(Utf8)
[[nodiscard]] FRT_CORE_API FWideString ToWide (FStringView Utf8) noexcept;
}
