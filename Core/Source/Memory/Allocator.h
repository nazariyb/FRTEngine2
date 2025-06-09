#pragma once
#include "Core.h"
#include "CoreTypes.h"


namespace frt::memory
{
class FRT_CORE_API IAllocator
{
public:
	virtual ~IAllocator () = default;

	virtual void* Allocate (uint64 Size) = 0;
	virtual void Free (void* Memory) = 0;

	virtual void DeleteManaged (void* Memory) = 0;
};
}
