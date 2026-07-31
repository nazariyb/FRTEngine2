#pragma once

#include <limits>

#include "Core.h"
#include "CoreTypes.h"


// Precision used for positions, distances, and other world-space measurements.
// Selected by Premake via applyPrecision() (see Premake/common.lua):
//
//   premake5 vs2022 --precision=double
//
// Valid values are 32 (float) and 64 (double). The fallback below keeps the header
// usable in tooling and IDE parsers that do not see the generated project defines.
//
// This is a CPU-side choice only. DirectXMath and every GPU-facing struct stay 32-bit;
// double precision buys simulation and measurement range, not upload width.
#ifndef FRT_REAL_PRECISION
	#define FRT_REAL_PRECISION 32
#endif

#if FRT_REAL_PRECISION != 32 && FRT_REAL_PRECISION != 64
	#error "FRT_REAL_PRECISION must be 32 (float) or 64 (double)"
#endif


namespace frt
{
#if FRT_REAL_PRECISION == 64
using Real = double;
#else
using Real = float;
#endif

inline constexpr bool bDoublePrecision = (FRT_REAL_PRECISION == 64);

inline constexpr Real RealEpsilon = (std::numeric_limits<Real>::epsilon)();
inline constexpr Real RealMax     = (std::numeric_limits<Real>::max)();
inline constexpr Real RealLowest  = (std::numeric_limits<Real>::lowest)();

static_assert(sizeof(Real) * 8u == FRT_REAL_PRECISION, "Real does not match FRT_REAL_PRECISION");


// Literal suffix: 1.5_r is `Real` at whichever precision is configured. Avoids
// double->float narrowing warnings in 32-bit builds and silent float-precision
// arithmetic in 64-bit ones.
constexpr Real operator""_r (long double InValue)
{
	return static_cast<Real>(InValue);
}

constexpr Real operator""_r (unsigned long long InValue)
{
	return static_cast<Real>(InValue);
}


// ---------------------------------------------------------------------------------
// ABI guard
//
// frt::real appears inside types exported across the Core DLL boundary. If Core and a
// module linking it were built with different FRT_REAL_PRECISION values, those types
// would be reinterpreted at a different size and field offset — silently, at runtime,
// producing corrupted transforms rather than any diagnostic.
//
// Premake applies the same value to every project, so a mismatch means stale generated
// project files or a hand-edited build. Cheap to check, extremely annoying to debug
// otherwise. FRT_VERIFY_REAL_PRECISION_ABI() expands the *caller's* macro value, so it
// compares the host module against Core rather than Core against itself.
// ---------------------------------------------------------------------------------

/** @return the FRT_REAL_PRECISION value Core itself was compiled with. */
FRT_CORE_API uint32 GetCoreRealPrecision ();

/** Reports and aborts if InHostPrecision disagrees with Core. Fatal in all configurations. */
FRT_CORE_API void VerifyRealPrecisionAbi (uint32 InHostPrecision);
}

#define FRT_VERIFY_REAL_PRECISION_ABI() ::frt::VerifyRealPrecisionAbi(FRT_REAL_PRECISION)
