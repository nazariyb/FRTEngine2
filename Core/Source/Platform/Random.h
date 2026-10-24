#pragma once

#include <span>

#include "Core.h"
#include "CoreTypes.h"


/**
 * Random bytes from the operating system's cryptographically secure generator.
 *
 * For identities that must be unique across machines and time (SGuid::NewRandom, Assets/AssetId.h):
 * the OS generator is seeded from hardware entropy and shared by the whole system, so two processes
 * started in the same millisecond, or two machines restored from one image, still draw different
 * bytes - which no seeded PRNG of our own can promise. Not for simulation or gameplay randomness:
 * those need seeds and reproducibility, and this is neither.
 *
 * Windows: BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG), the system-preferred
 * generator without opening an algorithm provider (Platform/Windows/RandomWindows.cpp). Not
 * CoCreateGuid: the GUID struct's little-endian Data1-3 make its memory differ from its text. Other
 * platforms would use getrandom (Linux) or arc4random_buf (Apple).
 */
namespace frt::platform
{
// Fills Out, of any size, with random bytes. Thread-safe and usable before the memory pool exists.
// The OS generator cannot fail for valid arguments; if it ever does, the process ends with a message
// rather than handing out predictable bytes.
FRT_CORE_API void FillRandomBytes (std::span<uint8> Out) noexcept;
}
