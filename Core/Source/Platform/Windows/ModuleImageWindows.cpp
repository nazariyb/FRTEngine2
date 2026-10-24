#include "Platform/ModuleImage.h"

#include <Windows.h>


namespace frt::platform
{
SModuleImage GetModuleImage (void* Module) noexcept
{
	// A module handle is its image's base address, where the DOS header points to the PE headers;
	// SizeOfImage covers every section as mapped.
	void* const module = Module != nullptr ? Module : static_cast<void*>(GetModuleHandleW(nullptr));
	const auto* base = static_cast<const uint8*>(module);
	if (base == nullptr)
	{
		return {};
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return {};
	}
	const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (headers->Signature != IMAGE_NT_SIGNATURE)
	{
		return {};
	}
	return { base, base + headers->OptionalHeader.SizeOfImage };
}
}
