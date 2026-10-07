// Directional albedo E(mu, alpha) of single-scattering GGX with F = 1 and height-correlated
// Smith G2, for the multiple-scattering energy compensation in Common/BSDF.hlsli, plus the
// Schlick split-sum bias B = integral of (1 - VoH)^5 f cos, so that the Fresnel-weighted albedo
// for any F0 is exactly F0 * (E - B) + B.
// x = mu (cos theta_o) in [0,1], y = sqrt(alpha) in [0,1], see SampleEnergyLut for the remap.

#include "Common/BSDF.hlsli"
#include "Common/Random.hlsli"

cbuffer RootConstants : register(b0)
{
	uint g_Output;
	uint g_Size;
	uint g_Pad0;
	uint g_Pad1;
};

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= g_Size))
	{
		return;
	}
	// Texel i holds the value at i / (N - 1); SampleEnergyLut remaps [0,1] onto texel centres.
	const float mu = max(float(Id.x) / float(g_Size - 1u), 1e-3f);
	const float sqrtAlpha = float(Id.y) / float(g_Size - 1u);
	const float alpha = max(sqrtAlpha * sqrtAlpha, kMinAlpha);
	const float3 V = float3(SafeSqrt(1.0f - mu * mu), 0.0f, mu);

	// Quasi-Monte Carlo over the VNDF; E = mean(G2 / G1) with this importance sampling.
	const uint sampleCount = 4096u;
	float sum = 0.0f;
	float bias = 0.0f;
	for (uint i = 0u; i < sampleCount; ++i)
	{
		const float2 u = float2((float(i) + 0.5f) / float(sampleCount), UintToUnitFloat(reversebits(i)));
		const float3 H = SampleGgxBoundedVndf(V, alpha, u);
		const float3 L = reflect(-V, H);
		if (L.z <= 0.0f)
		{
			continue;
		}
		const float pdf = PdfGgxBoundedVndfReflection(V, H, alpha);
		if (pdf <= 0.0f)
		{
			continue;
		}
		const float D = D_GGX(H.z, alpha);
		const float G = SmithG2HeightCorrelated(V.z, L.z, alpha);
		const float w = (D * G / (4.0f * V.z)) / pdf;
		const float c = 1.0f - saturate(dot(V, H));
		sum += w;
		bias += w * (c * c) * (c * c) * c;
	}
	const float E = saturate(sum / float(sampleCount));
	const float B = min(saturate(bias / float(sampleCount)), E);
	RWTexture2D<float2> output = ResourceDescriptorHeap[g_Output];
	output[Id.xy] = float2(E, B);
}
