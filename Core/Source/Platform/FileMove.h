#pragma once

#include <filesystem>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


/**
 * A rename that never destroys what it lands on (design §4.6, §12.9), apart from Platform/FileSystem.h only so that adding
 * it does not rebuild everything that includes that header. ReplaceFileAtomic and std::filesystem::rename both overwrite
 * their target on Windows, and an orphan-pairing or Explorer-race move must not silently destroy another asset's meta.
 */
namespace frt::platform
{
// From is at To afterwards, flushed to disk when this returns. Fails if To exists, unless From and To name the same file (a
// case-only rename). False always means that nothing moved: once the file has moved the answer is true, and a flush that
// cannot be had (a holder that keeps the file past the retries, a failing disk) is a warning in the log. A read-only file
// and a directory cannot be opened to flush, so their move is as durable as the file system's own log makes it, soon after.
// Windows: MoveFileExW with MOVEFILE_WRITE_THROUGH and without MOVEFILE_REPLACE_EXISTING, then FlushFileBuffers on To, each
// retried for about a second while a scanner holds the file; implemented beside the other file functions in
// Platform/Windows/FileSystemWindows.cpp.
FRT_CORE_API bool MoveFileNoReplace (const std::filesystem::path& From, const std::filesystem::path& To, FString* OutError = nullptr);

template <NarrowText T, NarrowText U>
bool MoveFileNoReplace (const T&, const U&, FString* = nullptr) = delete;
}
