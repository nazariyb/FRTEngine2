#pragma once

// CRegistryJournal's test seam (a friend, Assets/Registry/Journal.h), shared by the journal's tests and the registry
// service's.

#include <cstddef>

#include "Assets/Registry/Journal.h"


namespace frt::assets
{
// The next Append writes only Bytes of its frames and fails, as a full disk cuts a write short.
struct SRegistryJournalTestAccess
{
	static void FailNextAppendAfter (CRegistryJournal& Journal, size_t Bytes) { Journal.FailAppendAfterBytes = Bytes; }
};
}
