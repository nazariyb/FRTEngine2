#ifndef FRT_PACKING_HLSLI
#define FRT_PACKING_HLSLI

#include "Common/Math.hlsli"

// Octahedral encoding of unit vectors (Cigolle et al. 2014), result in [-1, 1]^2.
float2 OctWrap (float2 V)
{
	return (1.0f - abs(V.yx)) * select(V.xy >= 0.0f, 1.0f, -1.0f);
}

float2 EncodeOctahedral (float3 N)
{
	N /= (abs(N.x) + abs(N.y) + abs(N.z));
	N.xy = N.z >= 0.0f ? N.xy : OctWrap(N.xy);
	return N.xy;
}

float3 DecodeOctahedral (float2 E)
{
	float3 n = float3(E.x, E.y, 1.0f - abs(E.x) - abs(E.y));
	const float t = saturate(-n.z);
	n.xy += select(n.xy >= 0.0f, -t, t);
	return normalize(n);
}

// snorm16x2 packing of an octahedral vector.
uint PackSnorm2x16 (float2 V)
{
	const int2 q = int2(round(clamp(V, -1.0f, 1.0f) * 32767.0f));
	return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16u);
}

float2 UnpackSnorm2x16 (uint P)
{
	const int2 q = int2(int(P << 16u) >> 16, int(P) >> 16);
	return max(float2(q) / 32767.0f, -1.0f);
}

uint PackUnorm2x16 (float2 V)
{
	const uint2 q = uint2(round(saturate(V) * 65535.0f));
	return q.x | (q.y << 16u);
}

float2 UnpackUnorm2x16 (uint P)
{
	return float2(P & 0xFFFFu, P >> 16u) / 65535.0f;
}

uint PackHalf2 (float2 V)
{
	return f32tof16(V.x) | (f32tof16(V.y) << 16u);
}

float2 UnpackHalf2 (uint P)
{
	return float2(f16tof32(P & 0xFFFFu), f16tof32(P >> 16u));
}

// Equal-area octahedral sphere mapping (Clarberg 2008): uniform texel solid angle, used by
// the environment map so a luminance pyramid gives an exact importance-sampling pdf.
float2 EqualAreaSphereToSquare (float3 D)
{
	const float3 a = abs(D);
	const float r = sqrt(saturate(1.0f - a.z));
	const float phi = atan2(min(a.x, a.y), max(max(a.x, a.y), 1e-20f)) * (2.0f / kPi);
	float u = r * (1.0f - phi);
	float v = r * phi;
	if (a.x < a.y)
	{
		const float t = u; u = v; v = t;
	}
	if (D.z < 0.0f)
	{
		const float t = u;
		u = 1.0f - v;
		v = 1.0f - t;
	}
	u = D.x < 0.0f ? -u : u;
	v = D.y < 0.0f ? -v : v;
	return float2(u, v) * 0.5f + 0.5f;
}

float3 EqualAreaSquareToSphere (float2 P)
{
	const float2 uv = P * 2.0f - 1.0f;
	const float u = uv.x;
	const float v = uv.y;
	const float up = abs(u);
	const float vp = abs(v);
	const float signedDistance = 1.0f - (up + vp);
	const float d = abs(signedDistance);
	const float r = 1.0f - d;
	const float phi = (r == 0.0f ? 1.0f : (vp - up) / r + 1.0f) * (kPi / 4.0f);
	const float z = signedDistance >= 0.0f ? (1.0f - r * r) : -(1.0f - r * r);
	const float cosPhi = (u < 0.0f ? -1.0f : 1.0f) * abs(cos(phi));
	const float sinPhi = (v < 0.0f ? -1.0f : 1.0f) * abs(sin(phi));
	const float s = r * sqrt(max(2.0f - r * r, 0.0f));
	return float3(cosPhi * s, sinPhi * s, z);
}

#endif
