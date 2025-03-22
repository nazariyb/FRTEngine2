#include "TLSF.h"

#include <new>
#include <cstring>

#include "Asserts.h"
#include "Math/MathUtility.h"

namespace frt::memory
{
#pragma region BlockHeader

	uint64 TLSF::SBlockHeader::GetSize() const
	{
		return Size & ~(FreeBit | PrevFreeBit);
	}

	void TLSF::SBlockHeader::SetSize(uint64 NewSize)
	{
		const uint64 oldSize = Size;
		Size = NewSize | (oldSize & (FreeBit | PrevFreeBit));
	}

	bool TLSF::SBlockHeader::IsLast() const
	{
		return GetSize() == 0;
	}

	bool TLSF::SBlockHeader::IsFree() const
	{
		return Size & FreeBit;
	}

	void TLSF::SBlockHeader::SetFree()
	{
		Size |= FreeBit;
	}

	void TLSF::SBlockHeader::SetUsed()
	{
		Size &= ~FreeBit;
	}

	bool TLSF::SBlockHeader::IsPrevFree() const
	{
		return Size & PrevFreeBit;
	}

	void TLSF::SBlockHeader::SetPrevFree()
	{
		Size |= PrevFreeBit;
	}

	void TLSF::SBlockHeader::SetPrevUsed()
	{
		Size &= ~PrevFreeBit;
	}

	TLSF::SBlockHeader* TLSF::SBlockHeader::Cast(void* Ptr)
	{
		return reinterpret_cast<SBlockHeader*>((uint8*)Ptr - StartOffset);
	}

	void* TLSF::SBlockHeader::Cast(SBlockHeader* Block)
	{
		return (uint8*)Block + StartOffset;
	}

	TLSF::SBlockHeader* TLSF::SBlockHeader::OffsetToBlock(const void* Ptr, int64 InSize)
	{
		return reinterpret_cast<SBlockHeader*>((uint8*)Ptr + InSize);
	}

	TLSF::SBlockHeader* TLSF::SBlockHeader::PrevBlock()
	{
		frt_assert(IsPrevFree());
		return PrevPhysBlock;
	}

	TLSF::SBlockHeader* TLSF::SBlockHeader::NextBlock()
	{
		frt_assert(!IsLast());
		SBlockHeader* next= OffsetToBlock(Cast(this), GetSize() - Overhead);
		return next;
	}

	TLSF::SBlockHeader* TLSF::SBlockHeader::LinkNext()
	{
		SBlockHeader* next = NextBlock();
		next->PrevPhysBlock = this;
		return next;
	}

	void TLSF::SBlockHeader::MarkAsFree()
	{
		SBlockHeader* next = LinkNext();
		next->SetPrevFree();
		SetFree();
	}

	void TLSF::SBlockHeader::MarkAsUsed()
	{
		SBlockHeader* next = NextBlock();
		next->SetPrevUsed();
		SetUsed();
	}

#pragma endregion BlockHeader

#pragma region Control

	TLSF::SControl::SControl()
	{
		BlockNull.NextFree = &BlockNull;
		BlockNull.PrevFree = &BlockNull;

		FirstLevelBitmap = 0ull;
		for (uint32 i = 0u; i < FirstLevelIndexCount; ++i)
		{
			SecondLevelBitmap[i] = 0ull;
			for (uint32 j = 0u; j < SecondLevelIndexCount; ++j)
			{
				FreeBlocks[i][j] = &BlockNull;
			}
		}
	}

	TLSF::SBlockHeader* TLSF::SControl::SearchSuitableBlock(uint32& InOutFli, uint32& InOutSli)
	{
		uint32 fl = InOutFli, sl = InOutSli;

		// Search for a block in the list associated with the given fl/sl index
		uint64 slMap = SecondLevelBitmap[fl] & (~0ull << sl);
		if (!slMap)
		{
			// If block doesn't exist, search in the next largest first-level list
			const uint64 flMap = FirstLevelBitmap & (~0u << (fl + 1u));
			if (!flMap)
			{
				return nullptr;
			}

			fl = math::GetIndexOfFirstOneBit(flMap);
			InOutFli = fl;
			slMap = SecondLevelBitmap[fl];
		}

		frt_assert(slMap);
		sl = math::GetIndexOfFirstOneBit(slMap);
		InOutSli = sl;

		return FreeBlocks[fl][sl];
	}

	void TLSF::SControl::RemoveFreeBlock(SBlockHeader* Block, uint32 Fl, uint32 Sl)
	{
		SBlockHeader* prev = Block->PrevFree;
		SBlockHeader* next = Block->NextFree;
		frt_assert(prev);
		frt_assert(next);
		next->PrevFree = prev;
		prev->NextFree = next;

		// Set new list head
		if (FreeBlocks[Fl][Sl] == Block)
		{
			FreeBlocks[Fl][Sl] = next;

			// If the new head is null, clear the bitmap
			if (next == &BlockNull)
			{
				SecondLevelBitmap[Fl] &= ~(1ull << Sl);

				// If the second bitmap is empty, also clear the first one
				if (!SecondLevelBitmap[Fl])
				{
					FirstLevelBitmap &= ~(1ull << Fl);
				}
			}
		}
	}

	void TLSF::SControl::InsertFreeBlock(SBlockHeader* Block, uint32 Fl, uint32 Sl)
	{
		frt_assert(Block);

		SBlockHeader* current = FreeBlocks[Fl][Sl];
		frt_assert(current);

		Block->PrevFree = &BlockNull;
		Block->NextFree = current;
		current->PrevFree = Block;

		frt_assert(SBlockHeader::Cast(Block) == AlignPtr(SBlockHeader::Cast(Block), AlignSize));

		FreeBlocks[Fl][Sl] = Block;
		FirstLevelBitmap |= (1ull << Fl);
		SecondLevelBitmap[Fl] |= (1ull << Sl);
	}

	void TLSF::SControl::RemoveBlockFromFreeList(SBlockHeader* Block)
	{
		uint32 fl, sl;
		MappingInsert(Block->GetSize(), fl, sl);
		RemoveFreeBlock(Block, fl, sl);
	}

	void TLSF::SControl::InsertBlockIntoFreeList(SBlockHeader* Block)
	{
		uint32 fl, sl;
		MappingInsert(Block->GetSize(), fl, sl);
		InsertFreeBlock(Block, fl, sl);
	}

	bool TLSF::SControl::CanSplitBlock(SBlockHeader* Block, uint64 Size) const
	{
		return Block->GetSize() >= sizeof(SBlockHeader) + Size;
	}

	TLSF::SBlockHeader* TLSF::SControl::SplitBlock(SBlockHeader* Block, uint64 Size)
	{
		SBlockHeader* remaining = SBlockHeader::OffsetToBlock(SBlockHeader::Cast(Block), Size - Overhead);
		const uint64 remainingSize = Block->GetSize() - (Size + Overhead);
		frt_assert(SBlockHeader::Cast(remaining) == AlignPtr(SBlockHeader::Cast(remaining), AlignSize));

		frt_assert(Block->GetSize() == remainingSize + Size + Overhead);
		remaining->SetSize(remainingSize);
		frt_assert(remaining->GetSize() >= BlockMinSize);

		Block->SetSize(Size);
		remaining->MarkAsFree();

		return remaining;
	}

	TLSF::SBlockHeader* TLSF::SControl::AbsorbBlock(SBlockHeader* Block, SBlockHeader* PrevBlock)
	{
		frt_assert(!PrevBlock->IsLast());
		PrevBlock->Size += Block->GetSize() + Overhead;
		PrevBlock->LinkNext();
		return PrevBlock;
	}

	TLSF::SBlockHeader* TLSF::SControl::MergeBlockPrev(SBlockHeader* Block)
	{
		if (Block->IsPrevFree())
		{
			SBlockHeader* prev = Block->PrevBlock();
			frt_assert(prev);
			frt_assert(prev->IsFree());
			RemoveBlockFromFreeList(prev);
			Block = AbsorbBlock(Block, prev);
		}

		return Block;
	}

	TLSF::SBlockHeader* TLSF::SControl::MergeBlockNext(SBlockHeader* Block)
	{
		SBlockHeader* next = Block->NextBlock();
		frt_assert(next);

		if (next->IsFree())
		{
			frt_assert(!Block->IsLast());
			RemoveBlockFromFreeList(next);
			Block = AbsorbBlock(next, Block);
		}

		return Block;
	}

	void TLSF::SControl::TrimBlockFree(SBlockHeader* Block, uint64 Size)
	{
		frt_assert(Block->IsFree());
		if (CanSplitBlock(Block, Size))
		{
			SBlockHeader* remainingBlock = SplitBlock(Block, Size);
			Block->LinkNext();
			remainingBlock->SetPrevFree();
			InsertBlockIntoFreeList(remainingBlock);
		}
	}

	void TLSF::SControl::TrimBlockUsed(SBlockHeader* Block, uint64 Size)
	{
		frt_assert(!Block->IsFree());
		if (CanSplitBlock(Block, Size))
		{
			SBlockHeader* remainingBlock = SplitBlock(Block, Size);
			remainingBlock->SetPrevUsed();

			remainingBlock = MergeBlockNext(remainingBlock);
			InsertBlockIntoFreeList(remainingBlock);
		}
	}

	TLSF::SBlockHeader* TLSF::SControl::TrimBlockFreeLeading(SBlockHeader* Block, uint64 Size)
	{
		SBlockHeader* remainingBlock = Block;
		if (CanSplitBlock(Block, Size))
		{
			remainingBlock = SplitBlock(Block, Size - Overhead);
			remainingBlock->SetPrevFree();

			Block->LinkNext();
			InsertBlockIntoFreeList(Block);
		}

		return remainingBlock;
	}

	TLSF::SBlockHeader* TLSF::SControl::LocateBlockFree(uint64 Size)
	{
		uint32 fl = 0u, sl = 0u;
		SBlockHeader* block = nullptr;

		if (Size)
		{
			MappingSearch(Size, fl, sl);
			if (fl < FirstLevelIndexCount)
			{
				block = SearchSuitableBlock(fl, sl);
			}
		}

		if (block)
		{
			frt_assert(block->GetSize() >= Size);
			RemoveFreeBlock(block, fl, sl);
		}

		return block;
	}

	void* TLSF::SControl::PrepareBlockUsed(SBlockHeader* Block, uint64 Size)
	{
		void* ptr = nullptr;
		if (Block)
		{
			frt_assert(Size > 0ull);
			TrimBlockFree(Block, Size);
			Block->MarkAsUsed();
			ptr = SBlockHeader::Cast(Block);
		}
		return ptr;
	}

#pragma endregion Control

	uint64 TLSF::AlignUp(uint64 Value, uint64 Align)
	{
		frt_assert(math::IsPowerOfTwo(Align));
		return (Value + (Align - 1)) & ~(Align - 1);
	}

	uint64 TLSF::AlignDown(uint64 Value, uint64 Align)
	{
		frt_assert(math::IsPowerOfTwo(Align));
		return Value - (Value & (Align - 1));
	}

	void* TLSF::AlignPtr(const void* Ptr, uint64 Align)
	{
		return (void*)AlignUp((uint64)Ptr, Align);
	}

	uint64 TLSF::AdjustRequestedSize(uint64 Size, uint64 Align)
	{
		uint64 adjust = 0ull;
		if (Size)
		{
			const uint64 aligned = AlignUp(Size, Align);

			if (aligned < BlockMaxSize)
			{
				adjust = math::Max(aligned , BlockMinSize);
			}
		}
		return adjust;
	}

	void TLSF::MappingInsert(uint64 Size, uint32& OutFli, uint32& OutSli)
	{
		uint32 fl, sl;
		if (Size < SmallBlockSize)
		{
			// Store small blocks in the first list
			fl = 0;
			sl = (uint32)(Size / (uint64)(SmallBlockSize / SecondLevelIndexCount));
		}
		else
		{
			fl = math::GetIndexOfFirstOneBit(Size);
			sl = (uint32)(Size >> (fl - SecondLevelIndexCountLog2)) ^ (1u << SecondLevelIndexCountLog2);
			fl -= (FirstLevelIndexShift - 1u);
		}
		OutFli = fl;
		OutSli = sl;
	}

	void TLSF::MappingSearch(uint64 Size, uint32& OutFli, uint32& OutSli)
	{
		if (Size >= SmallBlockSize)
		{
			const uint64 round = (1 << (math::GetIndexOfFirstOneBit(Size) - SecondLevelIndexCountLog2)) - 1ll;
			Size += round;
		}
		MappingInsert(Size, OutFli, OutSli);
	}

#pragma region TLSF Core

	TLSF::TLSF()
	{
	}

	TLSF::TLSF(uint64 InSize)
	{
		new (this) SControl;
		AddPool((uint8*)this + GetSize(), InSize - GetSize());
	}

	TLSF::~TLSF()
	{
	}

	void* TLSF::AddPool(void* InMemory, uint64 InSize)
	{
		frt_assert((uint64)InMemory % AlignSize == 0);

		constexpr uint64 poolOverhead = GetPoolOverhead();
		const uint64 poolBytes = AlignDown(InSize - poolOverhead, AlignSize);

		if (poolBytes < BlockMinSize || poolBytes > BlockMaxSize)
		{
			// print warning
			return nullptr;
		}

		SBlockHeader* block = SBlockHeader::OffsetToBlock(InMemory, -(int64)Overhead);
		block->SetSize(poolBytes);
		block->SetFree();
		block->SetPrevUsed();
		((SControl*)this)->InsertBlockIntoFreeList(block);

		SBlockHeader* next = block->LinkNext();
		next->SetSize(0ull);
		next->SetUsed();
		next->SetPrevFree();

		return InMemory;
	}

	void TLSF::RemovePool(void* InMemory)
	{
		SBlockHeader* block = SBlockHeader::OffsetToBlock(InMemory, -(int64)Overhead);

		frt_assert(block->IsFree());
		frt_assert(!block->NextBlock()->IsFree());
		frt_assert(block->NextBlock()->GetSize() == 0ull);

		uint32 fl, sl;

		MappingInsert(block->GetSize(), fl, sl);
		((SControl*)InMemory)->RemoveFreeBlock(block, fl, sl);
	}

	void* TLSF::Malloc(uint64 Size)
	{
		const uint64 adjust = AdjustRequestedSize(Size, AlignSize);
		SControl* control = (SControl*)this;
		SBlockHeader* block = control->LocateBlockFree(adjust);
		void* memory = control->PrepareBlockUsed(block, adjust);
		return memory;
	}

	void* TLSF::Realloc(void* Memory, uint64 Size)
	{
		SControl* control = (SControl*)this;
		void* newMemory = nullptr;

		// Zero size request is treated as free
		if (Memory && Size == 0ull)
		{
			Free(Memory);
		}
		// nullptr treated as malloc
		else if (!Memory)
		{
			newMemory = Malloc(Size);
		}
		else
		{
			SBlockHeader* block = SBlockHeader::Cast(Memory);
			SBlockHeader* next = block->NextBlock();

			frt_assert(!block->IsFree());

			const uint64 currentSize = block->GetSize();
			const uint64 combinedSize = currentSize + next->GetSize() + Overhead;
			const uint64 adjust = AdjustRequestedSize(Size, AlignSize);

			if (adjust > currentSize && (!next->IsFree() || adjust > combinedSize))
			{
				newMemory = Malloc(Size);
				if (newMemory)
				{
					const uint64 minSize = math::Min(currentSize, Size);
					std::memcpy(newMemory, Memory, minSize);
					std::memset((uint8*)newMemory + minSize, 0, Size - minSize);
					Free(Memory);
				}
			}
			else
			{
				if (adjust > currentSize)
				{
					control->MergeBlockNext(block);
					block->MarkAsUsed();
				}

				control->TrimBlockUsed(block, adjust);
				newMemory = Memory;
			}
		}

		return newMemory;
	}

	void* TLSF::Memalign(uint64 Align, uint64 Size)
	{
		const uint64 adjust = AdjustRequestedSize(Size, AlignSize);
		SControl* control = (SControl*)this;

		const uint64 gapMin = sizeof(SBlockHeader);
		const uint64 sizeWithGap = AdjustRequestedSize(adjust + Align + gapMin, Align);

		const uint64 alignedSize = (adjust && Align > AlignSize) ? sizeWithGap : adjust;

		SBlockHeader* block = control->LocateBlockFree(alignedSize);

		frt_assert(sizeof(SBlockHeader) == BlockMinSize + Overhead);

		if (block)
		{
			void* ptr = SBlockHeader::Cast(block);
			void* aligned = AlignPtr(ptr, Align);
			uint64 gap = (uint8*)aligned - ptr;

			if (gap && gap < gapMin)
			{
				const uint64 gapRemain = gapMin - gap;
				const uint64 offset = math::Max(gapRemain, Align);
				const void* nextAligned = (uint8*)aligned + offset;

				aligned = AlignPtr(nextAligned, Align);
				gap = (uint8*)aligned - ptr;
			}

			if (gap)
			{
				frt_assert(gap >= gapMin);
				block = control->TrimBlockFreeLeading(block, gap);
			}
		}

		return control->PrepareBlockUsed(block, adjust);
	}

	void TLSF::Free(void* Memory)
	{
		if (Memory)
		{
			SControl* control = (SControl*)this;
			SBlockHeader* block = SBlockHeader::Cast(Memory);
			frt_assert(!block->IsFree());

			block->MarkAsFree();
			block = control->MergeBlockPrev(block);
			block = control->MergeBlockNext(block);
			control->InsertBlockIntoFreeList(block);
		}
	}

#pragma endregion TLSF Core
}
