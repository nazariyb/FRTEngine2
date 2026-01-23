#pragma once

#if defined(_WINDOWS)

#include "Input/PlatformInput.h"

#include <Windows.h>


namespace frt::input
{
FRT_CORE_API void HandleWin32Message (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
}

#endif
