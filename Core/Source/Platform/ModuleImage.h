#pragma once

#include <cstdint>

#include "Core.h"
#include "CoreTypes.h"


/**
 * The address range a loaded module - the executable or a DLL - occupies: its code, its constant
 * data (string literals included) and its static variables. A pointer into it dangles once the
 * module unloads, so whatever keeps pointers a game module handed over (component types, delegate
 * subscriptions, CPU scope names) drops them by this range around the unload (spec §6.7, §8.15).
 *
 * An OS fact, so it lives here: Windows reads it from the module's PE headers; ELF and Mach-O
 * platforms will read theirs (dl_iterate_phdr, the load commands). A header of its own, light
 * enough for the header-only function templates (Functional/UniqueFunction.h) to take it.
 */
namespace frt::platform
{
struct SModuleImage
{
	const void* Begin = nullptr;
	const void* End = nullptr;   // one past the last byte

	// As integers: comparing pointers into different objects with < is unspecified.
	[[nodiscard]] bool Contains (const void* Address) const noexcept
	{
		const uintptr_t address = reinterpret_cast<uintptr_t>(Address);
		return address >= reinterpret_cast<uintptr_t>(Begin) && address < reinterpret_cast<uintptr_t>(End);
	}
};

// The image of a loaded module: an HMODULE on Windows (the image's base address), nullptr for the
// executable. Empty (Contains nothing) when Module is no loaded image. Call it while the module is
// loaded: its headers go with it.
[[nodiscard]] FRT_CORE_API SModuleImage GetModuleImage (void* Module = nullptr) noexcept;
}
