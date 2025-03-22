#pragma once
#include <cstddef>

#include "CoreTypes.h"

namespace frt::memory
{
	struct TLSF
	{
		static constexpr uint32 SecondLevelIndexCountLog2 = 5u; // public

		static constexpr uint32 AlignSizeLog2 = 3u;
		static constexpr uint32 AlignSize = 1u << AlignSizeLog2;
		static constexpr uint32 FirstLevelIndexMax = 48u;
		static constexpr uint32 SecondLevelIndexCount = 1u << SecondLevelIndexCountLog2;
		static constexpr uint32 FirstLevelIndexShift = SecondLevelIndexCountLog2 + AlignSizeLog2;
		static constexpr uint32 FirstLevelIndexCount = FirstLevelIndexMax - FirstLevelIndexShift + 1u;

		static constexpr uint32 SmallBlockSize = 1u << FirstLevelIndexShift;

		struct SBlockHeader
		{
			SBlockHeader* PrevPhysBlock = nullptr;
			uint64 Size = 0u;

			// Valid only if block is free
			SBlockHeader* NextFree = nullptr;
			SBlockHeader* PrevFree = nullptr;

			[[nodiscard]] uint64 GetSize() const;
			void SetSize(uint64 NewSize);

			[[nodiscard]] bool IsLast() const;
			[[nodiscard]] bool IsFree() const;

			void SetFree();
			void SetUsed();

			[[nodiscard]] bool IsPrevFree() const;
			void SetPrevFree();
			void SetPrevUsed();

			static SBlockHeader* Cast(void* Ptr);
			static void* Cast(SBlockHeader* Block);

			/** Return location of next block after block of given size */
			static SBlockHeader* OffsetToBlock(const void* Ptr, int64 InSize);
			SBlockHeader* PrevBlock();
			SBlockHeader* NextBlock();
			/** Link a new block with its physical neighbor, return the neighbor. */
			SBlockHeader* LinkNext();
			void MarkAsFree();
			void MarkAsUsed();
		};

		static constexpr uint64 FreeBit = 1ull << 0;
		static constexpr uint64 PrevFreeBit = 1ull << 1;
		static constexpr uint64 Overhead = sizeof(uint64);
		static constexpr uint64 StartOffset = offsetof(SBlockHeader, Size) + sizeof(uint64);
		static constexpr uint64 BlockMinSize = sizeof(SBlockHeader) - sizeof(SBlockHeader*);
		static constexpr uint64 BlockMaxSize = 1ull << FirstLevelIndexMax;

		struct SControl
		{
			SBlockHeader BlockNull;

			uint64 FirstLevelBitmap;
			uint64 SecondLevelBitmap[FirstLevelIndexCount];

			SBlockHeader* FreeBlocks[FirstLevelIndexCount][SecondLevelIndexCount];

			SControl();

			SBlockHeader* SearchSuitableBlock(uint32& InOutFli, uint32& InOutSli);
			void RemoveFreeBlock(SBlockHeader* Block, uint32 Fl, uint32 Sl);
			void InsertFreeBlock(SBlockHeader* Block, uint32 Fl, uint32 Sl);
			void RemoveBlockFromFreeList(SBlockHeader* Block);
			void InsertBlockIntoFreeList(SBlockHeader* Block);

			bool CanSplitBlock(SBlockHeader* Block, uint64 Size) const;
			SBlockHeader* SplitBlock(SBlockHeader* Block, uint64 Size);
			SBlockHeader* AbsorbBlock(SBlockHeader* Block, SBlockHeader* PrevBlock);
			SBlockHeader* MergeBlockPrev(SBlockHeader* Block);
			SBlockHeader* MergeBlockNext(SBlockHeader* Block);
			void TrimBlockFree(SBlockHeader* Block, uint64 Size);
			void TrimBlockUsed(SBlockHeader* Block, uint64 Size);
			SBlockHeader* TrimBlockFreeLeading(SBlockHeader* Block, uint64 Size);
			SBlockHeader* LocateBlockFree(uint64 Size);
			void* PrepareBlockUsed(SBlockHeader* Block, uint64 Size);
		};

		static uint64 AlignUp(uint64 Value, uint64 Align);
		static uint64 AlignDown(uint64 Value, uint64 Align);
		static void* AlignPtr(const void* Ptr, uint64 Align);

		static uint64 AdjustRequestedSize(uint64 Size, uint64 Align);
		static void MappingInsert(uint64 Size, uint32& OutFli, uint32& OutSli);
		static void MappingSearch(uint64 Size, uint32& OutFli, uint32& OutSli);

		static constexpr uint64 GetSize() { return sizeof(SControl); }
		static constexpr uint64 GetPoolOverhead() { return 2u * Overhead; }

		TLSF();
		TLSF(uint64 InSize);
		~TLSF();

		void* AddPool(void* InMemory, uint64 InSize);
		void RemovePool(void* InMemory);

		void* Malloc(uint64 Size);
		void* Realloc(void* Memory, uint64 Size);
		void* Memalign(uint64 Align, uint64 Size);
		void Free(void* Memory);
	};
}
