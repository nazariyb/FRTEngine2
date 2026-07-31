#pragma once

#include <cassert>
#include <cstdio>


/**
 * Asserts compile out unless FRT_ASSERTIONS_ENABLED is 1; Debug builds default to on.
 *
 * The previous spelling was `#define ASSERTIONS_ENABLED _DEBUG` guarded by
 * `#if defined(ASSERTIONS_ENABLED)`, which is true regardless of what the macro expands
 * to - so asserts were compiled into Release as well, and a failure there would
 * __debugbreak() a shipping build. Define FRT_ASSERTIONS_ENABLED to 1 yourself if you
 * want them in Release deliberately.
 */
#ifndef FRT_ASSERTIONS_ENABLED
	#if defined(_DEBUG)
		#define FRT_ASSERTIONS_ENABLED 1
	#else
		#define FRT_ASSERTIONS_ENABLED 0
	#endif
#endif

// Kept for source compatibility with the old spelling.
#define ASSERTIONS_ENABLED FRT_ASSERTIONS_ENABLED


#if FRT_ASSERTIONS_ENABLED

// Declared by hand rather than including Windows.h: Asserts.h is pulled in by Core.h and
// so by very nearly every translation unit.
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* lpOutputString);

namespace frt::asserts
{
/**
 * Reports a failed assertion before the break.
 *
 * A bare __debugbreak() gives no expression, no file and no line, and in a windowed build
 * with no console attached it surfaces as nothing but STATUS_BREAKPOINT (0x80000003) -
 * enough to know something failed, not enough to know what.
 *
 * Written to both stderr and the debugger: a console or redirected build sees the former,
 * a windowed one only ever sees the latter.
 */
inline void ReportFailure (const char* InExpression, const char* InFile, int InLine)
{
	char message[1024];
	std::snprintf(message, sizeof(message),
		"ASSERT FAILED: %s\n  at %s:%d\n", InExpression, InFile, InLine);

	std::fputs(message, stderr);
	std::fflush(stderr);

	OutputDebugStringA(message);
}
}

// Deliberately an if/else rather than do/while(false): several call sites omit the
// trailing semicolon (Renderer.cpp among them), and this form tolerates that. The usual
// caveat applies - an unbraced `if (x) frt_assert(y)` with no semicolon would capture a
// following else.
#define frt_assert(expr)\
	if (expr) {}\
	else\
	{\
		::frt::asserts::ReportFailure(#expr, __FILE__, __LINE__);\
		__debugbreak();\
	}

#else

// Must expand to nothing, not to ((void)0): call sites without a trailing semicolon would
// otherwise fail to compile in builds that have assertions disabled.
#define frt_assert(expr)

#endif
