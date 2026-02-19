#include "Common.hlsl"

// Raytracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure SceneBVH : register(t0);

Texture2D<float4> gMaterialTextures[16] : register(t1);
StructuredBuffer<VSInput> gVertices : register(t17);
StructuredBuffer<uint> gIndices : register(t18);
SamplerState gLinearSampler : register(s0);


struct ShadowHitInfo
{
	bool isHit;
};


// 32-bit hash/PRNG step
uint NextUint (inout uint s)
{
	s ^= s >> 17;
	s *= 0xED5AD4BBu;
	s ^= s >> 11;
	s *= 0xAC4C1B51u;
	s ^= s >> 15;
	s *= 0x31848BABu;
	s ^= s >> 14;
	return s;
}

float NextFloat01 (inout uint s)
{
	return (NextUint(s) & 0x00FFFFFFu) / 16777216.0f; // [0,1)
}

static const uint MaterialTextureIndexInvalid = 0xFFFFFFFF;

uint GetMaterialTextureIndex (uint index)
{
	if (index < 4)
	{
		return gMaterialTextureIndices0[index];
	}
	if (index < 8)
	{
		return gMaterialTextureIndices1[index - 4];
	}
	if (index < 12)
	{
		return gMaterialTextureIndices2[index - 8];
	}
	return gMaterialTextureIndices3[index - 12];
}

float ComputeTextureLod (
	float2 uv0,
	float2 uv1,
	float2 uv2,
	float3 position0,
	float3 position1,
	float3 position2,
	uint2 textureSize)
{
	if (textureSize.x == 0 || textureSize.y == 0)
	{
		return 0.0f;
	}

	const float2 duv1 = uv1 - uv0;
	const float2 duv2 = uv2 - uv0;
	const float uvArea2 = abs(duv1.x * duv2.y - duv1.y * duv2.x);
	if (uvArea2 <= 1e-8f)
	{
		return 0.0f;
	}

	const float3 edge1 = position1 - position0;
	const float3 edge2 = position2 - position0;
	const float worldArea2 = length(cross(edge1, edge2));
	if (worldArea2 <= 1e-8f)
	{
		return 0.0f;
	}

	const float invProjY = abs(gProj[1][1]);
	if (invProjY <= 1e-8f || gRenderTargetSize.y <= 1.0f)
	{
		return 0.0f;
	}

	const float tanHalfFovY = 1.0f / invProjY;
	const float pixelWorldSize = (2.0f * max(RayTCurrent(), 0.0f) * tanHalfFovY) / gRenderTargetSize.y;

	const float worldUnitsPerUv = sqrt(worldArea2 / uvArea2);
	if (worldUnitsPerUv <= 1e-8f)
	{
		return 0.0f;
	}

	const float uvFootprint = pixelWorldSize / worldUnitsPerUv;
	const float texelFootprint = uvFootprint * max((float)textureSize.x, (float)textureSize.y);
	return log2(max(texelFootprint, 1e-8f));
}

[shader("closesthit")]
void ClosestHit (inout HitInfo payload, Attributes attrib)
{
	float3 lightPos = float3(0, 2, -3); // TODO: pass actual pos from code

	// shadow

	float3 hitDir = WorldRayDirection();
	// Find the world-space hit position
	float3 worldOrigin = WorldRayOrigin() + RayTCurrent() * hitDir;

	float3 lightDir = normalize(lightPos - worldOrigin);

	// Fire a shadow ray. The direction is hard-coded here, but can be fetched
	// from a constant-buffer
	RayDesc ray;
	ray.Origin = worldOrigin;
	ray.Direction = lightDir;
	ray.TMin = 0.01;
	ray.TMax = 100000;

	// Initialize the ray payload
	ShadowHitInfo shadowPayload;
	shadowPayload.isHit = false;

	// Trace the ray
	TraceRay(
		// Acceleration structure
		SceneBVH,
		// Flags can be used to specify the behavior upon hitting a surface
		RAY_FLAG_NONE,
		// Instance inclusion mask, which can be used to mask out some geometry to
		// this ray by and-ing the mask with a geometry mask. The 0xFF flag then
		// indicates no geometry will be masked
		0xFF,
		// Depending on the type of ray, a given object can have several hit
		// groups attached (ie. what to do when hitting to compute regular
		// shading, and what to do when hitting to compute shadows). Those hit
		// groups are specified sequentially in the SBT, so the value below
		// indicates which offset (on 4 bits) to apply to the hit groups for this
		// ray. In this sample we only have one hit group per object, hence an
		// offset of 0.
		1,
		// The offsets in the SBT can be computed from the object ID, its instance
		// ID, but also simply by the order the objects have been pushed in the
		// acceleration structure. This allows the application to group shaders in
		// the SBT in the same order as they are added in the AS, in which case
		// the value below represents the stride (4 bits representing the number
		// of hit groups) between two consecutive objects.
		2,
		// Index of the miss shader to use in case several consecutive miss
		// shaders are present in the SBT. This allows to change the behavior of
		// the program when no geometry have been hit, for example one to return a
		// sky color for regular rendering, and another returning a full
		// visibility value for shadow rays. This sample has only one miss shader,
		// hence an index 0
		1,
		// Ray information to trace
		ray,
		// Payload associated to the ray, which will be used to communicate
		// between the hit/miss shaders and the raygen
		shadowPayload);
	// float factor = shadowPayload.isHit ? 0.3 : 1.0;
	const float factor = 1.0;
	const uint currentDepth = payload.depth;
	const uint maxReflectionDepth = 10u;

	float3 barycentrics =
		float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

	const uint primitiveIndex = PrimitiveIndex();
	const uint indexBase = primitiveIndex * 3;
	const uint i0 = gIndices[indexBase + 0];
	const uint i1 = gIndices[indexBase + 1];
	const uint i2 = gIndices[indexBase + 2];

	const VSInput v0 = gVertices[i0];
	const VSInput v1 = gVertices[i1];
	const VSInput v2 = gVertices[i2];
	const float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;
	const float3 worldPosition0 = mul(ObjectToWorld3x4(), float4(v0.position, 1.0f));
	const float3 worldPosition1 = mul(ObjectToWorld3x4(), float4(v1.position, 1.0f));
	const float3 worldPosition2 = mul(ObjectToWorld3x4(), float4(v2.position, 1.0f));
	float3 geometricNormal = normalize(cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0));
	float3 normal = normalize(v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z);
	normal = normalize(mul((float3x3)ObjectToWorld3x4(), normal));

	const float geomFlip = 1.0f - 2.0f * step(0.0f, dot(geometricNormal, hitDir));
	const float shadeFlip = 1.0f - 2.0f * step(0.0f, dot(normal, hitDir));
	geometricNormal *= geomFlip;
	normal *= shadeFlip;
	normal = lerp(geometricNormal, normal, step(0.0f, dot(normal, geometricNormal)));

	float3 tangent = normalize(v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z);
	tangent = normalize(mul((float3x3)ObjectToWorld3x4(), tangent));

	// Gram-Schmidt to keep tangent orthogonal to normal
	tangent = normalize(tangent - dot(tangent, normal) * normal);
	const float3 bitangent = normalize(cross(normal, tangent));
	float3x3 tangentToWorld = float3x3(tangent, bitangent, normal);

	uint2 p = DispatchRaysIndex().xy;
	uint seed = p.x * 1973u ^ p.y * 9277u ^ gFrameIndex * 26699u ^ payload.depth * 3181u;

	float u1 = NextFloat01(seed);
	float u2 = NextFloat01(seed);

	float r = sqrt(u1); // we don't want to oversample the center
	float phi = 2.0f * 3.14159265359f * u2;

	float3 reflectDir = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - u1)); // unit hemisphere dir

	// float3 reflectDir = normalize(reflect(hitDir, normal));
	reflectDir = normalize(mul(reflectDir, tangentToWorld));

	// texture & color
	float3 hitColor = gDiffuseAlbedo.xyz;

	const uint baseColorTextureIndex = GetMaterialTextureIndex(0);
	if (baseColorTextureIndex != MaterialTextureIndexInvalid)
	{
		const float2 wrappedUv = frac(uv);
		uint2 textureSize = uint2(0, 0);
		gMaterialTextures[baseColorTextureIndex].GetDimensions(textureSize.x, textureSize.y);
		const float lod = ComputeTextureLod(
			v0.uv, v1.uv, v2.uv,
			worldPosition0, worldPosition1, worldPosition2,
			textureSize);
		const float3 sampledColor = gMaterialTextures[baseColorTextureIndex]
									.SampleLevel(gLinearSampler, wrappedUv, lod)
									.rgb;
		hitColor *= sampledColor;
	}

	// float3 reflectDir = normalize(reflect(hitDir, normal));

	if (currentDepth >= maxReflectionDepth)
	{
		// payload.color = hitColor * factor;
		payload.color = float3(1.f, 0.07f, 0.94f);
		payload.distance = RayTCurrent();
		return;
	}

	RayDesc reflectedRay;
	reflectedRay.Origin = worldOrigin;
	reflectedRay.Direction = reflectDir;
	reflectedRay.TMin = 0.01;
	reflectedRay.TMax = 100000;

	HitInfo reflectedPayload;
	reflectedPayload.color = float3(0.0f, 0.0f, 0.0f);
	reflectedPayload.distance = 0.0f;
	reflectedPayload.depth = currentDepth + 1u;
	TraceRay(SceneBVH, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 2, 0, reflectedRay, reflectedPayload);

	payload.color = reflectedPayload.color * hitColor * factor;
	payload.distance = RayTCurrent();
}
