#ifndef FRT_MATH_HLSLI
#define FRT_MATH_HLSLI

static const float kPi = 3.14159265358979323846f;
static const float kTwoPi = 6.28318530717958647692f;
static const float kInvPi = 0.31830988618379067154f;
static const float kInvTwoPi = 0.15915494309189533577f;
static const float kInvFourPi = 0.07957747154594766788f;
static const float kHalfPi = 1.57079632679489661923f;
static const float kFloatMax = 3.402823466e+38f;

float Sq (float X) { return X * X; }
float2 Sq (float2 X) { return X * X; }
float3 Sq (float3 X) { return X * X; }
float MaxComponent (float3 V) { return max(V.x, max(V.y, V.z)); }
float MinComponent (float3 V) { return min(V.x, min(V.y, V.z)); }
float SafeSqrt (float X) { return sqrt(max(X, 0.0f)); }
float SafeAcos (float X) { return acos(clamp(X, -1.0f, 1.0f)); }
float SafeAsin (float X) { return asin(clamp(X, -1.0f, 1.0f)); }
float SafeDivide (float A, float B) { return B != 0.0f ? A / B : 0.0f; }

bool IsFiniteFloat (float X) { return !isnan(X) && !isinf(X); }
bool IsFiniteFloat3 (float3 X) { return IsFiniteFloat(X.x) && IsFiniteFloat(X.y) && IsFiniteFloat(X.z); }

// Orthonormal basis from a unit vector (Duff et al. 2017, "Building an Orthonormal Basis, Revisited").
void BuildOrthonormalBasis (float3 N, out float3 T, out float3 B)
{
	const float s = N.z >= 0.0f ? 1.0f : -1.0f;
	const float a = -1.0f / (s + N.z);
	const float b = N.x * N.y * a;
	T = float3(1.0f + s * N.x * N.x * a, s * b, -s * N.x);
	B = float3(b, s + N.y * N.y * a, -N.y);
}

float3 ToLocal (float3 V, float3 T, float3 B, float3 N) { return float3(dot(V, T), dot(V, B), dot(V, N)); }
float3 ToWorld (float3 V, float3 T, float3 B, float3 N) { return V.x * T + V.y * B + V.z * N; }

// Row-major 3x4 (three float4 rows) applied to a point / vector.
float3 TransformPoint (float4 M[3], float3 P)
{
	return float3(dot(M[0].xyz, P) + M[0].w, dot(M[1].xyz, P) + M[1].w, dot(M[2].xyz, P) + M[2].w);
}

float3 TransformVector (float4 M[3], float3 V)
{
	return float3(dot(M[0].xyz, V), dot(M[1].xyz, V), dot(M[2].xyz, V));
}

// Power heuristic and balance heuristic for two strategies.
float MisBalance (float PdfA, float PdfB) { return PdfA / max(PdfA + PdfB, 1e-30f); }
float MisPower (float PdfA, float PdfB)
{
	const float a2 = PdfA * PdfA;
	return a2 / max(a2 + PdfB * PdfB, 1e-30f);
}

// The path tracer's ViewZ guide carries one flag in its lowest mantissa bit: 1 = the surface was
// seen through a delta mirror (primary surface replacement, PathTracer.hlsl WriteGBuffer). History
// and filter taps must agree on it: a reflected floor lies on the real floor's plane.
uint ViewZSeenInMirror (float ViewZ) { return asuint(ViewZ) & 1u; }

#endif
