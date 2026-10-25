#pragma once

#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "CoreTypes.h"
#include "Asserts.h"
#include "Containers/String.h"


/**
 * The registry's storage primitives (design §5.2 "Mutation strategy", §5.9): columns split into fixed pages
 * that views share copy-on-write, so the owner thread can commit a delta while other threads keep reading
 * the view they acquired, and a commit copies only the pages it touches.
 *
 * The rule every container here follows: a page carries the store epoch it was created in. The store bumps
 * its epoch whenever it publishes a view (CRegistryStore), which freezes every existing page: the next write
 * to a frozen page clones it first (GetMutable), so a published view never sees a byte change. Readers never
 * write and never look at epochs. Everything lives on the CRT heap (std containers), never the engine's
 * memory pool: views are read from any thread.
 *
 * Copying a container copies its page pointers only (O(pages)): that copy is how a view is published.
 */
namespace frt::assets
{
inline constexpr uint32 RegistryPageRowsLog2 = 12u;
inline constexpr uint32 RegistryPageRows = 1u << RegistryPageRowsLog2;   // 4096 rows per page (§5.2)
inline constexpr uint32 RegistryPageMask = RegistryPageRows - 1u;


/**
 * A column of T in pages of 4096 rows. Rows are appended at the end and stay where they are; a write to a
 * row in a frozen page clones that page. T is default-constructible and copyable (trivial types are copied
 * as bytes).
 */
template <typename T>
class CPagedColumn
{
public:
	struct SPage
	{
		uint64 Epoch = 0u;
		T Rows[RegistryPageRows] {};
	};

	[[nodiscard]] uint32 Size () const noexcept { return Count; }
	[[nodiscard]] bool IsEmpty () const noexcept { return Count == 0u; }

	[[nodiscard]] const T& operator[] (uint32 Row) const noexcept
	{
		frt_assert(Row < Count);
		return Pages[Row >> RegistryPageRowsLog2]->Rows[Row & RegistryPageMask];
	}

	// The row for writing: clones its page first when the page belongs to an older epoch (a published view).
	[[nodiscard]] T& GetMutable (uint32 Row, uint64 Epoch)
	{
		frt_assert(Row < Count);
		std::shared_ptr<SPage>& page = Pages[Row >> RegistryPageRowsLog2];
		if (page->Epoch != Epoch)
		{
			std::shared_ptr<SPage> copy = std::make_shared<SPage>(*page);
			copy->Epoch = Epoch;
			page = std::move(copy);
		}
		return page->Rows[Row & RegistryPageMask];
	}

	void Set (uint32 Row, const T& Value, uint64 Epoch) { GetMutable(Row, Epoch) = Value; }

	// Writes only when the value differs, so an unchanged row never clones its page.
	void SetIfChanged (uint32 Row, const T& Value, uint64 Epoch)
		requires std::equality_comparable<T>
	{
		if (!((*this)[Row] == Value))
		{
			GetMutable(Row, Epoch) = Value;
		}
	}

	// Appends a row; returns its index.
	uint32 Append (const T& Value, uint64 Epoch)
	{
		const uint32 row = Count;
		Grow(Count + 1u, Epoch);
		GetMutable(row, Epoch) = Value;
		return row;
	}

	// Grows to NewSize rows (new rows are T{}); never shrinks.
	void Grow (uint32 NewSize, uint64 Epoch)
	{
		while (NewSize > static_cast<uint32>(Pages.size()) * RegistryPageRows)
		{
			std::shared_ptr<SPage> page = std::make_shared<SPage>();
			page->Epoch = Epoch;
			Pages.push_back(std::move(page));
		}
		if (NewSize > Count)
		{
			Count = NewSize;
		}
	}

	// Drops every row (the pages go with the last view that holds them).
	void Clear () noexcept
	{
		Pages.clear();
		Count = 0u;
	}

	[[nodiscard]] uint32 GetPageCount () const noexcept { return static_cast<uint32>(Pages.size()); }
	// The rows of one page (the last page's span ends at Size()).
	[[nodiscard]] std::span<const T> GetPage (uint32 PageIndex) const noexcept
	{
		const uint32 first = PageIndex * RegistryPageRows;
		const uint32 rows = Count - first < RegistryPageRows ? Count - first : RegistryPageRows;
		return std::span<const T>(Pages[PageIndex]->Rows, rows);
	}
	// The page's identity, for tests of copy-on-write sharing.
	[[nodiscard]] const void* GetPageAddress (uint32 PageIndex) const noexcept { return Pages[PageIndex].get(); }

	// Bytes of every page this column holds (shared pages included: what a view keeps alive).
	[[nodiscard]] uint64 GetResidentBytes () const noexcept
	{
		return static_cast<uint64>(Pages.size()) * sizeof(SPage) + Pages.capacity() * sizeof(std::shared_ptr<SPage>);
	}

private:
	std::vector<std::shared_ptr<SPage>> Pages;
	uint32 Count = 0u;
};


/**
 * One bit per row, packed into a paged column of 64-bit words (the type tables' Dead and PresentBits).
 */
class CPagedBitmap
{
public:
	[[nodiscard]] uint32 Size () const noexcept { return Rows; }

	[[nodiscard]] bool Test (uint32 Row) const noexcept
	{
		frt_assert(Row < Rows);
		return ((Words[Row >> 6u] >> (Row & 63u)) & 1u) != 0u;
	}

	void Set (uint32 Row, bool bValue, uint64 Epoch)
	{
		frt_assert(Row < Rows);
		const uint64 bit = uint64(1) << (Row & 63u);
		const uint64 word = Words[Row >> 6u];
		const uint64 updated = bValue ? (word | bit) : (word & ~bit);
		if (updated != word)
		{
			Words.Set(Row >> 6u, updated, Epoch);
		}
	}

	// Grows to NewRows bits (new bits are 0).
	void Grow (uint32 NewRows, uint64 Epoch)
	{
		Words.Grow((NewRows + 63u) >> 6u, Epoch);
		if (NewRows > Rows)
		{
			Rows = NewRows;
		}
	}

	void Clear () noexcept
	{
		Words.Clear();
		Rows = 0u;
	}

	// The word holding bits [64 * Word, 64 * Word + 64): for loops that build query bitmasks.
	[[nodiscard]] uint64 GetWord (uint32 Word) const noexcept { return Words[Word]; }
	[[nodiscard]] uint32 GetWordCount () const noexcept { return Words.Size(); }
	[[nodiscard]] const CPagedColumn<uint64>& GetWords () const noexcept { return Words; }
	[[nodiscard]] uint64 GetResidentBytes () const noexcept { return Words.GetResidentBytes(); }

private:
	CPagedColumn<uint64> Words;
	uint32 Rows = 0u;
};


/**
 * Per-row lists of T appended to one shared pool (§5.2 "CPagedOverflow": MultiValue tags; also labels and
 * companions). Replacing a row's list appends the new list and leaves the old one as garbage until the
 * table is rebuilt; lists are never edited in place, so a view's lists never change.
 */
template <typename T>
class CPagedPool
{
public:
	// Appends Values contiguously; returns the index of the first one.
	uint32 Append (std::span<const T> Values, uint64 Epoch)
	{
		const uint32 first = Items.Size();
		if (!Values.empty())
		{
			Items.Grow(first + static_cast<uint32>(Values.size()), Epoch);
			for (uint32 i = 0u; i < Values.size(); ++i)
			{
				Items.Set(first + i, Values[i], Epoch);
			}
		}
		return first;
	}

	[[nodiscard]] const T& operator[] (uint32 Index) const noexcept { return Items[Index]; }
	[[nodiscard]] uint32 Size () const noexcept { return Items.Size(); }
	void Clear () noexcept { Items.Clear(); }
	[[nodiscard]] uint64 GetResidentBytes () const noexcept { return Items.GetResidentBytes(); }

private:
	CPagedColumn<T> Items;
};

// A row's list in a CPagedPool.
struct SPoolRange
{
	uint32 Begin = 0u;
	uint32 Count = 0u;

	constexpr bool operator== (const SPoolRange&) const noexcept = default;
};


/**
 * Append-only UTF-8 strings addressed by id (NAMES, STRINGS and the registry's path pool). Bytes live in
 * 64 KiB blocks (a longer string gets a block of its own); a string never moves and never changes. A new
 * string may be written into the unused tail of a block that a published view shares: no reader ever reads
 * bytes past the strings its view knows, so those bytes are not shared memory in the C++ sense. The fill
 * level of the last block belongs to this copy of the pool, never to the block.
 */
class CStringPool
{
public:
	static constexpr uint32 BlockBytes = 64u * 1024u;

	struct SRef
	{
		uint32 Block = 0u;
		uint32 Offset = 0u;
		uint32 Length = 0u;
	};

	[[nodiscard]] uint32 Size () const noexcept { return Refs.Size(); }

	[[nodiscard]] FStringView Get (uint32 Id) const noexcept
	{
		const SRef& ref = Refs[Id];
		return FStringView(Blocks[ref.Block]->Data.get() + ref.Offset, ref.Length);
	}

	uint32 Add (FStringView Text, uint64 Epoch)
	{
		return Refs.Append(Store(Text), Epoch);
	}

	// Stores bytes without giving them an id (a folded shadow, CNamePool).
	SRef Store (FStringView Text)
	{
		SRef ref;
		ref.Length = Text.Len();
		if (Text.Len() > BlockBytes)
		{
			ref.Block = NewBlock(Text.Len());
			LastBlockUsed = Text.Len();   // full: nothing else goes into an oversized block
		}
		else
		{
			if (Blocks.empty() || LastBlockUsed + Text.Len() > Blocks.back()->Capacity)
			{
				NewBlock(BlockBytes);
				LastBlockUsed = 0u;
			}
			ref.Block = static_cast<uint32>(Blocks.size() - 1u);
			ref.Offset = LastBlockUsed;
			LastBlockUsed += Text.Len();
		}
		if (Text.Len() > 0u)
		{
			std::memcpy(Blocks[ref.Block]->Data.get() + ref.Offset, Text.GetData(), Text.Len());
		}
		return ref;
	}

	[[nodiscard]] const SRef& GetRef (uint32 Id) const noexcept { return Refs[Id]; }

	[[nodiscard]] FStringView Resolve (const SRef& Ref) const noexcept
	{
		return FStringView(Blocks[Ref.Block]->Data.get() + Ref.Offset, Ref.Length);
	}

	void Clear () noexcept
	{
		Blocks.clear();
		Refs.Clear();
		LastBlockUsed = 0u;
	}

	[[nodiscard]] uint64 GetResidentBytes () const noexcept
	{
		uint64 bytes = Refs.GetResidentBytes() + Blocks.capacity() * sizeof(std::shared_ptr<SBlock>);
		for (const std::shared_ptr<SBlock>& block : Blocks)
		{
			bytes += sizeof(SBlock) + block->Capacity;
		}
		return bytes;
	}

private:
	struct SBlock
	{
		std::unique_ptr<char[]> Data;
		uint32 Capacity = 0u;
	};

	uint32 NewBlock (uint32 Capacity)
	{
		std::shared_ptr<SBlock> block = std::make_shared<SBlock>();
		block->Data.reset(new char[Capacity == 0u ? 1u : Capacity]);
		block->Capacity = Capacity;
		Blocks.push_back(std::move(block));
		return static_cast<uint32>(Blocks.size() - 1u);
	}

	std::vector<std::shared_ptr<SBlock>> Blocks;
	CPagedColumn<SRef> Refs;
	uint32 LastBlockUsed = 0u;
};


/**
 * An open-addressing hash index from a 64-bit hash to uint32 values (row numbers), with linear probing over
 * a paged slot column, so an insert copies one page of a published view, never the table. It stores no
 * keys: the caller compares candidates against its own columns. Several entries may share a hash or a key
 * (multi-maps: FILES rows equal under a case-insensitive rule, records equal under the portable fold).
 *
 * A slot is the hash's upper 32 bits (the probe start, also after a rehash) and Value + 1; 0 is empty and
 * 1 << 32 a tombstone. The table grows (and drops its tombstones) at half load. The in-memory hashes used
 * here (wyhash) are never persisted: snapshots rebuild every index (§5.3).
 */
class CPagedHashIndex
{
public:
	// Visit(uint32 Value) -> bool: false stops. Visits the entries whose stored hash bits equal Hash's.
	template <typename TVisit>
	void ForEach (uint64 Hash, TVisit&& Visit) const
	{
		if (Capacity == 0u)
		{
			return;
		}
		const uint32 tag = MakeTag(Hash);
		const uint32 mask = Capacity - 1u;
		for (uint32 position = tag & mask;; position = (position + 1u) & mask)
		{
			const uint64 slot = Slots[position];
			if (slot == EmptySlot)
			{
				return;
			}
			if (slot != TombstoneSlot && static_cast<uint32>(slot >> 32u) == tag)
			{
				if (!Visit(static_cast<uint32>(slot) - 1u))
				{
					return;
				}
			}
		}
	}

	void Insert (uint64 Hash, uint32 Value, uint64 Epoch)
	{
		frt_assert(Value != ~0u);
		if ((Used + Tombstones + 1u) * 2u > Capacity)
		{
			Rehash(Used + 1u, Epoch);
		}
		const uint32 tag = MakeTag(Hash);
		const uint32 mask = Capacity - 1u;
		for (uint32 position = tag & mask;; position = (position + 1u) & mask)
		{
			const uint64 slot = Slots[position];
			if (slot == EmptySlot || slot == TombstoneSlot)
			{
				if (slot == TombstoneSlot)
				{
					--Tombstones;
				}
				Slots.Set(position, MakeSlot(tag, Value), Epoch);
				++Used;
				return;
			}
		}
	}

	// Removes one entry (Hash, Value); false when there is none.
	bool Erase (uint64 Hash, uint32 Value, uint64 Epoch)
	{
		if (Capacity == 0u)
		{
			return false;
		}
		const uint32 tag = MakeTag(Hash);
		const uint64 wanted = MakeSlot(tag, Value);
		const uint32 mask = Capacity - 1u;
		for (uint32 position = tag & mask;; position = (position + 1u) & mask)
		{
			const uint64 slot = Slots[position];
			if (slot == EmptySlot)
			{
				return false;
			}
			if (slot == wanted)
			{
				Slots.Set(position, TombstoneSlot, Epoch);
				--Used;
				++Tombstones;
				return true;
			}
		}
	}

	void Clear () noexcept
	{
		Slots.Clear();
		Capacity = 0u;
		Used = 0u;
		Tombstones = 0u;
	}

	// Sizes the table for Count entries up front (a bulk load).
	void Reserve (uint32 Count, uint64 Epoch)
	{
		if ((Count + Tombstones) * 2u > Capacity)
		{
			Rehash(Count, Epoch);
		}
	}

	[[nodiscard]] uint32 GetCount () const noexcept { return Used; }
	[[nodiscard]] uint32 GetCapacity () const noexcept { return Capacity; }
	[[nodiscard]] uint64 GetResidentBytes () const noexcept { return Slots.GetResidentBytes(); }

private:
	static constexpr uint64 EmptySlot = 0u;
	static constexpr uint64 TombstoneSlot = uint64(1) << 32u;

	[[nodiscard]] static uint32 MakeTag (uint64 Hash) noexcept { return static_cast<uint32>(Hash >> 32u); }
	[[nodiscard]] static uint64 MakeSlot (uint32 Tag, uint32 Value) noexcept
	{
		return (static_cast<uint64>(Tag) << 32u) | static_cast<uint64>(Value + 1u);
	}

	// Sizes the table so MinEntries fill at most 3/8 of it (it grows again past half), which keeps probes
	// short without quadrupling: a quarter-full table cost 32-64 bytes per entry.
	void Rehash (uint32 MinEntries, uint64 Epoch)
	{
		uint32 capacity = 16u;
		while (static_cast<uint64>(capacity) * 3u < static_cast<uint64>(MinEntries) * 8u)
		{
			capacity *= 2u;
		}
		CPagedColumn<uint64> slots;
		slots.Grow(capacity, Epoch);
		const uint32 mask = capacity - 1u;
		for (uint32 i = 0u; i < Capacity; ++i)
		{
			const uint64 slot = Slots[i];
			if (slot == EmptySlot || slot == TombstoneSlot)
			{
				continue;
			}
			for (uint32 position = static_cast<uint32>(slot >> 32u) & mask;; position = (position + 1u) & mask)
			{
				if (slots[position] == EmptySlot)
				{
					slots.Set(position, slot, Epoch);
					break;
				}
			}
		}
		Slots = std::move(slots);
		Capacity = capacity;
		Tombstones = 0u;
	}

	CPagedColumn<uint64> Slots;
	uint32 Capacity = 0u;
	uint32 Used = 0u;
	uint32 Tombstones = 0u;
};


// Spreads a small integer key (a row number) over 64 bits for CPagedHashIndex.
[[nodiscard]] constexpr uint64 MixRegistryKey (uint64 Key) noexcept
{
	Key ^= Key >> 33u;
	Key *= 0xff51afd7ed558ccdull;
	Key ^= Key >> 33u;
	Key *= 0xc4ceb9fe1a85ec53ull;
	Key ^= Key >> 33u;
	return Key;
}
}
