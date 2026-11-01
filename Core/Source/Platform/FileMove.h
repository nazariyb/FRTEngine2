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
// From is at To afterwards, durable when this returns. Fails if To exists, unless From and To name the same file (a
// case-only rename). Windows: MoveFileExW with MOVEFILE_WRITE_THROUGH and without MOVEFILE_REPLACE_EXISTING; implemented
// beside the other file functions in Platform/Windows/FileSystemWindows.cpp.
FRT_CORE_API bool MoveFileNoReplace (const std::filesystem::path& From, const std::filesystem::path& To, FString* OutError = nullptr);

template <NarrowText T, NarrowText U>
bool MoveFileNoReplace (const T&, const U&, FString* = nullptr) = delete;
}
