#pragma once

// How the registry's §5.11 benchmarks (the Registry, Query and Snapshot suites) judge a measurement.
//
// Every row prints what it measured beside §5.11's figure ("met" or "MISSED") and, where M3 measured that the real
// schema cannot meet it, the figure proposed for §5.11's written revision. What a run judges depends on the build:
//	- Debug prints only.
//	- Release and Development (FRT.bat verify runs Core-Test in Development): a regression guard per row and build,
//	  set from what M3 measured inside a whole Core-Test run of that build, with headroom for a busy machine. Inside
//	  the whole suite a row takes up to about twice what it takes run alone, in either build (the 10k load 26 ms
//	  against 12-14 ms alone in Release), so some guards sit above §5.11's figures; each lies below the row's pre-P3
//	  figure measured the same way wherever P3 improved it, so losing an optimization fails the run where machine
//	  noise does not. A guard is a regression check, not §5.11's figure: the printed status says whether that is
//	  met. The DISABLED_ suites (100k assets and more) run alone, and their guards are set from runs alone.
//	- FRT_PERF_STRICT=1 holds §5.11's figure (or the proposed revision) exactly instead: for a Release run alone.
// Figures are for this project's machine (i7-13700K, processor affinity 0xFFFFFC), as §5.11's are.

#include <gtest/gtest.h>

#include <cstdio>

#include "BuildConfig.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


namespace frt::assets::bench
{
struct SBudget
{
	double Spec = -1.0;          // §5.11's figure at this size (< 0: none)
	double Revision = -1.0;      // the figure proposed for §5.11's written revision (< 0: none, the spec holds)
	double Release = -1.0;       // the regression guard in Release (< 0: not guarded)
	double Development = -1.0;   // ... in Development

	[[nodiscard]] double GetJudged () const noexcept { return Revision >= 0.0 ? Revision : Spec; }
	[[nodiscard]] double GetGuard () const noexcept { return FRT_BUILD_RELEASE ? Release : Development; }
};

[[nodiscard]] inline bool IsPerfStrict ()
{
	return platform::GetEnvironmentValue("FRT_PERF_STRICT").value_or(FString()) == "1";
}

/**
 * Prints one measurement as "<Prefix> <Name> <value> <unit> (5.11 ...; proposed ...; guard ...) <Note>", records it
 * as a test property and judges it (see the header comment). bJudge false: printed only (a size §5.11 reports
 * without a budget).
 */
inline void CheckBudget (const char* Prefix, const char* Name, double Measured, const SBudget& Budget, const char* Unit, const char* Note = "",
	bool bJudge = true)
{
	FString line = FString::Format("{} {:<34} {:>10.4f} {}", Prefix, Name, Measured, Unit);
	FString limits;
	if (Budget.Spec >= 0.0)
	{
		limits.AppendFormat("5.11 {:.3f}: {}", Budget.Spec, Measured <= Budget.Spec ? "met" : "MISSED");
	}
	if (Budget.Revision >= 0.0)
	{
		limits.AppendFormat("{}proposed revision {:.3f}: {}", limits.IsEmpty() ? "" : "; ", Budget.Revision, Measured <= Budget.Revision ? "met" : "MISSED");
	}
	constexpr bool bOptimized = !FRT_BUILD_DEBUG;
	const double guard = Budget.GetGuard();
	if constexpr (bOptimized)
	{
		if (guard >= 0.0)
		{
			limits.AppendFormat("{}guard {:.3f}", limits.IsEmpty() ? "" : "; ", guard);
		}
	}
	line.AppendFormat(" ({})", limits.IsEmpty() ? FStringView("not budgeted") : limits.View());
	if (Note != nullptr && Note[0] != '\0')
	{
		line.AppendFormat(" {}", Note);
	}
	std::printf("%s\n", line.CStr());
	::testing::Test::RecordProperty(Name, FString::Format("{:.4f}", Measured).CStr());
	if constexpr (bOptimized)
	{
		if (!bJudge)
		{
			return;
		}
		if (IsPerfStrict())
		{
			if (Budget.GetJudged() >= 0.0)
			{
				EXPECT_LE(Measured, Budget.GetJudged()) << Name << " (FRT_PERF_STRICT: 5.11 or the proposed revision)";
			}
		}
		else if (guard >= 0.0)
		{
			EXPECT_LE(Measured, guard) << Name << " (regression guard)";
		}
	}
	else
	{
		(void)bJudge;
	}
}
}
