#ifndef FRT_SAMPLING_HLSLI
#define FRT_SAMPLING_HLSLI

#include "Common/Math.hlsli"

// Concentric disk mapping (Shirley & Chiu).
float2 SampleDiskConcentric (float2 U)
{
	const float2 o = 2.0f * U - 1.0f;
	if (o.x == 0.0f && o.y == 0.0f)
	{
		return 0.0f;
	}
	float r, theta;
	if (abs(o.x) > abs(o.y))
	{
		r = o.x;
		theta = (kPi / 4.0f) * (o.y / o.x);
	}
	else
	{
		r = o.y;
		theta = kHalfPi - (kPi / 4.0f) * (o.x / o.y);
	}
	return r * float2(cos(theta), sin(theta));
}

// Cosine-weighted hemisphere around +Z. pdf = cos(theta) / pi.
float3 SampleCosineHemisphere (float2 U)
{
	const float2 d = SampleDiskConcentric(U);
	const float z = SafeSqrt(1.0f - dot(d, d));
	return float3(d, z);
}

float3 SampleUniformSphere (float2 U)
{
	const float z = 1.0f - 2.0f * U.x;
	const float r = SafeSqrt(1.0f - z * z);
	const float phi = kTwoPi * U.y;
	return float3(r * cos(phi), r * sin(phi), z);
}

// Uniform direction inside a cone around +Z. pdf = 1 / (2 pi (1 - cosMax)).
float3 SampleUniformCone (float2 U, float CosThetaMax)
{
	const float cosTheta = (1.0f - U.x) + U.x * CosThetaMax;
	const float sinTheta = SafeSqrt(1.0f - cosTheta * cosTheta);
	const float phi = kTwoPi * U.y;
	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Uniform point on a triangle (barycentrics), low-distortion mapping (Heitz 2019).
float3 SampleUniformTriangle (float2 U)
{
	float b0, b1;
	if (U.x < U.y)
	{
		b0 = U.x / 2.0f;
		b1 = U.y - b0;
	}
	else
	{
		b1 = U.y / 2.0f;
		b0 = U.x - b1;
	}
	return float3(b0, b1, 1.0f - b0 - b1);
}

/**
 * Area-preserving spherical rectangle sampling (Ureña, Fajardo & King 2013), with the
 * cancellation-free solid angle formulation (sum of asin terms instead of acos - 2pi) that keeps
 * small or distant rectangles stable in single precision.
 *
 * Rectangle: Corner + s * ExtentU + t * ExtentV, s,t in [0,1]. Returns the sampled point and
 * writes the pdf in solid angle. Falls back to area sampling for tiny solid angles.
 */
struct SSphericalRect
{
	float3 X;
	float3 Y;
	float3 Z;
	float Z0;
	float X0, X1, Y0, Y1;
	float B0, B1, K;
	float SolidAngle;
	float G2, G3;
	bool bValid;
};

SSphericalRect InitSphericalRect (float3 Origin, float3 Corner, float3 ExtentU, float3 ExtentV)
{
	SSphericalRect r = (SSphericalRect)0;
	const float lenU = length(ExtentU);
	const float lenV = length(ExtentV);
	r.X = ExtentU / max(lenU, 1e-20f);
	r.Y = ExtentV / max(lenV, 1e-20f);
	r.Z = cross(r.X, r.Y);

	const float3 d = Corner - Origin;
	r.Z0 = dot(d, r.Z);
	if (r.Z0 > 0.0f)
	{
		r.Z = -r.Z;
		r.Z0 = -r.Z0;
	}
	r.X0 = dot(d, r.X);
	r.Y0 = dot(d, r.Y);
	r.X1 = r.X0 + lenU;
	r.Y1 = r.Y0 + lenV;

	float4 nz = float4(-r.Y0, r.X1, r.Y1, -r.X0);
	nz /= sqrt(nz * nz + r.Z0 * r.Z0);
	const float g0 = SafeAsin(-nz.x * nz.y);
	const float g1 = SafeAsin(-nz.y * nz.z);
	r.G2 = SafeAsin(-nz.z * nz.w);
	r.G3 = SafeAsin(-nz.w * nz.x);
	r.SolidAngle = -(g0 + g1 + r.G2 + r.G3);
	r.B0 = nz.x;
	r.B1 = nz.z;
	r.bValid = r.SolidAngle > 1e-6f && abs(r.Z0) > 1e-7f && min(min(nz.x * nz.x, nz.y * nz.y), min(nz.z * nz.z, nz.w * nz.w)) < 0.99999f;
	return r;
}

float3 SampleSphericalRect (SSphericalRect R, float3 Origin, float2 U)
{
	const float au = U.x * R.SolidAngle + R.G2 + R.G3;
	const float fu = SafeDivide(cos(au) * R.B0 + R.B1, sin(au));
	float cu = (fu >= 0.0f ? 1.0f : -1.0f) / sqrt(fu * fu + R.B0 * R.B0);
	cu = clamp(cu, -1.0f, 1.0f);
	float xu = -(cu * R.Z0) / max(sqrt(1.0f - cu * cu), 1e-7f);
	xu = clamp(xu, R.X0, R.X1);
	const float d2 = xu * xu + R.Z0 * R.Z0;
	const float h0 = R.Y0 / sqrt(d2 + R.Y0 * R.Y0);
	const float h1 = R.Y1 / sqrt(d2 + R.Y1 * R.Y1);
	const float hv = h0 + U.y * (h1 - h0);
	const float hv2 = hv * hv;
	const float yv = (hv2 < 1.0f - 1e-6f) ? hv * sqrt(d2 / (1.0f - hv2)) : R.Y1;
	return Origin + xu * R.X + yv * R.Y + R.Z0 * R.Z;
}

#endif
