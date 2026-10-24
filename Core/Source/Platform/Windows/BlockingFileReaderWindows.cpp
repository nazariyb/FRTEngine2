#include "Platform/BlockingFileReader.h"

#include <utility>

#include "Platform/Windows/WindowsCommon.h"


namespace frt::platform
{
namespace
{
// The most one ReadFile call is asked for: its count is a DWORD, and smaller calls keep a failure
// from losing much. Larger reads are split.
constexpr uint64 MaxReadPerCall = 1ull << 30u;
}


CBlockingFileReader::~CBlockingFileReader ()
{
	Close();
}

CBlockingFileReader::CBlockingFileReader (CBlockingFileReader&& Other) noexcept
	: Handle(std::exchange(Other.Handle, nullptr))
	, Size(std::exchange(Other.Size, 0u))
{}

CBlockingFileReader& CBlockingFileReader::operator= (CBlockingFileReader&& Other) noexcept
{
	if (this != &Other)
	{
		Close();
		Handle = std::exchange(Other.Handle, nullptr);
		Size = std::exchange(Other.Size, 0u);
	}
	return *this;
}

bool CBlockingFileReader::Open (const std::filesystem::path& Path, FString* OutError)
{
	Close();

	// Sharing read, write and delete, like ReadFileText: never in a writer's way (see the header).
	windows::CScopedHandle file(CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file.IsValid())
	{
		windows::SetError(OutError, "Cannot open", Path, GetLastError());
		return false;
	}

	// A directory opens only with FILE_FLAG_BACKUP_SEMANTICS, which is not asked for: no check needed.
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file.Get(), &size))
	{
		windows::SetError(OutError, "Cannot get the size of", Path, GetLastError());
		return false;
	}

	Handle = file.Release();
	Size = static_cast<uint64>(size.QuadPart);
	return true;
}

void CBlockingFileReader::Close () noexcept
{
	if (Handle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(Handle));
		Handle = nullptr;
	}
	Size = 0u;
}

std::optional<uint64> CBlockingFileReader::ReadAt (uint64 Offset, std::span<uint8> Destination, FString* OutError)
{
	if (Handle == nullptr)
	{
		if (OutError != nullptr)
		{
			*OutError = "ReadAt on a reader that is not open";
		}
		return std::nullopt;
	}

	uint64 done = 0u;
	while (done < Destination.size())
	{
		const uint64 remaining = Destination.size() - done;
		const DWORD wanted = static_cast<DWORD>(remaining < MaxReadPerCall ? remaining : MaxReadPerCall);
		const uint64 position = Offset + done;

		// A synchronous handle with an OVERLAPPED reads at its offset and waits: a positional read,
		// with no file pointer shared between calls.
		OVERLAPPED overlapped = {};
		overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
		overlapped.OffsetHigh = static_cast<DWORD>(position >> 32u);
		DWORD read = 0u;
		if (!ReadFile(static_cast<HANDLE>(Handle), Destination.data() + done, wanted, &read, &overlapped))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_HANDLE_EOF)
			{
				break;   // at or past the end: what was read so far is all there is
			}
			if (OutError != nullptr)
			{
				*OutError = FString::Format("Reading {} bytes at offset {} failed: {} ({})", wanted, position,
					GetSystemErrorMessage(error), error);
			}
			return std::nullopt;
		}
		if (read == 0u)
		{
			break;
		}
		done += read;
	}
	return done;
}

bool CBlockingFileReader::ReadExactAt (uint64 Offset, std::span<uint8> Destination, FString* OutError)
{
	const std::optional<uint64> read = ReadAt(Offset, Destination, OutError);
	if (!read.has_value())
	{
		return false;
	}
	if (*read != Destination.size())
	{
		if (OutError != nullptr)
		{
			*OutError = FString::Format("Reading {} bytes at offset {} found only {} (end of file at {})", Destination.size(), Offset,
				*read, Offset + *read);
		}
		return false;
	}
	return true;
}
}
