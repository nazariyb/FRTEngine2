// Contrast-adaptive sharpening of the resolved TAA/TAAU image, after AMD FidelityFX RCAS
// (FSR 1, Lottes 2021): a 5-tap cross with the largest negative lobe that cannot push any
// channel outside [0, 1] given the ring's min/max. RCAS is defined on display-referred [0, 1)
// values. The HDR input is mapped there with the same exposure-scaled max-Reinhard used by the
// resolve and mapped back afterwards, and the output is additionally limited to the local
// 5-tap range so the inverse tonemap cannot amplify an overshoot (see main). The history is
// never sharpened: feeding sharpened colour back would compound every frame.

#include "Common/Bindings.hlsli"
#include "Shared/UpscalerData.h"
#include "Upscaler/UpscalerCommon.hlsli"

ConstantBuffer<SUpscalerSharpenConstants> g_Sharpen : register(b0);

// Lobe limit: at -0.25 the filter would be singular (1 + 4 lobe = 0); RCAS keeps a margin.
static const float kRcasLimit = 0.25f - 1.0f / 16.0f;

float3 LoadTonemapped (Texture2D<float4> Source, int2 Texel, int2 MaxTexel)
{
	return TonemapMax(SanitizeHdr(Source[clamp(Texel, 0, MaxTexel)].rgb) * g_Sharpen.Exposure);
}

// Luma x2 used by the RCAS noise detector.
float RcasLuma (float3 C) { return C.g + 0.5f * (C.r + C.b); }

[numthreads(8, 8, 1)]
void main (uint3 DispatchThreadId : SV_DispatchThreadID)
{
	if (any(DispatchThreadId.xy >= g_Sharpen.Size))
	{
		return;
	}
	const int2 p = int2(DispatchThreadId.xy);
	const int2 maxTexel = int2(g_Sharpen.Size) - 1;
	Texture2D<float4> source = Tex2D(g_Sharpen.Input);

	//    b
	//  d e f
	//    h
	const float3 b = LoadTonemapped(source, p + int2(0, -1), maxTexel);
	const float3 d = LoadTonemapped(source, p + int2(-1, 0), maxTexel);
	const float3 e = LoadTonemapped(source, p, maxTexel);
	const float3 f = LoadTonemapped(source, p + int2(1, 0), maxTexel);
	const float3 h = LoadTonemapped(source, p + int2(0, 1), maxTexel);

	const float3 mn4 = min(min(b, d), min(f, h));
	const float3 mx4 = max(max(b, d), max(f, h));
	const float3 localMin = min(mn4, e);
	const float3 localMax = max(mx4, e);
	// Per channel, the lobe w for which (w * ring + e) / (4w + 1) just reaches 0 (hitMin) or
	// 1 (hitMax); the most restrictive channel wins, so no channel clips. The denominator
	// 4 mn4 - 4 is 0 when the ring is at the peak. It is kept negative so the result is never
	// 0 * inf = NaN.
	const float3 hitMin = localMin * rcp(max(4.0f * mx4, 1e-6f));
	const float3 hitMax = (1.0f - localMax) * rcp(min(4.0f * mn4 - 4.0f, -1e-6f));
	const float3 lobeRgb = max(-hitMin, hitMax);
	float lobe = max(-kRcasLimit, min(MaxComponent(lobeRgb), 0.0f)) * g_Sharpen.Sharpness;

	if ((g_Sharpen.Flags & UpscalerSharpenFlag_Denoise) != 0u)
	{
		// Attenuate on isolated luma outliers (residual path-tracing noise): the centre's
		// deviation from the ring average, relative to the local luma range.
		const float bL = RcasLuma(b);
		const float dL = RcasLuma(d);
		const float eL = RcasLuma(e);
		const float fL = RcasLuma(f);
		const float hL = RcasLuma(h);
		const float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
		const float nz = saturate(abs(0.25f * (bL + dL + fL + hL) - eL) * rcp(max(range, 1e-6f)));
		lobe *= 1.0f - 0.5f * nz;
	}

	// Deviation from RCAS. RCAS only guarantees the output stays inside the [0, 1] peak range,
	// and on display-referred LDR that is harmless. Here it is fatal: max-Reinhard values near
	// 1 decode to huge HDR values. Example, noise attenuation off: a 9.0 pixel on a 1.0 ring
	// (t = 0.9 / 0.5) came out at t = 1.0 for Sharpness 1, which is the 65000 clamp, and at
	// 11.7 (+30%) for the default 0.25. So the output is also limited to the 5-tap range.
	// Edges still steepen (pixels on a slope are pushed toward the nearer extreme), but no
	// peak or highlight gains energy.
	const float3 sharpened = clamp((lobe * (b + d + f + h) + e) * rcp(4.0f * lobe + 1.0f), localMin, localMax);
	const float3 result = InverseTonemapMax(sharpened) * g_Sharpen.InvExposure;
	RwTex2D(g_Sharpen.Output)[p] = float4(SanitizeHdr(result), 1.0f);
}
