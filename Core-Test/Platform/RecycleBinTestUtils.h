#pragma once

#include <filesystem>

#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::test
{
// Deletes for good what a test moved to the Recycle Bin, so that running the suite leaves nothing
// in the user's Bin: every item whose name starts with NamePrefix (unique to the test run) and that
// came from Directory (compared by identity, so short and long spellings of the path agree). Nothing
// else is touched. Returns how many items it found and deleted - finding them there is also the half
// of "trash, then restore" a test can check.
uint32 PurgeFromRecycleBin (FStringView NamePrefix, const std::filesystem::path& Directory);
}
