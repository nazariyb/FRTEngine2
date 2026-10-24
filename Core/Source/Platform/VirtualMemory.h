#pragma once

#include "Core.h"
#include "CoreTypes.h"


/**
 * Address space without memory, then memory where it is needed (design §8.15, §15 I27).
 *
 * The asset runtime keeps tables that must never move once handed out (the slot table, §8.2) and
 * large payload blocks it gives back to the OS as soon as they die (CAssetHeap's blocks of 1 MiB or
 * more, §8.7). Both reserve a range of addresses once, sized for the worst case, and commit pages
 * inside it as they fill: no reallocation, no copying, no pointer invalidation, and physical memory
 * only for what is in use. Every platform has these four steps (Windows VirtualAlloc/VirtualFree;
 * POSIX mmap(PROT_NONE), mprotect, madvise + PROT_NONE, munmap; consoles their SDK's equivalents),
 * so they are declared here once and never called directly from engine code.
 *
 * Granularity: a reservation starts on a multiple of GetAllocationGranularity() (64 KiB on
 * Windows) and its size is rounded up to whole pages; commits and decommits work on whole pages
 * (GetPageSize(), 4 KiB on x64) and must lie inside one reservation. Thread-safe (the OS
 * serializes), no engine allocator: usable on any thread and before the memory pool exists.
 */
namespace frt::platform
{
// The size of one page: the unit of CommitVirtual and DecommitVirtual. A power of two.
[[nodiscard]] FRT_CORE_API uint64 GetPageSize () noexcept;
// What reservation addresses are aligned to: a power of two, a multiple of the page size.
[[nodiscard]] FRT_CORE_API uint64 GetAllocationGranularity () noexcept;

// Reserves Bytes of address space (rounded up to whole pages) with no memory behind it: touching it
// before CommitVirtual is an access violation. Returns its base, aligned to the allocation
// granularity, or nullptr when Bytes is 0 or the address space is exhausted. Reserving costs no
// memory, so reserve for the worst case (x64 has 128 TiB of user address space).
[[nodiscard]] FRT_CORE_API void* ReserveVirtual (uint64 Bytes) noexcept;

// Backs [Address, Address + Bytes) with read-write memory. Address and Bytes are rounded out to
// whole pages; the range must lie inside one reservation. Newly committed pages read as zero;
// committing pages that are already committed keeps their contents. False when the system is out
// of commit (memory plus page file) or the range is not reserved.
FRT_CORE_API bool CommitVirtual (void* Address, uint64 Bytes) noexcept;

// Gives the pages of [Address, Address + Bytes) back to the OS and keeps the addresses reserved:
// touching them is an access violation again, and a later CommitVirtual brings them back as
// zeros. Address must be page-aligned and Bytes a whole number of pages (only whole pages can be
// given back; rounding out would drop a neighbour's data). False if the range is not reserved.
FRT_CORE_API bool DecommitVirtual (void* Address, uint64 Bytes) noexcept;

// Ends the reservation that ReserveVirtual returned Base for, with all its committed pages.
// ReservedBytes is the size it was reserved with (POSIX munmap needs it; Windows ignores it).
FRT_CORE_API bool ReleaseVirtual (void* Base, uint64 ReservedBytes) noexcept;
}
