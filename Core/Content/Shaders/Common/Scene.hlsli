#ifndef FRT_SCENE_HLSLI
#define FRT_SCENE_HLSLI

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Packing.hlsli"

struct SRtVertex
{
	float3 Position;
	float3 Normal;
	float4 Tangent;   // xyz tangent, w bitangent sign
	float2 Uv;
};

SRtVertex LoadRtVertex (ByteAddressBuffer Buffer, uint VertexIndex)
{
	const uint address = VertexIndex * RtVertexStride;
	const uint4 a = Buffer.Load4(address);
	const uint3 b = Buffer.Load3(address + 16u);
	SRtVertex v;
	v.Position = asfloat(a.xyz);
	v.Normal = DecodeOctahedral(UnpackSnorm2x16(a.w));
	const uint tangentBits = b.x;
	v.Tangent.xyz = DecodeOctahedral(UnpackSnorm2x16(tangentBits & ~1u));
	v.Tangent.w = (tangentBits & 1u) ? -1.0f : 1.0f;
	v.Uv = asfloat(b.yz);
	return v;
}

struct STriangleVertices
{
	float3 P[3];       // world space
	float2 Uv[3];
	float3 N[3];       // object space
	float4 T[3];       // object space + sign
};

STriangleVertices LoadTriangle (SInstanceData Instance, SGeometryData Geometry, uint PrimitiveIndex)
{
	ByteAddressBuffer indices = ByteBufferNonUniform(Geometry.IndexBufferSrv);
	ByteAddressBuffer vertices = ByteBufferNonUniform(Geometry.VertexBufferSrv);
	const uint3 tri = indices.Load3((Geometry.IndexOffset + PrimitiveIndex * 3u) * 4u) + Geometry.VertexOffset;

	STriangleVertices t;
	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		const SRtVertex v = LoadRtVertex(vertices, tri[i]);
		t.P[i] = TransformPoint(Instance.ObjectToWorld, v.Position);
		t.Uv[i] = v.Uv;
		t.N[i] = v.Normal;
		t.T[i] = v.Tangent;
	}
	return t;
}

float3 PrevWorldPosition (SInstanceData Instance, SGeometryData Geometry, uint PrimitiveIndex, float3 Bary)
{
	ByteAddressBuffer indices = ByteBufferNonUniform(Geometry.IndexBufferSrv);
	ByteAddressBuffer vertices = ByteBufferNonUniform(Geometry.VertexBufferSrv);
	const uint3 tri = indices.Load3((Geometry.IndexOffset + PrimitiveIndex * 3u) * 4u) + Geometry.VertexOffset;
	float3 p = 0.0f;
	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		p += Bary[i] * asfloat(vertices.Load3(tri[i] * RtVertexStride));
	}
	return TransformPoint(Instance.PrevObjectToWorld, p);
}

// ------------------------------------------------------------------------------------------
// Surface hit
// ------------------------------------------------------------------------------------------

struct SSurfaceHit
{
	float3 Position;         // precise, from barycentrics
	float3 GeometricNormal;  // unit, world, facing the incoming ray
	float3 ShadingNormal;    // unit, world, same hemisphere as GeometricNormal
	float3 Tangent;
	float3 Bitangent;
	float2 Uv;
	float TriangleUvArea;    // |duv1 x duv2|
	float TriangleWorldArea; // |e1 x e2|
	bool bFrontFace;
	uint InstanceIndex;
	uint GeometryIndex;      // global
	uint PrimitiveIndex;
	uint MaterialIndex;
	uint GeometryFlags;
};

SSurfaceHit ReconstructSurfaceHit (
	uint InstanceIndex,
	uint LocalGeometryIndex,
	uint PrimitiveIndex,
	float2 Barycentrics,
	float3 RayDirection)
{
	SSurfaceHit hit = (SSurfaceHit)0;
	const SInstanceData instance = LoadInstance(InstanceIndex);
	const uint geometryIndex = instance.GeometryBase + LocalGeometryIndex;
	const SGeometryData geometry = LoadGeometry(geometryIndex);
	const STriangleVertices tri = LoadTriangle(instance, geometry, PrimitiveIndex);
	const float3 b = float3(1.0f - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);

	hit.InstanceIndex = InstanceIndex;
	hit.GeometryIndex = geometryIndex;
	hit.PrimitiveIndex = PrimitiveIndex;
	hit.MaterialIndex = geometry.MaterialIndex;
	hit.GeometryFlags = geometry.Flags;

	hit.Position = tri.P[0] * b.x + tri.P[1] * b.y + tri.P[2] * b.z;
	hit.Uv = tri.Uv[0] * b.x + tri.Uv[1] * b.y + tri.Uv[2] * b.z;

	const float3 e1 = tri.P[1] - tri.P[0];
	const float3 e2 = tri.P[2] - tri.P[0];
	const float3 ngRaw = cross(e1, e2);
	hit.TriangleWorldArea = length(ngRaw);
	float3 ng = ngRaw / max(hit.TriangleWorldArea, 1e-30f);
	const float2 duv1 = tri.Uv[1] - tri.Uv[0];
	const float2 duv2 = tri.Uv[2] - tri.Uv[0];
	hit.TriangleUvArea = abs(duv1.x * duv2.y - duv1.y * duv2.x);

	float3 n = tri.N[0] * b.x + tri.N[1] * b.y + tri.N[2] * b.z;
	n = normalize(TransformVector(instance.NormalToWorld, n));
	if (any(isnan(n)))
	{
		n = ng;
	}

	float4 t = tri.T[0] * b.x + tri.T[1] * b.y + tri.T[2] * b.z;
	float3 tw = TransformVector(instance.ObjectToWorld, t.xyz);
	const float tangentSign = (tri.T[0].w < 0.0f ? -1.0f : 1.0f) *
		((instance.Flags & InstanceFlag_NegativeDeterminant) != 0u ? -1.0f : 1.0f);

	// Face the incoming ray. Front-facing is decided on the geometric normal.
	hit.bFrontFace = dot(ng, RayDirection) < 0.0f;
	if (!hit.bFrontFace)
	{
		ng = -ng;
	}
	if (dot(n, ng) < 0.0f)
	{
		n = -n;
	}
	hit.GeometricNormal = ng;
	hit.ShadingNormal = n;

	// Gram-Schmidt the tangent; fall back to an arbitrary frame for untangented meshes.
	tw = tw - n * dot(n, tw);
	const float tl = length(tw);
	if (tl > 1e-6f)
	{
		hit.Tangent = tw / tl;
		hit.Bitangent = cross(n, hit.Tangent) * tangentSign;
	}
	else
	{
		BuildOrthonormalBasis(n, hit.Tangent, hit.Bitangent);
	}
	return hit;
}

// ------------------------------------------------------------------------------------------
// Texture LOD from ray cones (Akenine-Möller et al. 2021). ConeWidth is the cone width at the
// hit; the base LOD maps world footprint to texel footprint through the triangle's uv density.
// ------------------------------------------------------------------------------------------

float ComputeRayConeLod (SSurfaceHit Hit, float ConeWidth, float3 RayDirection, uint2 TextureSize)
{
	if (Hit.TriangleUvArea <= 0.0f || Hit.TriangleWorldArea <= 0.0f)
	{
		return 0.0f;
	}
	const float lambda = 0.5f * log2(Hit.TriangleUvArea / Hit.TriangleWorldArea);
	const float cosTheta = max(abs(dot(RayDirection, Hit.GeometricNormal)), 0.05f);
	return lambda + log2(max(abs(ConeWidth), 1e-12f) / cosTheta) + 0.5f * log2(float(TextureSize.x * TextureSize.y));
}

float4 SampleMaterialTexture (uint TextureIndex, float2 Uv, float Lod)
{
	Texture2D<float4> tex = Tex2DNonUniform(TextureIndex);
	return tex.SampleLevel(s_LinearWrap, Uv, max(Lod, 0.0f));
}

float SampleTextureLod (uint TextureIndex, SSurfaceHit Hit, float ConeWidth, float3 RayDirection)
{
	Texture2D<float4> tex = Tex2DNonUniform(TextureIndex);
	uint w, h;
	tex.GetDimensions(w, h);
	return ComputeRayConeLod(Hit, ConeWidth, RayDirection, uint2(w, h));
}

// ------------------------------------------------------------------------------------------
// Material evaluation at a hit
// ------------------------------------------------------------------------------------------

struct SMaterialSample
{
	float3 BaseColor;
	float Opacity;
	float Metallic;
	float Roughness;          // perceptual
	float3 Emission;          // radiance
	float3 ShadingNormal;     // after normal mapping
	float SpecularFactor;
	uint Flags;
};

// Bends a shading normal toward the geometric normal just enough that the mirror reflection of
// V leaves above the geometric surface (Cycles' ensure_valid_specular_reflection). Normal maps
// and interpolated normals otherwise send part of every lobe below the surface, which loses
// energy (dark crevices, black silhouettes) and breaks the specular lobe at grazing angles.
// Ng and N in the same hemisphere as V.
float3 EnsureValidReflection (float3 Ng, float3 V, float3 N)
{
	const float3 R = 2.0f * dot(N, V) * N - V;
	const float Vz = dot(V, Ng);
	// Reflections may always be at least as shallow as the incoming direction.
	const float threshold = min(0.9f * Vz, 0.01f);
	if (dot(Ng, R) >= threshold)
	{
		return N;
	}
	// Frame: Ng = Z, X = component of N orthogonal to Ng. Solve for N' in the X-Z plane with
	// dot(reflect(V, N'), Ng) = threshold.
	const float3 Xraw = N - dot(N, Ng) * Ng;
	const float xl = length(Xraw);
	if (xl < 1e-6f)
	{
		return Ng;
	}
	const float3 X = Xraw / xl;
	const float Vx = dot(V, X);
	const float a = Sq(Vx) + Sq(Vz);
	const float b = 2.0f * (a + Vz * threshold);
	const float c = Sq(threshold + Vz);
	const float disc = SafeSqrt(Sq(b) - 4.0f * a * c);
	const float Nz2 = Vx < 0.0f ? 0.25f * (b + disc) / a : 0.25f * (b - disc) / a;
	const float Nx = SafeSqrt(1.0f - Nz2);
	const float Nz = SafeSqrt(Nz2);
	return normalize(Nx * X + Nz * Ng);
}

SMaterialSample EvaluateMaterial (inout SSurfaceHit Hit, float ConeWidth, float3 RayDirection, bool bNormalMapping)
{
	const SMaterialData m = LoadMaterial(Hit.MaterialIndex);
	SMaterialSample s;
	s.BaseColor = m.BaseColorFactor.rgb;
	s.Opacity = m.BaseColorFactor.a;
	s.Metallic = m.Metallic;
	s.Roughness = m.Roughness;
	s.Emission = m.EmissiveFactor;
	s.ShadingNormal = Hit.ShadingNormal;
	s.SpecularFactor = m.SpecularFactor;
	s.Flags = m.Flags;

	// One LOD per texture size class would be more exact; the base-colour LOD is a good proxy
	// for the others because glTF assets almost always use matching resolutions per material.
	float lod = 0.0f;
	if (m.BaseColorTexture != FRT_INVALID_INDEX)
	{
		lod = SampleTextureLod(m.BaseColorTexture, Hit, ConeWidth, RayDirection);
		const float4 c = SampleMaterialTexture(m.BaseColorTexture, Hit.Uv, lod);
		s.BaseColor *= c.rgb;
		s.Opacity *= c.a;
	}
	if (m.MetallicRoughnessTexture != FRT_INVALID_INDEX)
	{
		const float l = SampleTextureLod(m.MetallicRoughnessTexture, Hit, ConeWidth, RayDirection);
		const float4 mr = SampleMaterialTexture(m.MetallicRoughnessTexture, Hit.Uv, l);
		s.Roughness *= mr.g;
		s.Metallic *= mr.b;
	}
	if (m.EmissiveTexture != FRT_INVALID_INDEX && any(s.Emission > 0.0f))
	{
		const float l = SampleTextureLod(m.EmissiveTexture, Hit, ConeWidth, RayDirection);
		s.Emission *= SampleMaterialTexture(m.EmissiveTexture, Hit.Uv, l).rgb;
	}
	if (bNormalMapping && m.NormalTexture != FRT_INVALID_INDEX)
	{
		const float l = SampleTextureLod(m.NormalTexture, Hit, ConeWidth, RayDirection);
		float3 tn = SampleMaterialTexture(m.NormalTexture, Hit.Uv, l).xyz * 2.0f - 1.0f;
		tn.xy *= m.NormalScale;
		tn.z = SafeSqrt(1.0f - saturate(dot(tn.xy, tn.xy)));
		s.ShadingNormal = normalize(tn.x * Hit.Tangent + tn.y * Hit.Bitangent + tn.z * Hit.ShadingNormal);
	}
	if (dot(s.ShadingNormal, Hit.GeometricNormal) < 0.0f)
	{
		s.ShadingNormal = reflect(s.ShadingNormal, Hit.GeometricNormal);
	}
	s.ShadingNormal = EnsureValidReflection(Hit.GeometricNormal, -RayDirection, s.ShadingNormal);
	s.Roughness = saturate(s.Roughness);
	s.Metallic = saturate(s.Metallic);
	s.BaseColor = saturate(s.BaseColor);
	return s;
}

// Alpha only (any-hit / shadow candidates). Uses a footprint estimated from the hit distance.
float EvaluateOpacity (uint InstanceIndex, uint LocalGeometryIndex, uint PrimitiveIndex, float2 Barycentrics, float HitT)
{
	const SInstanceData instance = LoadInstance(InstanceIndex);
	const SGeometryData geometry = LoadGeometry(instance.GeometryBase + LocalGeometryIndex);
	const SMaterialData m = LoadMaterial(geometry.MaterialIndex);
	float alpha = m.BaseColorFactor.a;
	if (m.BaseColorTexture == FRT_INVALID_INDEX)
	{
		return alpha;
	}

	ByteAddressBuffer indices = ByteBufferNonUniform(geometry.IndexBufferSrv);
	ByteAddressBuffer vertices = ByteBufferNonUniform(geometry.VertexBufferSrv);
	const uint3 tri = indices.Load3((geometry.IndexOffset + PrimitiveIndex * 3u) * 4u) + geometry.VertexOffset;
	const float3 b = float3(1.0f - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);
	float2 uv[3];
	float3 p[3];
	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		uv[i] = asfloat(vertices.Load2(tri[i] * RtVertexStride + 20u));
		p[i] = TransformPoint(instance.ObjectToWorld, asfloat(vertices.Load3(tri[i] * RtVertexStride)));
	}
	const float2 hitUv = uv[0] * b.x + uv[1] * b.y + uv[2] * b.z;
	Texture2D<float4> tex = Tex2DNonUniform(m.BaseColorTexture);
	uint w, h;
	tex.GetDimensions(w, h);
	const float uvArea = abs((uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - (uv[1].y - uv[0].y) * (uv[2].x - uv[0].x));
	const float worldArea = length(cross(p[1] - p[0], p[2] - p[0]));
	float lod = 0.0f;
	if (uvArea > 0.0f && worldArea > 0.0f)
	{
		const float coneWidth = HitT * g_Frame.PixelSpreadAngle;
		lod = 0.5f * log2(uvArea / worldArea) + log2(max(coneWidth, 1e-12f)) + 0.5f * log2(float(w * h));
	}
	alpha *= tex.SampleLevel(s_LinearWrap, hitUv, max(lod, 0.0f)).a;
	return alpha;
}

#endif
