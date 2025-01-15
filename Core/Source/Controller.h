#pragma once

#include "Graphics/Camera.h"
#include "Memory/Memory.h"


namespace frt
{
	class FRT_CORE_API CController
	{
	public:
		CController();

	protected:
		memory::TMemoryHandle<graphics::CCamera, memory::DefaultAllocator> Camera;
	};
}
