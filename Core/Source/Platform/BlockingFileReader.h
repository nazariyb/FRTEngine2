#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


namespace frt::platform
{
/**
 * Synchronous, positional reads of one file: open, size, ReadAt(offset, bytes), close.
 *
 * The asset system reads in ranges (a chunk of a cooked file, one record of a DDC value, a file
 * hashed in pieces), and its files can pass 4 GB, where ReadWholeFile and anything sized by a
 * uint32 stop. So every offset and count here is 64-bit, a read names its own offset (no shared
 * file pointer to seek), and a read larger than one ReadFile call can carry is split.
 *
 * Buffered I/O through the OS cache (no FILE_FLAG_NO_BUFFERING, so offsets and sizes need no
 * sector alignment). The file is opened sharing read, write and delete, like ReadFileText: a
 * reader never makes an editor's save fail, and an atomic replace of the file (WriteFileAtomic)
 * goes through while it is open - the reader keeps the old bytes. Whether the file changed while
 * it was read is the caller's question (ReadFileChunksStable in FileSystem.h answers it).
 *
 * This is the synchronous reader the tools, the cooker and the tests use. In M6 it also becomes
 * the synchronous IIoBackend (design §8.15); until then it is a plain class. Uses no engine
 * allocator: safe on worker threads and before the memory pool exists. A reader is not meant to
 * be shared between threads; open one per thread.
 */
class FRT_CORE_API CBlockingFileReader
{
public:
	CBlockingFileReader () noexcept = default;
	~CBlockingFileReader ();

	CBlockingFileReader (const CBlockingFileReader&) = delete;
	CBlockingFileReader& operator= (const CBlockingFileReader&) = delete;
	CBlockingFileReader (CBlockingFileReader&& Other) noexcept;
	CBlockingFileReader& operator= (CBlockingFileReader&& Other) noexcept;

	// Opens Path for reading (closing whatever was open) and takes its size. False if it cannot be
	// opened (missing, a directory, no access), with the reason in OutError.
	bool Open (const std::filesystem::path& Path, FString* OutError = nullptr);
	template <NarrowText T>
	bool Open (const T&, FString* = nullptr) = delete;

	void Close () noexcept;
	[[nodiscard]] bool IsOpen () const noexcept { return Handle != nullptr; }

	// The file's size when it was opened. A file that grows or shrinks afterwards keeps this value;
	// ReadAt reports what is really there.
	[[nodiscard]] uint64 GetSize () const noexcept { return Size; }

	// Reads up to Destination.size() bytes from Offset. Returns the number of bytes read: fewer at
	// the end of the file, 0 at or past it. nullopt if the read failed or the reader is not open.
	[[nodiscard]] std::optional<uint64> ReadAt (uint64 Offset, std::span<uint8> Destination, FString* OutError = nullptr);
	// Exactly Destination.size() bytes from Offset; false on a failed or short read.
	[[nodiscard]] bool ReadExactAt (uint64 Offset, std::span<uint8> Destination, FString* OutError = nullptr);

private:
	void* Handle = nullptr;   // the OS file handle; nullptr when closed
	uint64 Size = 0u;
};
}
