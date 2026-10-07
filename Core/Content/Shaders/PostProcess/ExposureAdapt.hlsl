// Auto exposure, pass 2 (one group): percentile-trimmed mean of the histogram, temporal
// adaptation, exposure buffer update, histogram reset for the next frame.
//
// Trimmed mean: with the inclusive prefix sum C_i of the bin weights and total T, bin i owns
// the weight interval [C_i - c_i, C_i). Only the part inside [Low * T, High * T] counts, so
// the darkest (1 - Low) ... brightest (1 - High) fractions are ignored exactly, independent of
// how they are distributed over bins (UE4 "Histogram" eye adaptation, Low/High percent).
//
// Adaptation is frame-rate independent: the distance d to the target obeys
//   d'(t) = -speed                    for d > D (constant EV/s, like an iris)
//   d'(t) = -speed * d / D            for d <= D (exponential settle, C1 at d = D)
// and is integrated in closed form over g_Frame.DeltaTime, so 30 and 144 fps converge alike.

#include "Common/Bindings.hlsli"
#include "Shared/PostProcessData.h"

ConstantBuffer<SExposurePassConstants> g_Pass : register(b2);

groupshared uint gs_Prefix[ExposureHistogramBins];
groupshared float2 gs_Sum[ExposureHistogramBins];

bool PassFlag (uint Flag) { return (g_Pass.Flags & Flag) != 0u; }

float AdaptEV (float Current, float Target, float DeltaTime)
{
	const float delta = Target - Current;
	const float direction = delta >= 0.0f ? 1.0f : -1.0f;
	// Higher EV = brighter scene: adapting to light (pupil constriction) is the fast direction.
	const float speed = delta >= 0.0f ? g_Pass.SpeedUp : g_Pass.SpeedDown;
	const float transition = max(g_Pass.TransitionEV, 1e-3f);

	float distance = abs(delta);
	float time = DeltaTime;
	if (distance > transition)
	{
		const float linearTime = (distance - transition) / speed;
		if (time <= linearTime)
		{
			return Current + direction * speed * time;
		}
		distance = transition;
		time -= linearTime;
	}
	distance *= exp(-time * speed / transition);
	return Target - direction * distance;
}

[numthreads(256, 1, 1)]
void main (uint GroupIndex : SV_GroupIndex)
{
	RWByteAddressBuffer histogram = ResourceDescriptorHeap[g_Pass.HistogramBuffer];
	const uint binAddress = GroupIndex * 4u;

	if (PassFlag(ExposureFlag_ClearOnly))
	{
		histogram.Store(binAddress, 0u);
		return;
	}

	RWByteAddressBuffer exposureBuffer = ResourceDescriptorHeap[g_Pass.ExposureBuffer];
	const bool bAuto = PassFlag(ExposureFlag_Auto);

	const uint count = bAuto ? histogram.Load(binAddress) : 0u;
	// Reset for next frame's histogram pass; keep a snapshot for the UI / readback.
	histogram.Store(binAddress, 0u);
	exposureBuffer.Store(ExposureHistogramSnapshotOffset + binAddress, count);

	// Inclusive prefix sum (Hillis-Steele, 8 steps; trivially cheap for 256 entries).
	gs_Prefix[GroupIndex] = count;
	GroupMemoryBarrierWithGroupSync();
	[unroll]
	for (uint offset = 1u; offset < ExposureHistogramBins; offset <<= 1u)
	{
		const uint add = GroupIndex >= offset ? gs_Prefix[GroupIndex - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		gs_Prefix[GroupIndex] += add;
		GroupMemoryBarrierWithGroupSync();
	}

	const float total = float(gs_Prefix[ExposureHistogramBins - 1u]);
	const float upper = float(gs_Prefix[GroupIndex]);
	const float lower = upper - float(count);
	const float windowLow = g_Pass.LowPercentile * total;
	const float windowHigh = g_Pass.HighPercentile * total;
	const float clipped = max(0.0f, min(upper, windowHigh) - max(lower, windowLow));
	const float binLog2 = g_Pass.MinLog2Luminance
		+ (float(GroupIndex) + 0.5f) * (g_Pass.Log2LuminanceRange / float(ExposureHistogramBins));

	// Tree reduction of (weight, weight * log2 L).
	gs_Sum[GroupIndex] = float2(clipped, clipped * binLog2);
	GroupMemoryBarrierWithGroupSync();
	[unroll]
	for (uint stride = ExposureHistogramBins / 2u; stride > 0u; stride >>= 1u)
	{
		if (GroupIndex < stride)
		{
			gs_Sum[GroupIndex] += gs_Sum[GroupIndex + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (GroupIndex != 0u)
	{
		return;
	}

	const SExposureState previous = exposureBuffer.Load<SExposureState>(0u);
	// Explicit isfinite(): DXC's 'fast' compares alone do not reliably reject NaN.
	const bool bPreviousValid = previous.Valid == ExposureStateValidMagic
		&& isfinite(previous.AdaptedEV) && isfinite(previous.AverageLog2Luminance)
		&& isfinite(previous.Exposure) && previous.Exposure > 0.0f;
	const bool bReset = PassFlag(ExposureFlag_Reset) || !bPreviousValid;

	// g_Frame.DeltaTime is authoritative; the CPU-measured fallback covers hosts that leave it 0.
	float deltaTime = g_Frame.DeltaTime > 0.0f ? g_Frame.DeltaTime : g_Pass.FallbackDeltaTime;
	deltaTime = (deltaTime > 0.0f && !isinf(deltaTime)) ? min(deltaTime, 1.0f) : 0.0f;

	const float2 sum = gs_Sum[0];
	float averageLog2 = bPreviousValid ? previous.AverageLog2Luminance : 0.0f;
	float target;
	float adapted;
	if (!bAuto)
	{
		target = g_Pass.ManualEV;
		adapted = g_Pass.ManualEV;
	}
	else
	{
		if (sum.x > 0.0f)
		{
			averageLog2 = sum.y / sum.x;
			target = clamp(averageLog2 + ExposureLog2CalibrationOffset, g_Pass.MinEV, g_Pass.MaxEV);
		}
		else
		{
			// Nothing metered (empty / all-NaN frame): hold the current exposure.
			target = bPreviousValid ? previous.AdaptedEV : clamp(0.0f, g_Pass.MinEV, g_Pass.MaxEV);
		}
		adapted = bReset ? target : AdaptEV(clamp(previous.AdaptedEV, g_Pass.MinEV, g_Pass.MaxEV), target, deltaTime);
	}

	const float exposure = exp2(g_Pass.CompensationEV - adapted);

	SExposureState state;
	state.Exposure = exposure;
	state.AdaptedEV = adapted;
	state.TargetEV = target;
	state.AverageLog2Luminance = averageLog2;
	state.PreviousExposure = bPreviousValid ? previous.Exposure : exposure;
	state.MeteredWeight = total;
	state.Valid = ExposureStateValidMagic;
	state.FrameIndex = g_Frame.FrameIndex;
	exposureBuffer.Store<SExposureState>(0u, state);
}
