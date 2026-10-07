// Raster preview: a cheap forward approximation of the path-traced image for editing and as a
// fallback when ray tracing is unavailable. Direct sun (no shadows) + hemispherical sky
// ambient + emission, with the same materials and textures as the path tracer.

#include "Common/Bindings.hlsli"
#include "Common/BSDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/Packing.hlsli"
#include "Common/Sky.hlsli"

cbuffer RootConstants : register(b0)
{
	uint g_InstanceIndex;
	uint g_GeometryIndex;
	float g_MipBias;
	uint g_Pad0;
};

struct SVsInput
{
	float3 Position : POSITION;
	float2 Uv : TEXCOORD;
	float3 Normal : NORMAL;
	float3 Tangent : TANGENT;
	float3 Bitangent : BITANGENT;
	float4 Color : COLOR;
};

struct SVsOutput
{
	float4 Position : SV_POSITION;
	float3 WorldPosition : WORLDPOS;
	float3 PrevWorldPosition : PREVPOS;
	float2 Uv : TEXCOORD;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
};

SVsOutput VsMain (SVsInput In)
{
	const SInstanceData instance = LoadInstance(g_InstanceIndex);
	SVsOutput o;
	o.WorldPosition = TransformPoint(instance.ObjectToWorld, In.Position);
	o.PrevWorldPosition = TransformPoint(instance.PrevObjectToWorld, In.Position);
	o.Position = mul(g_Frame.ViewProj, float4(o.WorldPosition, 1.0f));
	o.Uv = In.Uv;
	o.Normal = normalize(TransformVector(instance.NormalToWorld, In.Normal));
	const float3 t = TransformVector(instance.ObjectToWorld, In.Tangent);
	const float3 c = cross(In.Normal, In.Tangent);
	float sign = dot(c, In.Bitangent) < 0.0f ? -1.0f : 1.0f;
	sign *= (instance.Flags & InstanceFlag_NegativeDeterminant) != 0u ? -1.0f : 1.0f;
	o.Tangent = float4(t, sign);
	return o;
}

struct SPsOutput
{
	float4 Color : SV_Target0;
	float2 Motion : SV_Target1;
	float ViewZ : SV_Target2;
};

float2 ProjectPixel (float4x4 M, float3 P)
{
	const float4 clip = mul(M, float4(P, 1.0f));
	const float2 ndc = clip.xy / clip.w;
	return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * g_Frame.RenderSize;
}

SPsOutput PsMain (SVsOutput In, bool bFrontFace : SV_IsFrontFace)
{
	const SGeometryData geometry = LoadGeometry(g_GeometryIndex);
	const SMaterialData m = LoadMaterial(geometry.MaterialIndex);

	float4 baseColor = m.BaseColorFactor;
	if (m.BaseColorTexture != FRT_INVALID_INDEX)
	{
		baseColor *= Tex2DNonUniform(m.BaseColorTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias);
	}
	if ((m.Flags & MaterialFlag_AlphaMask) != 0u && baseColor.a < m.AlphaCutoff)
	{
		discard;
	}
	float roughness = m.Roughness;
	float metallic = m.Metallic;
	if (m.MetallicRoughnessTexture != FRT_INVALID_INDEX)
	{
		const float4 mr = Tex2DNonUniform(m.MetallicRoughnessTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias);
		roughness *= mr.g;
		metallic *= mr.b;
	}

	float3 N = normalize(In.Normal);
	if (!bFrontFace)
	{
		N = -N;
	}
	if (m.NormalTexture != FRT_INVALID_INDEX)
	{
		float3 T = In.Tangent.xyz - N * dot(N, In.Tangent.xyz);
		if (dot(T, T) > 1e-8f)
		{
			T = normalize(T);
			const float3 B = cross(N, T) * In.Tangent.w;
			float3 tn = Tex2DNonUniform(m.NormalTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias).xyz * 2.0f - 1.0f;
			tn.xy *= m.NormalScale;
			tn.z = SafeSqrt(1.0f - saturate(dot(tn.xy, tn.xy)));
			N = normalize(tn.x * T + tn.y * B + tn.z * N);
		}
	}

	float3 emission = m.EmissiveFactor;
	if (m.EmissiveTexture != FRT_INVALID_INDEX)
	{
		emission *= Tex2DNonUniform(m.EmissiveTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias).rgb;
	}

	const float3 V = normalize(g_Frame.CameraPosition - In.WorldPosition);
	float3 T, B;
	BuildOrthonormalBasis(N, T, B);
	const float3 Vl = float3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4f));

	SBsdfParams p;
	p.F0 = lerp((0.04f * m.SpecularFactor).xxx, baseColor.rgb, metallic);
	p.DielectricF0 = 0.04f * m.SpecularFactor;
	p.DiffuseAlbedo = baseColor.rgb * (1.0f - metallic);
	p.Alpha = max(roughness * roughness, 0.02f);
	EvalEnergyTerms(p, Vl.z, EnergyTermsMirror(Vl.z)); // preview: no LUT, Schlick at NoV

	float3 color = emission;
	if (HasFrameFlag(FrameFlag_SunEnabled))
	{
		const float3 L = g_Frame.SunDirection;
		const float3 Ll = float3(dot(L, T), dot(L, B), dot(L, N));
		if (Ll.z > 0.0f)
		{
			const SBsdfEval e = EvalBsdf(p, Vl, Ll);
			// Sun radiance over its solid angle = irradiance at normal incidence.
			const float solidAngle = kTwoPi * (1.0f - g_Frame.SunCosAngularRadius);
			color += (e.Diffuse + e.Specular) * g_Frame.SunRadiance * solidAngle;
		}
	}
	// Hemispherical ambient: irradiance-ish from the sky in the normal direction (no occlusion).
	const float3 skyN = EvalSkyEnvironment(N);
	const float3 skyR = EvalSkyEnvironment(reflect(-V, N));
	color += p.DiffuseAlbedo * skyN + SpecularDirectionalAlbedo(p, Vl.z) * skyR;

	SPsOutput o;
	o.Color = float4(color, baseColor.a);
	o.Motion = ProjectPixel(g_Frame.PrevViewProjNoJitter, In.PrevWorldPosition) - ProjectPixel(g_Frame.ViewProjNoJitter, In.WorldPosition);
	// The lowest mantissa bit of ViewZ is the TAA's "seen in a mirror" flag (ViewZSeenInMirror,
	// set only by the path tracer): keep it clear.
	o.ViewZ = asfloat(asuint(dot(In.WorldPosition - g_Frame.CameraPosition, g_Frame.CameraForward)) & ~1u);
	return o;
}

// Sky for pixels the geometry did not cover (full-screen triangle at the far plane).
struct SSkyVsOutput
{
	float4 Position : SV_POSITION;
	float2 Ndc : NDC;
};

SSkyVsOutput VsSky (uint VertexId : SV_VertexID)
{
	SSkyVsOutput o;
	const float2 uv = float2((VertexId << 1u) & 2u, VertexId & 2u);
	o.Ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	o.Position = float4(o.Ndc, 0.0f, 1.0f); // reversed Z: 0 = far
	return o;
}

SPsOutput PsSky (SSkyVsOutput In)
{
	const float4 world = mul(g_Frame.InvViewProjNoJitter, float4(In.Ndc, 0.5f, 1.0f));
	const float3 dir = normalize(world.xyz / world.w - g_Frame.CameraPosition);
	SPsOutput o;
	o.Color = float4(EvalEnvironment(dir, true), 1.0f);
	const float3 farPoint = g_Frame.CameraPosition + dir * 1e5f;
	o.Motion = ProjectPixel(g_Frame.PrevViewProjNoJitter, farPoint) - ProjectPixel(g_Frame.ViewProjNoJitter, farPoint);
	o.ViewZ = 1e6f;
	return o;
}
