#include "Platform/Random.h"

#include <cstdio>
#include <cstdlib>

#include <Windows.h>
#include <bcrypt.h>

#include "Containers/String.h"
#include "Log/Log.h"
#include "Platform/Platform.h"

#pragma comment(lib, "bcrypt.lib")


namespace frt::platform
{
namespace
{
// BCryptGenRandom takes a ULONG count: larger requests go in pieces of this size.
constexpr size_t MaxRequestBytes = 0x40000000u;

[[noreturn]] void FatalRandomFailure (NTSTATUS Status) noexcept
{
	// Through the log like every engine message (stderr and the debugger, or a test's hook); flushed,
	// since abort does not.
	log::Write(log::Platform, log::ELevel::Error,
		FString::Format("FillRandomBytes: BCryptGenRandom failed (NTSTATUS {:#010x})", static_cast<uint32>(Status)));
	std::fflush(stderr);
	if (IsDebuggerPresent())
	{
		__debugbreak();
	}
	std::abort();
}
}


void FillRandomBytes (std::span<uint8> Out) noexcept
{
	while (!Out.empty())
	{
		const size_t count = Out.size() < MaxRequestBytes ? Out.size() : MaxRequestBytes;
		const NTSTATUS status = BCryptGenRandom(nullptr, Out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (!BCRYPT_SUCCESS(status))
		{
			FatalRandomFailure(status);
		}
		Out = Out.subspan(count);
	}
}
}
