#include "Precision.h"

#include <cstdio>
#include <cstdlib>

#include <Windows.h>


uint32 frt::GetCoreRealPrecision ()
{
	return FRT_REAL_PRECISION;
}

void frt::VerifyRealPrecisionAbi (uint32 InHostPrecision)
{
	const uint32 corePrecision = GetCoreRealPrecision();
	if (InHostPrecision == corePrecision)
	{
		return;
	}

	char message[256];
	std::snprintf(message, sizeof(message),
		"FATAL: FRT_REAL_PRECISION mismatch. Core was built with %u-bit real, "
		"the host module with %u-bit. Every project linking Core must use the same "
		"precision - regenerate the project files.\n",
		corePrecision, InHostPrecision);

	// Not frt_assert: this is fatal in Release too, and a windowed app has no console.
	std::fputs(message, stderr);
	OutputDebugStringA(message);

	std::abort();
}
