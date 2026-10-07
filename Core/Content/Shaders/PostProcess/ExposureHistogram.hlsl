// Auto exposure, pass 1: centre-weighted log2-luminance histogram of the HDR image.
//
// 256 bins over [MinLog2Luminance, MinLog2Luminance + Log2LuminanceRange]; values outside the
// range are clamped into the end bins so percentiles stay correctly ordered. NaN / negative /
// Inf pixels are skipped (they carry no usable luminance).
//
// The histogram is built from 4x4-pixel mean luminance, not single pixels. A light meter
// integrates over its acceptance angle; the log of a per-pixel Monte Carlo estimate does not:
// E[log L] < log E[L] (Jensen), so the noisier the image, the darker it meters. Per pixel, a
// 1500-frame Reference render and the denoised real-time image of the same view metered
// 0.54 EV apart; with 4x4 block means 0.02 EV (offline model of this pass, 30-90% window).
// The block is far below any scene feature that should steer the exposure.
//
// Each block adds a fixed-point weight (ExposureWeightScale x valid pixel fraction) from a
// Gaussian centre spot blended with uniform average metering. One group = 16x16 threads
// covering a 64x64 pixel tile, one 4x4 block per thread, and a
// per-group shared histogram keeps the global atomic traffic at ~(non-empty bins) per group.
// The tile is loaded with coalesced strided loads into groupshared memory first.

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "PostProcess/PostCommon.hlsli"
#include "Shared/PostProcessData.h"

ConstantBuffer<SExposurePassConstants> g_Pass : register(b2);

groupshared uint gs_Bins[ExposureHistogramBins];
// Luminance of the group's 64x64 tile; negative = no usable value (outside / NaN / Inf).
groupshared float gs_Luminance[64 * 64];

static const uint kTile = 64u;
static const uint kStride = 16u;
static const uint kBlock = 4u;

[numthreads(16, 16, 1)]
void main (uint3 GroupId : SV_GroupID, uint3 GroupThreadId : SV_GroupThreadID, uint GroupIndex : SV_GroupIndex)
{
	gs_Bins[GroupIndex] = 0u;

	Texture2D<float4> input = Tex2D(g_Pass.InputTexture);
	const float invPreExposure = GetInvPreExposure();
	const uint2 tileOrigin = GroupId.xy * kTile;
	const uint2 maxPixel = g_Pass.InputSize - 1u;

	// Load the tile with every load instruction of the group covering a dense 16x16 block
	// (pixels strided by 16 per thread): a contiguous 4x4 block per thread was 5x slower.
	[unroll]
	for (uint i = 0u; i < 16u; ++i)
	{
		const uint2 local = GroupThreadId.xy + uint2(i & 3u, i >> 2u) * kStride;
		const uint2 pixel = tileOrigin + local;
		const float luminance = Luminance(input.Load(int3(min(pixel, maxPixel), 0)).rgb) * invPreExposure;
		// Skip NaN, +-Inf and negative energy; zero luminance is legitimate. isfinite() must be
		// explicit: DXC emits 'fast' float compares, so '!(L >= 0)' compiles to 'fcmp fast ult'
		// and a driver honouring the no-NaN flag may let NaN through.
		const bool bUsable = all(pixel < g_Pass.InputSize) && isfinite(luminance) && luminance >= 0.0f;
		gs_Luminance[local.y * kTile + local.x] = bUsable ? luminance : -1.0f;
	}
	GroupMemoryBarrierWithGroupSync();

	// This thread's 4x4 block.
	const uint2 blockOrigin = GroupThreadId.xy * kBlock;
	const uint2 origin = tileOrigin + blockOrigin;
	float sum = 0.0f;
	float valid = 0.0f;
	[unroll]
	for (uint j = 0u; j < kBlock * kBlock; ++j)
	{
		const uint2 local = blockOrigin + uint2(j % kBlock, j / kBlock);
		const float luminance = gs_Luminance[local.y * kTile + local.x];
		sum += max(luminance, 0.0f);
		valid += luminance >= 0.0f ? 1.0f : 0.0f;
	}

	uint bin = 0u;
	uint weight = 0u;
	if (valid > 0.0f)
	{
		const float meanLuminance = sum / valid;
		const float log2Luminance = log2(max(meanLuminance, 1e-20f));
		const float t = saturate((log2Luminance - g_Pass.MinLog2Luminance) * g_Pass.InvLog2LuminanceRange);
		bin = min(uint(t * float(ExposureHistogramBins)), ExposureHistogramBins - 1u);

		// Centre-weighted metering: Gaussian in half-height units (aspect-correct, so the spot
		// is round), blended with uniform weighting. Every block keeps at least 1 so a frame that
		// is only bright at the borders still meters.
		const float2 blockCentre = float2(origin) + 0.5f * float(kBlock);
		const float2 d = (blockCentre - 0.5f * float2(g_Pass.InputSize)) * g_Pass.HalfHeightInv;
		const float spot = exp(-dot(d, d) * g_Pass.SpotInvTwoSigmaSq);
		const float w = lerp(1.0f, spot, g_Pass.CenterWeight) * (valid / float(kBlock * kBlock));
		weight = max(1u, uint(w * float(ExposureWeightScale) + 0.5f));
	}
	// Neighbouring blocks of smooth regions share a bin: one atomic per wave instead of one per
	// lane (per-lane atomics on the same LDS address serialise; this pass took 0.05 ms).
	if (WaveActiveAllEqual(bin))
	{
		const uint total = WaveActiveSum(weight);
		if (WaveIsFirstLane() && total != 0u)
		{
			InterlockedAdd(gs_Bins[bin], total);
		}
	}
	else if (weight != 0u)
	{
		InterlockedAdd(gs_Bins[bin], weight);
	}

	GroupMemoryBarrierWithGroupSync();

	const uint count = gs_Bins[GroupIndex];
	if (count != 0u)
	{
		RWByteAddressBuffer histogram = ResourceDescriptorHeap[g_Pass.HistogramBuffer];
		histogram.InterlockedAdd(GroupIndex * 4u, count);
	}
}
