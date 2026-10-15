// Raster preview: the scene rasterized with the path tracer's materials and BSDF, for editing and
// as the renderer of GPUs without ray tracing. Every light is evaluated without rays:
//   sun       GGX BSDF x sun shadow map (PCF, normal-offset bias);
//   emitters  emissive instances as sphere lights of the same power (CGpuScene::
//             GetRasterEmitters): exact sphere irradiance, GGX with the representative-point lobe
//             widening (Karis 2013), cube shadow maps;
//   sky       open scenes: cosine-convolved sky irradiance and GGX-prefiltered sky radiance
//             (split sum, with the GGX energy LUT the path tracer uses);
//             enclosed scenes (they declare portals): the sky enters only through the portal
//             openings (exact polygon irradiance; for specular, the part of the GGX lobe that
//             passes through the opening), and the energy-balance ambient of the enclosure
//             (CGpuScene::GetRasterAmbient) stands in for the interreflections a rasterizer
//             cannot compute.
// Normals follow the path tracer's conventions: the geometric normal faces the viewer and the
// shading normal is kept in its hemisphere, so both renderers shade the same side of a surface.

#include "Common/Bindings.hlsli"
#include "Common/BSDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/Packing.hlsli"
#include "Common/Sampling.hlsli"
#include "Common/Scene.hlsli"
#include "Common/Sky.hlsli"
#include "Shared/RasterData.h"

cbuffer RootConstants : register(b0)
{
	uint g_InstanceIndex;
	uint g_GeometryIndex;
	float g_MipBias;
	uint g_Pad0;
};

ConstantBuffer<SRasterConstants> g_Raster : register(b2);

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
	// precise: the depth prepass and the shading pass must produce bit-identical depth.
	precise float4 position = mul(g_Frame.ViewProj, float4(o.WorldPosition, 1.0f));
	o.Position = position;
	o.Uv = In.Uv;
	o.Normal = normalize(TransformVector(instance.NormalToWorld, In.Normal));
	const float3 t = TransformVector(instance.ObjectToWorld, In.Tangent);
	const float3 c = cross(In.Normal, In.Tangent);
	float sign = dot(c, In.Bitangent) < 0.0f ? -1.0f : 1.0f;
	sign *= (instance.Flags & InstanceFlag_NegativeDeterminant) != 0u ? -1.0f : 1.0f;
	o.Tangent = float4(t, sign);
	return o;
}

// Depth prepass: opaque surfaces need no pixel work; alpha-masked ones run the shading pass's
// alpha test, so both passes agree on coverage.
void PsDepth ()
{
}

void PsDepthMasked (SVsOutput In)
{
	const SGeometryData geometry = LoadGeometry(g_GeometryIndex);
	const SMaterialData m = LoadMaterial(geometry.MaterialIndex);
	float alpha = m.BaseColorFactor.a;
	if (m.BaseColorTexture != FRT_INVALID_INDEX)
	{
		alpha *= Tex2DNonUniform(m.BaseColorTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias).a;
	}
	if ((m.Flags & MaterialFlag_AlphaMask) != 0u && alpha < m.AlphaCutoff)
	{
		discard;
	}
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

// ---------------------------------------------------------------------------------------------
// Shadow maps (reversed Z: a larger depth is nearer the light; cleared to 0 = no caster)
// ---------------------------------------------------------------------------------------------

// Bilinear PCF from one Gather: the lit fraction of the 2x2 texels around Uv, weighted bilinearly.
float ShadowBilinear (Texture2D<float> Map, float2 Uv, float2 Texel, float Depth)
{
	const float2 st = Uv / Texel - 0.5f;
	const float2 f = frac(st);
	const float4 occluder = Map.GatherRed(s_PointClamp, (floor(st) + 1.0f) * Texel);
	const float4 lit = step(occluder, Depth.xxxx);    // x (0,1)  y (1,1)  z (1,0)  w (0,0)
	return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// 3x3 bilinear taps one texel apart: a 4x4-texel tent-like footprint, smooth under motion.
float ShadowPcf (Texture2D<float> Map, float2 Uv, float2 Texel, float Depth, float4 Bounds)
{
	float lit = 0.0f;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			lit += ShadowBilinear(Map, clamp(Uv + float2(x, y) * Texel, Bounds.xy, Bounds.zw), Texel, Depth);
		}
	}
	return lit * (1.0f / 9.0f);
}

// Normal-offset receiver bias: move the lookup off the surface along the geometric normal,
// more for grazing light (where one texel spans the most depth), plus half a texel toward the
// light. Scales with the world size of a shadow texel, so it adapts to the map resolution.
float3 ShadowReceiverOffset (float3 P, float3 Ng, float3 L, float TexelWorld)
{
	const float cosine = saturate(dot(Ng, L));
	const float sine = SafeSqrt(1.0f - cosine * cosine);
	return P + Ng * (TexelWorld * (0.75f + 1.5f * sine)) + L * (0.5f * TexelWorld);
}

float SunVisibility (float3 P, float3 Ng, float3 L)
{
	if ((g_Raster.Flags & RasterFlag_SunShadow) == 0u)
	{
		return 1.0f;
	}
	const float3 Po = ShadowReceiverOffset(P, Ng, L, g_Raster.SunShadowTexelWorld);
	const float4 clip = mul(g_Raster.Views[RasterSunShadowView].ViewProj, float4(Po, 1.0f));
	const float2 uv = clip.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(uv < 0.0f) || any(uv > 1.0f) || clip.z < 0.0f)
	{
		return 1.0f; // outside the fitted scene bounds: nothing can occlude it
	}
	const float2 texel = g_Raster.SunShadowTexel;
	return ShadowPcf(Tex2DFloat(g_Raster.SunShadowMap), uv, texel, saturate(clip.z), float4(0.5f * texel, 1.0f - 0.5f * texel));
}

static const float2 kPoissonDisk[16] = {
	float2(-0.94201624f, -0.39906216f), float2(0.94558609f, -0.76890725f), float2(-0.09418410f, -0.92938870f), float2(0.34495938f, 0.29387760f),
	float2(-0.91588581f, 0.45771432f), float2(-0.81544232f, -0.87912464f), float2(-0.38277543f, 0.27676845f), float2(0.97484398f, 0.75648379f),
	float2(0.44323325f, -0.97511554f), float2(0.53742981f, -0.47373420f), float2(-0.26496911f, -0.41893023f), float2(0.79197514f, 0.19090188f),
	float2(-0.24188840f, 0.99706507f), float2(-0.81409955f, 0.91437590f), float2(0.19984126f, 0.78641367f), float2(0.14383161f, -0.14100790f) };

// Soft shadow from a cube shadow map of a light with a size (PCSS, Fernando 2005): average the
// blockers' distance over the region the light covers, size the penumbra from it, then PCF
// over that penumbra. The Poisson disk turns per pixel and frame; the TAA integrates it.
float CubeVisibility (uint FirstView, float3 P, float3 Ng, float2 Pixel)
{
	if (FirstView == FRT_INVALID_INDEX)
	{
		return 1.0f;
	}
	const float3 origin = g_Raster.Views[FirstView].Origin;
	const float lightRadius = g_Raster.Views[FirstView].LightRadius;
	const float3 D = P - origin;
	const float3 a = abs(D);
	const float major = max(a.x, max(a.y, a.z));
	const uint face = (a.x >= a.y && a.x >= a.z) ? (D.x >= 0.0f ? 0u : 1u) : (a.y >= a.z ? (D.y >= 0.0f ? 2u : 3u) : (D.z >= 0.0f ? 4u : 5u));
	const float tile = g_Raster.LocalShadowTileTexels;
	// A cube-face texel spans 2 * depth / tile texels in world units at this depth.
	const float texelWorld = 2.0f * major / tile;
	const float3 Po = ShadowReceiverOffset(P, Ng, -D / max(length(D), 1e-6f), texelWorld);
	const SRasterShadowView view = g_Raster.Views[FirstView + face];
	const float4 clip = mul(view.ViewProj, float4(Po, 1.0f));
	if (clip.w <= 0.0f)
	{
		return 1.0f;
	}
	const float depth = saturate(clip.z / clip.w);
	const float nearZ = clip.z;            // reversed infinite projection: clip.z = near, clip.w = view z
	const float receiverZ = clip.w;
	const float2 texel = g_Raster.LocalShadowTexel;
	const float2 uv = view.AtlasRect.xy + saturate(clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f) * view.AtlasRect.zw;
	const float4 bounds = float4(view.AtlasRect.xy + 1.5f * texel, view.AtlasRect.xy + view.AtlasRect.zw - 1.5f * texel);
	Texture2D<float> map = Tex2DFloat(g_Raster.LocalShadowMap);

	// Blocker search over the light's footprint for blockers halfway to the receiver.
	const float searchTexels = clamp(lightRadius * tile / (2.0f * receiverZ), 1.0f, 12.0f);
	float blockerZ = 0.0f;
	float blockers = 0.0f;
	[unroll]
	for (uint i = 0u; i < 4u; ++i)
	{
		const float2 offset = float2((i & 1u) ? 0.5f : -0.5f, (i & 2u) ? 0.5f : -0.5f) * searchTexels;
		const float4 occluder = map.GatherRed(s_PointClamp, clamp(uv + offset * texel, bounds.xy, bounds.zw));
		const float4 isBlocker = step(depth + 1e-7f, occluder);
		blockers += dot(isBlocker, 1.0f);
		blockerZ += dot(isBlocker, nearZ / max(occluder, 1e-7f));
	}
	if (blockers <= 0.0f)
	{
		return 1.0f;
	}
	blockerZ /= blockers;
	const float penumbraTexels = lightRadius * max(receiverZ - blockerZ, 0.0f) / max(blockerZ, 1e-3f) * tile / (2.0f * receiverZ);
	const float radius = clamp(penumbraTexels, 1.0f, 16.0f);

	const float angle = kTwoPi * frac(52.9829189f * frac(dot(Pixel, float2(0.06711056f, 0.00583715f)) + 0.61803399f * float(g_Frame.FrameIndex & 63u)));
	const float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
	float lit = 0.0f;
	[unroll]
	for (uint k = 0u; k < 16u; ++k)
	{
		const float2 offset = mul(rotation, kPoissonDisk[k]) * radius;
		lit += ShadowBilinear(map, clamp(uv + offset * texel, bounds.xy, bounds.zw), texel, depth);
	}
	return lit * (1.0f / 16.0f);
}

// ---------------------------------------------------------------------------------------------
// Light integrals
// ---------------------------------------------------------------------------------------------

// Illuminance of a unit-radiance sphere seen under sin^2(sigma) = (r / d)^2 by a surface whose
// normal makes CosTheta with the sphere's centre, horizon included (Lagarde & de Rousiers 2014).
float SphereIlluminance (float CosTheta, float SinSigmaSq)
{
	SinSigmaSq = min(SinSigmaSq, 0.9999f);
	if (CosTheta * CosTheta > SinSigmaSq)
	{
		return kPi * SinSigmaSq * saturate(CosTheta);
	}
	const float sinTheta = SafeSqrt(1.0f - CosTheta * CosTheta);
	const float x = sqrt(1.0f / SinSigmaSq - 1.0f);
	const float y = -x * (CosTheta / max(sinTheta, 1e-6f));
	const float sinThetaSqrtY = sinTheta * SafeSqrt(1.0f - y * y);
	return max((CosTheta * SafeAcos(y) - x * sinThetaSqrtY) * SinSigmaSq + atan(sinThetaSqrtY / x), 0.0f);
}

// Irradiance per unit radiance from the rectangle C +- U +- V onto a surface (P, N): Lambert's
// polygon formula through the vector irradiance. Unclipped, i.e. exact while the opening is
// above the tangent plane (portals are small next to their distance).
float RectIrradiance (float3 P, float3 N, float3 C, float3 U, float3 V)
{
	float3 v[4] = { normalize(C - U - V - P), normalize(C + U - V - P), normalize(C + U + V - P), normalize(C - U + V - P) };
	float3 F = 0.0f;
	[unroll]
	for (uint i = 0u; i < 4u; ++i)
	{
		const float3 a = v[i];
		const float3 b = v[(i + 1u) & 3u];
		const float3 c = cross(a, b);
		const float s = length(c);
		if (s > 1e-7f)
		{
			F += atan2(s, dot(a, b)) * (c / s);
		}
	}
	F *= 0.5f;
	if (dot(F, C - P) < 0.0f)
	{
		F = -F; // the winding decides the sign; the vector irradiance points at the opening
	}
	return max(dot(F, N), 0.0f);
}

// Solid angle / (2 pi) of the intersection of two spherical caps (Oat & Sander 2007, smoothstep
// approximation as in Jimenez et al. 2016).
float CapIntersection (float CosCap1, float CosCap2, float CosDistance)
{
	const float r1 = SafeAcos(CosCap1);
	const float r2 = SafeAcos(CosCap2);
	const float d = SafeAcos(CosDistance);
	if (min(r1, r2) <= max(r1, r2) - d)
	{
		return 1.0f - max(CosCap1, CosCap2);
	}
	if (r1 + r2 <= d)
	{
		return 0.0f;
	}
	const float delta = abs(r1 - r2);
	const float x = 1.0f - saturate((d - delta) / max(r1 + r2 - delta, 1e-4f));
	return x * x * (3.0f - 2.0f * x) * (1.0f - max(CosCap1, CosCap2));
}

// Half-angle of the cone that holds most of a GGX reflection lobe: about twice the half-vector
// spread, so mirrors get a point and alpha = 1 the hemisphere.
float SpecularConeCos (float Alpha)
{
	return cos(min(2.0f * atan(Alpha), kHalfPi));
}

// Energy normalisation of a GGX lobe evaluated at the representative point of a round light
// (the point of the light nearest the mirror ray) with its own Alpha (Karis 2013). The light,
// of angular radius asin(sqrt(SinSigmaSq)), covers a cap of SinSigmaSq / (4 VoH) * pi in
// half-vector space (dw_L = 4 VoH dw_H), which acts like a second GGX lobe of that alpha^2;
// the two widths add in quadrature. Karis's a' = a + r / (2 d) ignores the 1 / VoH: tested
// against the integral of the lobe over the light, it was up to 3-4x too bright at grazing
// view and on near-mirrors; this form stays within ~10% of it where the light is reflected.
float RepresentativePointNormalization (float Alpha, float SinSigmaSq, float VoH)
{
	const float a2 = Alpha * Alpha;
	return a2 / min(a2 + SinSigmaSq / (4.0f * max(VoH, 1e-3f)), 1.0f);
}

// Frostbite's dominant direction of a GGX lobe: rough lobes lean from R toward N.
float3 SpecularDominantDirection (float3 N, float3 R, float Alpha)
{
	const float smoothness = saturate(1.0f - Alpha);
	return normalize(lerp(N, R, smoothness * (sqrt(smoothness) + Alpha)));
}

float3 SampleSkyIrradiance (float3 N)
{
	return Tex2D(g_Raster.SkyIrradiance).SampleLevel(s_LinearClamp, EqualAreaSphereToSquare(N), 0.0f).rgb;
}

float3 SampleSkyPrefiltered (float3 R, float Roughness)
{
	Texture3D<float4> map = ResourceDescriptorHeap[g_Raster.SkyPrefiltered];
	const float slices = float(g_Raster.SkyPrefilterSlices);
	const float z = (saturate(Roughness) * (slices - 1.0f) + 0.5f) / slices;
	return map.SampleLevel(s_LinearClamp, float3(EqualAreaSphereToSquare(R), z), 0.0f).rgb;
}

// The sky reaches P directly only when P is on the inner side of every portal of an enclosure.
bool IsInsideEnclosure (float3 P)
{
	if ((g_Raster.Flags & RasterFlag_Enclosed) == 0u)
	{
		return false;
	}
	for (uint i = 0u; i < g_Raster.PortalCount; ++i)
	{
		const SPortalData portal = g_Raster.Portals[i];
		if (dot(P - portal.Center, portal.Normal) < -1e-3f)
		{
			return false;
		}
	}
	return true;
}

struct SLighting
{
	float3 Diffuse;
	float3 Specular;
};

SPsOutput PsMain (SVsOutput In)
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
	roughness = saturate(roughness);
	metallic = saturate(metallic);
	baseColor.rgb = saturate(baseColor.rgb);

	const float3 P = In.WorldPosition;
	const float3 V = normalize(g_Frame.CameraPosition - P);

	// Geometric normal of the triangle (its plane, from the position derivatives within the
	// quad), facing the viewer: the path tracer's convention. The winding of the mesh and the
	// rasterizer's front-face rule then no longer decide which side gets lit.
	float3 Ng = cross(ddx(P), ddy(P));
	float3 N = normalize(In.Normal);
	Ng = dot(Ng, Ng) > 1e-24f ? normalize(Ng) : N;
	if (dot(Ng, V) < 0.0f)
	{
		Ng = -Ng;
	}
	if (dot(N, Ng) < 0.0f)
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
	if (dot(N, Ng) < 0.0f)
	{
		N = reflect(N, Ng);
	}
	N = EnsureValidReflection(Ng, V, N);

	float3 emission = m.EmissiveFactor;
	if (m.EmissiveTexture != FRT_INVALID_INDEX)
	{
		emission *= Tex2DNonUniform(m.EmissiveTexture).SampleBias(s_AnisoWrap, In.Uv, g_MipBias).rgb;
	}

	float3 T, B;
	BuildOrthonormalBasis(N, T, B);
	const float NoV = max(dot(V, N), 1e-4f);
	const float3 Vl = float3(dot(V, T), dot(V, B), NoV);

	// The path tracer's BSDF set-up (MakeBsdfParams): GGX energy LUT for the directional albedo
	// and the multiple-scattering compensation.
	SBsdfParams p;
	p.F0 = lerp((0.04f * m.SpecularFactor).xxx, baseColor.rgb, metallic);
	p.DielectricF0 = 0.04f * m.SpecularFactor;
	p.DiffuseAlbedo = baseColor.rgb * (1.0f - metallic);
	p.Alpha = roughness * roughness;
	if (p.Alpha < kMinAlpha)
	{
		p.Alpha = 0.0f;
	}
	float2 lut = EnergyTermsMirror(NoV);
	if (g_Frame.EnergyCompensationLut != FRT_INVALID_INDEX && p.Alpha > 0.0f)
	{
		lut = SampleEnergyLut(Tex2DFloat2(g_Frame.EnergyCompensationLut), s_LinearClamp, NoV, p.Alpha);
	}
	EvalEnergyTerms(p, NoV, lut);
	// Split-sum weights of image-based light: what the specular layer reflects of a uniform
	// environment, and what the diffuse base receives under the dielectric coat.
	const float3 specularAlbedo = SpecularDirectionalAlbedo(p, NoV);
	const float3 diffuseWeight = p.DiffuseAlbedo * (1.0f - p.DielectricSpecularAlbedo);

	const uint debug = g_Raster.DebugMode;
	float ao = 1.0f;
	if ((g_Raster.Flags & RasterFlag_AmbientOcclusion) != 0u)
	{
		ao = Tex2DFloat(g_Raster.AmbientOcclusion).Load(int3(In.Position.xy, 0));
	}
	// Specular occlusion from AO (Lagarde & de Rousiers 2014).
	const float specularOcclusion = saturate(pow(NoV + ao, exp2(-16.0f * p.Alpha - 1.0f)) - 1.0f + ao);
	// Light also bounces inside the occluded neighbourhood: bright surfaces lose less to AO
	// (multi-bounce fit of Jimenez et al. 2016, per channel of the diffuse albedo).
	const float3 aoA = 2.0404f * p.DiffuseAlbedo - 0.3324f;
	const float3 aoB = -4.7951f * p.DiffuseAlbedo + 0.6417f;
	const float3 aoC = 2.7552f * p.DiffuseAlbedo + 0.6903f;
	const float3 diffuseOcclusion = max(ao.xxx, ((ao * aoA + aoB) * ao + aoC) * ao);

	SLighting sun = (SLighting)0;
	SLighting emitters = (SLighting)0;
	SLighting sky = (SLighting)0;
	SLighting ambient = (SLighting)0;
	// Stays 0 where the surface faces away from the sun, so the sun-shadow debug view shows
	// where sunlight lands (it showed 1 there: facing-away and lit looked the same).
	float sunVisibility = 0.0f;

	// ---- sun ----
	if (HasFrameFlag(FrameFlag_SunEnabled))
	{
		const float3 L = g_Frame.SunDirection;
		const float3 Ll = float3(dot(L, T), dot(L, B), dot(L, N));
		if (Ll.z > 0.0f && dot(L, Ng) > 0.0f)
		{
			sunVisibility = SunVisibility(P, Ng, L);
			if (sunVisibility > 0.0f)
			{
				// Sun radiance over its solid angle = irradiance at normal incidence.
				const float3 irradiance = g_Frame.SunRadiance * (kTwoPi * (1.0f - g_Frame.SunCosAngularRadius)) * sunVisibility;
				sun.Diffuse = EvalDiffuse(p, NoV, Ll.z) * irradiance;
				// Specular: the solar disk as a round light, representative point and the lobe's
				// own alpha. (The lobe widened by the disk, evaluated at its centre and then also
				// renormalised, made smooth highlights 2-8x too dark, with a jump at kMinAlpha.)
				const float3 Rw = reflect(-V, N);
				const float cosToR = dot(Rw, L);
				const float sinSunSq = 1.0f - Sq(g_Frame.SunCosAngularRadius);
				float3 Ls = Rw;
				if (cosToR < g_Frame.SunCosAngularRadius)
				{
					const float3 toR = Rw - cosToR * L;
					Ls = dot(toR, toR) > 1e-12f ? g_Frame.SunCosAngularRadius * L + sqrt(sinSunSq) * normalize(toR) : L;
				}
				const float3 Lsl = float3(dot(Ls, T), dot(Ls, B), dot(Ls, N));
				if (Lsl.z > 0.0f)
				{
					SBsdfParams ps = p;
					ps.Alpha = max(p.Alpha, kMinAlpha);
					const float3 H = normalize(Vl + Lsl);
					sun.Specular = EvalSpecular(ps, Vl, Lsl, H) * RepresentativePointNormalization(ps.Alpha, sinSunSq, dot(Vl, H)) * irradiance;
				}
			}
		}
	}

	// ---- emitters as sphere lights ----
	for (uint i = 0u; i < g_Raster.EmitterCount; ++i)
	{
		const SRasterEmitterData E = g_Raster.Emitters[i];
		const float3 toCenter = E.Position - P;
		const float dist2 = dot(toCenter, toCenter);
		const float dist = sqrt(dist2);
		if (dist <= E.Radius)
		{
			continue;
		}
		const float3 Lc = toCenter / dist;
		const float sinSigmaSq = Sq(E.Radius) / dist2;
		const float illuminance = SphereIlluminance(dot(N, Lc), sinSigmaSq);
		if (illuminance <= 0.0f || dot(Lc, Ng) < -E.Radius / dist)
		{
			continue;
		}
		const float visibility = CubeVisibility(E.ShadowView, P, Ng, In.Position.xy);
		if (visibility <= 0.0f)
		{
			continue;
		}
		emitters.Diffuse += diffuseWeight * kInvPi * E.Radiance * illuminance * visibility;

		// Representative point: the point of the sphere closest to the mirror ray, where the
		// lobe keeps its own alpha and is renormalised for the sphere's size (Karis 2013, see
		// RepresentativePointNormalization). Evaluating a widened lobe as well applied the
		// widening twice: smooth surfaces lost most of the highlight (a mirror 99.9%).
		const float3 R = reflect(-V, N);
		const float3 centerToRay = dot(toCenter, R) * R - toCenter;
		const float3 closest = toCenter + centerToRay * saturate(E.Radius / max(length(centerToRay), 1e-6f));
		const float3 Ls = normalize(closest);
		const float3 Lsl = float3(dot(Ls, T), dot(Ls, B), dot(Ls, N));
		if (Lsl.z > 0.0f)
		{
			SBsdfParams ps = p;
			ps.Alpha = max(p.Alpha, kMinAlpha);
			const float3 H = normalize(Vl + Lsl);
			const float normalization = RepresentativePointNormalization(ps.Alpha, sinSigmaSq, dot(Vl, H));
			// Point-light equivalent: the sphere's illuminance at normal incidence.
			emitters.Specular += EvalSpecular(ps, Vl, Lsl, H) * normalization * E.Radiance * (kPi * sinSigmaSq) * visibility;
		}
	}

	// ---- sky ----
	const float3 R = reflect(-V, N);
	const float3 Rd = SpecularDominantDirection(N, R, p.Alpha);
	float portalSpecularFraction = 0.0f;
	if (HasFrameFlag(FrameFlag_SkyEnabled))
	{
		if (IsInsideEnclosure(P))
		{
			// A mirror's lobe (alpha 0) is a cone of zero size: the covered fraction would be
			// 0 / 0 and came out 0, so mirrors never showed the opening. Give it the smallest
			// lobe the BSDF evaluates instead.
			const float coneCos = SpecularConeCos(max(p.Alpha, kMinAlpha));
			for (uint i = 0u; i < g_Raster.PortalCount; ++i)
			{
				const SPortalData portal = g_Raster.Portals[i];
				const float3 toPortal = portal.Center - P;
				const float3 d = normalize(toPortal);
				// Sky radiance seen through the opening (small: one direction stands for it),
				// shadowed like an area light of the opening's size.
				const float visibility = CubeVisibility(g_Raster.PortalShadowViews[i], P, Ng, In.Position.xy);
				if (visibility <= 0.0f)
				{
					continue;
				}
				const float3 radiance = EvalSkyEnvironment(d) * visibility;
				sky.Diffuse += diffuseWeight * kInvPi * radiance * RectIrradiance(P, N, portal.Center, portal.AxisU, portal.AxisV);
				// Specular: the fraction of the lobe's cone that the opening's cone covers.
				const SSphericalRect rect = InitSphericalRect(P, portal.Center - portal.AxisU - portal.AxisV, 2.0f * portal.AxisU, 2.0f * portal.AxisV);
				const float solidAngle = rect.bValid ? rect.SolidAngle : portal.Area * saturate(dot(-d, portal.Normal)) / max(dot(toPortal, toPortal), 1e-6f);
				const float portalCos = 1.0f - min(solidAngle, kTwoPi) * kInvTwoPi;
				const float fraction = saturate(CapIntersection(portalCos, coneCos, dot(Rd, d)) / max(1.0f - coneCos, 1e-6f));
				portalSpecularFraction += fraction;
				sky.Specular += specularAlbedo * radiance * fraction;
			}
			portalSpecularFraction = saturate(portalSpecularFraction);
		}
		else
		{
			sky.Diffuse = diffuseWeight * kInvPi * SampleSkyIrradiance(N) * diffuseOcclusion;
			// Mirror directions below the geometric horizon would see the inside of the surface.
			const float horizon = saturate(1.0f + 4.0f * dot(Rd, Ng));
			sky.Specular = specularAlbedo * SampleSkyPrefiltered(Rd, roughness) * (horizon * horizon) * specularOcclusion;
		}
	}

	// ---- interreflections: the enclosure's energy-balance ambient ----
	if ((g_Raster.Flags & RasterFlag_Enclosed) != 0u && IsInsideEnclosure(P))
	{
		ambient.Diffuse = diffuseWeight * g_Raster.AmbientRadiance * diffuseOcclusion;
		ambient.Specular = specularAlbedo * g_Raster.AmbientRadiance * (1.0f - portalSpecularFraction) * specularOcclusion;
	}

	float3 color = emission + sun.Diffuse + sun.Specular + emitters.Diffuse + emitters.Specular +
		sky.Diffuse + sky.Specular + ambient.Diffuse + ambient.Specular;
	switch (debug)
	{
		case RasterDebug_Sun: color = sun.Diffuse + sun.Specular; break;
		case RasterDebug_Emitters: color = emitters.Diffuse + emitters.Specular; break;
		case RasterDebug_Sky: color = sky.Diffuse + sky.Specular; break;
		case RasterDebug_Ambient: color = ambient.Diffuse + ambient.Specular; break;
		case RasterDebug_Specular: color = sun.Specular + emitters.Specular + sky.Specular + ambient.Specular; break;
		case RasterDebug_Diffuse: color = sun.Diffuse + emitters.Diffuse + sky.Diffuse + ambient.Diffuse; break;
		case RasterDebug_SunShadow: color = sunVisibility.xxx; break;
		case RasterDebug_Occlusion: color = ao.xxx; break;
		case RasterDebug_Normal: color = N * 0.5f + 0.5f; break;
		default: break;
	}

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
	if (g_Raster.DebugMode != RasterDebug_None)
	{
		o.Color = 0.0f;
	}
	const float3 farPoint = g_Frame.CameraPosition + dir * 1e5f;
	o.Motion = ProjectPixel(g_Frame.PrevViewProjNoJitter, farPoint) - ProjectPixel(g_Frame.ViewProjNoJitter, farPoint);
	o.ViewZ = 1e6f;
	return o;
}
