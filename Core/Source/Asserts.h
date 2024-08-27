#pragma once

#include <cassert>

#define ASSERTIONS_ENABLED _DEBUG

#if defined(ASSERTIONS_ENABLED)
#define frt_assert(expr) assert(expr)
#else
#define frt_assert(expr)
#endif
