#include "Functional/MulticastDelegate.h"

#include <atomic>


namespace frt::detail
{
namespace
{
// Constant-initialized, so a delegate subscribed to from a static initializer still gets a fresh id.
std::atomic<uint64> NextDelegateHandleId = 1u;
}

uint64 NewDelegateHandleId () noexcept
{
	// 2^64 subscriptions never happen: the counter does not wrap back to 0 (no handle).
	return NextDelegateHandleId.fetch_add(1u, std::memory_order_relaxed);
}
}
